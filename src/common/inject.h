#pragma once
#include <windows.h>
#include <string>
#include <tlhelp32.h>
inline bool InjectPatch(HANDLE process, DWORD pid, const std::wstring& dll) {
    // All callers and children are x86. Resolve LoadLibraryW relative to its
    // actual owner (KernelBase on modern Windows), not by assuming equal ASLR.
    auto load = GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HMODULE owner{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(load), &owner)) return false;
    wchar_t ownerPath[MAX_PATH]{}; GetModuleFileNameW(owner, ownerPath, MAX_PATH);
    const wchar_t* ownerName = wcsrchr(ownerPath,L'\\'); ownerName=ownerName?ownerName+1:ownerPath;
    uintptr_t remoteOwner=0;
    HANDLE snap=CreateToolhelp32Snapshot(TH32CS_SNAPMODULE|TH32CS_SNAPMODULE32,pid);
    MODULEENTRY32W mod{sizeof(mod)};
    if(snap!=INVALID_HANDLE_VALUE) {
        if(Module32FirstW(snap,&mod)) do { if(!_wcsicmp(mod.szModule,ownerName)) remoteOwner=reinterpret_cast<uintptr_t>(mod.modBaseAddr); } while(Module32NextW(snap,&mod));
        CloseHandle(snap);
    }
    // Suspended processes may not have completed loader initialization yet.
    // Kernel32/KernelBase are system-wide shared mappings for same-bitness
    // processes; use the local address only for that initial-loader case.
    auto remoteLoad=remoteOwner ? remoteOwner+(reinterpret_cast<uintptr_t>(load)-reinterpret_cast<uintptr_t>(owner)) : reinterpret_cast<uintptr_t>(load);
    const SIZE_T size=(dll.size()+1)*sizeof(wchar_t);
    void* remote=VirtualAllocEx(process,nullptr,size,MEM_COMMIT|MEM_RESERVE,PAGE_READWRITE);
    if(!remote) return false;
    SIZE_T wrote{};
    if(!WriteProcessMemory(process,remote,dll.c_str(),size,&wrote)||wrote!=size) {VirtualFreeEx(process,remote,0,MEM_RELEASE);return false;}
    HANDLE thread=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(remoteLoad),remote,0,nullptr);
    if(!thread) {VirtualFreeEx(process,remote,0,MEM_RELEASE);return false;}
    DWORD wait=WaitForSingleObject(thread,15000), base=0;
    if(wait==WAIT_OBJECT_0) GetExitCodeThread(thread,&base);
    CloseHandle(thread);
    if(wait!=WAIT_OBJECT_0) return false; // Caller terminates owned child; don't free memory still in use.
    VirtualFreeEx(process,remote,0,MEM_RELEASE);
    if(!base) return false;
    HMODULE mapped=LoadLibraryExW(dll.c_str(),nullptr,DONT_RESOLVE_DLL_REFERENCES);
    if(!mapped) return false;
    auto init=GetProcAddress(mapped,"EnhancedInitialize");
    uintptr_t offset=init?reinterpret_cast<uintptr_t>(init)-reinterpret_cast<uintptr_t>(mapped):0;
    FreeLibrary(mapped);
    if(!offset) return false;
    thread=CreateRemoteThread(process,nullptr,0,reinterpret_cast<LPTHREAD_START_ROUTINE>(base+offset),nullptr,0,nullptr);
    if(!thread) return false;
    wait=WaitForSingleObject(thread,15000); DWORD ok=0;
    if(wait==WAIT_OBJECT_0) GetExitCodeThread(thread,&ok);
    CloseHandle(thread);
    return wait==WAIT_OBJECT_0&&ok==1;
}
