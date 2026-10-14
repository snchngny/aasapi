#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace aggregate {
// One producer and one consumer; all storage is allocated before audio starts.
class FrameRing {
    unsigned channels_;
    uint64_t capacity_;
    std::vector<float> samples_;
    alignas(64) std::atomic<uint64_t> write_{0};
    alignas(64) std::atomic<uint64_t> read_{0};
public:
    FrameRing(unsigned channels, unsigned capacity)
        : channels_(channels), capacity_(capacity), samples_(size_t(channels)*capacity) {
        if (!channels || !capacity || (capacity&(capacity-1))) throw std::invalid_argument("ring capacity must be a power of two");
    }
    unsigned channels() const { return channels_; }
    uint64_t available() const {
        const auto r = read_.load(std::memory_order_acquire);
        return write_.load(std::memory_order_acquire) - r;
    }
    bool push(const float* input, unsigned frames) {
        auto w = write_.load(std::memory_order_relaxed);
        auto r = read_.load(std::memory_order_acquire);
        if (frames > capacity_ - (w-r)) return false;
        for (unsigned i=0; i<frames; ++i)
            std::copy_n(input+size_t(i)*channels_, channels_,
                        samples_.data()+((w+i)&(capacity_-1))*channels_);
        write_.store(w+frames, std::memory_order_release);
        return true;
    }
    float peek(uint64_t relative, unsigned channel) const {
        const auto r=read_.load(std::memory_order_relaxed);
        return samples_[((r+relative)&(capacity_-1))*channels_+channel];
    }
    void consume(uint64_t frames) {
        read_.fetch_add(frames, std::memory_order_release);
    }
    void reset() { read_.store(0); write_.store(0); }
};

// Small band-limited fractional-delay filter. Rates are restricted to +/-2000 ppm.
// Coefficients are calculated once, never in an audio callback.
struct SincKernel {
    static constexpr unsigned taps=64, phases=1024;
    std::array<std::array<float,taps>,phases+1> coefficients{};
    SincKernel() {
        constexpr double pi=3.14159265358979323846, cutoff=0.96;
        for (unsigned p=0;p<=phases;++p) {
            double sum=0, fraction=double(p)/phases;
            for (unsigned k=0;k<taps;++k) {
                double x=double(k)-(taps/2-1)-fraction;
                double sinc=std::abs(x)<1e-12 ? cutoff : std::sin(pi*cutoff*x)/(pi*x);
                double window=0.42-0.5*std::cos(2*pi*k/(taps-1))+0.08*std::cos(4*pi*k/(taps-1));
                coefficients[p][k]=float(sinc*window); sum+=sinc*window;
            }
            for (auto& coefficient:coefficients[p]) coefficient=float(coefficient/sum);
        }
    }
};

class AdaptiveReader {
    FrameRing& ring_;
    const SincKernel& kernel_;
    double target_, phase_=0, filtered_=0, integral_=0, ratio_=1;
    bool primed_=false;
public:
    std::atomic<uint64_t> underruns{0};
    std::atomic<double> correction_ppm{0};
    std::atomic<double> queue_error{0};
    AdaptiveReader(FrameRing& ring,const SincKernel& kernel,unsigned target)
        : ring_(ring),kernel_(kernel),target_(target+SincKernel::taps) {}
    void reset() {
        phase_=filtered_=integral_=0; ratio_=1; primed_=false;
        underruns=0; correction_ppm=0;queue_error=0;
    }
    bool render(float* output,unsigned frames,double sample_rate) {
        const auto available=ring_.available();
        std::fill_n(output,size_t(frames)*ring_.channels(),0.f);
        if (!primed_) {
            if (available < target_) return false;
            primed_=true; filtered_=0;
        }
        double dt=frames/sample_rate;
        double error=double(available)-target_;
        // Packet batching is scheduling jitter, not an instantaneous clock-rate
        // change. A gentle controller avoids turning it into audible time drift.
        filtered_ += (dt/(2.+dt))*(error-filtered_);
        queue_error.store(filtered_,std::memory_order_relaxed);
        // Do not accumulate integral debt while rate correction is saturated.
        // Otherwise a large startup prefill drains below the safe queue later.
        double requested=1.5*filtered_+integral_;
        if((requested<2000||filtered_<0)&&(requested> -2000||filtered_>0))
            integral_=std::clamp(integral_+filtered_*dt*.08,-1500.,1500.);
        double ppm=std::clamp(1.5*filtered_+integral_,-2000.,2000.);
        ratio_=1.+ppm*1e-6; correction_ppm.store(ppm,std::memory_order_relaxed);
        const uint64_t required=uint64_t(std::floor(phase_+(frames-1)*ratio_))+SincKernel::taps;
        if (available<required) {
            ++underruns; primed_=false; phase_=0;
            ring_.consume(available); // Consumer only; never overwrite a producer's cursor.
            return false;
        }
        for (unsigned f=0;f<frames;++f) {
            auto base=uint64_t(phase_);
            double table_position=(phase_-double(base))*SincKernel::phases;
            unsigned p=std::min(unsigned(table_position),SincKernel::phases-1);
            float blend=float(table_position-p);
            for (unsigned c=0;c<ring_.channels();++c) {
                float value=0;
                for (unsigned k=0;k<SincKernel::taps;++k) {
                    float coefficient=kernel_.coefficients[p][k]*(1-blend)+kernel_.coefficients[p+1][k]*blend;
                    value+=ring_.peek(base+k,c)*coefficient;
                }
                output[size_t(f)*ring_.channels()+c]=value;
            }
            phase_+=ratio_;
        }
        auto consumed=uint64_t(phase_);
        ring_.consume(consumed); phase_-=double(consumed);
        return true;
    }
};
}
