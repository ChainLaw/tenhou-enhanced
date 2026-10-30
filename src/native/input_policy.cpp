#include "pe_imports.h"
#include "input_policy.h"
#include <cstring>
namespace InputPolicy {
namespace {
std::wstring configuration;
thread_local HHOOK keyboardHook=nullptr;
thread_local bool escapeAction=false;
thread_local unsigned escapeFlags=0;
using ProfileInt=UINT(WINAPI*)(LPCSTR,LPCSTR,INT,LPCSTR);
ProfileInt previousProfile=nullptr;
UINT WINAPI profileInt(LPCSTR section,LPCSTR key,INT fallback,LPCSTR file){
    // Reuse the native right-click state checks and actions. Override only this
    // synchronous read; never toggle or persist the user's mouse preferences.
    if(section&&key&&!_stricmp(section,"STATUS")&&!_stricmp(key,"FLAGS0")){
        if(escapeAction)return escapeFlags;
        fallback|=0x0c00; // Inverted double-click defaults; saved preferences win.
    }
    return previousProfile(section,key,fallback,file);
}
BOOL CALLBACK findGame(HWND window,LPARAM result){
    char cls[64]{};GetClassNameA(window,cls,64);
    if(!strcmp(cls,"WC_GAME")&&IsWindowVisible(window)){*reinterpret_cast<HWND*>(result)=window;return FALSE;}return TRUE;
}
LRESULT CALLBACK keyboard(int code,WPARAM wp,LPARAM lp){
    if(code>=0&&wp==PM_REMOVE){
        auto message=reinterpret_cast<MSG*>(lp);
        if(message->message==WM_KEYDOWN&&message->wParam==VK_ESCAPE){
            // Limit shortcuts to the native main window. Owned settings/dialog
            // windows retain their normal Escape-to-close behavior.
            HWND root=GetAncestor(message->hwnd,GA_ROOT);char cls[80]{};GetClassNameA(root,cls,80);
            if(!strcmp(cls,"WC_TENHOU_WIN_APP_MAIN")){
                char focusClass[32]{};GetClassNameA(message->hwnd,focusClass,32);
                HWND game=nullptr;EnumChildWindows(root,findGame,reinterpret_cast<LPARAM>(&game));
                const bool nativeTarget=!strcmp(focusClass,"WC_GAME")||!strcmp(focusClass,"WC_VIEW")||!strcmp(focusClass,"WC_TENHOU_WIN_APP_MAIN")||!_stricmp(focusClass,"Button");
                if(nativeTarget&&game&&HandleEscape(game,message->lParam))message->message=WM_NULL;
            }
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
}
bool HandleEscape(HWND game,LPARAM keyFlags){
    // 3010 is the native replay navigation child, rather than a playable table.
    if(!game||GetDlgItem(game,3010)||!previousProfile)return false;
    const bool pass=GetPrivateProfileIntW(L"STATUS",L"ESC_PASS",0,configuration.c_str())!=0;
    const bool discard=GetPrivateProfileIntW(L"STATUS",L"ESC_DISCARD",0,configuration.c_str())!=0;
    if(!pass&&!discard)return false;
    if(keyFlags&(LPARAM(1)<<30))return true; // one action per key press
    escapeFlags=GetPrivateProfileIntW(L"STATUS",L"FLAGS0",0x4c2d,configuration.c_str());
    escapeFlags=(escapeFlags&~(0x20u|0x200u))|(discard?0x20u:0)|(pass?0:0x200u);
    escapeAction=true;
    SendMessageA(game,WM_RBUTTONDOWN,0,0);
    escapeAction=false;
    return true;
}
void AttachThread(){if(!keyboardHook)keyboardHook=SetWindowsHookExW(WH_GETMESSAGE,keyboard,nullptr,GetCurrentThreadId());}
void DetachThread(){if(keyboardHook){UnhookWindowsHookEx(keyboardHook);keyboardHook=nullptr;}}
bool Install(HMODULE executable,const std::wstring& directory){
    configuration=directory+L"\\config.ini";
    return PeImports::Hook(executable,"GetPrivateProfileIntA",profileInt,previousProfile);
}
}
