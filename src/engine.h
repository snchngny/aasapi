#pragma once
#include "adaptive_audio.h"
#include "audio_backend.h"
#include "config.h"
#include "native_asio.h"
#include <functional>
#include <memory>
#include <thread>
#include <windows.h>

namespace aggregate {
inline std::string utf8(const wchar_t* value) {
    int size=WideCharToMultiByte(CP_UTF8,0,value,-1,nullptr,0,nullptr,nullptr);
    if(size<=1) return {};
    std::string text(size,'\0');
    WideCharToMultiByte(CP_UTF8,0,value,-1,text.data(),size,nullptr,nullptr);
    text.pop_back(); return text;
}
inline std::wstring wide(const std::string& value) {
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.c_str(),-1,nullptr,0);
    if(size<1) throw std::runtime_error("Invalid UTF-8");
    std::wstring text(size,L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.c_str(),-1,text.data(),size);
    text.pop_back(); return text;
}

class Engine {
public:
    struct Endpoint {
        Engine* engine;
        bool input,master=false,initialized=false,started=false;
        EndpointSpec spec;
        std::string name;
        ma_device device{};
        FrameRing ring;
        AdaptiveReader reader;
        std::vector<float> block;
        unsigned channel_offset=0;
        std::atomic<uint64_t> overruns{0},notifications{0},frames_delivered{0},largest_callback{0};
        std::atomic<float> output_peak{0};
        Endpoint(Engine* owner,bool capture,EndpointSpec endpoint,unsigned offset)
            :engine(owner),input(capture),spec(std::move(endpoint)),
             ring(spec.channels,32768),reader(ring,owner->kernel_,owner->queue_frames()),
             block(size_t(owner->config_.block_frames)*spec.channels),channel_offset(offset) {}
        ~Endpoint() { if(initialized) ma_device_uninit(&device); }
    };
private:
    Config config_;
    std::unique_ptr<NativeAsio> native_;
    SincKernel kernel_;
    ma_context context_{};
    bool context_initialized_=false,observe_outputs_=false;
    std::vector<std::unique_ptr<Endpoint>> capture_,playback_;
    std::atomic<bool> running_{false};
    std::thread simulation_thread_;
    std::vector<float> input_,output_;
    unsigned inputs_=0,outputs_=0,master_cursor_=0;
    using Process=void(*)(void*,const float*,float*,unsigned);
    Process process_=nullptr;
    void* process_user_=nullptr;
    std::atomic<uint64_t> callbacks_{0},max_callback_ticks_{0},deadline_misses_{0};
    LARGE_INTEGER frequency_{};

