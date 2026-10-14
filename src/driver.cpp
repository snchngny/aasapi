#include "asio_api.h"
#include "engine.h"
#include <cstring>
#include <memory>

namespace {
HMODULE module_handle=nullptr;
std::atomic<long> live_objects{0},server_locks{0};

std::filesystem::path configuration_path() {
    wchar_t buffer[32768]{};
    DWORD length=GetEnvironmentVariableW(L"AUDIO_AGGREGATE_CONFIG",buffer,32768);
    if(length && length<32768) return buffer;
    length=GetModuleFileNameW(module_handle,buffer,32768);
    if(!length || length>=32768) throw std::runtime_error("Cannot locate driver module");
    return std::filesystem::path(buffer).parent_path().parent_path()/"config"/"aggregate.ini";
}

class Driver final:public IASIO {
    std::atomic<ULONG> references_{1};
    std::unique_ptr<aggregate::Engine> engine_;
    aggregate::Config config_;
    ASIOCallbacks callbacks_{};
    struct Channel {
        bool input;
        unsigned index;
        std::array<std::vector<float>,2> buffers;
    };
    std::vector<Channel> channels_;
    bool buffers_created_=false,time_info_=false;
    unsigned half_=0;
    uint64_t sample_counter_=0;
    std::atomic<unsigned> clock_sequence_{0};
    std::atomic<uint64_t> position_{0},timestamp_{0};
    std::string error_="No error";

