#include "router_session.h"
#include <iostream>

int main() {
    try {
        aggregate::Config inventory_config;inventory_config.capture={{"missing",2}};inventory_config.playback={{"rme12",2}};
        std::vector<aggregate::WindowsPort> ports={{{"camo",2},"Camo",true},{{"rme12",2},"RME12",false},{{"rme34",2},"RME34",false},{{"rme56",2},"RME56",false},{{"mono",1},"Mono",false}};
        aggregate::include_windows_ports(inventory_config,ports);aggregate::include_windows_ports(inventory_config,ports);
        if(inventory_config.capture.size()!=2||inventory_config.playback.size()!=4||inventory_config.playback.back().channels!=1) throw std::runtime_error("All-port discovery, missing retention or dedup failed");
        float a[]={1,2,3,4},b[]={5,6,7,8},c[]={9,10,11,12},mono[]={13,14};float combined[14]{};
        aggregate::copy_port_channels(a,2,combined,7,0,2);aggregate::copy_port_channels(b,2,combined,7,2,2);aggregate::copy_port_channels(c,2,combined,7,4,2);aggregate::copy_port_channels(mono,1,combined,7,6,2);
        const float expected_ports[]={1,2,5,6,9,10,13,3,4,7,8,11,12,14};
        if(!std::equal(std::begin(combined),std::end(combined),std::begin(expected_ports))) throw std::runtime_error("Independent loopback channels crossed");
        aggregate::RoutingMatrix independent(7,7,7);independent.muted=false;independent.gain=1;
        for(unsigned i=0;i<7;++i) independent.connect(i,i,true);
        if(!independent.connected(0,0)) throw std::runtime_error("Same-device route wrongly prohibited");
        aggregate::Config config;config.simulation=true;config.block_frames=256;config.sample_rate=44100;config.pc_channels=4;config.output_delay_ms={3,0,0,0,0,0};
        config.capture={{"sim-a",2},{"sim-b",4}};config.playback={{"sim-a",2},{"sim-b",4}};
        aggregate::RoutingMatrix matrix(10,6,4);matrix.muted=false;matrix.gain=1;
        matrix.connect(4,0,true);matrix.connect(5,1,true);matrix.connect(4,4,true);matrix.connect(5,5,true);
        for(unsigned rate:{44100,96000}) for(unsigned cycle=0;cycle<2;++cycle) {
            config.sample_rate=rate;
            aggregate::RouterSession session(config,"",matrix);session.start();
            std::this_thread::sleep_for(std::chrono::milliseconds(150));session.stop();
            std::vector<float> output(config.block_frames*6);
            if(session.copy_stopped_output(output.data(),unsigned(output.size()))!=output.size()) throw std::runtime_error("No stopped output");
            for(unsigned f=0;f<config.block_frames;++f) for(unsigned c=0;c<6;++c) {
                float expected=(c==0||c==4)?.01f:(c==1||c==5)?.02f:0.f;
                if(std::abs(output[f*6+c]-expected)>1e-5f) throw std::runtime_error("Engine routing stride/fan-out mismatch");
            }
            if(session.health_flags()||!matrix.blocks.load()) throw std::runtime_error("Engine failed to run");
        }
        config.sample_rate=48000;
        aggregate::RoutingMatrix three_sources(12,6,6);three_sources.muted=false;three_sources.gain=1;three_sources.connect(6,0,true);
        {aggregate::RouterSession session(config,std::vector<aggregate::EndpointSpec>{{"",2},{"",2},{"",2}},three_sources);session.start();std::this_thread::sleep_for(std::chrono::milliseconds(100));session.stop();std::vector<float> result(config.block_frames*6);session.copy_stopped_output(result.data(),unsigned(result.size()));if(std::abs(result.back()-0.f)>1e-6f||std::abs(result[result.size()-6]-.01f)>1e-6f||session.health_flags()) throw std::runtime_error("Three-source session layout failed");}
        std::cout<<"PASS: all-port discovery + 3 stereo and mono independent streams + engine integration, 44100/96000 Hz, 4 PC channels, output delay, PC row offset, interleaved channel mapping, stereo fan-out, restart\n";
        return 0;
    } catch(const std::exception& e) {std::cerr<<"FAIL: "<<e.what()<<"\n";return 1;}
}