    static void data_callback(ma_device* device,void* output,const void* input,ma_uint32 frames) {
        auto& endpoint=*static_cast<Endpoint*>(device->pUserData);
        auto& engine=*endpoint.engine;unsigned delivered=frames;
        if(!engine.running_.load(std::memory_order_acquire)) return;
        endpoint.frames_delivered.fetch_add(frames,std::memory_order_relaxed);
        if(frames>endpoint.largest_callback.load(std::memory_order_relaxed)) endpoint.largest_callback.store(frames,std::memory_order_relaxed);
        if(endpoint.input) {
            if(input && !endpoint.ring.push(static_cast<const float*>(input),frames)) ++endpoint.overruns;
        } else if(endpoint.master) {
            engine.render_master(static_cast<float*>(output),frames);
        } else {
            // miniaudio's fixed-size adapter normally supplies one block; still handle larger calls.
            auto* destination=static_cast<float*>(output);
            while(frames) {
                unsigned count=std::min(frames,engine.config_.block_frames);
                endpoint.reader.render(destination,count,engine.config_.sample_rate);
                destination+=size_t(count)*endpoint.spec.channels; frames-=count;
            }
        }
        if(engine.observe_outputs_&&!endpoint.input&&output) {float peak=0;for(size_t i=0;i<size_t(delivered)*endpoint.spec.channels;++i) peak=std::max(peak,std::abs(static_cast<float*>(output)[i]));endpoint.output_peak=std::max(endpoint.output_peak.load(),peak);}
    }
    static void notification_callback(const ma_device_notification* notification) {
        auto* endpoint=static_cast<Endpoint*>(notification->pDevice->pUserData);
        if(endpoint->engine->running_ && (notification->type==ma_device_notification_type_stopped ||
           notification->type==ma_device_notification_type_rerouted)) ++endpoint->notifications;
    }
    void open_endpoint(Endpoint& endpoint) {
        ma_device_info* playback=nullptr,*capture=nullptr;
        ma_uint32 playback_count=0,capture_count=0;
        if(ma_context_get_devices(&context_,&playback,&playback_count,&capture,&capture_count)!=MA_SUCCESS)
            throw std::runtime_error("WASAPI enumeration failed");
        auto* list=endpoint.input?capture:playback;
        auto count=endpoint.input?capture_count:playback_count;
        const ma_device_info* chosen=nullptr;
        for(unsigned i=0;i<count;++i)
            if(utf8(list[i].id.wasapi)==endpoint.spec.id) { chosen=&list[i]; break; }
        if(!chosen) { if(native_) {endpoint.name="Unavailable: "+endpoint.spec.id; return;} throw std::runtime_error("Configured clock endpoint is unavailable: "+endpoint.spec.id); }
        ma_device_id id=chosen->id; endpoint.name=chosen->name;
        // Shared WASAPI exposes the Windows mix format, not necessarily all hardware channels.
        ma_device_info detail{};
        if(ma_context_get_device_info(&context_,endpoint.input?ma_device_type_capture:ma_device_type_playback,&id,&detail)!=MA_SUCCESS)
            throw std::runtime_error("Cannot inspect endpoint format");
        if(!config_.exclusive && detail.nativeDataFormatCount &&
           detail.nativeDataFormats[0].channels!=endpoint.spec.channels)
            throw std::runtime_error("Shared endpoint channel count must match its Windows mix format: "+endpoint.name);
        auto options=ma_device_config_init(endpoint.input?ma_device_type_capture:ma_device_type_playback);
        options.sampleRate=config_.sample_rate;
        // A native ASIO master can have a large fixed block. Keep secondary WASAPI
        // callbacks small so they do not consume an entire master block in one burst.
        options.periodSizeInFrames=native_?std::min(config_.block_frames,256u):config_.block_frames;
        options.periods=2;
        options.performanceProfile=ma_performance_profile_low_latency;
        options.wasapi.usage=ma_wasapi_usage_pro_audio;
        options.wasapi.noAutoStreamRouting=MA_TRUE;
        options.dataCallback=data_callback;
        options.notificationCallback=notification_callback;
        options.pUserData=&endpoint;
        if(endpoint.input) {
            options.capture.pDeviceID=&id;
            options.capture.channels=endpoint.spec.channels;
            options.capture.format=ma_format_f32;
            options.capture.shareMode=config_.exclusive?ma_share_mode_exclusive:ma_share_mode_shared;
        } else {
            options.playback.pDeviceID=&id;
            options.playback.channels=endpoint.spec.channels;
            options.playback.format=ma_format_f32;
            options.playback.shareMode=config_.exclusive?ma_share_mode_exclusive:ma_share_mode_shared;
        }
        auto result=ma_device_init(&context_,&options,&endpoint.device);
        if(result!=MA_SUCCESS) throw std::runtime_error("Cannot open endpoint ("+std::to_string(result)+"): "+endpoint.name);
        endpoint.initialized=true;
        auto actual_channels=endpoint.input?endpoint.device.capture.internalChannels:endpoint.device.playback.internalChannels;
        auto actual_rate=endpoint.device.sampleRate;
        if(actual_channels!=endpoint.spec.channels || actual_rate!=config_.sample_rate)
            throw std::runtime_error("Backend changed channels/rate; refusing implicit channel mixing");
    }
    static void native_callback(void* user,const float* input,float* output,unsigned) {
        auto& engine=*static_cast<Engine*>(user);
        if(engine.running_) engine.process_block(input,output);
    }
    void process_block(const float* native_input=nullptr,float* native_output=nullptr) {
        LARGE_INTEGER before{},after{}; QueryPerformanceCounter(&before);
        unsigned frames=config_.block_frames;
        std::fill(input_.begin(),input_.end(),0.f);
        if(native_input)
            for(unsigned f=0;f<frames;++f)
                std::copy_n(native_input+size_t(f)*native_->inputs(),native_->inputs(),input_.data()+size_t(f)*inputs_);
        if(config_.simulation) {
            // Distinct, deterministic input per channel for the ASIO ABI smoke test.
            for(unsigned f=0;f<frames;++f)
                for(unsigned c=0;c<inputs_;++c) input_[size_t(f)*inputs_+c]=float(c+1)*0.01f;
        } else {
            for(auto& endpoint:capture_) {
                if(endpoint->initialized) endpoint->reader.render(endpoint->block.data(),frames,config_.sample_rate); else std::fill(endpoint->block.begin(),endpoint->block.end(),0.f);
                for(unsigned f=0;f<frames;++f)
                    std::copy_n(endpoint->block.data()+size_t(f)*endpoint->spec.channels,endpoint->spec.channels,
                                input_.data()+size_t(f)*inputs_+endpoint->channel_offset);
            }
        }
        std::fill(output_.begin(),output_.end(),0.f);
        process_(process_user_,input_.data(),output_.data(),frames);
        if(native_output)
            for(unsigned f=0;f<frames;++f)
                std::copy_n(output_.data()+size_t(f)*outputs_,native_->outputs(),native_output+size_t(f)*native_->outputs());
        for(auto& endpoint:playback_) {
            for(unsigned f=0;f<frames;++f)
                for(unsigned c=0;c<endpoint->spec.channels;++c) {
                    float sample=output_[size_t(f)*outputs_+endpoint->channel_offset+c];
                    endpoint->block[size_t(f)*endpoint->spec.channels+c]=std::isfinite(sample)?std::clamp(sample,-1.f,1.f):0.f;
                }
            if(!endpoint->master && !config_.simulation && endpoint->initialized && !endpoint->ring.push(endpoint->block.data(),frames)) ++endpoint->overruns;
        }
        ++callbacks_; QueryPerformanceCounter(&after);
        uint64_t ticks=uint64_t(after.QuadPart-before.QuadPart);
        auto maximum=max_callback_ticks_.load(std::memory_order_relaxed);
        if(ticks>maximum) max_callback_ticks_.store(ticks,std::memory_order_relaxed);
        if(ticks*config_.sample_rate>uint64_t(frequency_.QuadPart)*frames) ++deadline_misses_;
    }
    void render_master(float* destination,unsigned frames) {
        auto& endpoint=*playback_[0];
        while(frames) {
            if(master_cursor_==config_.block_frames) { process_block(); master_cursor_=0; }
            unsigned count=std::min(frames,config_.block_frames-master_cursor_);
            std::copy_n(endpoint.block.data()+size_t(master_cursor_)*endpoint.spec.channels,
                        size_t(count)*endpoint.spec.channels,destination);
            master_cursor_+=count; destination+=size_t(count)*endpoint.spec.channels; frames-=count;
        }
    }
public:
    explicit Engine(Config config):config_(std::move(config)) {
        QueryPerformanceFrequency(&frequency_);
        if(!config_.native_asio_clsid.empty()) {
            native_=std::make_unique<NativeAsio>(config_.native_asio_clsid,config_.block_frames,config_.sample_rate);
            inputs_=native_->inputs(); outputs_=native_->outputs();
        }
        for(auto& spec:config_.capture) {
            capture_.push_back(std::make_unique<Endpoint>(this,true,spec,inputs_)); inputs_+=spec.channels;
        }
        for(auto& spec:config_.playback) {
            playback_.push_back(std::make_unique<Endpoint>(this,false,spec,outputs_)); outputs_+=spec.channels;
        }
        if(!native_) playback_[0]->master=true;
        if(inputs_>128 || outputs_>128) throw std::runtime_error("Aggregated channel count exceeds 128 per direction");
        input_.resize(size_t(inputs_)*config_.block_frames);
        output_.resize(size_t(outputs_)*config_.block_frames);
    }
    ~Engine() {
        stop(); native_.reset(); capture_.clear(); playback_.clear();
        if(context_initialized_) ma_context_uninit(&context_);
    }
    void prepare(Process process,void* user) {
        process_=process; process_user_=user;
        if(config_.simulation) return;
        if(native_) native_->prepare(native_callback,this);
        if(capture_.empty() && playback_.empty()) return;
        ma_backend backend=ma_backend_wasapi;
        if(ma_context_init(&backend,1,nullptr,&context_)!=MA_SUCCESS) throw std::runtime_error("WASAPI initialization failed");
        context_initialized_=true;
        for(auto* list:{&capture_,&playback_}) for(auto& endpoint:*list) try {if(endpoint->spec.enabled) open_endpoint(*endpoint);else endpoint->name="Inactive: "+endpoint->spec.id;} catch(const std::exception& e) {if(!native_) throw; if(endpoint->initialized) {ma_device_uninit(&endpoint->device);endpoint->initialized=false;} endpoint->name=std::string("Unavailable: ")+e.what();}
    }
    void start() {
        if(running_) throw std::runtime_error("Already started");
        callbacks_=0; max_callback_ticks_=0; deadline_misses_=0; master_cursor_=config_.block_frames;
        for(auto* list:{&capture_,&playback_})
            for(auto& endpoint:*list) {
                endpoint->ring.reset(); endpoint->reader.reset(); endpoint->overruns=0; endpoint->notifications=0;
                endpoint->output_peak=0;endpoint->frames_delivered=0; endpoint->largest_callback=0;
            }
        running_=true;
        try {
            if(config_.simulation) {
                simulation_thread_=std::thread([this] {
                    auto origin=std::chrono::steady_clock::now(); uint64_t count=0;
                    while(running_.load(std::memory_order_acquire)) {
                        process_block(); ++count;
                        std::this_thread::sleep_until(origin+std::chrono::nanoseconds(count*uint64_t(config_.block_frames)*1000000000ull/config_.sample_rate));
                    }
                });
                return;
            }
            // Start capture and slave outputs before the output clock master.
            for(auto& endpoint:capture_) {
                if(!endpoint->initialized) continue;
                if(ma_device_start(&endpoint->device)!=MA_SUCCESS) throw std::runtime_error("Capture start failed");
                endpoint->started=true;
            }
            for(size_t i=playback_.size();i-->0;) {
                auto& endpoint=*playback_[i];
                if(!endpoint.initialized) continue;
                if(ma_device_start(&endpoint.device)!=MA_SUCCESS) throw std::runtime_error("Playback start failed");
                endpoint.started=true;
            }
            if(native_) native_->start();
        } catch(...) { stop(); throw; }
    }
    void stop() {
        running_=false;
        if(native_) native_->stop();
        if(simulation_thread_.joinable()) simulation_thread_.join();
        for(auto* list:{&playback_,&capture_})
            for(auto& endpoint:*list) if(endpoint->started) {
                ma_device_stop(&endpoint->device); endpoint->started=false;
            }
    }
    void observe_outputs(bool value) {observe_outputs_=value;}
    std::vector<double> clock_queue_levels() const {std::vector<double> result;for(auto* endpoints:{&capture_,&playback_}) for(auto& endpoint:*endpoints) if(endpoint->initialized&&!endpoint->master) result.push_back(endpoint->reader.queue_error.load(std::memory_order_relaxed));return result;}
    std::string output_levels() const {std::ostringstream text;for(auto& endpoint:playback_) if(endpoint->spec.enabled) text<<endpoint->name<<"="<<endpoint->output_peak.load()<<" ("<<endpoint->frames_delivered.load()<<" frames) / ";return text.str();}
    unsigned queue_frames() const { return config_.block_frames*config_.queue_blocks; }
    unsigned inputs() const { return inputs_; }
    unsigned outputs() const { return outputs_; }
    unsigned native_inputs() const { return native_?native_->inputs():0; }
    unsigned native_outputs() const { return native_?native_->outputs():0; }
    const Config& config() const { return config_; }
    bool running() const { return running_; }
    bool recovery_requested() const { if(native_&&native_->events) return true; for(auto* list:{&capture_,&playback_}) for(auto& endpoint:*list) if(endpoint->notifications) return true; return false; }
    unsigned health_flags() const {
        unsigned flags=deadline_misses_?4:0;
        if(native_ && native_->events) flags|=2;
        if(native_ && native_->deadline_misses) flags|=4;
        for(auto* list:{&capture_,&playback_}) for(auto& endpoint:*list) {
            if(endpoint->reader.underruns || endpoint->overruns) flags|=1;
            if(endpoint->notifications || (!config_.simulation&&endpoint->spec.enabled&&!endpoint->initialized)) flags|=2;
        }
        return flags;
    }
    unsigned copy_stopped_output(float* samples,unsigned capacity) const {
        if(running_ || !samples || capacity<output_.size()) return 0;
        std::copy(output_.begin(),output_.end(),samples); return unsigned(output_.size());
    }
    // Conservative software/backend budget, not a measured analog round-trip latency.
    unsigned latency_budget(bool input) const {
        unsigned maximum=native_?native_->latency(input):0;
        const auto& endpoints=input?capture_:playback_;
        for(auto& endpoint:endpoints) {
            unsigned period=endpoint->initialized?(input?endpoint->device.capture.internalPeriodSizeInFrames:endpoint->device.playback.internalPeriodSizeInFrames):config_.block_frames;
            unsigned periods=endpoint->initialized?(input?endpoint->device.capture.internalPeriods:endpoint->device.playback.internalPeriods):2;
            unsigned queue=(!input && endpoint->master)?0:queue_frames()+SincKernel::taps;
            maximum=std::max(maximum,period*periods+queue+config_.block_frames);
        }
        return maximum;
    }
    std::string diagnostics() const {
        std::ostringstream text;
        text<<"{\"health_flags\":"<<health_flags()<<",\"native_asio\":"<<(native_?"true":"false")<<",\"callbacks\":"<<callbacks_.load()<<",\"max_process_us\":"
            <<max_callback_ticks_.load()*1000000./frequency_.QuadPart
            <<",\"deadline_misses\":"<<deadline_misses_.load()
            <<",\"native_callback_max_us\":"<<(native_?native_->maximum_us():0.)
            <<",\"native_deadline_misses\":"<<(native_?native_->deadline_misses.load():0)
            <<",\"native_input_latency_frames\":"<<(native_?native_->latency(true):0)
            <<",\"native_output_latency_frames\":"<<(native_?native_->latency(false):0)<<",\"endpoints\":[";
        bool first=true;
        for(auto* list:{&capture_,&playback_}) for(auto& endpoint:*list) {
            if(!first) text<<','; first=false;
            text<<"{\"direction\":\""<<(endpoint->input?"capture":"playback")<<"\",\"offset\":"<<endpoint->channel_offset
                <<",\"channels\":"<<endpoint->spec.channels<<",\"master\":"<<(endpoint->master?"true":"false")
                <<",\"queue_frames\":"<<endpoint->ring.available()<<",\"correction_ppm\":"<<endpoint->reader.correction_ppm.load()
                <<",\"underruns\":"<<endpoint->reader.underruns.load()<<",\"overruns\":"<<endpoint->overruns.load()
                <<",\"device_notifications\":"<<endpoint->notifications.load()
                <<",\"frames_delivered\":"<<endpoint->frames_delivered.load()<<",\"largest_callback\":"<<endpoint->largest_callback.load()
                <<",\"backend_rate\":"<<(endpoint->initialized?(endpoint->input?endpoint->device.capture.internalSampleRate:endpoint->device.playback.internalSampleRate):0)
                <<",\"backend_period_frames\":"<<(endpoint->initialized?(endpoint->input?endpoint->device.capture.internalPeriodSizeInFrames:endpoint->device.playback.internalPeriodSizeInFrames):0)
                <<",\"backend_periods\":"<<(endpoint->initialized?(endpoint->input?endpoint->device.capture.internalPeriods:endpoint->device.playback.internalPeriods):0)<<'}';
        }
        text<<"]}"; return text.str();
    }
};
}
