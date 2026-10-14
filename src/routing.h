#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <stdexcept>
#include <vector>

namespace aggregate {
struct RouteChannel { std::string key,name,endpoint,family; };
inline bool feedback_blocked(const RouteChannel& source,const RouteChannel& destination) {
    return (!source.endpoint.empty() && source.endpoint==destination.endpoint)
        || (!source.family.empty() && source.family==destination.family)
        || destination.family=="unknown-native";
}
class RoutingMatrix {
    unsigned inputs_,outputs_,pc_channels_;
    std::unique_ptr<std::atomic<bool>[]> routes_;
    std::unique_ptr<std::atomic<bool>[]> forbidden_;
    std::unique_ptr<std::atomic<float>[]> input_peaks_,output_peaks_;
    std::vector<float> weights_;
    float previous_gain_=0;
public:
    std::atomic<float> gain{.1f};
    std::atomic<bool> muted{true},test_tone{false};
    std::atomic<uint64_t> clipped_samples{0},blocks{0};
    explicit RoutingMatrix(unsigned inputs,unsigned outputs,unsigned pc_channels=2):inputs_(inputs),outputs_(outputs),pc_channels_(pc_channels),
        routes_(new std::atomic<bool>[size_t(inputs)*outputs]{}),
        forbidden_(new std::atomic<bool>[size_t(inputs)*outputs]{}),
        input_peaks_(new std::atomic<float>[inputs]{}),output_peaks_(new std::atomic<float>[outputs]{}),
        weights_(size_t(inputs)*outputs,0) {
        if(pc_channels<1 || pc_channels>64 || inputs<pc_channels || inputs>192 || !outputs || outputs>128) throw std::runtime_error("Invalid routing dimensions");
    }
    unsigned pc_channels() const { return pc_channels_; }
    unsigned inputs() const { return inputs_; }
    unsigned outputs() const { return outputs_; }
    void permit(unsigned input,unsigned output,bool allowed) {auto index=size_t(input)*outputs_+output;forbidden_[index]=!allowed;}
    void connect(unsigned input,unsigned output,bool enabled) { if(input>=inputs_ || output>=outputs_) throw std::out_of_range("route"); auto index=size_t(input)*outputs_+output; if(enabled&&forbidden_[index].load()) throw std::runtime_error("Loopback return is blocked");routes_[index]=enabled; }
    bool requested(unsigned input,unsigned output) const { return routes_[size_t(input)*outputs_+output].load(); }
    bool connected(unsigned input,unsigned output) const {auto i=size_t(input)*outputs_+output;return routes_[i].load()&&!forbidden_[i].load();}
    bool input_connected(unsigned input) const { for(unsigned o=0;o<outputs_;++o) if(requested(input,o)) return true; return false; }
    bool output_connected(unsigned output) const { for(unsigned i=0;i<inputs_;++i) if(requested(i,output)) return true; return false; }
    float input_peak(unsigned input) const { return input_peaks_[input].load(); }
    float output_peak(unsigned output) const { return output_peaks_[output].load(); }
    void reset_audio_state() {
        std::fill(weights_.begin(),weights_.end(),0); previous_gain_=0;
        for(unsigned i=0;i<inputs_;++i) input_peaks_[i]=0;
        for(unsigned o=0;o<outputs_;++o) output_peaks_[o]=0;
        blocks=0; clipped_samples=0;
    }
    void render(const float* hardware,const float* pc,float* output,unsigned frames) {
        std::fill_n(output,size_t(frames)*outputs_,0.f);
        for(unsigned i=0;i<inputs_;++i) {
            float peak=0;
            for(unsigned f=0;f<frames;++f) {
                float x=i<pc_channels_?pc[size_t(f)*pc_channels_+i]:hardware[size_t(f)*(inputs_-pc_channels_)+i-pc_channels_];
                if(std::isfinite(x)) peak=std::max(peak,std::abs(x));
            }
            input_peaks_[i]=std::max(peak,input_peaks_[i].load()*.9f);
            for(unsigned o=0;o<outputs_;++o) {
                size_t index=size_t(i)*outputs_+o;
                const float target=routes_[index].load(std::memory_order_relaxed)&&!forbidden_[index].load(std::memory_order_relaxed)?1.f:0.f;
                float weight=weights_[index];
                if(!target && !weight) continue;
                for(unsigned f=0;f<frames;++f) {
                    weight+=std::clamp(target-weight,-1.f/240,1.f/240);
                    float x=i<pc_channels_?pc[size_t(f)*pc_channels_+i]:hardware[size_t(f)*(inputs_-pc_channels_)+i-pc_channels_];
                    if(std::isfinite(x)) output[size_t(f)*outputs_+o]+=x*weight;
                }
                weights_[index]=weight;
            }
        }
        const float target=muted.load()?0.f:std::clamp(gain.load(),0.f,1.f);
        uint64_t clips=0;
        for(unsigned f=0;f<frames;++f) {
            previous_gain_+=std::clamp(target-previous_gain_,-1.f/240,1.f/240);
            for(unsigned o=0;o<outputs_;++o) {
                auto& x=output[size_t(f)*outputs_+o]; x*=previous_gain_;
                if(std::abs(x)>1) ++clips;
                x=std::clamp(x,-1.f,1.f);
            }
        }
        for(unsigned o=0;o<outputs_;++o) {
            float peak=0; for(unsigned f=0;f<frames;++f) peak=std::max(peak,std::abs(output[size_t(f)*outputs_+o]));
            output_peaks_[o]=std::max(peak,output_peaks_[o].load()*.9f);
        }
        clipped_samples.fetch_add(clips); ++blocks;
    }
};
}
