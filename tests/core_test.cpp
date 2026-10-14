#include "adaptive_audio.h"
#include "config.h"
#include "native_asio.h"
#include <iostream>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <limits>
using namespace aggregate;
void require(bool condition,const char* message) { if(!condition) throw std::runtime_error(message); }

void ring_test() {
    FrameRing ring(2,8);
    float first[]={1,101,2,102,3,103,4,104,5,105,6,106};
    require(ring.push(first,6),"ring initial push");
    ring.consume(5); require(ring.push(first,6),"ring wrap push");
    require(ring.available()==7,"ring size after wrap");
    require(ring.peek(0,0)==6 && ring.peek(1,1)==101 && ring.peek(6,0)==6,"ring channel/wrap ordering");
    require(!ring.push(first,2),"overflow must reject atomically");
    require(ring.available()==7,"overflow cannot mutate cursor");
}
void codec_test() {
    for(ASIOSampleType type:{ASIOSTFloat32LSB,ASIOSTFloat64LSB,ASIOSTInt32LSB,ASIOSTInt24LSB,ASIOSTInt16LSB}) {
        alignas(8) std::array<uint8_t,128> buffer{};
        float values[]={-1.f,-.5f,0.f,.5f,1.f};
        for(unsigned i=0;i<5;++i) NativeAsio::encode(buffer.data(),type,i,values[i]);
        for(unsigned i=0;i<5;++i)
            require(std::abs(NativeAsio::decode(buffer.data(),type,i)-values[i])<.00004f,"native ASIO sample conversion");
        NativeAsio::encode(buffer.data(),type,0,std::numeric_limits<float>::quiet_NaN());
        require(NativeAsio::decode(buffer.data(),type,0)==0,"non-finite output must become silence");
        NativeAsio::encode(buffer.data(),type,1,2.f);
        require(NativeAsio::decode(buffer.data(),type,1)<=1.f,"native ASIO saturation");
    }
}
void concurrent_test() {
    FrameRing ring(2,1024); constexpr unsigned total=200000;
    std::atomic<bool> failed{false};
    std::thread producer([&] {
        for(unsigned i=0;i<total;++i) {
            float frame[]={float(i),float(i+1)};
            while(!ring.push(frame,1)) std::this_thread::yield();
        }
    });
    for(unsigned i=0;i<total;++i) {
        while(!ring.available()) std::this_thread::yield();
        if(ring.peek(0,0)!=float(i) || ring.peek(0,1)!=float(i+1)) failed=true;
        ring.consume(1);
    }
    producer.join(); require(!failed,"SPSC concurrent channel corruption");
}
void drift_test(const SincKernel& kernel,double ppm,unsigned channels) {
    constexpr unsigned frames=256,rate=48000,target=512;
    constexpr double seconds=180;
    FrameRing ring(channels,8192); AdaptiveReader reader(ring,kernel,target);
    std::vector<float> input(size_t(frames+8)*channels),output(size_t(frames)*channels);
    double credit=0,squared=0; uint64_t source_frame=0,overruns=0,measured=0;
    for(unsigned iteration=0;iteration<unsigned(seconds*rate/frames);++iteration) {
        credit+=frames*(1.+ppm*1e-6);
        unsigned produced=unsigned(credit); credit-=produced;
        for(unsigned f=0;f<produced;++f) {
            double sine=std::sin(2*3.14159265358979323846*1000*double(source_frame+f)/rate);
            for(unsigned c=0;c<channels;++c) input[size_t(f)*channels+c]=float(sine*(c+1)/channels);
        }
        source_frame+=produced;
        if(!ring.push(input.data(),produced)) ++overruns;
        bool rendered=reader.render(output.data(),frames,rate);
        if(rendered && iteration>2000) for(unsigned f=0;f<frames;++f) {
            for(unsigned c=1;c<channels;++c)
                require(std::abs(output[size_t(f)*channels+c]-(c+1)*output[size_t(f)*channels])<2e-5,"resampler channel separation");
            double value=output[size_t(f)*channels+channels-1]; squared+=value*value; ++measured;
        }
    }
    require(overruns==0 && reader.underruns==0,"clock drift caused xrun");
    require(std::abs(reader.correction_ppm.load()-ppm)<30,"clock controller did not converge");
    require(std::abs(std::sqrt(squared/measured)-std::sqrt(.5))<.005,"sine amplitude changed");
    require(ring.available()>128 && ring.available()<800,"queue escaped bounded latency");
    std::cout<<"drift="<<ppm<<" ppm, channels="<<channels<<", correction="<<reader.correction_ppm.load()
        <<", remaining="<<ring.available()<<", xruns=0\n";
}
void starvation_test(const SincKernel& kernel) {
    FrameRing ring(2,2048); AdaptiveReader reader(ring,kernel,512);
    std::vector<float> data(1024*2,.25f),out(256*2,999.f);
    require(!reader.render(out.data(),256,48000),"unprimed reader");
    require(out[0]==0 && out.back()==0,"startup silence");
    require(ring.push(data.data(),1024),"prefill");
    for(unsigned i=0;i<8;++i) reader.render(out.data(),256,48000);
    require(reader.underruns>0,"starvation counter");
    require(out[0]==0 && out.back()==0,"starvation silence");
    require(ring.push(data.data(),1024),"recovery prefill");
    require(reader.render(out.data(),256,48000),"starvation recovery");
    require(std::abs(out[0]-.25f)<1e-6,"DC normalization");
}
void frequency_test(const SincKernel& kernel) {
    for(double frequency:{100.,1000.,10000.,18000.,20000.}) {
        FrameRing ring(1,4096); AdaptiveReader reader(ring,kernel,512);
        std::array<float,256> input{},output{};
        double sum=0; unsigned count=0; uint64_t frame=0;
        for(unsigned block=0;block<200;++block) {
            for(auto& sample:input) sample=float(std::sin(2*3.14159265358979323846*frequency*double(frame++)/48000));
            require(ring.push(input.data(),256),"frequency test overrun");
            if(reader.render(output.data(),256,48000) && block>20)
                for(auto sample:output) { sum+=double(sample)*sample; ++count; }
        }
        double db=20*std::log10(std::sqrt(2*sum/count));
        std::cout<<"filter "<<frequency<<" Hz: "<<db<<" dB\n";
        require(std::abs(db)<0.15,"resampler passband error exceeds 0.15 dB");
    }
}
void jitter_test(const SincKernel& kernel) {
    FrameRing ring(2,8192); AdaptiveReader reader(ring,kernel,2048);
    std::vector<float> input(1024*2,.1f),output(256*2);
    unsigned bursts[]={0,512,128,384}; double credit=0;
    for(unsigned block=0;block<180*48000/256;++block) {
        double ppm=block<60*48000/256?500.:-500.;
        credit+=bursts[block%4]*(1.+ppm*1e-6);
        unsigned produced=unsigned(credit); credit-=produced;
        require(ring.push(input.data(),produced),"bursty device overflow");
        reader.render(output.data(),256,48000);
    }
    require(!reader.underruns,"bursty device underrun");
    require(std::abs(reader.correction_ppm.load()+500)<35,"clock step did not converge");
    std::cout<<"bursty callbacks +500 -> -500 ppm: correction="<<reader.correction_ppm.load()<<", xruns=0\n";
}
void startup_prefill_test(const SincKernel& kernel) {
    FrameRing ring(2,8192); AdaptiveReader reader(ring,kernel,2048);
    std::vector<float> input(6144*2,.1f),output(1024*2);
    require(ring.push(input.data(),6144),"startup prefill");
    for(unsigned block=0;block<180*48000/1024;++block) {
        require(ring.push(input.data(),block%2?1280:768),"startup queue overflow");
        require(reader.render(output.data(),1024,48000),"startup queue starvation");
    }
    require(!reader.underruns,"startup integral windup underrun");
    require(std::abs(reader.correction_ppm.load())<100,"startup correction did not settle");
    std::cout<<"large startup prefill: correction="<<reader.correction_ppm.load()<<", xruns=0\n";
}
void performance_test(const SincKernel& kernel) {
    constexpr unsigned channels=64,frames=128;
    FrameRing ring(channels,4096); AdaptiveReader reader(ring,kernel,256);
    std::vector<float> input(size_t(512)*channels,.1f),output(size_t(frames)*channels);
    require(ring.push(input.data(),192),"benchmark prefill");
    std::vector<double> times;
    for(unsigned i=0;i<400;++i) {
        require(ring.push(input.data(),frames),"benchmark overflow");
        auto before=std::chrono::steady_clock::now();
        require(reader.render(output.data(),frames,48000),"benchmark starvation");
        times.push_back(std::chrono::duration<double,std::micro>(std::chrono::steady_clock::now()-before).count());
    }
    std::sort(times.begin(),times.end());
    std::cout<<"64ch / 128 frames resampler: median="<<times[200]<<" us, p95="<<times[380]<<" us, max="<<times.back()<<" us; block budget=2666.67 us\n";
    require(times[380]<2666.67,"resampler p95 exceeds block deadline");
}
int main() {
    try {
        auto before=std::chrono::steady_clock::now();
        SincKernel kernel; ring_test(); codec_test(); concurrent_test(); starvation_test(kernel);
        drift_test(kernel,100,2); drift_test(kernel,-250,4); drift_test(kernel,1000,2);
        frequency_test(kernel); jitter_test(kernel); startup_prefill_test(kernel); performance_test(kernel);
        std::cout<<"PASS: ring, concurrency, starvation, passband, performance, 900 seconds of simulated clocks; elapsed="
            <<std::chrono::duration<double>(std::chrono::steady_clock::now()-before).count()<<"s\n";
        return 0;
    } catch(const std::exception& error) { std::cerr<<"FAIL: "<<error.what()<<"\n"; return 1; }
}
