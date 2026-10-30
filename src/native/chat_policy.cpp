#include "pe_imports.h"
#include "chat_policy.h"
#include "input_policy.h"
#include <commctrl.h>
#include <cstring>
#include <utility>
#include <vector>

namespace ChatPolicy {
namespace {
std::wstring configuration;
constexpr UINT_PTR subclassId=0x43484154;
constexpr UINT growSettingsMessage=WM_APP+0x346;
constexpr wchar_t minimumHeight[]=L"TenhouOtherMinimumHeight",sheetExpanded[]=L"TenhouSettingsExpanded";
thread_local HHOOK settingsHook=nullptr;
using CreateA=HWND(WINAPI*)(DWORD,LPCSTR,LPCSTR,DWORD,int,int,int,int,HWND,HMENU,HINSTANCE,LPVOID);
CreateA previousCreate=nullptr;
bool enabled(const wchar_t* key){return GetPrivateProfileIntW(L"STATUS",key,0,configuration.c_str())!=0;}
RECT rectangle(HWND control,HWND parent){RECT r{};GetWindowRect(control,&r);MapWindowPoints(nullptr,parent,reinterpret_cast<POINT*>(&r),2);return r;}
LRESULT CALLBACK pageObserver(HWND page,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    if(msg==WM_COMMAND&&HIWORD(wp)==BN_CLICKED){
        const int control=LOWORD(wp);
        const wchar_t* key=control==roomPopupControl?L"CHAT_ROOM_POPUP":control==InputPolicy::escapePassControl?L"ESC_PASS":control==InputPolicy::escapeDiscardControl?L"ESC_DISCARD":nullptr;
        if(key){const bool checked=SendDlgItemMessageW(page,control,BM_GETCHECK,0,0)==BST_CHECKED;
            WritePrivateProfileStringW(L"STATUS",key,checked?L"1":L"0",configuration.c_str());return 0;}
    }
    if(msg==WM_WINDOWPOSCHANGING&&lp){auto pos=reinterpret_cast<WINDOWPOS*>(lp);const int height=static_cast<int>(reinterpret_cast<INT_PTR>(GetPropW(page,minimumHeight)));if(!(pos->flags&SWP_NOSIZE)&&pos->cy<height)pos->cy=height;}
    if(msg==growSettingsMessage){
        HWND sheet=GetParent(page),tab=FindWindowExW(sheet,nullptr,WC_TABCONTROLW,nullptr);char cls[32]{};GetClassNameA(sheet,cls,32);
        if(tab&&!strcmp(cls,"#32770")&&!GetPropW(sheet,sheetExpanded)){
            SetPropW(sheet,sheetExpanded,reinterpret_cast<HANDLE>(1));
            const int extra=static_cast<int>(wp);const auto tabRect=rectangle(tab,sheet);
            for(HWND child=GetWindow(sheet,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
                const auto r=rectangle(child,sheet);
                if(child==tab)SetWindowPos(child,nullptr,0,0,r.right-r.left,r.bottom-r.top+extra,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
                else if(r.top>=tabRect.bottom)SetWindowPos(child,nullptr,r.left,r.top+extra,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
            }
            RECT r{};GetWindowRect(sheet,&r);SetWindowPos(sheet,nullptr,0,0,r.right-r.left,r.bottom-r.top+extra,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
        }
        return 0;
    }
    if(msg==WM_NCDESTROY){RemovePropW(page,minimumHeight);RemoveWindowSubclass(page,pageObserver,id);}
    return DefSubclassProc(page,msg,wp,lp);
}
LRESULT CALLBACK observeDialogs(int code,WPARAM wp,LPARAM lp){
    if(code>=0){auto message=reinterpret_cast<CWPRETSTRUCT*>(lp);if(message->message==WM_INITDIALOG)EnhanceSettingsPage(message->hwnd);}
    return CallNextHookEx(nullptr,code,wp,lp);
}
LRESULT CALLBACK rootObserver(HWND root,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR){
    if(msg==0x8204&&static_cast<UINT_PTR>(lp)>0xffff&&!strcmp(reinterpret_cast<const char*>(lp),"CHAT OPEN")){
        // The native room-switch path explicitly opens CHAT even when CHAT=0.
        // Keep the existing always-show setting, and gate that second path with
        // the new preference. No transport or room-selection messages change.
        if(!enabled(L"CHAT")&&!enabled(L"CHAT_ROOM_POPUP"))return 0;
    }
    if(msg==WM_NCDESTROY){RemoveWindowSubclass(root,rootObserver,id);InputPolicy::DetachThread();if(settingsHook){UnhookWindowsHookEx(settingsHook);settingsHook=nullptr;}}
    return DefSubclassProc(root,msg,wp,lp);
}
HWND WINAPI createA(DWORD ex,LPCSTR cls,LPCSTR title,DWORD style,int x,int y,int width,int height,HWND parent,HMENU menu,HINSTANCE module,LPVOID param){
    HWND result=previousCreate(ex,cls,title,style,x,y,width,height,parent,menu,module,param);
    if(result&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_TENHOU_WIN_APP_MAIN"))AttachRoot(result);
    if(result&&parent&&reinterpret_cast<UINT_PTR>(cls)>0xffff&&!strcmp(cls,"WC_VIEW")){
        char rootClass[80]{};GetClassNameA(parent,rootClass,80);
        if(!strcmp(rootClass,"WC_TENHOU_WIN_APP_MAIN"))AttachRoot(parent);
    }
    return result;
}
}
void AttachRoot(HWND root){
    SetWindowSubclass(root,rootObserver,subclassId,0);
    InputPolicy::AttachThread();
    if(!settingsHook)settingsHook=SetWindowsHookExW(WH_CALLWNDPROCRET,observeDialogs,nullptr,GetCurrentThreadId());
}
void EnhanceSettingsPage(HWND page){
    if(configuration.empty()||GetDlgItem(page,roomPopupControl))return;
    HWND anchor=GetDlgItem(page,1057),following=GetDlgItem(page,1059);
    if(!anchor||GetParent(anchor)!=page||!following||!GetDlgItem(page,1056)||!GetDlgItem(page,1064)||!GetDlgItem(page,1049))return;
    const RECT previous=rectangle(anchor,page),next=rectangle(following,page);const int gap=next.top-previous.top;
    if(gap<=0)return;
    std::vector<std::pair<HWND,RECT>> moved;
    const auto autoWin=rectangle(GetDlgItem(page,1063),page);
    for(HWND child=GetWindow(page,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
        const auto r=rectangle(child,page);if(r.top>=autoWin.top)moved.emplace_back(child,r);
    }
    const auto add=[&](int control,const wchar_t* text,int top,const wchar_t* key){
        HWND checkbox=CreateWindowExW(0,L"BUTTON",text,WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
            previous.left,top,previous.right-previous.left,previous.bottom-previous.top,page,reinterpret_cast<HMENU>(control),GetModuleHandleW(nullptr),nullptr);
        if(checkbox){SendMessageW(checkbox,WM_SETFONT,SendMessageW(anchor,WM_GETFONT,0,0),FALSE);SendMessageW(checkbox,BM_SETCHECK,enabled(key)?BST_CHECKED:BST_UNCHECKED,0);}return checkbox;
    };
    HWND chat=add(roomPopupControl,L"切换房间弹窗",next.top,L"CHAT_ROOM_POPUP");
    HWND pass=add(InputPolicy::escapePassControl,L"对局中ESC键跳过",next.top+5*gap,L"ESC_PASS");
    HWND discard=add(InputPolicy::escapeDiscardControl,L"对局中ESC键摸切",next.top+6*gap,L"ESC_DISCARD");
    if(!chat||!pass||!discard){if(chat)DestroyWindow(chat);if(pass)DestroyWindow(pass);if(discard)DestroyWindow(discard);return;}
    // Keep the original mouse control IDs and native handlers; only reorder
    // their geometry/tab order, then append the two independent Escape options.
    HWND after=chat;SetWindowPos(chat,anchor,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE);
    const int order[]={1061,1059,1062,1060,InputPolicy::escapePassControl,InputPolicy::escapeDiscardControl};
    for(int i=0;i<6;++i){HWND control=GetDlgItem(page,order[i]);SetWindowPos(control,after,previous.left,next.top+(i+1)*gap,0,0,SWP_NOSIZE|SWP_NOACTIVATE);after=control;}
    for(const auto& item:moved){const auto& r=item.second;SetWindowPos(item.first,after,r.left,r.top+3*gap,0,0,SWP_NOSIZE|SWP_NOACTIVATE);after=item.first;}
    const auto pageRect=rectangle(page,GetParent(page));const int pageHeight=pageRect.bottom-pageRect.top+2*gap;
    SetPropW(page,minimumHeight,reinterpret_cast<HANDLE>(static_cast<INT_PTR>(pageHeight)));
    SetWindowSubclass(page,pageObserver,subclassId,0);
    SetWindowPos(page,nullptr,0,0,pageRect.right-pageRect.left,pageHeight,SWP_NOMOVE|SWP_NOZORDER|SWP_NOACTIVATE);
    PostMessageW(page,growSettingsMessage,2*gap,0);
}
bool Install(HMODULE executable,const std::wstring& directory){
    configuration=directory+L"\\config.ini";
    return PeImports::Hook(executable,"CreateWindowExA",createA,previousCreate);
}
}