    static void process(void* user,const float* input,float* output,unsigned frames) {
        auto& driver=*static_cast<Driver*>(user);
        LARGE_INTEGER ticks{},frequency{};
        QueryPerformanceCounter(&ticks); QueryPerformanceFrequency(&frequency);
        uint64_t ns=uint64_t((long double)ticks.QuadPart*1000000000./frequency.QuadPart);
        driver.clock_sequence_.fetch_add(1,std::memory_order_acq_rel);
        driver.timestamp_.store(ns,std::memory_order_relaxed);
        driver.position_.store(driver.sample_counter_,std::memory_order_relaxed);
        driver.clock_sequence_.fetch_add(1,std::memory_order_release);
        for(auto& channel:driver.channels_) {
            auto& buffer=channel.buffers[driver.half_];
            if(channel.input)
                for(unsigned f=0;f<frames;++f) buffer[f]=input[size_t(f)*driver.engine_->inputs()+channel.index];
            else std::fill(buffer.begin(),buffer.end(),0.f);
        }
        if(driver.time_info_) {
            ASIOTime time{};
            time.timeInfo.sampleRate=driver.config_.sample_rate;
            time.timeInfo.speed=1.;
            split_samples(driver.position_.load(std::memory_order_relaxed),time.timeInfo.samplePosition);
            split_samples(ns,time.timeInfo.systemTime);
            time.timeInfo.flags=kSystemTimeValid|kSamplePositionValid|kSampleRateValid;
            driver.callbacks_.bufferSwitchTimeInfo(&time,driver.half_,ASIOTrue);
        } else driver.callbacks_.bufferSwitch(driver.half_,ASIOTrue);
        for(auto& channel:driver.channels_) if(!channel.input)
            for(unsigned f=0;f<frames;++f)
                output[size_t(f)*driver.engine_->outputs()+channel.index]=channel.buffers[driver.half_][f];
        driver.sample_counter_+=frames;
        driver.half_^=1;
    }
    ASIOError failure(const std::exception& exception) { error_=exception.what(); return ASE_HWMalfunction; }
public:
    Driver() { ++live_objects; }
    ~Driver() { engine_.reset(); --live_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(IsEqualIID(iid,IID_IUnknown) || IsEqualIID(iid,aggregate_clsid)) {
            *result=static_cast<IASIO*>(this); AddRef(); return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto count=--references_; if(!count) delete this; return count; }
    ASIOBool init(void*) override {
        if(engine_) return ASIOTrue;
        try {
            config_=aggregate::load_config(configuration_path());
            engine_=std::make_unique<aggregate::Engine>(config_);
            return ASIOTrue;
        } catch(const std::exception& exception) { failure(exception); return ASIOFalse; }
    }
    void getDriverName(char* name) override { if(name) std::strcpy(name,"AASAPI"); }
    long getDriverVersion() override { return 1; }
    void getErrorMessage(char* message) override {
        if(message) { std::strncpy(message,error_.c_str(),123); message[123]=0; }
    }
    ASIOError start() override {
        if(!buffers_created_ || engine_->running()) return ASE_InvalidMode;
        position_=0; timestamp_=0; half_=0; sample_counter_=0; clock_sequence_=0;
        try { engine_->start(); return ASE_OK; } catch(const std::exception& exception) { return failure(exception); }
    }
    ASIOError stop() override { if(engine_) engine_->stop(); return ASE_OK; }
    ASIOError getChannels(long* inputs,long* outputs) override {
        if(!engine_ || !inputs || !outputs) return ASE_InvalidParameter;
        *inputs=engine_->inputs(); *outputs=engine_->outputs(); return ASE_OK;
    }
    ASIOError getLatencies(long* input,long* output) override {
        if(!engine_ || !input || !output) return ASE_InvalidParameter;
        *input=engine_->latency_budget(true); *output=engine_->latency_budget(false); return ASE_OK;
    }
    ASIOError getBufferSize(long* minimum,long* maximum,long* preferred,long* granularity) override {
        if(!engine_ || !minimum || !maximum || !preferred || !granularity) return ASE_InvalidParameter;
        *minimum=*maximum=*preferred=config_.block_frames; *granularity=0; return ASE_OK;
    }
    ASIOError canSampleRate(ASIOSampleRate rate) override { return rate==config_.sample_rate?ASE_OK:ASE_NoClock; }
    ASIOError getSampleRate(ASIOSampleRate* rate) override {
        if(!rate || !engine_) return ASE_InvalidParameter;
        *rate=config_.sample_rate; return ASE_OK;
    }
    ASIOError setSampleRate(ASIOSampleRate rate) override { return canSampleRate(rate); }
    ASIOError getClockSources(ASIOClockSource* clocks,long* count) override {
        if(!clocks || !count || *count<1) return ASE_InvalidParameter;
        clocks[0]={}; clocks[0].index=0; clocks[0].associatedChannel=-1; clocks[0].associatedGroup=-1;
        clocks[0].isCurrentSource=ASIOTrue;
        std::strcpy(clocks[0].name,config_.native_asio_clsid.empty()?"Playback endpoint 0":"Native ASIO master");
        *count=1; return ASE_OK;
    }
    ASIOError setClockSource(long reference) override { return reference==0?ASE_OK:ASE_InvalidParameter; }
    ASIOError getSamplePosition(ASIOSamples* position,ASIOTimeStamp* timestamp) override {
        if(!position || !timestamp) return ASE_InvalidParameter;
        for(;;) {
            auto sequence=clock_sequence_.load(std::memory_order_acquire);
            if(sequence&1) continue;
            auto p=position_.load(std::memory_order_relaxed),t=timestamp_.load(std::memory_order_relaxed);
            std::atomic_thread_fence(std::memory_order_acquire);
            if(sequence==clock_sequence_.load(std::memory_order_relaxed)) {
                split_samples(p,*position); split_samples(t,*timestamp); return ASE_OK;
            }
        }
    }
    ASIOError getChannelInfo(ASIOChannelInfo* info) override {
        if(!engine_ || !info || info->channel<0 || unsigned(info->channel)>=(info->isInput?engine_->inputs():engine_->outputs()))
            return ASE_InvalidParameter;
        info->type=ASIOSTFloat32LSB; info->isActive=ASIOFalse;
        unsigned native_channels=info->isInput?engine_->native_inputs():engine_->native_outputs();
        if(unsigned(info->channel)<native_channels) {
            info->channelGroup=0;
            std::snprintf(info->name,sizeof(info->name),"Native%s:%02u",info->isInput?"In":"Out",unsigned(info->channel)+1);
            for(auto& channel:channels_) if(channel.input==bool(info->isInput) && channel.index==unsigned(info->channel)) info->isActive=ASIOTrue;
            return ASE_OK;
        }
        unsigned offset=native_channels,group=native_channels?1:0;
        const auto& endpoints=info->isInput?config_.capture:config_.playback;
        for(const auto& endpoint:endpoints) {
            if(unsigned(info->channel)<offset+endpoint.channels) break;
            offset+=endpoint.channels; ++group;
        }
        info->channelGroup=group;
        std::snprintf(info->name,sizeof(info->name),"%s%u:%02u",info->isInput?"Input":"Output",group,unsigned(info->channel)-offset+1);
        for(auto& channel:channels_) if(channel.input==bool(info->isInput) && channel.index==unsigned(info->channel)) info->isActive=ASIOTrue;
        return ASE_OK;
    }
    ASIOError createBuffers(ASIOBufferInfo* buffers,long count,long frames,ASIOCallbacks* callbacks) override {
        if(!engine_ || buffers_created_) return ASE_InvalidMode;
        if(!buffers || !callbacks || count<1 || count>256 || frames!=long(config_.block_frames)) return ASE_InvalidParameter;
        bool supports_time=callbacks->asioMessage && callbacks->bufferSwitchTimeInfo &&
            callbacks->asioMessage(kAsioSupportsTimeInfo,0,nullptr,nullptr)==1;
        if(!supports_time && !callbacks->bufferSwitch) return ASE_InvalidParameter;
        for(long i=0;i<count;++i) {
            if(buffers[i].channelNum<0 || unsigned(buffers[i].channelNum)>=(buffers[i].isInput?engine_->inputs():engine_->outputs())) return ASE_InvalidParameter;
            for(long j=0;j<i;++j) if(buffers[i].isInput==buffers[j].isInput && buffers[i].channelNum==buffers[j].channelNum) return ASE_InvalidParameter;
        }
        try {
            channels_.reserve(count);
            for(long i=0;i<count;++i) {
                Channel channel{bool(buffers[i].isInput),unsigned(buffers[i].channelNum),{}};
                for(auto& buffer:channel.buffers) buffer.resize(frames,0.f);
                channels_.push_back(std::move(channel));
            }
            callbacks_=*callbacks; time_info_=supports_time;
            engine_->prepare(process,this);
            for(long i=0;i<count;++i) for(unsigned h=0;h<2;++h) buffers[i].buffers[h]=channels_[i].buffers[h].data();
            buffers_created_=true; return ASE_OK;
        } catch(const std::exception& exception) {
            failure(exception); channels_.clear();
            engine_.reset();
            try { engine_=std::make_unique<aggregate::Engine>(config_); } catch(...) {}
            return ASE_HWMalfunction;
        }
    }
    ASIOError disposeBuffers() override {
        if(!engine_) return ASE_InvalidMode;
        if(engine_) engine_->stop();
        channels_.clear(); buffers_created_=false;
        engine_.reset();
        try { engine_=std::make_unique<aggregate::Engine>(config_); return ASE_OK; }
        catch(const std::exception& exception) { return failure(exception); }
    }
    ASIOError controlPanel() override { return ASE_NotPresent; }
    ASIOError future(long selector,void*) override { return selector==kAsioCanTimeInfo?ASE_SUCCESS:ASE_NotPresent; }
    ASIOError outputReady() override { return ASE_NotPresent; }
    std::string diagnostics() const { return engine_?engine_->diagnostics():"{}"; }
    unsigned health_flags() const { return engine_?engine_->health_flags():8; }
    unsigned copy_stopped_output(float* output,unsigned capacity) const { return engine_?engine_->copy_stopped_output(output,capacity):0; }
};

class Factory final:public IClassFactory {
    std::atomic<ULONG> references_{1};
public:
    Factory() { ++live_objects; }
    ~Factory() { --live_objects; }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** result) override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(IsEqualIID(iid,IID_IUnknown) || IsEqualIID(iid,IID_IClassFactory)) { *result=this; AddRef(); return S_OK; }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override { auto count=--references_; if(!count) delete this; return count; }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer,REFIID iid,void** result) override {
        if(!result) return E_POINTER;
        *result=nullptr;
        if(outer) return CLASS_E_NOAGGREGATION;
        try { auto* driver=new Driver; auto hr=driver->QueryInterface(iid,result); driver->Release(); return hr; }
        catch(...) { return E_OUTOFMEMORY; }
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override { if(lock) ++server_locks; else --server_locks; return S_OK; }
};
}
extern "C" HRESULT STDMETHODCALLTYPE DllGetClassObject(REFCLSID clsid,REFIID iid,void** result) {
    if(!result) return E_POINTER;
    *result=nullptr;
    if(!IsEqualCLSID(clsid,aggregate_clsid)) return CLASS_E_CLASSNOTAVAILABLE;
    try { auto* factory=new Factory; auto hr=factory->QueryInterface(iid,result); factory->Release(); return hr; }
    catch(...) { return E_OUTOFMEMORY; }
}
extern "C" HRESULT STDMETHODCALLTYPE DllCanUnloadNow() { return live_objects==0 && server_locks==0?S_OK:S_FALSE; }
extern "C" __declspec(dllexport) unsigned AggregateDiagnostics(IASIO* driver,char* output,unsigned capacity) {
    if(!driver || !output || !capacity) return 0;
    try {
        auto text=static_cast<Driver*>(driver)->diagnostics();
        if(text.size()+1>capacity) return 0;
        std::memcpy(output,text.c_str(),text.size()+1); return unsigned(text.size());
    } catch(...) { return 0; }
}
extern "C" __declspec(dllexport) unsigned AggregateHealthFlags(IASIO* driver) {
    return driver?static_cast<Driver*>(driver)->health_flags():8;
}
extern "C" __declspec(dllexport) unsigned AggregateCopyStoppedOutput(IASIO* driver,float* output,unsigned capacity) {
    return driver?static_cast<Driver*>(driver)->copy_stopped_output(output,capacity):0;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID) {
    if(reason==DLL_PROCESS_ATTACH) { module_handle=module; DisableThreadLibraryCalls(module); }
    return TRUE;
}
