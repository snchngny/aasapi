#pragma once
#include "engine.h"
namespace aggregate {
inline std::vector<std::string> device_fingerprint() {
    ma_context context{};ma_backend backend=ma_backend_wasapi;std::vector<std::string> ids;
    if(ma_context_init(&backend,1,nullptr,&context)!=MA_SUCCESS) throw std::runtime_error("Device refresh failed");
    ma_device_info *p=nullptr,*c=nullptr;ma_uint32 np=0,nc=0;
    auto result=ma_context_get_devices(&context,&p,&np,&c,&nc);
    if(result==MA_SUCCESS) {for(unsigned i=0;i<np;++i) ids.push_back("out:"+utf8(p[i].id.wasapi)+":"+p[i].name);for(unsigned i=0;i<nc;++i) ids.push_back("in:"+utf8(c[i].id.wasapi)+":"+c[i].name);}
    ma_context_uninit(&context);if(result!=MA_SUCCESS) throw std::runtime_error("Device refresh enumeration failed");std::sort(ids.begin(),ids.end());return ids;
}
}
