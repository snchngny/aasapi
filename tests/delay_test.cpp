#include "calibration_session.h"
#include <iostream>
#include <random>
void require(bool value,const char* reason) {if(!value) throw std::runtime_error(reason);}
int main() {
    try {
        std::deque<std::vector<double>> history;
        for(unsigned i=0;i<20;++i) history.push_back({100.+(i%2?20:-20),-50.});
        require(aggregate::calibration_queues_stable(history,48000),"Stable clock queues with packet jitter rejected");
        for(unsigned i=0;i<20;++i) history[i][0]+=3*i;
        require(!aggregate::calibration_queues_stable(history,48000),"Startup queue drift accepted");
        aggregate::PortDelays saved{{"asio:device:out:0",.125},{"wasapi:tv:0",12.5},{"offline:0",30}};
        std::vector<aggregate::RouteChannel> ports{{"wasapi:tv:0"},{"asio:device:out:0"}};
        auto frames=aggregate::delay_frames(saved,ports,48000);require(frames[0]==600&&frames[1]==6,"Stable IDs after reorder, fractional milliseconds");
        aggregate::OutputDelay delay(2,48000,{},frames);std::vector<float> signal(1400);signal[0]=signal[1]=1;delay.process(signal.data(),700,false);
        require(signal[1200]==1&&signal[13]==1&&signal[0]==0,"Independent sample-accurate delays");
        auto aligned=aggregate::align_delays({384,3360},48000);require(aligned==std::vector<unsigned>({2976,0}),"Fast output waits for slow output");
        unsigned confirmations=0;
        auto refined=aggregate::refine_calibration({100,140},48000,[&](const std::vector<unsigned>& values,unsigned attempt) {
            ++confirmations;
            // Synthesize and analyze the acoustic return after a 24-sample
            // path change; verify that corrections are accumulated and rebased.
            auto reference=aggregate::calibration_signal(48000);std::vector<unsigned> measured;
            for(unsigned c=0;c<2;++c) {
                unsigned arrival=(c?140:124)+values[c];std::vector<float> recorded(reference.size()+1200);
                std::copy(reference.begin(),reference.end(),recorded.begin()+200+arrival);
                measured.push_back(aggregate::measure_delay(reference,recorded,200,800).frames);
            }
            return measured;
        },[](auto&){});
        require(confirmations==2&&refined.corrections==std::vector<unsigned>({16,0})&&refined.residual==std::vector<unsigned>({140,140}),"Acoustic residual refinement failed");
        unsigned attempts=0;bool limited=false;
        try {aggregate::refine_calibration({100,100},48000,[&](auto&,unsigned) {++attempts;return std::vector<unsigned>{100,105};},[](auto&){});}
        catch(const std::exception& e) {limited=std::string(e.what()).find("0.1 ms")!=std::string::npos;}
        require(limited&&attempts==6,"Non-converging refinement must stop without success");
        auto boundary=aggregate::refine_calibration({100,100},48000,[](auto&,unsigned) {return std::vector<unsigned>{100,104};},[](auto&){});
        require(boundary.residual[1]-boundary.residual[0]==4,"Sample-rounded 0.1 ms threshold incorrect");
        bool interrupted=false;
        try {aggregate::refine_calibration({100,100},48000,[](auto&,unsigned)->std::vector<unsigned> {throw std::runtime_error("cancel-test");},[](auto&){});}
        catch(const std::exception& e) {interrupted=std::string(e.what())=="cancel-test";}
        require(interrupted,"Refinement must propagate cancellation or invalid measurement");
        for(auto invalid:{"nan","-1","2000.1","1junk"}) {bool rejected=false;try {aggregate::valid_delay(invalid);}catch(...) {rejected=true;}require(rejected,"Invalid delay accepted");}
        std::ostringstream out;aggregate::write_delays(out,saved);require(out.str().find("offline:0")!=std::string::npos,"Offline ID lost");
        aggregate::CalibrationCapture capture(1,2,0,8000,{0,1},{});
        for(unsigned c=0;c<2;++c) for(unsigned trial=0;trial<3;++trial) {unsigned at=8000*(2+(c*4+trial+1)*3)+200+c*321;
            for(unsigned i=0;i<capture.reference.size();++i) capture.captured[at+i]=-capture.reference[i]*.4f;}
        require(capture.analyze()==std::vector<unsigned>({200,521}),"Repeated inverted-return calibration");
        unsigned unstable=8000*(2+3*3)+200;
        for(unsigned i=0;i<capture.reference.size();++i) capture.captured[unstable+i]=0;
        for(unsigned i=0;i<capture.reference.size();++i) capture.captured[unstable+80+i]=-capture.reference[i]*.4f;
        bool unstable_rejected=false;try {capture.analyze();}catch(...) {unstable_rejected=true;}require(unstable_rejected,"Unstable trials accepted");
        std::fill(capture.captured.begin(),capture.captured.end(),0.f);bool rejected=false;try {capture.analyze();}catch(...) {rejected=true;}require(rejected,"Silence accepted");
        aggregate::CalibrationCapture interleaved(3,4,2,8000,{1,3},{});
        std::vector<float> microphone(3*64),speaker(4*64);for(unsigned i=0;i<64;++i) {microphone[i*3]=.9f;microphone[i*3+2]=.125f;}
        interleaved.recording=false;std::fill(speaker.begin(),speaker.end(),1.f);
        aggregate::CalibrationCapture::process(&interleaved,microphone.data(),speaker.data(),64);
        require(interleaved.idle&&interleaved.position==0&&interleaved.idle_frames==64&&std::all_of(speaker.begin(),speaker.end(),[](auto x){return x==0;}),"Idle calibration must acknowledge silence without recording");
        interleaved.recording=true;
        std::vector<unsigned> emitted(4);
        while(interleaved.position<interleaved.captured.size()) {aggregate::CalibrationCapture::process(&interleaved,microphone.data(),speaker.data(),64);
            for(unsigned i=0;i<64;++i) for(unsigned c=0;c<4;++c) if(speaker[i*4+c]!=0) ++emitted[c];}
        unsigned burst=4*unsigned(std::count_if(interleaved.reference.begin(),interleaved.reference.end(),[](float x){return x!=0;}));
        require(emitted==std::vector<unsigned>({0,burst,0,burst}),"Each selected channel must receive a conditioning burst plus three measured bursts; other channels silent");
        require(interleaved.captured[20000]==.125f&&interleaved.maximum_peak==.125f,"Selected interleaved microphone channel incorrect");
        auto sweep=aggregate::calibration_signal(48000);std::vector<float> acoustic(40000);float filtered=0;
        for(unsigned i=0;i<sweep.size();++i) {filtered=.3f*sweep[i]+.7f*filtered;acoustic[2000+731+i]+=-filtered*.8f;acoustic[2000+731+i+320]+=-filtered*.2f;}
        auto acoustic_result=aggregate::measure_delay(sweep,acoustic,2000,5000);
        require(std::abs(int(acoustic_result.frames)-731)<16&&acoustic_result.correlation>.35,"Filtered and reflected acoustic sweep not detected");
        std::mt19937 random(143);std::normal_distribution<float> noise(0,.00015f);
        std::vector<float> quiet(40000);for(auto& value:quiet) value=noise(random)+.04f;
        for(unsigned i=0;i<sweep.size();++i) quiet[2731+i]-=sweep[i]*.005f;
        auto quiet_result=aggregate::measure_delay(sweep,quiet,2000,5000);
        require(std::abs(int(quiet_result.frames)-731)<3&&quiet_result.correlation<.35,"Quiet probe below noise with DC offset not detected");
        for(auto& value:quiet) value=noise(random)+.04f;
        bool noise_rejected=false;try {aggregate::measure_delay(sweep,quiet,2000,5000);}catch(...) {noise_rejected=true;}require(noise_rejected,"Noise-only recording accepted");
        for(unsigned i=0;i<quiet.size();++i) quiet[i]=.01f*std::sin(i*.12)+noise(random);
        bool tone_rejected=false;try {aggregate::measure_delay(sweep,quiet,2000,5000);}catch(...) {tone_rejected=true;}require(tone_rejected,"Unrelated tone accepted");
        aggregate::Config simulated;simulated.simulation=true;simulated.sample_rate=8000;simulated.capture={{"sim",1}};simulated.playback={{"sim",2}};
        HANDLE cancel=CreateEventW(nullptr,TRUE,TRUE,nullptr);bool canceled=false;try {aggregate::calibrate_outputs(simulated,0,{0,1},cancel,[](auto&){});}catch(const std::exception& e) {canceled=std::string(e.what()).find("中止")!=std::string::npos;}CloseHandle(cancel);require(canceled,"Calibration cancellation failed");
        std::cout<<"PASS: port ID reorder/offline preservation, fractional sample delays, alignment, invalid values, repeated measurement, silence rejection\n";
        return 0;
    }catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
