#include "InputAssets.h"
#include <windows.h>
#include <Xinput.h>
#include <fstream>
#include <stdexcept>
namespace zengine::input {
std::filesystem::path AssetPath(const std::filesystem::path& assets) {
    const auto root=std::filesystem::canonical(assets);
    const auto path=std::filesystem::weakly_canonical(root/L"Input.zinput");
    if(_wcsicmp(path.parent_path().c_str(),root.c_str())!=0 || _wcsicmp(path.filename().c_str(),L"Input.zinput")!=0)
        throw std::runtime_error("Input Map must remain in the project Assets root (no links).");
    return path;
}
std::string Load(const std::filesystem::path& assets) {
    const auto path=AssetPath(assets); const auto size=std::filesystem::file_size(path);
    if(size>256*1024)throw std::runtime_error("Input Map exceeds 256 KiB.");
    std::ifstream in(path,std::ios::binary); std::string text(static_cast<std::size_t>(size),'\0');
    if(!in.read(text.data(),static_cast<std::streamsize>(size)))throw std::runtime_error("Cannot read Input Map.");
    Decode(text); return text;
}
void Save(const std::filesystem::path& assets,const Map& map,const std::string* expected) {
    const auto text=Encode(map); const auto path=AssetPath(assets);
    auto check=[&] { if(expected ? Load(assets)!=*expected : std::filesystem::exists(path)) throw std::runtime_error("Input Map changed on disk. Reload before saving."); }; check();
    std::filesystem::path temp; HANDLE handle=INVALID_HANDLE_VALUE;
    for(unsigned i=0;i<1000 && handle==INVALID_HANDLE_VALUE;++i) {
        temp=path; temp+=L".save-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(i);
        handle=CreateFileW(temp.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);
        if(handle==INVALID_HANDLE_VALUE && GetLastError()!=ERROR_FILE_EXISTS) throw std::runtime_error("Cannot create Input Map save file.");
    }
    if(handle==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create Input Map save file.");
    try {
        DWORD written=0; if(!WriteFile(handle,text.data(),static_cast<DWORD>(text.size()),&written,nullptr) || written!=text.size() || !FlushFileBuffers(handle))throw std::runtime_error("Cannot write Input Map.");
        CloseHandle(handle); handle=INVALID_HANDLE_VALUE; check();
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_WRITE_THROUGH|(expected?MOVEFILE_REPLACE_EXISTING:0)))throw std::runtime_error("Cannot replace Input Map; original preserved.");
    }catch(...){if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);DeleteFileW(temp.c_str());throw;}
}
void Ensure(const std::filesystem::path& assets) { if(!std::filesystem::exists(AssetPath(assets)))Save(assets,{}); }
Hardware PollWindows(bool focused) {
    Hardware h; if(!focused)return h;
    for(int i=0;i<256;++i)h.keys[i]=(GetAsyncKeyState(i)&0x8000)!=0;
    // System-only DLL lookup; missing XInput simply means no gamepads. No external dependency.
    struct Api { HMODULE dll=LoadLibraryExW(L"xinput1_4.dll",nullptr,LOAD_LIBRARY_SEARCH_SYSTEM32); using Fn=DWORD(WINAPI*)(DWORD,XINPUT_STATE*); Fn get=dll?reinterpret_cast<Fn>(GetProcAddress(dll,"XInputGetState")):nullptr; ~Api(){if(dll)FreeLibrary(dll);} };
    static const Api api;
    if(api.get)for(DWORD i=0;i<4;++i) { XINPUT_STATE s{}; if(api.get(i,&s)==ERROR_SUCCESS) {
        auto& p=h.pads[i]; p.connected=true;p.buttons=s.Gamepad.wButtons;
        auto axis=[](SHORT n){return n<0?static_cast<float>(n)/32768.0f:static_cast<float>(n)/32767.0f;};
        p.axes={axis(s.Gamepad.sThumbLX),axis(s.Gamepad.sThumbLY),axis(s.Gamepad.sThumbRX),axis(s.Gamepad.sThumbRY),s.Gamepad.bLeftTrigger/255.0f,s.Gamepad.bRightTrigger/255.0f};
    }}
    return h;
}
}
