#pragma once
#include "asio_api.h"
#include "config.h"
#include <atomic>
#include <cmath>
#include <cstring>
#include <vector>

namespace aggregate {
// One native ASIO clock master per process. Other devices use independent WASAPI streams.
class NativeAsio {
    IASIO* driver_=nullptr;
    bool com_initialized_=false,buffers_created_=false,started_=false;
    long inputs_=0,outputs_=0,frames_=0,input_latency_=0,output_latency_=0;
    unsigned sample_rate_=48000;
    std::vector<ASIOBufferInfo> buffers_;
    std::vector<ASIOSampleType> types_;
    std::vector<float> input_,output_;
    ASIOCallbacks callbacks_{};
    using Process=void(*)(void*,const float*,float*,unsigned);
    Process process_=nullptr;
    void* user_=nullptr;
    LARGE_INTEGER frequency_{};
    inline static std::atomic<NativeAsio*> active_{nullptr};

public:
    static float decode(const void* buffer,ASIOSampleType type,unsigned frame) {
        switch(type) {
            case ASIOSTFloat32LSB: return static_cast<const float*>(buffer)[frame];
            case ASIOSTFloat64LSB: return float(static_cast<const double*>(buffer)[frame]);
            case ASIOSTInt32LSB: return float(static_cast<const int32_t*>(buffer)[frame]/2147483648.);
            case ASIOSTInt16LSB: return static_cast<const int16_t*>(buffer)[frame]/32768.f;
            case ASIOSTInt24LSB: {
                auto* p=static_cast<const uint8_t*>(buffer)+size_t(frame)*3;
                int32_t value=int32_t(uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16));
                if(value&0x800000) value|=int32_t(0xff000000u);
                return value/8388608.f;
            }
            default: return 0;
        }
    }
    static void encode(void* buffer,ASIOSampleType type,unsigned frame,float sample) {
        double value=std::isfinite(sample)?std::clamp(double(sample),-1.,1.):0.;
        switch(type) {
            case ASIOSTFloat32LSB: static_cast<float*>(buffer)[frame]=float(value); break;
            case ASIOSTFloat64LSB: static_cast<double*>(buffer)[frame]=value; break;
            case ASIOSTInt32LSB: static_cast<int32_t*>(buffer)[frame]=int32_t(std::clamp(value*2147483648.,-2147483648.,2147483647.)); break;
            case ASIOSTInt16LSB: static_cast<int16_t*>(buffer)[frame]=int16_t(std::clamp(value*32768.,-32768.,32767.)); break;
            case ASIOSTInt24LSB: {
                int32_t n=int32_t(std::clamp(value*8388608.,-8388608.,8388607.));
                auto* p=static_cast<uint8_t*>(buffer)+size_t(frame)*3;
                p[0]=uint8_t(n); p[1]=uint8_t(uint32_t(n)>>8); p[2]=uint8_t(uint32_t(n)>>16); break;
            }
        }
    }
private:
    static void callback(long half,ASIOBool) {
        auto* self=active_.load(std::memory_order_acquire);
        if(!self || half<0 || half>1) return;
        LARGE_INTEGER before{},after{}; QueryPerformanceCounter(&before);
        for(long c=0;c<self->inputs_;++c)
            for(long f=0;f<self->frames_;++f)
                self->input_[size_t(f)*self->inputs_+c]=decode(self->buffers_[c].buffers[half],self->types_[c],unsigned(f));
        std::fill(self->output_.begin(),self->output_.end(),0.f);
        self->process_(self->user_,self->input_.data(),self->output_.data(),unsigned(self->frames_));
        for(long c=0;c<self->outputs_;++c)
            for(long f=0;f<self->frames_;++f)
                encode(self->buffers_[self->inputs_+c].buffers[half],self->types_[self->inputs_+c],unsigned(f),self->output_[size_t(f)*self->outputs_+c]);
        // Calling outputReady is an optimization; unsupported drivers return ASE_NotPresent.
        self->driver_->outputReady();
        QueryPerformanceCounter(&after);
        uint64_t elapsed=uint64_t(after.QuadPart-before.QuadPart);
        if(elapsed>self->maximum_ticks.load(std::memory_order_relaxed)) self->maximum_ticks.store(elapsed,std::memory_order_relaxed);
        if(elapsed*self->sample_rate_>uint64_t(self->frequency_.QuadPart)*self->frames_) ++self->deadline_misses;
    }
    static ASIOTime* time_callback(ASIOTime* time,long half,ASIOBool direct) { callback(half,direct); return time; }
    static void rate_changed(ASIOSampleRate rate) {
        auto* self=active_.load(); if(self && rate!=self->sample_rate_) self->events.fetch_add(1);
    }
    static long message(long selector,long value,void*,double*) {
        if(selector==kAsioEngineVersion) return 2;
        if(selector==kAsioSupportsTimeInfo) return 1;
        if(selector==kAsioSelectorSupported)
            return value==kAsioResetRequest || value==kAsioResyncRequest || value==kAsioLatenciesChanged;
        if(selector==kAsioResetRequest || selector==kAsioResyncRequest || selector==kAsioLatenciesChanged) {
            if(auto* self=active_.load()) ++self->events;
            return 0; // No real-time reconfiguration in this prototype; diagnostics marks failure.
        }
        return 0;
    }
    void check(ASIOError error,const char* operation) {
        if(error==ASE_OK) return;
        char detail[124]{}; driver_->getErrorMessage(detail);
        throw std::runtime_error(std::string("Native ASIO ")+operation+": "+detail+" ("+std::to_string(error)+")");
    }
    void close() {
        stop();
        if(buffers_created_) { driver_->disposeBuffers(); buffers_created_=false; }
        NativeAsio* expected=this; active_.compare_exchange_strong(expected,nullptr);
        if(driver_) { driver_->Release(); driver_=nullptr; }
        if(com_initialized_) { CoUninitialize(); com_initialized_=false; }
    }
public:
    std::atomic<uint64_t> events{0};
    std::atomic<uint64_t> maximum_ticks{0},deadline_misses{0};
    NativeAsio(const std::string& clsid,unsigned frames,unsigned sample_rate=48000,bool inspect=false):frames_(frames),sample_rate_(sample_rate) {
        try {
            QueryPerformanceFrequency(&frequency_);
            CLSID id{};
            auto text=std::wstring(clsid.begin(),clsid.end());
            if(FAILED(CLSIDFromString(text.c_str(),&id)) || IsEqualCLSID(id,aggregate_clsid)) throw std::runtime_error("Invalid native ASIO CLSID");
            auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
            com_initialized_=SUCCEEDED(com);
            if(FAILED(com) && com!=RPC_E_CHANGED_MODE) throw std::runtime_error("COM initialization failed");
            auto created=CoCreateInstance(id,nullptr,CLSCTX_INPROC_SERVER,id,reinterpret_cast<void**>(&driver_));
            if(FAILED(created) || !driver_) {
                char detail[32]{}; std::snprintf(detail,sizeof(detail),"0x%08lx",static_cast<unsigned long>(created));
                throw std::runtime_error(std::string("Cannot create native ASIO driver: ")+detail);
            }
            if(!driver_->init(GetDesktopWindow())) throw std::runtime_error("Native ASIO initialization failed");
            check(driver_->getChannels(&inputs_,&outputs_),"getChannels");
            if(inputs_<0 || inputs_>128 || outputs_<1 || outputs_>128) throw std::runtime_error("Unsupported native ASIO channel count");
            ASIOSampleRate rate=0; check(driver_->getSampleRate(&rate),"getSampleRate");
            if(!inspect && rate!=sample_rate_) throw std::runtime_error("Native ASIO sample rate differs from configuration; select its current rate or change the manufacturer settings");
            long minimum=0,maximum=0,preferred=0,granularity=0;
            check(driver_->getBufferSize(&minimum,&maximum,&preferred,&granularity),"getBufferSize");
            if(!inspect && (frames_<minimum || frames_>maximum || (granularity>0 && (frames_-minimum)%granularity) ||
               (granularity==-1 && (frames_&(frames_-1))))) throw std::runtime_error("Native ASIO block_frames unsupported: min="+std::to_string(minimum)+", max="+std::to_string(maximum)+", preferred="+std::to_string(preferred)+", granularity="+std::to_string(granularity));
            input_.resize(size_t(frames_)*inputs_); output_.resize(size_t(frames_)*outputs_);
        } catch(...) { close(); throw; }
    }
    ~NativeAsio() { close(); }
    void prepare(Process process,void* user) {
        process_=process; user_=user;
        NativeAsio* expected=nullptr;
        if(!active_.compare_exchange_strong(expected,this)) throw std::runtime_error("Another native ASIO master is active in this process");
        for(long c=0;c<inputs_+outputs_;++c) {
            ASIOBufferInfo buffer{}; buffer.isInput=c<inputs_?ASIOTrue:ASIOFalse;
            buffer.channelNum=c<inputs_?c:c-inputs_;
            ASIOChannelInfo channel{}; channel.isInput=buffer.isInput; channel.channel=buffer.channelNum;
            check(driver_->getChannelInfo(&channel),"getChannelInfo");
            if(channel.type!=ASIOSTFloat32LSB && channel.type!=ASIOSTFloat64LSB && channel.type!=ASIOSTInt32LSB &&
               channel.type!=ASIOSTInt24LSB && channel.type!=ASIOSTInt16LSB) throw std::runtime_error("Unsupported native ASIO sample format: "+std::to_string(channel.type));
            buffers_.push_back(buffer); types_.push_back(channel.type);
        }
        callbacks_={callback,rate_changed,message,time_callback};
        check(driver_->createBuffers(buffers_.data(),long(buffers_.size()),frames_,&callbacks_),"createBuffers"); buffers_created_=true;
        check(driver_->getLatencies(&input_latency_,&output_latency_),"getLatencies");
    }
    void start() { events=0; maximum_ticks=0; deadline_misses=0; check(driver_->start(),"start"); started_=true; }
    void stop() { if(started_) { driver_->stop(); started_=false; } }
    unsigned current_rate() { ASIOSampleRate rate=0; check(driver_->getSampleRate(&rate),"getSampleRate"); return unsigned(rate); }
    std::vector<unsigned> sample_rates() { std::vector<unsigned> values; for(unsigned r:{8000,16000,22050,32000,44100,48000,88200,96000,176400,192000}) if(driver_->canSampleRate(r)==ASE_OK) values.push_back(r); return values; }
    std::vector<unsigned> buffer_sizes() { long a,b,p,g; check(driver_->getBufferSize(&a,&b,&p,&g),"getBufferSize"); std::vector<unsigned> values; for(long n=std::max(32L,a);n<=std::min(2048L,b);++n) if((g==0&&n==p)||(g==-1&&!(n&(n-1)))||(g>0&&(n-a)%g==0)) values.push_back(unsigned(n)); return values; }
    unsigned preferred_buffer() { long a,b,p,g; check(driver_->getBufferSize(&a,&b,&p,&g),"getBufferSize"); return unsigned(p); }
    void control_panel() { check(driver_->controlPanel(),"controlPanel"); }
    unsigned inputs() const { return unsigned(inputs_); }
    std::string channel_name(bool input,unsigned channel) {
        ASIOChannelInfo info{}; info.isInput=input?ASIOTrue:ASIOFalse; info.channel=long(channel);
        check(driver_->getChannelInfo(&info),"getChannelInfo"); return info.name;
    }
    unsigned outputs() const { return unsigned(outputs_); }
    unsigned latency(bool input) const { return unsigned(input?input_latency_:output_latency_); }
    double maximum_us() const { return maximum_ticks.load()*1000000./frequency_.QuadPart; }
};
}
