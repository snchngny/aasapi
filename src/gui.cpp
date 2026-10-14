#include <windows.h>
#include <filesystem>
#include <string>

// Keep the distributed entry point compatible; all controls live in the router.
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    wchar_t executable[32768]{};GetModuleFileNameW(nullptr,executable,32768);
    auto path=std::filesystem::path(executable).parent_path()/L"aggregate_router.exe";
    auto command=L"\""+path.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(path.c_str(),command.data(),nullptr,nullptr,FALSE,0,nullptr,path.parent_path().c_str(),&startup,&process)) {
        MessageBoxW(nullptr,L"AASAPIを開けません。build内のaggregate_router.exeを確認してください",L"AASAPI",MB_OK|MB_ICONERROR);return 1;
    }
    CloseHandle(process.hThread);CloseHandle(process.hProcess);return 0;
}
