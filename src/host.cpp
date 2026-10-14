#include "asio_api.h"
#include "engine.h"
#include "endpoint_status.h"
#include <iostream>
#include <iomanip>
#include <charconv>

namespace {
struct Inventory {
    ma_context context{};
    std::vector<ma_device_info> capture,playback;
    Inventory() {
        ma_backend backend=ma_backend_wasapi;
        if(ma_context_init(&backend,1,nullptr,&context)!=MA_SUCCESS) throw std::runtime_error("Cannot initialize WASAPI");
        ma_device_info* p=nullptr,*c=nullptr; ma_uint32 np=0,nc=0;
        if(ma_context_get_devices(&context,&p,&np,&c,&nc)!=MA_SUCCESS) { ma_context_uninit(&context); throw std::runtime_error("Cannot enumerate devices"); }
        capture.assign(c,c+nc); playback.assign(p,p+np);
        for(auto* list:{&capture,&playback}) for(auto& item:*list) {
            ma_device_info detail{};
            if(ma_context_get_device_info(&context,list==&capture?ma_device_type_capture:ma_device_type_playback,&item.id,&detail)==MA_SUCCESS) item=detail;
        }
    }
    ~Inventory() { ma_context_uninit(&context); }
};
unsigned number(const std::string& value) {
    unsigned n=0; auto result=std::from_chars(value.data(),value.data()+value.size(),n);
    if(result.ec!=std::errc{} || result.ptr!=value.data()+value.size()) throw std::runtime_error("Invalid integer: "+value);
    return n;
}
std::vector<unsigned> indexes(const std::string& value) {
    std::vector<unsigned> result;
    if(value=="none") return result;
    std::istringstream stream(value); std::string item;
    while(std::getline(stream,item,',')) result.push_back(number(item));
    if(result.empty()) throw std::runtime_error("Empty endpoint selection");
    return result;
}
void list_devices() {
    Inventory inventory;
    for(auto* list:{&inventory.capture,&inventory.playback}) {
        std::cout<<(list==&inventory.capture?"CAPTURE":"PLAYBACK")<<"\n";
        for(unsigned i=0;i<list->size();++i) {
            auto& item=(*list)[i];
            std::cout<<i<<" | "<<item.name;
            if(item.nativeDataFormatCount) std::cout<<" | "<<item.nativeDataFormats[0].channels<<" ch | "<<item.nativeDataFormats[0].sampleRate<<" Hz";
            else std::cout<<" | format unknown";
            auto status=aggregate::endpoint_status(aggregate::utf8(item.id.wasapi));if(status.available) std::cout<<" | volume="<<status.volume<<" | muted="<<status.muted;
            std::cout<<"\n  id="<<aggregate::utf8(item.id.wasapi)<<"\n";
        }
    }
}
struct Host {
    IASIO* driver=nullptr;
    std::vector<ASIOBufferInfo> buffers;
    long frames=0;
    unsigned rate=48000;
    bool simulation=false,tone=false;
    std::atomic<uint64_t> calls{0},errors{0};
    static Host* current;
    static void callback(long half,ASIOBool) {
        auto& self=*current;
        for(auto& channel:self.buffers) {
            auto* samples=static_cast<float*>(channel.buffers[half]);
            if(channel.isInput && self.simulation)
                for(long f=0;f<self.frames;++f)
                    if(std::abs(samples[f]-float(channel.channelNum+1)*.01f)>1e-6f) { ++self.errors; break; }
            if(!channel.isInput) {
                // Unique output channel/frame tags in simulation; hardware probes stay silent.
                for(long f=0;f<self.frames;++f)
                    samples[f]=self.simulation?float(channel.channelNum+1)*.01f+float(f)*.000001f:0.f;
            }
        }
        ++self.calls;
    }
    static long message(long selector,long,void*,double*) {
        if(selector==kAsioSupportsTimeInfo) return 1;
        if(selector==kAsioEngineVersion) return 2;
        return 0;
    }
    static ASIOTime* time_callback(ASIOTime* time,long half,ASIOBool direct) {
        if(!(time->timeInfo.flags&kSamplePositionValid) || time->timeInfo.sampleRate!=current->rate) ++current->errors;
        callback(half,direct); return time;
    }
};
Host* Host::current=nullptr;
int probe(const std::filesystem::path& config_path,unsigned seconds,unsigned cycles) {
    auto config=aggregate::load_config(config_path);
    if(seconds<1 || seconds>600 || cycles<1 || cycles>10) throw std::runtime_error("seconds=1..600, cycles=1..10");
    SetEnvironmentVariableW(L"AUDIO_AGGREGATE_CONFIG",std::filesystem::absolute(config_path).c_str());
    wchar_t executable[32768]{}; GetModuleFileNameW(nullptr,executable,32768);
    auto dll_path=std::filesystem::path(executable).parent_path()/"audio_aggregate.dll";
    HMODULE dll=LoadLibraryW(dll_path.c_str());
    if(!dll) throw std::runtime_error("Cannot load audio_aggregate.dll");
    using GetClass=HRESULT(STDMETHODCALLTYPE*)(REFCLSID,REFIID,void**);
    using CanUnload=HRESULT(STDMETHODCALLTYPE*)();
    using Diagnostics=unsigned(*)(IASIO*,char*,unsigned);
    using Health=unsigned(*)(IASIO*);
    using CopyOutput=unsigned(*)(IASIO*,float*,unsigned);
    auto get_class=reinterpret_cast<GetClass>(GetProcAddress(dll,"DllGetClassObject"));
    auto can_unload=reinterpret_cast<CanUnload>(GetProcAddress(dll,"DllCanUnloadNow"));
    auto diagnostics=reinterpret_cast<Diagnostics>(GetProcAddress(dll,"AggregateDiagnostics"));
    auto health=reinterpret_cast<Health>(GetProcAddress(dll,"AggregateHealthFlags"));
    auto copy_output=reinterpret_cast<CopyOutput>(GetProcAddress(dll,"AggregateCopyStoppedOutput"));
    if(!get_class || !can_unload || !diagnostics || !health || !copy_output) throw std::runtime_error("Missing DLL exports");
    IClassFactory* factory=nullptr; Host host; Host::current=&host; host.simulation=config.simulation;host.rate=config.sample_rate;
    if(FAILED(get_class(aggregate_clsid,IID_IClassFactory,reinterpret_cast<void**>(&factory)))) throw std::runtime_error("Cannot get ASIO class factory");
    auto hr=factory->CreateInstance(nullptr,aggregate_clsid,reinterpret_cast<void**>(&host.driver)); factory->Release();
    if(FAILED(hr) || !host.driver) throw std::runtime_error("Cannot create ASIO driver");
    auto check=[&](ASIOError result,const char* operation) {
        if(result!=ASE_OK) { char error[124]{}; host.driver->getErrorMessage(error); throw std::runtime_error(std::string(operation)+": "+error+" ("+std::to_string(result)+")"); }
    };
    if(!host.driver->init(nullptr)) { char error[124]{}; host.driver->getErrorMessage(error); host.driver->Release(); throw std::runtime_error(error); }
    try {
        long inputs=0,outputs=0,minimum=0,maximum=0,granularity=0;
        check(host.driver->getChannels(&inputs,&outputs),"getChannels");
        check(host.driver->getBufferSize(&minimum,&maximum,&host.frames,&granularity),"getBufferSize");
        std::cout<<"ASIO: "<<inputs<<" inputs + "<<outputs<<" outputs, "<<host.frames<<" frames @ "<<config.sample_rate<<" Hz\n";
        for(bool input:{true,false}) for(long c=0;c<(input?inputs:outputs);++c) {
            ASIOBufferInfo buffer{}; buffer.isInput=input?ASIOTrue:ASIOFalse; buffer.channelNum=c; host.buffers.push_back(buffer);
            ASIOChannelInfo info{}; info.isInput=buffer.isInput; info.channel=c; check(host.driver->getChannelInfo(&info),"getChannelInfo");
            if(info.type!=ASIOSTFloat32LSB) throw std::runtime_error("Unexpected channel sample type");
            std::cout<<"  "<<info.name<<"\n";
        }
        ASIOCallbacks callbacks{Host::callback,nullptr,Host::message,Host::time_callback};
        if(config.simulation) {
            if(host.driver->canSampleRate(44100)!=ASE_NoClock) throw std::runtime_error("Unsupported sample rate accepted");
            if(host.driver->start()!=ASE_InvalidMode) throw std::runtime_error("Start without buffers accepted");
            ASIOBufferInfo duplicates[2]{};
            duplicates[0].channelNum=duplicates[1].channelNum=0;
            if(host.driver->createBuffers(duplicates,2,host.frames,&callbacks)!=ASE_InvalidParameter)
                throw std::runtime_error("Duplicate output channels accepted");
            if(host.driver->createBuffers(host.buffers.data(),long(host.buffers.size()),host.frames+1,&callbacks)!=ASE_InvalidParameter)
                throw std::runtime_error("Invalid block size accepted");
        }
        unsigned total_health=0;
        for(unsigned cycle=0;cycle<cycles;++cycle) {
            check(host.driver->createBuffers(host.buffers.data(),long(host.buffers.size()),host.frames,&callbacks),"createBuffers");
            long in_latency=0,out_latency=0; check(host.driver->getLatencies(&in_latency,&out_latency),"getLatencies");
            std::cout<<"Estimated software/backend budget (NOT measured analog latency): input="<<in_latency<<", output="<<out_latency<<" frames\n";
            host.calls=0; host.errors=0;
            check(host.driver->start(),"start");
            if(config.simulation && host.driver->start()!=ASE_InvalidMode) throw std::runtime_error("Double start accepted");
            wchar_t stop_name[256]{};
            GetEnvironmentVariableW(L"AUDIO_AGGREGATE_STOP_EVENT",stop_name,256);
            HANDLE stop_event=stop_name[0]?OpenEventW(SYNCHRONIZE,FALSE,stop_name):nullptr;
            auto began=std::chrono::steady_clock::now();
            if(stop_event) { WaitForSingleObject(stop_event,seconds*1000); CloseHandle(stop_event); }
            else std::this_thread::sleep_for(std::chrono::seconds(seconds));
            auto elapsed=std::chrono::duration<double>(std::chrono::steady_clock::now()-began).count();
            check(host.driver->stop(),"stop");
            ASIOSamples final_position{}; ASIOTimeStamp final_time{};
            check(host.driver->getSamplePosition(&final_position,&final_time),"getSamplePosition");
            uint64_t sample_position=(uint64_t(final_position.hi)<<32)|final_position.lo;
            if(!host.calls || sample_position!=(host.calls.load()-1)*uint64_t(host.frames) || (!final_time.hi && !final_time.lo))
                throw std::runtime_error("ASIO sample position/timestamp inconsistent");
            if(config.simulation) {
                std::vector<float> final_output(size_t(outputs)*host.frames);
                if(copy_output(host.driver,final_output.data(),unsigned(final_output.size()))!=final_output.size()) throw std::runtime_error("Cannot inspect stopped output");
                for(long f=0;f<host.frames;++f) for(long c=0;c<outputs;++c)
                    if(std::abs(final_output[size_t(f)*outputs+c]-(float(c+1)*.01f+float(f)*.000001f))>1e-6f)
                        throw std::runtime_error("ASIO output channel/frame mapping mismatch");
            }
            char report[16384]{}; if(!diagnostics(host.driver,report,sizeof(report))) throw std::runtime_error("Cannot read diagnostics");
            std::cout<<"Cycle "<<cycle+1<<": calls="<<host.calls<<", channel_errors="<<host.errors<<"\n"<<report<<"\n";
            bool failed=host.calls<elapsed*config.sample_rate/unsigned(host.frames)/2 || host.errors!=0;
            total_health|=health(host.driver);
            check(host.driver->disposeBuffers(),"disposeBuffers");
            if(failed) throw std::runtime_error("ASIO callbacks/channel verification failed");
        }
        host.driver->Release(); host.driver=nullptr;
        if(can_unload()!=S_OK) throw std::runtime_error("DLL still has live COM objects");
        FreeLibrary(dll); Host::current=nullptr;
        if(total_health) { std::cerr<<"UNSTABLE: health_flags="<<total_health<<" (1=xrun, 2=device event, 4=processing deadline)\n"; return 3; }
        return 0;
    } catch(...) { if(host.driver) { host.driver->stop(); host.driver->Release(); } FreeLibrary(dll); Host::current=nullptr; throw; }
}
}
int main(int argc,char** argv) {
    SetConsoleOutputCP(CP_UTF8);
    std::cout<<std::unitbuf;
    try {
        if(argc==2 && std::string(argv[1])=="list") { list_devices(); return 0; }
        if(argc>=5 && std::string(argv[1])=="make-config") {
            Inventory inventory;
            std::ofstream file(std::filesystem::path(aggregate::wide(argv[2])),std::ios::out|std::ios::trunc);
            if(!file) throw std::runtime_error("Cannot write configuration");
            file<<"[engine]\nsample_rate=48000\nblock_frames=256\nqueue_blocks=2\nexclusive=false\nsimulation=false\n";
            for(bool input:{true,false}) {
                auto selections=indexes(argv[input?3:4]); auto& list=input?inventory.capture:inventory.playback;
                if(!input && selections.empty()) throw std::runtime_error("At least one playback endpoint is required");
                for(unsigned i=0;i<selections.size();++i) {
                    auto index=selections[i];
                    if(index>=list.size() || !list[index].nativeDataFormatCount) throw std::runtime_error("Endpoint index/format unavailable");
                    auto& endpoint=list[index];
                    file<<"\n# "<<endpoint.name<<"\n["<<(input?"capture":"playback")<<'.'<<i<<"]\nid="<<aggregate::utf8(endpoint.id.wasapi)
                        <<"\nchannels="<<endpoint.nativeDataFormats[0].channels<<"\n";
                }
            }
            std::cout<<"Configuration written: "<<argv[2]<<"\n"; return 0;
        }
        if(argc>=3 && std::string(argv[1])=="probe")
            return probe(std::filesystem::path(aggregate::wide(argv[2])),argc>3?number(argv[3]):5,argc>4?number(argv[4]):1);
        std::cout<<"aggregate_host list\naggregate_host make-config <path> <capture-index,...|none> <playback-index,...>\naggregate_host probe <config-path> [seconds=5] [cycles=1]\nProbe always writes silence; no input recordings are saved.\n";
        return argc==1?0:2;
    } catch(const std::exception& error) { std::cerr<<"ERROR: "<<error.what()<<"\n"; return 1; }
}
