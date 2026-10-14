#include "engine.h"
#include "latency_measurement.h"
#include "calibration_session.h"
#include "port_inventory.h"
#include <iostream>
#include <iomanip>
struct Capture {
    unsigned inputs,outputs,input,output,emitted,position=0;
    std::vector<float> reference,captured;
    static void process(void* user,const float* input,float* output,unsigned frames) {
        auto& s=*static_cast<Capture*>(user);
        std::fill_n(output,size_t(frames)*s.outputs,0.f);
        for(unsigned f=0;f<frames;++f) {
            unsigned at=s.position++;
            if(at<s.captured.size()) s.captured[at]=input[size_t(f)*s.inputs+s.input];
            if(at>=s.emitted&&at-s.emitted<s.reference.size()) output[size_t(f)*s.outputs+s.output]=s.reference[at-s.emitted];
        }
    }
};
int main(int argc,char** argv) {
    try {
        if(argc>=5&&std::string(argv[1])=="--session") {
            if(argc>8) throw std::runtime_error("--session config.ini router.txt result.json [dBFS] [warmup-seconds] [--apply]");
            if(argc==8&&std::string(argv[7])!="--apply") throw std::runtime_error("Unknown session option");
            HANDLE single=CreateMutexW(nullptr,FALSE,L"Local\\AudioAggregateRoutingWindow");
            if(!single) throw std::runtime_error("Cannot reserve audio session");
            if(GetLastError()==ERROR_ALREADY_EXISTS) {CloseHandle(single);throw std::runtime_error("GUIまたは別の校正を完全終了してから実行してください");}
            struct CloseSession {HANDLE handle;~CloseSession(){CloseHandle(handle);}} close_session{single};
            auto config=aggregate::load_config(argv[2]);
            if(config.simulation) throw std::runtime_error("Session calibration requires real devices");
            std::ifstream routes(argv[3]);if(!routes) throw std::runtime_error("Cannot read routing settings");
            std::vector<std::string> lines,keys;std::string line,mic;
            while(std::getline(routes,line)) {lines.push_back(line);std::istringstream fields(line);std::string kind,key;fields>>kind;
                if(kind=="calibration_mic") fields>>std::quoted(mic);
                if(kind=="calibration_output") {fields>>std::quoted(key);keys.push_back(key);}
                if(kind=="port") {std::string direction,id;unsigned channels=0;if(!(fields>>direction>>std::quoted(id)>>channels)||id.empty()||channels<1||channels>64||(direction!="input"&&direction!="output")) throw std::runtime_error("Invalid saved port");auto& specs=direction=="input"?config.capture:config.playback;
                    if(std::none_of(specs.begin(),specs.end(),[&](auto& spec){return spec.id==id;})) specs.push_back({id,channels});}
            }
            routes.close();
            aggregate::include_windows_ports(config,aggregate::windows_ports());
            unsigned native_in=0,native_out=0;
            if(!config.native_asio_clsid.empty()) {aggregate::NativeAsio native(config.native_asio_clsid,config.block_frames,config.sample_rate);native_in=native.inputs();native_out=native.outputs();}
            unsigned input=~0u,offset=native_in;std::vector<unsigned> selected;
            for(auto& spec:config.capture) {spec.enabled=false;for(unsigned c=0;c<spec.channels;++c) if(mic=="wasapi:"+spec.id+":"+std::to_string(c)) {input=offset+c;spec.enabled=true;}offset+=spec.channels;}
            for(unsigned c=0;c<native_in;++c) if(mic=="asio:"+config.native_asio_clsid+":in:"+std::to_string(c)) input=c;
            for(auto& key:keys) {unsigned found=~0u;offset=native_out;
                for(unsigned c=0;c<native_out;++c) if(key=="asio:"+config.native_asio_clsid+":out:"+std::to_string(c)) found=c;
                for(auto& spec:config.playback) {for(unsigned c=0;c<spec.channels;++c) if(key=="wasapi:"+spec.id+":"+std::to_string(c)) found=offset+c;offset+=spec.channels;}
                if(found==~0u||std::find(selected.begin(),selected.end(),found)!=selected.end()) throw std::runtime_error("Unavailable/duplicate output: "+key);
                selected.push_back(found);
            }
            if(input==~0u||selected.size()<2) throw std::runtime_error("Saved microphone and at least two outputs required");
            offset=native_out;for(auto& spec:config.playback) {spec.enabled=false;for(auto c:selected) if(c>=offset&&c<offset+spec.channels) spec.enabled=true;offset+=spec.channels;}
            double level=argc>=6?std::stod(argv[5]):-24;unsigned warmup=argc>=7?std::stoul(argv[6]):60;
            if(!std::isfinite(level)||level< -60||level> -6||warmup<2||warmup>120) throw std::runtime_error("dBFS must be -60..-6, warmup 2..120 seconds");
            config.output_delay_ms.clear();config.output_delay_frames.clear();
            HANDLE cancel=CreateEventW(nullptr,TRUE,FALSE,nullptr);
            aggregate::CalibrationResult result;
            try {result=aggregate::calibrate_outputs(config,input,selected,cancel,[](auto& text){std::cout<<text<<std::endl;},float(std::pow(10.,level/20.)/.015),warmup);}catch(...) {CloseHandle(cancel);throw;}CloseHandle(cancel);
            std::ofstream output(argv[4]);output<<"{\"sample_rate\":"<<config.sample_rate<<",\"outputs\":[";
            for(unsigned i=0;i<keys.size();++i) {if(i) output<<',';output<<"{\"key\":"<<std::quoted(keys[i])<<",\"measured_ms\":"<<result.measured[i]*1000./config.sample_rate<<",\"correction_ms\":"<<result.corrections[i]*1000./config.sample_rate<<",\"verified_ms\":"<<result.residual[i]*1000./config.sample_rate<<'}';}output<<"]}\n";output.close();if(!output) throw std::runtime_error("Cannot write result");
            if(argc==8&&std::string(argv[7])=="--apply") {
                auto candidate=std::filesystem::path(argv[3]);candidate+=".candidate";std::ofstream saved(candidate);
                for(auto& previous:lines) {std::istringstream fields(previous);std::string kind,key;fields>>kind;if(kind=="delay") fields>>std::quoted(key);if(kind=="delay"&&std::find(keys.begin(),keys.end(),key)!=keys.end()) continue;saved<<previous<<'\n';}
                for(unsigned i=0;i<keys.size();++i) saved<<"delay "<<std::quoted(keys[i])<<' '<<result.corrections[i]*1000./config.sample_rate<<'\n';saved.close();if(!saved||!MoveFileExW(candidate.c_str(),std::filesystem::path(argv[3]).c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot apply validated corrections");
            }
            std::cout<<"SUCCESS: verified output difference <= 0.1 ms\n";return 0;
        }
        if(argc==2&&std::string(argv[1])=="--self-test") {
            auto reference=aggregate::calibration_signal();std::vector<float> captured(32000);
            for(unsigned i=0;i<reference.size();++i) captured[1000+731+i]=-reference[i]*.5f;
            auto result=aggregate::measure_delay(reference,captured,1000,2000);
            if(result.frames!=731||result.correlation<.99) throw std::runtime_error("Delay measurement regression");
            bool rejected=false;try {aggregate::measure_delay(reference,std::vector<float>(32000),1000,2000);}catch(...) {rejected=true;}
            if(!rejected) throw std::runtime_error("Silence must not produce a delay");
            std::cout<<"PASS: delay measurement, inverted return, silence rejection\n";return 0;
        }
        if(argc!=6||std::string(argv[1])!="--play-signal") throw std::runtime_error("Usage: aggregate_calibrate --play-signal config.ini input-index output-index result.json (zero-based; emits -36 dBFS, 500 ms sweep)");
        auto config=aggregate::load_config(argv[2]);if(config.simulation) throw std::runtime_error("Calibration needs real input/output");
        aggregate::Engine engine(config);unsigned input=std::stoul(argv[3]),output=std::stoul(argv[4]);
        if(input>=engine.inputs()||output>=engine.outputs()) throw std::runtime_error("Calibration channel index out of range");
        Capture capture{engine.inputs(),engine.outputs(),input,output,config.sample_rate/2,0,aggregate::calibration_signal(config.sample_rate),std::vector<float>(config.sample_rate*4)};
        engine.prepare(Capture::process,&capture);engine.start();std::this_thread::sleep_for(std::chrono::seconds(4));engine.stop();
        if(engine.health_flags()) throw std::runtime_error("Device/buffer errors during measurement; result rejected");
        auto result=aggregate::measure_delay(capture.reference,capture.captured,capture.emitted,config.sample_rate*2);
        std::ofstream out(argv[5]);out<<"{\"roundtrip_frames\":"<<result.frames<<",\"roundtrip_ms\":"<<result.frames*1000./config.sample_rate<<",\"correlation\":"<<result.correlation<<",\"input\":"<<input<<",\"output\":"<<output<<"}\n";
        if(!out) throw std::runtime_error("Cannot save calibration result");
        std::cout<<"Roundtrip: "<<result.frames*1000./config.sample_rate<<" ms; compare outputs using the same input path\n";return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<"\n";return 1;}
}
