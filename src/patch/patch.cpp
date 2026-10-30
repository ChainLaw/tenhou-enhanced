#include "pe_imports.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <propkey.h>
#include <commctrl.h>
#include <bcrypt.h>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <string>
#include "inject.h"
#include "counter.h"
#include "localization.h"
#include "web_lobby.h"
#include "web_transport.h"
#include "chat_policy.h"
#include "input_policy.h"
#pragma comment(linker,"/EXPORT:EnhancedInitialize=_EnhancedInitialize@4")
namespace {
HMODULE self;
std::wstring root, oldRoot, dllPath;
std::string rootA;
SRWLOCK logLock=SRWLOCK_INIT;
WallCounter counter;
std::atomic<int> remaining{-1};
std::atomic<bool> loadingRound{true};
void* lastTableObject=nullptr;
HWND lastTableWindow=nullptr;
int panelGeneration=0;
bool diagnostic=false;
bool writePointer(void* address,const void* data,size_t size);
using Consume=void(__thiscall*)(void*,HWND,void*);
Consume originalConsume=nullptr;
std::wstring widen(const char* s) {
    if(!s)return {};
    int n=MultiByteToWideChar(CP_ACP,0,s,-1,nullptr,0);
    std::wstring out(n,L'\0'); if(n)MultiByteToWideChar(CP_ACP,0,s,-1,out.data(),n);
    if(n)out.pop_back();return out;
}
std::string narrow(const std::wstring& s) {
    int n=WideCharToMultiByte(CP_ACP,0,s.c_str(),-1,nullptr,0,nullptr,nullptr);
    std::string out(n,'\0');if(n)WideCharToMultiByte(CP_ACP,0,s.c_str(),-1,out.data(),n,nullptr,nullptr);
    if(n)out.pop_back();return out;
}
void log(const char* line) {
    AcquireSRWLockExclusive(&logLock);
    HANDLE f=CreateFileW((root+L"\\enhanced.log").c_str(),FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f!=INVALID_HANDLE_VALUE){ char head[64];wsprintfA(head,"[%lu:%lu] ",GetCurrentProcessId(),GetTickCount());DWORD n;WriteFile(f,head,static_cast<DWORD>(strlen(head)),&n,nullptr);WriteFile(f,line,static_cast<DWORD>(strlen(line)),&n,nullptr);WriteFile(f,"\r\n",2,&n,nullptr);CloseHandle(f); }
    ReleaseSRWLockExclusive(&logLock);
}
bool prefix(const std::wstring& path,const std::wstring& base) {
    return path.size()>=base.size()&&!_wcsnicmp(path.c_str(),base.c_str(),base.size())&&(path.size()==base.size()||path[base.size()]==L'\\');
}
std::wstring mapPath(const wchar_t* p) {
    if(!p)return {};
    std::wstring s=p;std::replace(s.begin(),s.end(),L'/',L'\\');
    if(s.rfind(L"\\\\?\\",0)==0)s.erase(0,4);
    for(const auto& base:{oldRoot,root+L"\\C-EGG\\tenhou"}) {
        if(prefix(s,base)) {
            auto tail=s.substr(base.size());
            if(tail==L"\\130"||tail.rfind(L"\\130\\",0)==0)tail.erase(0,4);
            s=root+tail;break;
        }
    }
    if(!s.empty()&&s[0]!=L'\\'&&(s.size()<2||s[1]!=L':'))s=root+L"\\"+s;
    return s;
}
std::string pathA(const char* p) {return narrow(mapPath(widen(p).c_str()));}
// IAT changes affect only the original executable; calls from this DLL use
// unmodified system APIs, avoiding recursion and global OS hooks.
const PROPERTYKEY taskbarKeys[]={PKEY_AppUserModel_RelaunchCommand,PKEY_AppUserModel_RelaunchDisplayNameResource,
    PKEY_AppUserModel_RelaunchIconResource,PKEY_AppUserModel_ID};
constexpr UINT_PTR taskbarSubclassId=0x54424943;
LRESULT CALLBACK taskbarObserver(HWND window,UINT message,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    if(message==WM_NCDESTROY) {
        IPropertyStore* store=nullptr;
        if(SUCCEEDED(SHGetPropertyStoreForWindow(window,IID_PPV_ARGS(&store)))) {
            PROPVARIANT empty{};for(const auto& key:taskbarKeys)store->SetValue(key,empty);
            store->Release();
        }
        RemoveWindowSubclass(window,taskbarObserver,id);
    }
    return DefSubclassProc(window,message,wp,lp);
}
BOOL WINAPI hShowWindow(HWND window,int command) {
    char name[64]{};GetClassNameA(window,name,sizeof(name));
    DWORD_PTR attached=0;
    if((!strcmp(name,"WC_TENHOU_WIN_APP_MAIN")||!strcmp(name,"WC_TENHOU_WIN_APP_LAUNCHER"))&&
       !GetWindowSubclass(window,taskbarObserver,taskbarSubclassId,&attached)&&
       SetWindowSubclass(window,taskbarObserver,taskbarSubclassId,0)) {
        // The visible client is a child process of the enhanced launcher.
        // Give its taskbar button an explicit icon and safe relaunch target.
        const auto module=GetModuleHandleW(nullptr);
        auto large=LoadImageA(module,"ICON.ICO",IMAGE_ICON,GetSystemMetrics(SM_CXICON),GetSystemMetrics(SM_CYICON),LR_SHARED);
        auto smallIcon=LoadImageA(module,"ICON.ICO",IMAGE_ICON,GetSystemMetrics(SM_CXSMICON),GetSystemMetrics(SM_CYSMICON),LR_SHARED);
        if(large)SendMessageW(window,WM_SETICON,ICON_BIG,reinterpret_cast<LPARAM>(large));
        if(smallIcon)SendMessageW(window,WM_SETICON,ICON_SMALL,reinterpret_cast<LPARAM>(smallIcon));
        IPropertyStore* store=nullptr;
        HRESULT result=SHGetPropertyStoreForWindow(window,IID_PPV_ARGS(&store));
        if(SUCCEEDED(result)) {
            const auto executable=root+L"\\TenhouEnhanced.exe";
            const std::wstring values[]={L"\""+executable+L"\"",L"Tenhou Enhanced",executable+L",0",L"ChainLaw.TenhouEnhanced"};
            static_assert(sizeof(values)/sizeof(values[0])==sizeof(taskbarKeys)/sizeof(taskbarKeys[0]));
            for(size_t i=0;i<sizeof(taskbarKeys)/sizeof(taskbarKeys[0])&&SUCCEEDED(result);++i) {
                PROPVARIANT value{};value.vt=VT_LPWSTR;value.pwszVal=const_cast<wchar_t*>(values[i].c_str());
                result=store->SetValue(taskbarKeys[i],value);
            }
            store->Release();
        }
        if(FAILED(result))log("Taskbar identity: failed to set window properties.");
    }
    return ShowWindow(window,command);
}
HANDLE WINAPI hCreateFileA(LPCSTR p,DWORD a,DWORD sh,LPSECURITY_ATTRIBUTES sa,DWORD d,DWORD fl,HANDLE t){return CreateFileW(mapPath(widen(p).c_str()).c_str(),a,sh,sa,d,fl,t);}
HANDLE WINAPI hCreateFileW(LPCWSTR p,DWORD a,DWORD sh,LPSECURITY_ATTRIBUTES sa,DWORD d,DWORD fl,HANDLE t){return CreateFileW(mapPath(p).c_str(),a,sh,sa,d,fl,t);}
BOOL WINAPI hCreateDirectoryA(LPCSTR p,LPSECURITY_ATTRIBUTES a){return CreateDirectoryW(mapPath(widen(p).c_str()).c_str(),a);}
BOOL WINAPI hCreateDirectoryW(LPCWSTR p,LPSECURITY_ATTRIBUTES a){return CreateDirectoryW(mapPath(p).c_str(),a);}
BOOL WINAPI hDeleteFileA(LPCSTR p){return DeleteFileW(mapPath(widen(p).c_str()).c_str());}
BOOL WINAPI hDeleteFileW(LPCWSTR p){return DeleteFileW(mapPath(p).c_str());}
BOOL WINAPI hRemoveDirectoryA(LPCSTR p){return RemoveDirectoryW(mapPath(widen(p).c_str()).c_str());}
BOOL WINAPI hRemoveDirectoryW(LPCWSTR p){return RemoveDirectoryW(mapPath(p).c_str());}
DWORD WINAPI hGetFileAttributesA(LPCSTR p){return GetFileAttributesW(mapPath(widen(p).c_str()).c_str());}
DWORD WINAPI hGetFileAttributesW(LPCWSTR p){return GetFileAttributesW(mapPath(p).c_str());}
BOOL WINAPI hGetFileAttributesExA(LPCSTR p,GET_FILEEX_INFO_LEVELS l,LPVOID o){return GetFileAttributesExW(mapPath(widen(p).c_str()).c_str(),l,o);}
BOOL WINAPI hGetFileAttributesExW(LPCWSTR p,GET_FILEEX_INFO_LEVELS l,LPVOID o){return GetFileAttributesExW(mapPath(p).c_str(),l,o);}
BOOL WINAPI hCopyFileA(LPCSTR a,LPCSTR b,BOOL f){return CopyFileW(mapPath(widen(a).c_str()).c_str(),mapPath(widen(b).c_str()).c_str(),f);}
BOOL WINAPI hCopyFileW(LPCWSTR a,LPCWSTR b,BOOL f){return CopyFileW(mapPath(a).c_str(),mapPath(b).c_str(),f);}
BOOL WINAPI hMoveFileA(LPCSTR a,LPCSTR b){return MoveFileW(mapPath(widen(a).c_str()).c_str(),mapPath(widen(b).c_str()).c_str());}
BOOL WINAPI hMoveFileW(LPCWSTR a,LPCWSTR b){return MoveFileW(mapPath(a).c_str(),mapPath(b).c_str());}
BOOL WINAPI hMoveFileExA(LPCSTR a,LPCSTR b,DWORD f){return MoveFileExW(mapPath(widen(a).c_str()).c_str(),b?mapPath(widen(b).c_str()).c_str():nullptr,f);}
BOOL WINAPI hMoveFileExW(LPCWSTR a,LPCWSTR b,DWORD f){return MoveFileExW(mapPath(a).c_str(),b?mapPath(b).c_str():nullptr,f);}
BOOL WINAPI hSetCurrentDirectoryA(LPCSTR){return SetCurrentDirectoryW(root.c_str());}
BOOL WINAPI hSetCurrentDirectoryW(LPCWSTR){return SetCurrentDirectoryW(root.c_str());}
DWORD WINAPI hGetTempPathW(DWORD n,LPWSTR out){auto s=root+L"\\cache\\";if(n>s.size()){wcscpy_s(out,n,s.c_str());return static_cast<DWORD>(s.size());}return static_cast<DWORD>(s.size()+1);}
DWORD WINAPI hGetTempPathA(DWORD n,LPSTR out){auto s=rootA+"\\cache\\";if(n>s.size()){strcpy_s(out,n,s.c_str());return static_cast<DWORD>(s.size());}return static_cast<DWORD>(s.size()+1);}
UINT WINAPI hGetTempFileNameA(LPCSTR,LPCSTR prefix,UINT u,LPSTR out){return GetTempFileNameA((rootA+"\\cache").c_str(),prefix,u,out);}
UINT WINAPI hGetTempFileNameW(LPCWSTR,LPCWSTR prefix,UINT u,LPWSTR out){return GetTempFileNameW((root+L"\\cache").c_str(),prefix,u,out);}
DWORD WINAPI hGetPrivateProfileStringA(LPCSTR a,LPCSTR b,LPCSTR c,LPSTR d,DWORD n,LPCSTR p){auto s=pathA(p);return GetPrivateProfileStringA(a,b,c,d,n,p?s.c_str():p);}
DWORD WINAPI hGetPrivateProfileStringW(LPCWSTR a,LPCWSTR b,LPCWSTR c,LPWSTR d,DWORD n,LPCWSTR p){auto s=mapPath(p);return GetPrivateProfileStringW(a,b,c,d,n,p?s.c_str():p);}
UINT WINAPI hGetPrivateProfileIntA(LPCSTR a,LPCSTR b,INT d,LPCSTR p){auto s=pathA(p);return GetPrivateProfileIntA(a,b,d,p?s.c_str():p);}
UINT WINAPI hGetPrivateProfileIntW(LPCWSTR a,LPCWSTR b,INT d,LPCWSTR p){auto s=mapPath(p);return GetPrivateProfileIntW(a,b,d,p?s.c_str():p);}
DWORD WINAPI hGetPrivateProfileSectionA(LPCSTR a,LPSTR b,DWORD n,LPCSTR p){auto s=pathA(p);return GetPrivateProfileSectionA(a,b,n,p?s.c_str():p);}
DWORD WINAPI hGetPrivateProfileSectionW(LPCWSTR a,LPWSTR b,DWORD n,LPCWSTR p){auto s=mapPath(p);return GetPrivateProfileSectionW(a,b,n,p?s.c_str():p);}
BOOL WINAPI hWritePrivateProfileStringA(LPCSTR a,LPCSTR b,LPCSTR c,LPCSTR p){auto s=pathA(p);if(a&&b&&!_stricmp(a,"LOG")&&!_stricmp(b,"PATH"))c="./mjlog";return WritePrivateProfileStringA(a,b,c,p?s.c_str():p);}
BOOL WINAPI hWritePrivateProfileStringW(LPCWSTR a,LPCWSTR b,LPCWSTR c,LPCWSTR p){auto s=mapPath(p);if(a&&b&&!_wcsicmp(a,L"LOG")&&!_wcsicmp(b,L"PATH"))c=L"./mjlog";return WritePrivateProfileStringW(a,b,c,p?s.c_str():p);}
BOOL WINAPI hWritePrivateProfileSectionA(LPCSTR a,LPCSTR b,LPCSTR p){auto s=pathA(p);return WritePrivateProfileSectionA(a,b,p?s.c_str():p);}
BOOL WINAPI hWritePrivateProfileSectionW(LPCWSTR a,LPCWSTR b,LPCWSTR p){auto s=mapPath(p);return WritePrivateProfileSectionW(a,b,p?s.c_str():p);}
// Keep enumeration's ANSI structure conversion in the OS implementation.
HANDLE WINAPI hFindFirstFileA(LPCSTR p,LPWIN32_FIND_DATAA d){auto s=pathA(p);return FindFirstFileA(s.c_str(),d);}
HANDLE WINAPI hFindFirstFileW(LPCWSTR p,LPWIN32_FIND_DATAW d){return FindFirstFileW(mapPath(p).c_str(),d);}
DWORD WINAPI hGetFullPathNameA(LPCSTR p,DWORD n,LPSTR out,LPSTR* part){auto s=pathA(p);return GetFullPathNameA(s.c_str(),n,out,part);}
DWORD WINAPI hGetFullPathNameW(LPCWSTR p,DWORD n,LPWSTR out,LPWSTR* part){return GetFullPathNameW(mapPath(p).c_str(),n,out,part);}
bool localChild(const wchar_t* app,const wchar_t* cmd) {
    std::wstring p;
    if(app&&*app)p=app;
    else {int n=0;auto args=CommandLineToArgvW(cmd?cmd:L"",&n);if(args&&n)p=args[0];if(args)LocalFree(args);}
    p=mapPath(p.c_str());auto at=p.find_last_of(L"\\");auto name=p.substr(at==p.npos?0:at+1);
    return prefix(p,root)&&(!_wcsicmp(name.c_str(),L"tenhou.exe")||!_wcsicmp(name.c_str(),L"launcher.exe")||!_wcsicmp(name.c_str(),L"logana.exe"));
}
bool finishChild(BOOL ok,PROCESS_INFORMATION* pi,DWORD flags) {
    if(!ok)return false;
    if(!InjectPatch(pi->hProcess,pi->dwProcessId,dllPath)) {log("Child injection failed; terminated owned child before entry.");TerminateProcess(pi->hProcess,5);CloseHandle(pi->hThread);CloseHandle(pi->hProcess);*pi={};SetLastError(ERROR_DLL_INIT_FAILED);return false;}
    if(!(flags&CREATE_SUSPENDED))ResumeThread(pi->hThread);
    log("Child patch initialized before resume.");return true;
}
BOOL WINAPI hCreateProcessA(LPCSTR app,LPSTR cmd,LPSECURITY_ATTRIBUTES pa,LPSECURITY_ATTRIBUTES ta,BOOL inherit,DWORD flags,LPVOID env,LPCSTR cwd,LPSTARTUPINFOA si,LPPROCESS_INFORMATION pi) {
    auto wa=widen(app),wc=widen(cmd);
    if(!localChild(app?wa.c_str():nullptr,wc.c_str()))return CreateProcessA(app,cmd,pa,ta,inherit,flags,env,cwd,si,pi);
    auto mappedApp=pathA(app);
    std::string mappedCmd=cmd?cmd:"";
    auto old=narrow(oldRoot);for(auto pos=mappedCmd.find(old);pos!=mappedCmd.npos;pos=mappedCmd.find(old)) {
        auto length=old.size();if(mappedCmd.compare(pos+length,4,"\\130")==0)length+=4;
        mappedCmd.replace(pos,length,rootA);
    }
    return finishChild(CreateProcessA(app?mappedApp.c_str():nullptr,cmd?mappedCmd.data():nullptr,pa,ta,inherit,flags|CREATE_SUSPENDED,env,rootA.c_str(),si,pi),pi,flags);
}
BOOL WINAPI hCreateProcessW(LPCWSTR app,LPWSTR cmd,LPSECURITY_ATTRIBUTES pa,LPSECURITY_ATTRIBUTES ta,BOOL inherit,DWORD flags,LPVOID env,LPCWSTR cwd,LPSTARTUPINFOW si,LPPROCESS_INFORMATION pi) {
    if(!localChild(app,cmd))return CreateProcessW(app,cmd,pa,ta,inherit,flags,env,cwd,si,pi);
    auto ma=mapPath(app);std::wstring mc=cmd?cmd:L"";
    for(auto pos=mc.find(oldRoot);pos!=mc.npos;pos=mc.find(oldRoot)){auto len=oldRoot.size();if(mc.compare(pos+len,4,L"\\130")==0)len+=4;mc.replace(pos,len,root);}
    return finishChild(CreateProcessW(app?ma.c_str():nullptr,cmd?mc.data():nullptr,pa,ta,inherit,flags|CREATE_SUSPENDED,env,root.c_str(),si,pi),pi,flags);
}
// Add the count to the SAME vector path as the native honba number. The
// client applies its own brush, outline, transform and antialiasing afterwards.
struct RectF {float X,Y,Width,Height;};
extern "C" int WINAPI GdipMeasureString(void*,const WCHAR*,int,void*,const RectF*,void*,RectF*,int*,int*);
extern "C" int WINAPI GdipGetFontSize(void*,float*);
using AddPath= int(__thiscall*)(void*,const WCHAR*,int,void*,int,float,const RectF*,void*);
AddPath originalAddPath=nullptr;
using RefreshPanel=void*(__thiscall*)(void*,void*,HWND,void*,int);
RefreshPanel refreshPanel=nullptr;
using ReceiveEvent=void(__thiscall*)(void*,void*);
ReceiveEvent originalReceiveEvent=nullptr;
void __fastcall receiveEvent(void* object,void*,void* nativeString) {
    auto data=*reinterpret_cast<const char**>(static_cast<BYTE*>(nativeString)+4);
    auto length=*reinterpret_cast<unsigned*>(static_cast<BYTE*>(nativeString)+8);
    if(data&&length>=3&&length<=1024*1024&&BeginsRoundLoading(std::string_view(data,length))) {
        bool wasLoading=loadingRound.exchange(true);
        if(!wasLoading&&object==lastTableObject&&IsWindow(lastTableWindow)&&refreshPanel) {
            // Remove already-rasterized digits BEFORE the native ingress
            // changes the roster or captures a new-game animation frame.
            auto offset=*reinterpret_cast<int*>(*reinterpret_cast<BYTE**>(object)+4);
            auto state=static_cast<BYTE*>(object)+offset;
            BYTE rect[8]{};
            refreshPanel(state+0x204,rect,lastTableWindow,state+0x70,-1);
            InvalidateRect(lastTableWindow,nullptr,FALSE);
            if(diagnostic)log("Round ingress: stale panel count erased before roster update.");
        }
    }
    originalReceiveEvent(object,nativeString);
}
using NativeDrawIcon=void(__cdecl*)(void*,int,int,int,int,int);
NativeDrawIcon originalDrawIcon=nullptr;
struct CounterRow {float extra=0;WCHAR digits[12]{};};
thread_local CounterRow row;
// The native layout centers [7 + honba width + gap + 7 + deposits width].
// Extend its first measured width by [remaining width + the SAME gap]. This
// centers all three items and naturally moves the second icon and its number.
int WINAPI measureHonba(void* graphics,const WCHAR* text,int length,void* font,
                        const RectF* layout,void* format,RectF* bounds,int* fitted,int* lines) {
    row={};
    int result=GdipMeasureString(graphics,text,length,font,layout,format,bounds,fitted,lines);
    int value=loadingRound.load()?-1:remaining.load();
    if(result||!bounds||value<0)return result;
    swprintf_s(row.digits,L"%d",value);
    RectF countBounds{};float size=0;
    if(!GdipMeasureString(graphics,row.digits,-1,font,layout,format,&countBounds,nullptr,nullptr)&&
       !GdipGetFontSize(font,&size)) {
        // Supported binary: font size = 12 * scale; native gap = 4 * scale.
        row.extra=countBounds.Width+size/3.0f;
        bounds->Width+=row.extra;
    }
    return result;
}
void __cdecl drawHonbaIcon(void* graphics,int id,int x,int y,int center,int mode) {
    originalDrawIcon(graphics,id,static_cast<int>(std::lround(x+row.extra)),y,center,mode);
}
int __fastcall addHonbaPath(void* path,void*,const WCHAR* text,int length,
                           void* family,int style,float size,const RectF* layout,void* format) {
    ++panelGeneration;
    if(!layout||row.extra<=0)return originalAddPath(path,text,length,family,style,size,layout,format);
    RectF honba=*layout;honba.X+=row.extra;
    int result=originalAddPath(path,text,length,family,style,size,&honba,format);
    // The original number starts seven units after its icon. The newly
    // reserved left-hand slot begins at that icon's unshifted position.
    RectF count=*layout;count.X-=7.0f;
    originalAddPath(path,row.digits,-1,family,style,size,&count,format);
    if(diagnostic){char msg[200];sprintf_s(msg,"centered row: remaining=%ls x=%.2f honba_x=%.2f size=%.2f gap=%.2f extra=%.2f",row.digits,count.X,honba.X,size,size/3.0f,row.extra);log(msg);}
    return result;
}
// Signature-locked call-site hook: thiscall -> fastcall shim (ECX=this,
// EDX unused, then HWND and native string). Original bytes are E8 15 41 00 00.
void __fastcall consume(void* object,void*,HWND window,void* nativeString) {
    lastTableObject=object;lastTableWindow=window;
    // Bound reads; the object and string are owned by the original UI thread.
    auto data=*reinterpret_cast<const char**>(static_cast<BYTE*>(nativeString)+4);
    auto len=*reinterpret_cast<unsigned*>(static_cast<BYTE*>(nativeString)+8);
    if(!data||len<3||len>1024*1024){originalConsume(object,window,nativeString);return;}
    // Own the bytes before the original consumer can mutate its string.
    std::string xml(data,len);data=xml.data();
    bool init=IsRoundInitialization(xml);
    bool snapshot=init&&xml[1]=='R';
    auto vbtable=*reinterpret_cast<BYTE**>(object);
    auto offset=*reinterpret_cast<int*>(vbtable+4);
    unsigned type=*reinterpret_cast<unsigned*>(static_cast<BYTE*>(object)+offset+0x100);
    int previous=remaining.load();
    counter.set_type(type);
    // INIT may paint while establishing the new mode. Never expose the
    // previous round's count during that original-handler transition.
    if(init)counter.clear();else counter.event(xml);
    remaining.store(counter.remaining());
    int generation=panelGeneration;
    originalConsume(object,window,nativeString);
    if(init) {
        // CPU INIT changes the mode inside the original handler. Reading it
        // before that handler incorrectly initializes a new sanma game at 70.
        type=*reinterpret_cast<unsigned*>(static_cast<BYTE*>(object)+offset+0x100);
        counter.set_type(type);
        if(snapshot) {
            auto state=static_cast<const char*>(object)+offset;
            counter.restore(RemainingFromNativeWall(std::string_view(state,0x1de)));
        } else counter.event(xml);
        remaining.store(counter.remaining());
        loadingRound.store(false);
    }
    // Some events update other table objects without rebuilding the center.
    // Rebuild the existing native panel on that same UI thread when needed.
    // Never touch the table after BYE/PROF can have torn it down.
    if((init||(remaining.load()>=0&&previous!=remaining.load()&&generation==panelGeneration))&&refreshPanel) {
        BYTE rect[8]{};
        auto state=static_cast<BYTE*>(object)+offset;
        refreshPanel(state+0x204,rect,window,state+0x70,-1);
        InvalidateRect(window,nullptr,FALSE);
    }
    diagnostic=GetPrivateProfileIntW(L"diagnostics",L"events",0,(root+L"\\enhanced.ini").c_str())!=0;
    if(diagnostic) {
        // Log only tag NAME and count, never XML attributes, IDs or tile values.
        unsigned end=1;while(end<len&&end<17&&data[end]>='A'&&data[end]<='Z')++end;
        char msg[100];sprintf_s(msg,"event=%.*s remaining=%d type=%u",static_cast<int>(end-1),data+1,counter.remaining(),type);log(msg);
    }
}
bool knownClient() {
    wchar_t exe[MAX_PATH]{};GetModuleFileNameW(nullptr,exe,MAX_PATH);
    HANDLE f=CreateFileW(exe,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(f==INVALID_HANDLE_VALUE)return false;
    BCRYPT_ALG_HANDLE alg{};BCRYPT_HASH_HANDLE hash{};BYTE result[32]{};bool ok=false;
    if(BCryptOpenAlgorithmProvider(&alg,BCRYPT_SHA256_ALGORITHM,nullptr,0)>=0&&BCryptCreateHash(alg,&hash,nullptr,0,nullptr,0,0)>=0) {
        BYTE buf[16384];DWORD n;bool success=true;
        for(;;){if(!ReadFile(f,buf,sizeof(buf),&n,nullptr)){success=false;break;}if(!n)break;if(BCryptHashData(hash,buf,n,0)<0){success=false;break;}}
        if(success&&BCryptFinishHash(hash,result,sizeof(result),0)>=0) {
            const BYTE expected[]={0x96,0x16,0x4c,0xdb,0x63,0xe0,0x43,0x54,0x1c,0x27,0x57,0x2a,0x22,0xb8,0x95,0x07,0x2a,0x8f,0x9a,0x0a,0x0e,0xe9,0x7d,0x87,0x7a,0x3c,0xb3,0x40,0xe3,0x27,0xff,0xe6};
            ok=!memcmp(result,expected,32);
        }
    }
    if(hash)BCryptDestroyHash(hash);if(alg)BCryptCloseAlgorithmProvider(alg,0);CloseHandle(f);return ok;
}
bool writePointer(void* address,const void* data,size_t size) {
    DWORD old;if(!VirtualProtect(address,size,PAGE_EXECUTE_READWRITE,&old))return false;
    memcpy(address,data,size);DWORD ignored;VirtualProtect(address,size,old,&ignored);FlushInstructionCache(GetCurrentProcess(),address,size);return true;
}
struct Hook {const char* name;void* fn;};
#define H(name) {#name,reinterpret_cast<void*>(h##name)}
Hook hooks[]={
    H(ShowWindow),
    H(CreateFileA),H(CreateFileW),H(CreateDirectoryA),H(CreateDirectoryW),H(DeleteFileA),H(DeleteFileW),H(RemoveDirectoryA),H(RemoveDirectoryW),
    H(GetFileAttributesA),H(GetFileAttributesW),H(GetFileAttributesExA),H(GetFileAttributesExW),H(CopyFileA),H(CopyFileW),H(MoveFileA),H(MoveFileW),H(MoveFileExA),H(MoveFileExW),
    H(SetCurrentDirectoryA),H(SetCurrentDirectoryW),H(GetTempPathA),H(GetTempPathW),H(GetTempFileNameA),H(GetTempFileNameW),
    H(GetPrivateProfileStringA),H(GetPrivateProfileStringW),H(GetPrivateProfileIntA),H(GetPrivateProfileIntW),H(GetPrivateProfileSectionA),H(GetPrivateProfileSectionW),
    H(WritePrivateProfileStringA),H(WritePrivateProfileStringW),H(WritePrivateProfileSectionA),H(WritePrivateProfileSectionW),H(FindFirstFileA),H(FindFirstFileW),H(GetFullPathNameA),H(GetFullPathNameW),
    H(CreateProcessA),H(CreateProcessW)
};
#undef H
bool installIat(BYTE* base) {
    unsigned installed=0;
    const bool ok=PeImports::Visit(reinterpret_cast<HMODULE>(base),[&](const PeImports::Import& entry){
        if(!entry.name)return true;
        for(const auto& hook:hooks)if(!strcmp(entry.name,hook.name)){
            if(!PeImports::Replace(entry.slot,hook.fn))return false;
            ++installed;break;
        }
        return true;
    });
    if(!ok)return false;
    char msg[100];sprintf_s(msg,"IAT hooks installed: %u",installed);log(msg);return installed>0;
}
}
extern "C" DWORD WINAPI EnhancedInitialize(void*) {
    static LONG started=0;if(InterlockedCompareExchange(&started,1,0))return 1;
    wchar_t path[MAX_PATH]{};GetModuleFileNameW(self,path,MAX_PATH);dllPath=path;
    root=dllPath.substr(0,dllPath.find_last_of(L"\\/"));rootA=narrow(root);
    if(widen(rootA.c_str())!=root)return 0; // Original client is ANSI; never silently mangle its root.
    wchar_t local[MAX_PATH]{};if(!SHGetSpecialFolderPathW(nullptr,local,CSIDL_LOCAL_APPDATA,FALSE))return 0;
    oldRoot=std::wstring(local)+L"\\C-EGG\\tenhou";
    SetCurrentDirectoryW(root.c_str());CreateDirectoryW((root+L"\\cache").c_str(),nullptr);CreateDirectoryW((root+L"\\mjlog").c_str(),nullptr);
    SetEnvironmentVariableW(L"TEMP",(root+L"\\cache").c_str());SetEnvironmentVariableW(L"TMP",(root+L"\\cache").c_str());
    // Native auto-save otherwise defaults to Documents\\My Tenhou. Keep the
    // value relative so relocating the complete folder remains supported.
    WritePrivateProfileStringW(L"LOG",L"PATH",L"./mjlog",(root+L"\\config.ini").c_str());
    auto ini=root+L"\\enhanced.ini";
    diagnostic=GetPrivateProfileIntW(L"diagnostics",L"events",0,ini.c_str())!=0;
    log("Enhanced initialization started.");
    auto base=reinterpret_cast<BYTE*>(GetModuleHandleW(nullptr));
    if(!installIat(base))return 0;
    if(GetPrivateProfileIntW(L"localization",L"enabled",1,ini.c_str())) {
        auto count=Localization::Install(GetModuleHandleW(nullptr));
        char message[100];sprintf_s(message,"Chinese localization display hooks installed: %u",count);log(message);
    }
    GetModuleFileNameW(nullptr,path,MAX_PATH);const wchar_t* name=wcsrchr(path,L'\\');name=name?name+1:path;
    if(!_wcsicmp(name,L"tenhou.exe")) {
        if(knownClient()) {
            // Native bottom-right audio menu: zero means enabled. These are
            // session defaults; its existing SE/BGM toggles remain unchanged.
            base[0x1034c0]=0; // SE on
            base[0x103378]=1; // BGM off
            InputPolicy::Install(GetModuleHandleW(nullptr),root);
            ChatPolicy::Install(GetModuleHandleW(nullptr),root);
            if(WebLobby::Install(GetModuleHandleW(nullptr),root,true)&&
               !WebTransport::Install(GetModuleHandleW(nullptr),root))return 0;
            // The earlier ingress updates names before queued XML consumption.
            // Validate every caller before installing any of these hooks.
            const unsigned receiveSites[]={0x7b15a,0x7c6bf,0x7fbd9,0x9bd3f};
            for(auto rva:receiveSites) {
                auto call=base+rva;int relative=0;memcpy(&relative,call+1,4);
                if(call[0]!=0xe8||call+5+relative!=base+0x89c40)return 0;
            }
            BYTE expected[]={0xe8,0x15,0x41,0x00,0x00};auto site=base+0x8dec6;
            if(memcmp(site,expected,5))return 0;
            BYTE panelExpected[]={0xe8,0xe5,0x0e,0x00,0x00};auto panelSite=base+0x83ea6;
            if(memcmp(panelSite,panelExpected,5))return 0;
            BYTE measureExpected[]={0xe8,0x2e,0xa2,0x05,0x00};auto measureSite=base+0x83d19;
            BYTE iconExpected[]={0xe8,0x39,0x53,0xfe,0xff};auto iconSite=base+0x83e02;
            if(memcmp(measureSite,measureExpected,5)||memcmp(iconSite,iconExpected,5))return 0;
            originalDrawIcon=reinterpret_cast<NativeDrawIcon>(base+0x69140);
            BYTE measurePatch[5]={0xe8};auto measureOffset=reinterpret_cast<BYTE*>(&measureHonba)-(measureSite+5);
            memcpy(measurePatch+1,&measureOffset,4);
            BYTE iconPatch[5]={0xe8};auto iconOffset=reinterpret_cast<BYTE*>(&drawHonbaIcon)-(iconSite+5);
            memcpy(iconPatch+1,&iconOffset,4);
            if(!writePointer(measureSite,measurePatch,5)||!writePointer(iconSite,iconPatch,5))return 0;
            originalAddPath=reinterpret_cast<AddPath>(base+0x84d90);
            refreshPanel=reinterpret_cast<RefreshPanel>(base+0x82f30);
            BYTE panelPatch[5]={0xe8};auto panelOffset=reinterpret_cast<BYTE*>(&addHonbaPath)-(panelSite+5);
            memcpy(panelPatch+1,&panelOffset,4);
            if(!writePointer(panelSite,panelPatch,5))return 0;
            originalConsume=reinterpret_cast<Consume>(base+0x91fe0);
            BYTE patch[5]={0xe8};auto offset=reinterpret_cast<BYTE*>(&consume)-(site+5);memcpy(patch+1,&offset,4);
            if(!writePointer(site,patch,5))return 0;
            originalReceiveEvent=reinterpret_cast<ReceiveEvent>(base+0x89c40);
            for(auto rva:receiveSites) {
                auto call=base+rva;BYTE incomingPatch[5]={0xe8};
                auto relative=reinterpret_cast<BYTE*>(&receiveEvent)-(call+5);
                memcpy(incomingPatch+1,&relative,4);
                if(!writePointer(call,incomingPatch,5))return 0;
            }
            log("Known client: native XML event and center-panel glyph hooks installed.");
        } else log("UNSUPPORTED CLIENT SHA256: enhanced hooks active; counter deliberately disabled.");
    }
    log("Enhanced initialization complete.");return 1;
}
BOOL WINAPI DllMain(HINSTANCE module,DWORD reason,LPVOID){if(reason==DLL_PROCESS_ATTACH){self=module;DisableThreadLibraryCalls(module);}return TRUE;}
