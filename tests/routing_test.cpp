#include "routing.h"
#include "output_delay.h"
#include "config.h"
#include <chrono>
#include <iostream>
#include <limits>

void require(bool condition,const char* message) {if(!condition) throw std::runtime_error(message);}
int main() {
    try {
        auto config_path=std::filesystem::temp_directory_path()/("audio-config-test-"+std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())+".ini");
        {std::ofstream file(config_path);file<<"[engine]\nsample_rate=44100\nblock_frames=192\npc_channels=6\noutput_delay_ms=0,12,2000\nnative_wasapi_endpoint=a,b\nsimulation=true\n[playback.0]\nid=sim\nchannels=2\n";}
        auto config=aggregate::load_config(config_path);std::filesystem::remove(config_path);
        require(config.sample_rate==44100&&config.block_frames==192&&config.pc_channels==6&&config.output_delay_ms[2]==2000&&config.native_endpoint_matches("b")&&!config.native_endpoint_matches("bb"),"Portable configuration and exact multiple endpoint mapping");
        aggregate::RoutingMatrix visibility(6,8);
        require(!visibility.input_connected(5)&&!visibility.output_connected(7),"Empty routing has no connected channels");
        visibility.connect(5,7,true);
        require(visibility.input_connected(5)&&visibility.output_connected(7)&&!visibility.output_connected(0),"Connected higher channels must be visible regardless of backend or index");
        visibility.connect(5,7,false);
        require(!visibility.input_connected(5)&&!visibility.output_connected(7),"Last disconnect removes channel from basic view");
        aggregate::RoutingMatrix multi(6,4,4); multi.gain=1;multi.muted=false;
        multi.connect(3,2,true);multi.connect(4,3,true);
        std::vector<float> pc4(512*4),hw2(512*2),out4(512*4);
        for(unsigned f=0;f<512;++f) {pc4[f*4+3]=.4f;hw2[f*2]=.2f;}
        multi.render(hw2.data(),pc4.data(),out4.data(),512);
        require(std::abs(out4[511*4+2]-.4f)<1e-6f&&std::abs(out4[511*4+3]-.2f)<1e-6f,"Multichannel PC and hardware offset");
        aggregate::OutputDelay delay(2,1000,{3,0});
        float delayed[12]={1,2,0,0,0,0,0,0,0,0,0,0};delay.process(delayed,6,false);
        require(delayed[0]==0&&delayed[1]==2&&delayed[6]==1,"Per-output delay must preserve other channels");
        float muted[12]={1};delay.process(muted,6,true);require(std::all_of(std::begin(muted),std::end(muted),[](float x){return x==0;}),"Delayed output obeys mute");
        constexpr unsigned frames=512;
        aggregate::RoutingMatrix matrix(4,4);
        std::vector<float> pc(frames*2),hardware(frames*2),output(frames*4);
        for(unsigned f=0;f<frames;++f) {pc[f*2]=.2f;pc[f*2+1]=-.3f;hardware[f*2]=.1f;hardware[f*2+1]=.4f;}
        matrix.connect(0,0,true);matrix.connect(1,1,true);matrix.connect(0,2,true);matrix.connect(1,3,true);
        matrix.render(hardware.data(),pc.data(),output.data(),frames);
        require(std::all_of(output.begin(),output.end(),[](float x){return x==0;}),"Initial mute must produce silence");
        require(std::abs(matrix.input_peak(0)-.2f)<1e-6f,"PC input meter");
        matrix.muted=false;matrix.gain=1;
        matrix.render(hardware.data(),pc.data(),output.data(),frames);
        for(unsigned f=240;f<frames;++f) {
            require(std::abs(output[f*4]-.2f)<1e-5f && output[f*4]==output[f*4+2],"Left fan-out");
            require(std::abs(output[f*4+1]+.3f)<1e-5f && output[f*4+1]==output[f*4+3],"Right fan-out");
        }
        matrix.connect(2,0,true);matrix.connect(3,0,true);matrix.render(hardware.data(),pc.data(),output.data(),frames);
        require(std::abs(output.back()-(-.3f))<1e-5f,"Other outputs must stay independent");
        require(std::abs(output[(frames-1)*4]-.7f)<1e-5f,"Hardware + PC summing");
        matrix.connect(0,2,false);matrix.render(hardware.data(),pc.data(),output.data(),frames);
        require(output[(frames-1)*4+2]==0,"Disconnect must fade to silence");
        for(auto& x:pc) x=2;
        matrix.render(hardware.data(),pc.data(),output.data(),frames);
        require(output[(frames-1)*4]==1 && matrix.clipped_samples>0,"Summed output must saturate and count clips");
        pc[0]=std::numeric_limits<float>::quiet_NaN();pc[1]=std::numeric_limits<float>::infinity();
        matrix.render(hardware.data(),pc.data(),output.data(),frames);
        require(std::all_of(output.begin(),output.end(),[](float x){return std::isfinite(x)&&std::abs(x)<=1;}),"Non-finite input handling");
        matrix.muted=true;matrix.render(hardware.data(),pc.data(),output.data(),frames);
        require(output[(frames-1)*4]==0,"Mute transition must reach silence");
        aggregate::RouteChannel source{{},{},"tv",""},tv{{},{},"tv",""},rme{{},{},{},"RME"},unknown{{},{},{},"unknown-native"};
        require(aggregate::feedback_blocked(source,tv),"Same endpoint loopback must be blocked");
        require(!aggregate::feedback_blocked(source,rme),"Different devices can route");
        source.family="RME";require(aggregate::feedback_blocked(source,rme),"WDM to ASIO same-device loopback must be blocked");
        require(aggregate::feedback_blocked(source,unknown),"Unmapped native device must not accept PC return");
        matrix.permit(0,0,false);
        bool rejected=false;try {matrix.connect(0,0,true);}catch(const std::runtime_error&){rejected=true;}
        require(matrix.requested(0,0),"Blocked route intent must survive temporary unavailability");
        matrix.permit(0,0,true);require(matrix.connected(0,0),"Blocked route resumes when permitted");matrix.permit(0,0,false);
        require(rejected&&!matrix.connected(0,0),"Forbidden route must also be rejected by audio model");
        std::cout<<"PASS: stereo fan-out, independent channels, summing, live connect/disconnect, mute/gain ramps, clipping, meters, feedback guards\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
