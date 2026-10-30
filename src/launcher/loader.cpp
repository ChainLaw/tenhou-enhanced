#include <windows.h>
#include <shellapi.h>
#include <string>
#include "inject.h"
int WINAPI wWinMain(HINSTANCE,HINSTANCE,PWSTR,int) {
    wchar_t file[MAX_PATH]{};
    if(!GetModuleFileNameW(nullptr,file,MAX_PATH)) return 1;
    std::wstring root=file; root.resize(root.find_last_of(L"\\/"));
    std::wstring exe=root+L"\\launcher.exe", dll=root+L"\\tenhou_patch.dll";
    int argc=0; auto argv=CommandLineToArgvW(GetCommandLineW(),&argc);
    std::wstring cmd=L"\""+exe+L"\"";
    for(int i=1;i<argc;++i) {
        // Preserve Windows quoting, including paths ending in backslashes.
        cmd+=L" \""; unsigned slashes=0;
        for(const wchar_t* p=argv[i];*p;++p) {
            if(*p==L'\\') {++slashes;continue;}
            if(*p==L'"') cmd.append(slashes*2+1,L'\\'); else cmd.append(slashes,L'\\');
            slashes=0; cmd+=*p;
        }
        cmd.append(slashes*2,L'\\');cmd+=L'"';
    }
    LocalFree(argv);
    if(GetFileAttributesW(exe.c_str())==INVALID_FILE_ATTRIBUTES||GetFileAttributesW(dll.c_str())==INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(nullptr,L"Place TenhouEnhanced.exe, tenhou_patch.dll and the original launcher.exe in the same directory.",L"Tenhou Enhanced",MB_ICONERROR);return 2;
    }
    auto config=root+L"\\config.ini";
    HANDLE f=CreateFileW(config.c_str(),GENERIC_WRITE,FILE_SHARE_READ,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE) {MessageBoxW(nullptr,L"The Tenhou Enhanced directory must be writable.",L"Tenhou Enhanced",MB_ICONERROR);return 3;}
    CloseHandle(f);
    STARTUPINFOW si{sizeof(si)}; PROCESS_INFORMATION pi{};
    if(!CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_SUSPENDED,nullptr,root.c_str(),&si,&pi)) return 4;
    if(!InjectPatch(pi.hProcess,pi.dwProcessId,dll)) {
        TerminateProcess(pi.hProcess,5);CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
        MessageBoxW(nullptr,L"Patch initialization failed. No unpatched client was started. See enhanced.log if present.",L"Tenhou Enhanced",MB_ICONERROR);return 5;
    }
    ResumeThread(pi.hThread);CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);return 0;
}
