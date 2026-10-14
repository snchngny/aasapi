#pragma once
#include "engine.h"
#include "routing.h"

namespace aggregate {
struct WindowsPort { EndpointSpec spec; std::string name; bool input=false; };
inline std::vector<WindowsPort> windows_ports() {
    ma_context context{}; ma_backend backend=ma_backend_wasapi;
    if(ma_context_init(&backend,1,nullptr,&context)!=MA_SUCCESS) throw std::runtime_error("WASAPI enumeration failed");
    std::vector<WindowsPort> result;
    try {
        ma_device_info *p=nullptr,*c=nullptr;ma_uint32 np=0,nc=0;
        if(ma_context_get_devices(&context,&p,&np,&c,&nc)!=MA_SUCCESS) throw std::runtime_error("WASAPI enumeration failed");
        for(bool input:{true,false}) for(unsigned i=0;i<(input?nc:np);++i) {
            auto item=(input?c:p)[i];ma_device_info detail{};
            if(ma_context_get_device_info(&context,input?ma_device_type_capture:ma_device_type_playback,&item.id,&detail)!=MA_SUCCESS || !detail.nativeDataFormatCount)
                throw std::runtime_error("Cannot inspect Windows port: "+std::string(item.name));
            result.push_back({{utf8(item.id.wasapi),detail.nativeDataFormats[0].channels},std::string(item.isDefault?"[Windows既定] ":"")+item.name,input});
        }
    } catch(...) {ma_context_uninit(&context);throw;}
    ma_context_uninit(&context);return result;
}
inline void include_windows_ports(Config& config,const std::vector<WindowsPort>& ports) {
    for(const auto& port:ports) {
        auto& specs=port.input?config.capture:config.playback;
        auto found=std::find_if(specs.begin(),specs.end(),[&](const auto& s){return s.id==port.spec.id;});
        if(found==specs.end()) specs.push_back(port.spec);else found->channels=port.spec.channels;
    }
}
inline std::string loopback_key(const std::string& id,unsigned channel) {return "loopback:"+id+":"+std::to_string(channel);}
inline void copy_port_channels(const float* source,unsigned channels,float* destination,unsigned total,unsigned offset,unsigned frames) {
    for(unsigned f=0;f<frames;++f) std::copy_n(source+size_t(f)*channels,channels,destination+size_t(f)*total+offset);
}
}
