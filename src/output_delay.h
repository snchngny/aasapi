#pragma once
#include <vector>
#include <algorithm>
namespace aggregate {
class OutputDelay {
    std::vector<std::vector<float>> buffers_;
    std::vector<unsigned> cursor_;
public:
    OutputDelay(unsigned channels,unsigned rate,const std::vector<unsigned>& milliseconds,const std::vector<unsigned>& samples={}):buffers_(channels),cursor_(channels) {
        for(unsigned c=0;c<std::min(milliseconds.size(),buffers_.size());++c) buffers_[c].resize(size_t(milliseconds[c])*rate/1000);
        for(unsigned c=0;c<std::min(samples.size(),buffers_.size());++c) buffers_[c].resize(samples[c]);
    }
    void process(float* output,unsigned frames,bool muted) {
        for(unsigned c=0;c<buffers_.size();++c) if(!buffers_[c].empty()) for(unsigned f=0;f<frames;++f) {
            auto& value=output[size_t(f)*buffers_.size()+c];auto& cursor=cursor_[c];
            if(muted) {buffers_[c][cursor]=0;value=0;}else std::swap(value,buffers_[c][cursor]);
            cursor=(cursor+1)%buffers_[c].size();
        }
    }
};
}
