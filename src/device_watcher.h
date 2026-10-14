#pragma once
#include <mmdeviceapi.h>
#include <atomic>
namespace aggregate {
// Notifications only mark the inventory dirty; COM/audio work stays on the UI thread.
class DeviceWatcher final:public IMMNotificationClient {
    std::atomic<ULONG> references_{1};
    IMMDeviceEnumerator* enumerator_=nullptr;
public:
    std::atomic<bool> changed{false};
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** result) override {
        if(!result) return E_POINTER;*result=nullptr;
        if(id==__uuidof(IUnknown)||id==__uuidof(IMMNotificationClient)) {*result=static_cast<IMMNotificationClient*>(this);AddRef();return S_OK;}return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {return ++references_;}
    ULONG STDMETHODCALLTYPE Release() override {return --references_;}
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR,DWORD) override {changed=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {changed=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override {changed=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow,ERole,LPCWSTR) override {changed=true;return S_OK;}
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR,const PROPERTYKEY key) override {
        constexpr PROPERTYKEY format_key{{0xf19f064d,0x082c,0x4e27,{0xbc,0x73,0x68,0x82,0xa1,0xbb,0x8e,0x4c}},0};
        if(IsEqualPropertyKey(key,format_key)) changed=true;return S_OK;
    }
    void start() {
        if(SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator),nullptr,CLSCTX_ALL,__uuidof(IMMDeviceEnumerator),reinterpret_cast<void**>(&enumerator_))))
            if(FAILED(enumerator_->RegisterEndpointNotificationCallback(this))) {enumerator_->Release();enumerator_=nullptr;}
    }
    void stop() {if(enumerator_) {enumerator_->UnregisterEndpointNotificationCallback(this);enumerator_->Release();enumerator_=nullptr;}}
    ~DeviceWatcher() {stop();}
};
}
