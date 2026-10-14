#pragma once
#include <vector>
#include <cmath>
#include <stdexcept>
#include <cstdint>
#include <string>
#include <algorithm>
#include <complex>
namespace aggregate {
inline std::vector<float> calibration_signal(unsigned rate=48000) {
    // A windowed sweep concentrates energy in the audible microphone/speaker band.
    std::vector<float> signal(std::max(256u,rate/2));
    double duration=double(signal.size())/rate,low=300,high=std::min(5000.,rate*.35);
    constexpr double pi=3.14159265358979323846;
    for(unsigned i=0;i<signal.size();++i) {double t=double(i)/rate;
        double window=.5-.5*std::cos(2*pi*i/(signal.size()-1));
        signal[i]=float(.015*window*std::sin(2*pi*(low*t+(high-low)*t*t/(2*duration))));}
    return signal;
}
struct DelayMeasurement {unsigned frames;double correlation;};
// Worker-thread only. FFT convolution keeps long, quiet probes inexpensive.
inline void calibration_fft(std::vector<std::complex<double>>& values,bool inverse) {
    const size_t count=values.size();
    for(size_t i=1,j=0;i<count;++i) {size_t bit=count>>1;for(;j&bit;bit>>=1) j^=bit;j^=bit;if(i<j) std::swap(values[i],values[j]);}
    constexpr double pi=3.14159265358979323846;
    for(size_t length=2;length<=count;length*=2) {
        auto step=std::polar(1.,(inverse?2:-2)*pi/length);
        for(size_t start=0;start<count;start+=length) {std::complex<double> factor=1;
            for(size_t j=0;j<length/2;++j) {auto a=values[start+j],b=values[start+j+length/2]*factor;values[start+j]=a+b;values[start+j+length/2]=a-b;factor*=step;}}
    }
    if(inverse) for(auto& value:values) value/=double(count);
}
inline DelayMeasurement measure_delay(const std::vector<float>& reference,const std::vector<float>& captured,unsigned emitted_at,unsigned maximum_delay) {
    if(reference.empty()||captured.size()<size_t(emitted_at)+reference.size()+maximum_delay) throw std::runtime_error("Insufficient calibration samples");
    size_t count=reference.size(),span=count+maximum_delay,fft_size=1;
    while(fft_size<span+count-1) fft_size*=2;
    std::vector<std::complex<double>> probe(fft_size),recording(fft_size);
    std::vector<double> sums(span+1),energies(span+1),scores(size_t(maximum_delay)+1);
    double mean=0;for(auto x:reference) mean+=x;mean/=count;
    double reference_energy=0;for(size_t i=0;i<count;++i) {double x=reference[i]-mean;reference_energy+=x*x;probe[count-1-i]=x;}
    if(reference_energy<=1e-24) throw std::runtime_error("試験信号が無音です");
    for(size_t i=0;i<span;++i) {double x=captured[emitted_at+i];recording[i]=x;sums[i+1]=sums[i]+x;energies[i+1]=energies[i]+x*x;}
    calibration_fft(probe,false);calibration_fft(recording,false);
    for(size_t i=0;i<fft_size;++i) recording[i]*=probe[i];
    calibration_fft(recording,true);
    DelayMeasurement best{};
    for(unsigned delay=0;delay<=maximum_delay;++delay) {
        double sum=sums[delay+count]-sums[delay];
        double energy=std::max(0.,energies[delay+count]-energies[delay]-sum*sum/count);
        double dot=recording[delay+count-1].real();
        double score=energy>1e-24?std::min(1.,std::abs(dot)/std::sqrt(energy*reference_energy)):0;
        scores[delay]=score;
        if(score>best.correlation) best={delay,score};
    }
    // A weak return is accepted only when its peak stands well above unrelated
    // offsets. Lowering the old fixed threshold alone would accept noise.
    double sum=0,square=0;size_t samples=0;
    for(unsigned i=0;i<=maximum_delay;++i) if(std::abs(double(i)-best.frames)>64) {sum+=scores[i];square+=scores[i]*scores[i];++samples;}
    double background=samples?sum/samples:0;
    double deviation=samples?std::sqrt(std::max(0.,square/samples-background*background)):0;
    double required=std::max(.08,background+8*deviation);
    if(best.correlation<required) throw std::runtime_error("試験音を雑音と区別できません（相関 "+std::to_string(best.correlation)+"、必要 "+std::to_string(required)+"）。マイク位置・周囲の音・音声強調を確認してください");
    return best;
}
}
