#pragma once
#include "routing.h"
#include <map>
#include <iomanip>
#include <sstream>
namespace aggregate {
using PortDelays=std::map<std::string,double>;
inline double valid_delay(const std::string& text) {
    size_t used=0;double value=std::stod(text,&used);
    if(used!=text.size()||!std::isfinite(value)||value<0||value>2000) throw std::runtime_error("追加遅延は0〜2000 msで入力してください");
    return value;
}
inline std::vector<unsigned> delay_frames(const PortDelays& delays,const std::vector<RouteChannel>& ports,unsigned rate) {
    std::vector<unsigned> result(ports.size());
    for(unsigned c=0;c<ports.size();++c) {auto found=delays.find(ports[c].key);if(found!=delays.end()) result[c]=unsigned(std::llround(found->second*rate/1000.));}
    return result;
}
inline void write_delays(std::ostream& out,const PortDelays& delays) {
    out<<"delay_version 1\n"<<std::setprecision(12);
    for(auto& [key,value]:delays) out<<"delay "<<std::quoted(key)<<' '<<value<<'\n';
}
inline std::vector<unsigned> align_delays(const std::vector<unsigned>& measured,unsigned rate) {
    if(measured.empty()) throw std::runtime_error("計測する出力を選択してください");
    unsigned maximum=*std::max_element(measured.begin(),measured.end());std::vector<unsigned> result;
    for(auto value:measured) {if(maximum-value>rate*2) throw std::runtime_error("補正値が2000 msを超えています");result.push_back(maximum-value);}
    return result;
}
}
