#pragma once
#include "engine.h"
#include "routing.h"
#include "output_delay.h"
#include "port_inventory.h"

namespace aggregate {
class LoopbackInput {
    ma_context context_{};
    ma_device device_{};
    bool context_ready_=false,device_ready_=false;
    FrameRing ring_;
    unsigned rate_;
    SincKernel kernel_;
    AdaptiveReader reader_;
    static void callback(ma_device* device,void*,const void* input,ma_uint32 frames) {
        auto& self=*static_cast<LoopbackInput*>(device->pUserData);
        self.frames.fetch_add(frames);
        if(input && !self.ring_.push(static_cast<const float*>(input),frames)) ++self.overruns;
    }
public:
    std::atomic<uint64_t> frames{0},overruns{0};
    explicit LoopbackInput(const std::string& endpoint,unsigned target,unsigned channels=2,unsigned rate=48000):ring_(channels,32768),rate_(rate),reader_(ring_,kernel_,target) {
        if(endpoint.empty()) return;
        try {
            ma_backend backend=ma_backend_wasapi;
            if(ma_context_init(&backend,1,nullptr,&context_)!=MA_SUCCESS) throw std::runtime_error("PC loopback context failed");
            context_ready_=true;
            ma_device_id id{}; auto text=wide(endpoint); wcsncpy_s(id.wasapi,text.c_str(),_TRUNCATE);
            auto options=ma_device_config_init(ma_device_type_loopback);
            options.capture.pDeviceID=&id; options.capture.format=ma_format_f32; options.capture.channels=channels;
            options.sampleRate=rate; options.periodSizeInFrames=256;
            options.wasapi.noAutoStreamRouting=MA_TRUE; options.pUserData=this; options.dataCallback=callback;
            ma_device_info *p=nullptr,*c=nullptr;ma_uint32 np=0,nc=0;
            if(ma_context_get_devices(&context_,&p,&np,&c,&nc)!=MA_SUCCESS) throw std::runtime_error("PC loopback enumeration failed");
            bool present=false;for(unsigned i=0;i<np;++i) if(utf8(p[i].id.wasapi)==endpoint) present=true;
            if(!present) throw std::runtime_error("Loopback endpoint unavailable: "+endpoint);
            if(ma_device_init(&context_,&options,&device_)!=MA_SUCCESS) throw std::runtime_error("PC loopback initialization failed");
            device_ready_=true;
        } catch(...) { close(); throw; }
    }
    ~LoopbackInput() { close(); }
    void close() { if(device_ready_) { ma_device_uninit(&device_); device_ready_=false; } if(context_ready_) { ma_context_uninit(&context_); context_ready_=false; } }
    void start() { if(device_ready_ && ma_device_start(&device_)!=MA_SUCCESS) throw std::runtime_error("PC loopback start failed"); }
    void render(float* output,unsigned count) { reader_.render(output,count,rate_); }
    uint64_t underruns() const { return reader_.underruns.load(); }
};
class RouterSession {
    RoutingMatrix& matrix_;
    Engine engine_;
    struct LoopbackPort { EndpointSpec spec; unsigned offset; std::unique_ptr<LoopbackInput> input; std::vector<float> block; std::string error; };
    std::vector<LoopbackPort> loopbacks_;
    std::vector<float> pc_;
    double phase_=0;
    unsigned rate_;
    OutputDelay delay_;
    static void process(void* user,const float* input,float* output,unsigned frames) {
        auto& self=*static_cast<RouterSession*>(user);
        std::fill(self.pc_.begin(),self.pc_.end(),0.f);
        for(auto& port:self.loopbacks_) if(port.input) {port.input->render(port.block.data(),frames);copy_port_channels(port.block.data(),port.spec.channels,self.pc_.data(),self.matrix_.pc_channels(),port.offset,frames);}
        if(self.matrix_.test_tone.load()) for(unsigned f=0;f<frames;++f) {
            float x=float(std::sin(self.phase_)*.0316227766); // -30 dBFS confirmation tone.
            self.phase_+=2*3.14159265358979323846*440/self.rate_;
            if(self.phase_>2*3.14159265358979323846) self.phase_-=2*3.14159265358979323846;
            for(unsigned c=0;c<self.matrix_.pc_channels();++c) self.pc_[size_t(f)*self.matrix_.pc_channels()+c]=x;
        }
        self.matrix_.render(input,self.pc_.data(),output,frames);
        self.delay_.process(output,frames,self.matrix_.muted.load());
    }
public:
    RouterSession(const Config& config,const std::string& pc_endpoint,RoutingMatrix& matrix,const std::vector<RouteChannel>& outputs={},const std::string& pc_family={})
        :RouterSession(config,std::vector<EndpointSpec>{{pc_endpoint,config.pc_channels}},matrix) {}
    RouterSession(const Config& config,const std::vector<EndpointSpec>& sources,RoutingMatrix& matrix):matrix_(matrix),engine_(config),
        pc_(size_t(config.block_frames)*matrix.pc_channels(),0),rate_(config.sample_rate),delay_(matrix.outputs(),config.sample_rate,config.output_delay_ms,config.output_delay_frames) {
        if(matrix.inputs()!=engine_.inputs()+matrix.pc_channels() || matrix.outputs()!=engine_.outputs()) throw std::runtime_error("Device channel count changed; reopen routing window");
        unsigned offset=0;
        for(const auto& spec:sources) {
            LoopbackPort port{spec,offset,nullptr,std::vector<float>(size_t(config.block_frames)*spec.channels),{}};
            try {port.input=std::make_unique<LoopbackInput>(spec.id,config.block_frames*config.queue_blocks,spec.channels,config.sample_rate);} catch(const std::exception& e) {port.error=e.what();}
            offset+=spec.channels;loopbacks_.push_back(std::move(port));
        }
        if(offset!=matrix.pc_channels()) throw std::runtime_error("Loopback channel layout mismatch");
        matrix_.reset_audio_state(); engine_.prepare(process,this);
    }
    ~RouterSession() { engine_.stop(); }
    void start() { for(auto& port:loopbacks_) if(port.input) try {port.input->start();} catch(const std::exception& e) {port.error=e.what();port.input.reset();} engine_.start(); }
    void stop() { engine_.stop(); }
    bool recovery_requested() const {return engine_.recovery_requested();}
    unsigned health_flags() const {unsigned flags=engine_.health_flags();for(auto& port:loopbacks_) {if(!port.error.empty()) flags|=2;if(port.input&&(port.input->underruns()||port.input->overruns.load())) flags|=1;}return flags;}
    unsigned copy_stopped_output(float* output,unsigned capacity) const {return engine_.copy_stopped_output(output,capacity);}
    std::string diagnostics() const {
        std::string text=engine_.diagnostics();
        for(auto& port:loopbacks_) text+="\nloopback="+port.spec.id+", channels="+std::to_string(port.spec.channels)+", frames="+std::to_string(port.input?port.input->frames.load():0)+", error="+port.error;
        return text+", clipped_samples="+std::to_string(matrix_.clipped_samples.load());
    }
};
}
