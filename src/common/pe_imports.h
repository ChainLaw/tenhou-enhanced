#pragma once
#include <windows.h>
#include <cstring>

// Only the original x86 executable's imports are changed. Keep traversal and
// pointer writes shared so chained hooks retain the previous import target.
namespace PeImports {
struct Import {
    const char* library;
    const char* name; // Null for an ordinal import.
    unsigned ordinal;
    DWORD* slot;
};

template<class Visitor>
bool Visit(HMODULE module,Visitor visitor) {
    static_assert(sizeof(void*)==4,"The client import table is PE32");
    auto base=reinterpret_cast<BYTE*>(module);
    auto dos=reinterpret_cast<IMAGE_DOS_HEADER*>(base);
    auto nt=reinterpret_cast<IMAGE_NT_HEADERS32*>(base+dos->e_lfanew);
    const auto rva=nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress;
    if(!rva)return true;
    for(auto descriptor=reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(base+rva);descriptor->Name;++descriptor) {
        if(!descriptor->OriginalFirstThunk)continue;
        auto names=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+descriptor->OriginalFirstThunk);
        auto iat=reinterpret_cast<IMAGE_THUNK_DATA32*>(base+descriptor->FirstThunk);
        for(;names->u1.AddressOfData;++names,++iat) {
            const bool ordinal=IMAGE_SNAP_BY_ORDINAL32(names->u1.Ordinal)!=0;
            const Import entry{
                reinterpret_cast<const char*>(base+descriptor->Name),
                ordinal?nullptr:reinterpret_cast<const char*>(reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(base+names->u1.AddressOfData)->Name),
                ordinal?static_cast<unsigned>(IMAGE_ORDINAL32(names->u1.Ordinal)):0,
                &iat->u1.Function
            };
            if(!visitor(entry))return false;
        }
    }
    return true;
}

inline bool Replace(DWORD* slot,void* target) {
    DWORD old=0;
    if(!VirtualProtect(slot,sizeof(*slot),PAGE_READWRITE,&old))return false;
    *slot=reinterpret_cast<DWORD>(target);
    DWORD ignored=0;VirtualProtect(slot,sizeof(*slot),old,&ignored);
    return true;
}

template<class Function>
bool Hook(HMODULE module,const char* name,Function target,Function& previous) {
    bool installed=false;
    Visit(module,[&](const Import& entry) {
        if(!entry.name||strcmp(entry.name,name))return true;
        const auto original=reinterpret_cast<Function>(*entry.slot);
        installed=Replace(entry.slot,reinterpret_cast<void*>(target));
        if(installed)previous=original;
        return false;
    });
    return installed;
}
}
