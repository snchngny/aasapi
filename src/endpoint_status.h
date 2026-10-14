#pragma once
#include "engine.h"
#include <mmdeviceapi.h>
#include <endpointvolume.h>
namespace aggregate {
struct EndpointStatus {bool available=false,muted=false;float volume=0;};
inline EndpointStatus endpoint_status(const std::string& id) {
    EndpointStatus result;auto com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
    IMMDeviceEnumerator* enumerator=nullptr;IMMDevice* device=nullptr;IAudioEndpointVolume* volume=nullptr;
    if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator)))) {
        if(SUCCEEDED(enumerator->GetDevice(wide(id).c_str(),&device))&&SUCCEEDED(device->Activate(__uuidof(IAudioEndpointVolume),CLSCTX_ALL,nullptr,reinterpret_cast<void**>(&volume)))) {
            BOOL muted=FALSE;if(SUCCEEDED(volume->GetMute(&muted))&&SUCCEEDED(volume->GetMasterVolumeLevelScalar(&result.volume))) {result.available=true;result.muted=muted!=FALSE;}
        }
    }
    if(volume) volume->Release();if(device) device->Release();if(enumerator) enumerator->Release();if(SUCCEEDED(com)) CoUninitialize();return result;
}
}
