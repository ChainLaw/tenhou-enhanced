#include "pe_imports.h"
#include "localization.h"
#include "web_lobby.h"
#include <commctrl.h>
#include <commdlg.h>
#include <oleauto.h>
#include <algorithm>
#include <cstring>
#include <regex>
#include <unordered_map>
#include <vector>
namespace Localization {
namespace {
struct TranslationEntry {const wchar_t* source;const wchar_t* target;};
#include "localization_data.inc"
struct Pattern {std::wregex expression;std::vector<std::wstring> targetParts;};
const std::wregex spec(L"%(?:[-+ #0]*[0-9]*(?:\\.[0-9]+)?(?:hh|ll|[hlLwI])?[diuoxXfFeEgGaAcCsSpn]|%)");
std::wstring escapeRegex(std::wstring_view s) {
    std::wstring out;for(auto c:s){if(wcschr(L"\\.^$|()[]{}*+?",c))out+=L'\\';out+=c;}return out;
}
struct Dictionary {
    std::unordered_map<std::wstring,std::wstring> exact;
    std::vector<Pattern> patterns;
    Dictionary() {
        for(auto& entry:kTranslations) {
            std::wstring key=entry.source;exact.emplace(key,entry.target);
            std::wsregex_iterator it(key.begin(),key.end(),spec),end;
            if(it==end)continue;
            std::wstring rx=L"^";size_t at=0;
            for(;it!=end;++it) {
                rx+=escapeRegex(std::wstring_view(key).substr(at,it->position()-at));
                auto token=it->str();wchar_t kind=token.back();
                if(token==L"%%")rx+=L"%";
                else if(wcschr(L"diu",kind))rx+=L"([+-]?[0-9]+)";
                else if(wcschr(L"xXop",kind))rx+=L"([0-9a-fA-FxX]+)";
                else rx+=L"([\\s\\S]*?)";
                at=it->position()+it->length();
            }
            rx+=escapeRegex(std::wstring_view(key).substr(at))+L"$";
            Pattern p{std::wregex(rx),{}};std::wstring target=entry.target;at=0;
            for(std::wsregex_iterator jt(target.begin(),target.end(),spec);jt!=end;++jt) {
                auto literal=target.substr(at,jt->position()-at);
                if(jt->str()==L"%%") { /* No current numeric templates use escaped percent. */ }
                p.targetParts.push_back(literal);at=jt->position()+jt->length();
            }
            p.targetParts.push_back(target.substr(at));patterns.push_back(std::move(p));
        }
    }
};
Dictionary& dictionary(){static Dictionary d;return d;}
std::wstring convert(UINT cp,const char* s,int n) {
    if(!s)return {};if(n<0)n=static_cast<int>(strlen(s));if(!n)return {};
    int count=MultiByteToWideChar(cp,0,s,n,nullptr,0);if(!count)return {};
    std::wstring out(count,L'\0');MultiByteToWideChar(cp,0,s,n,out.data(),count);return out;
}
std::wstring wide(const wchar_t* s,int n=-1){return s?std::wstring(s,n<0?wcslen(s):static_cast<size_t>(n)):std::wstring();}
bool isString(const void* p){return reinterpret_cast<UINT_PTR>(p)>0xffff;}
}
std::wstring Translate(std::wstring_view input,bool* recognized) {
    if(recognized)*recognized=false;
    std::wstring text(input);auto& d=dictionary();
    auto exact=d.exact.find(text);
    if(exact!=d.exact.end()){if(recognized)*recognized=true;return exact->second;}
    if(std::all_of(text.begin(),text.end(),[](wchar_t c){return c<128;}))return text;
    // Preserve native padding/newlines around short UI labels.
    auto first=text.find_first_not_of(L" \t\r\n"),last=text.find_last_not_of(L" \t\r\n");
    if(first!=text.npos&&(first!=0||last+1!=text.size())) {
        auto trimmed=d.exact.find(text.substr(first,last-first+1));
        if(trimmed!=d.exact.end()){if(recognized)*recognized=true;return text.substr(0,first)+trimmed->second+text.substr(last+1);}
    }
    // Formatted values are copied verbatim, never interpreted as format code.
    if(text.size()<=8192)for(auto& p:d.patterns) {
        std::wsmatch match;
        if(std::regex_match(text,match,p.expression)&&match.size()==p.targetParts.size()) {
            std::wstring out=p.targetParts[0];
            for(size_t i=1;i<match.size();++i)out+=match[i].str()+p.targetParts[i];
            if(recognized)*recognized=true;return out;
        }
    }
    // Composite messages are assembled from complete known fragments. Do not
    // replace short words inside names, paths, or arbitrary unknown text.
    bool isStep=text.size()>=6&&text.compare(0,2,L" [")==0&&text.back()==L']';
    bool hasKyu=text.find(L'級')!=text.npos||text.find(L"本場")!=text.npos;
    if((text.size()>8||isStep||hasKyu)&&text.size()<=65536) {
        std::wstring out;size_t at=0;bool changed=false;size_t minLen=isStep?2:4;
        while(at<text.size()) {
            bool found=false;
            for(auto& e:kTranslations) {
                size_t n=wcslen(e.source);
                bool isKyu=(n==2&&e.source[0]>=L'１'&&e.source[0]<=L'９'&&e.source[1]==L'級');
                if((n<minLen&&!isKyu)||wcschr(e.source,L'%')||!wcscmp(e.source,e.target))continue;
                if(text.compare(at,n,e.source)==0){out+=e.target;at+=n;changed=found=true;break;}
            }
            if(!found)out+=text[at++];
        }
        if(changed){if(recognized)*recognized=true;return out;}
    }
    return text;
}
std::wstring DecodeDisplay(const char* s,int n) {
    if(!s)return {};
    auto japanese=convert(932,s,n);bool known=false;auto translated=Translate(japanese,&known);
    if(known)return translated;
    // Unknown/user-originated strings retain the application's current ACP.
    return Translate(convert(CP_ACP,s,n));
}
namespace {
int WINAPI mbToWide(UINT cp,DWORD flags,LPCCH s,int cb,LPWSTR out,int capacity) {
    if(s&&(cp==CP_ACP||cp==CP_THREAD_ACP||cp==932)&&cb!=0) {
        auto japanese=convert(932,s,cb);bool known=false;Translate(japanese,&known);
        if(known) {
            // Repair decoding only here, not translation: converted strings
            // can also be used as native configuration keys or comparison data.
            int required=static_cast<int>(japanese.size())+(cb<0?1:0);
            if(!out||!capacity)return required;
            if(capacity<required){SetLastError(ERROR_INSUFFICIENT_BUFFER);return 0;}
            memcpy(out,japanese.data(),japanese.size()*sizeof(wchar_t));if(cb<0)out[japanese.size()]=0;return required;
        }
    }
    return MultiByteToWideChar(cp,flags,s,cb,out,capacity);
}
int displayMessage(HWND w,const std::wstring& text,const std::wstring& title,UINT flags){
    if((flags&MB_TYPEMASK)==MB_OK&&text.rfind(L"剩余会员有效期不满足参加条件",0)==0&&WebLobby::ShowNotice(w,WebLobby::Notice::MembershipExpired))return IDOK;
    if((flags&MB_TYPEMASK)==MB_OK&&text.rfind(L"不满足参加条件",0)==0&&WebLobby::ShowNotice(w,WebLobby::Notice::EligibilityRejected))return IDOK;
    return MessageBoxW(w,text.c_str(),title.c_str(),flags);
}
int WINAPI messageA(HWND w,LPCSTR text,LPCSTR title,UINT flags){return displayMessage(w,DecodeDisplay(text),DecodeDisplay(title),flags);}
int WINAPI messageW(HWND w,LPCWSTR text,LPCWSTR title,UINT flags){return displayMessage(w,Translate(wide(text)),Translate(wide(title)),flags);}
BOOL WINAPI appendA(HMENU m,UINT flags,UINT_PTR id,LPCSTR text){if((flags&(MF_BITMAP|MF_OWNERDRAW|MF_SEPARATOR))||!isString(text))return AppendMenuA(m,flags,id,text);return AppendMenuW(m,flags,id,DecodeDisplay(text).c_str());}
BOOL WINAPI appendW(HMENU m,UINT flags,UINT_PTR id,LPCWSTR text){if((flags&(MF_BITMAP|MF_OWNERDRAW|MF_SEPARATOR))||!isString(text))return AppendMenuW(m,flags,id,text);return AppendMenuW(m,flags,id,Translate(text).c_str());}
BOOL WINAPI titleA(HWND w,LPCSTR s){return SetWindowTextW(w,DecodeDisplay(s).c_str());}
BOOL WINAPI titleW(HWND w,LPCWSTR s){return SetWindowTextW(w,Translate(wide(s)).c_str());}
BOOL WINAPI itemA(HWND w,int id,LPCSTR s){return SetDlgItemTextW(w,id,DecodeDisplay(s).c_str());}
BOOL WINAPI itemW(HWND w,int id,LPCWSTR s){return SetDlgItemTextW(w,id,Translate(wide(s)).c_str());}
bool stringMessage(HWND w,UINT msg) {
    if(msg==WM_SETTEXT)return true;
    LONG style=GetWindowLongW(w,GWL_STYLE);
    if(msg==CB_ADDSTRING||msg==CB_INSERTSTRING)return !(style&(CBS_OWNERDRAWFIXED|CBS_OWNERDRAWVARIABLE))||(style&CBS_HASSTRINGS);
    if(msg==LB_ADDSTRING||msg==LB_INSERTSTRING)return !(style&(LBS_OWNERDRAWFIXED|LBS_OWNERDRAWVARIABLE))||(style&LBS_HASSTRINGS);
    return false;
}
bool isWideStatic(HWND w) {
    wchar_t cls[32]{};
    return w&&GetClassNameW(w,cls,32)&&!_wcsicmp(cls,L"WC_ITEM_STATIC");
}
LRESULT WINAPI sendA(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==0x8204&&wp&&isString(reinterpret_cast<const void*>(lp))&&!strcmp(reinterpret_cast<const char*>(lp),"WRITEHTML")){auto s=Translate(reinterpret_cast<LPCWSTR>(wp));return SendMessageA(w,msg,reinterpret_cast<WPARAM>(s.c_str()),lp);}
    if(msg==WM_SETTEXT&&lp&&isWideStatic(w)){auto raw=wide(reinterpret_cast<LPCWSTR>(lp));auto s=(raw==L"ランキング戦")?raw:Translate(raw);return SendMessageA(w,msg,wp,reinterpret_cast<LPARAM>(s.c_str()));}
    if(lp&&stringMessage(w,msg)){auto s=DecodeDisplay(reinterpret_cast<LPCSTR>(lp));return SendMessageW(w,msg,wp,reinterpret_cast<LPARAM>(s.c_str()));}
    return SendMessageA(w,msg,wp,lp);
}
BSTR WINAPI allocBstr(const OLECHAR* s) {
    if(s&&(!wcsncmp(s,L"<html>",6)||!wcsncmp(s,L"updateList(",11))){auto t=Translate(s);return SysAllocString(t.c_str());}
    return SysAllocString(s);
}
LRESULT WINAPI sendW(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(lp&&stringMessage(w,msg)){auto s=Translate(reinterpret_cast<LPCWSTR>(lp));return SendMessageW(w,msg,wp,reinterpret_cast<LPARAM>(s.c_str()));}
    return SendMessageW(w,msg,wp,lp);
}
LRESULT WINAPI sendItemA(HWND w,int id,UINT msg,WPARAM wp,LPARAM lp){return sendA(GetDlgItem(w,id),msg,wp,lp);}
LRESULT WINAPI sendItemW(HWND w,int id,UINT msg,WPARAM wp,LPARAM lp){return sendW(GetDlgItem(w,id),msg,wp,lp);}
thread_local HWND paintingWelcome=nullptr;
LRESULT CALLBACK welcomeObserver(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR) {
    // The client can paint through a memory DC. Keep the owner explicitly
    // rather than depending on WindowFromDC for the login hint's placement.
    HWND previous=paintingWelcome;
    if(msg==WM_ERASEBKGND)paintingWelcome=w;
    auto result=DefSubclassProc(w,msg,wp,lp);
    paintingWelcome=previous;
    if(msg==WM_NCDESTROY)RemoveWindowSubclass(w,welcomeObserver,id);
    return result;
}
void positionWelcomeHint(HDC dc,const std::wstring& hint,int& x,int& y) {
    if(!paintingWelcome)return;
    HWND input=GetDlgItem(paintingWelcome,0x0bba),details=GetDlgItem(paintingWelcome,0x0bbe);
    if(!input||!details)return;
    RECT field{},link{};SIZE label{};
    if(!GetWindowRect(input,&field)||!GetWindowRect(details,&link)||
       !GetTextExtentPoint32W(dc,hint.c_str(),static_cast<int>(hint.size()),&label))return;
    MapWindowPoints(nullptr,paintingWelcome,reinterpret_cast<POINT*>(&field),2);
    MapWindowPoints(nullptr,paintingWelcome,reinterpret_cast<POINT*>(&link),2);
    const int linkWidth=link.right-link.left,linkHeight=link.bottom-link.top;
    if(linkWidth<=0||linkHeight<=0)return;
    const int gap=12,below=10;
    const int height=std::max(static_cast<int>(label.cy),linkHeight);
    const int left=(field.left+field.right-label.cx-gap-linkWidth)/2;
    const int top=field.bottom+below;
    x=left;y=top+(height-label.cy)/2;
    const int linkX=left+label.cx+gap,linkY=top+(height-linkHeight)/2;
    if(link.left!=linkX||link.top!=linkY)
        SetWindowPos(details,nullptr,linkX,linkY,0,0,SWP_NOSIZE|SWP_NOZORDER|SWP_NOACTIVATE);
}
HWND WINAPI createA(DWORD ex,LPCSTR cls,LPCSTR caption,DWORD style,int x,int y,int width,int height,HWND parent,HMENU menu,HINSTANCE instance,LPVOID param) {
    auto c=isString(cls)?convert(CP_ACP,cls,-1):std::wstring();
    auto text=isString(caption)?DecodeDisplay(caption):std::wstring();
    HWND window=CreateWindowExW(ex,isString(cls)?c.c_str():reinterpret_cast<LPCWSTR>(cls),isString(caption)?text.c_str():reinterpret_cast<LPCWSTR>(caption),style,x,y,width,height,parent,menu,instance,param);
    if(window&&c==L"WC_WELCOME")SetWindowSubclass(window,welcomeObserver,0x49444849,0);
    return window;
}
BOOL WINAPI textOut(HDC dc,int x,int y,LPCWSTR text,int length){auto raw=wide(text,length);auto s=Translate(raw);if(raw==L"プレーヤIDを入力してください。")positionWelcomeHint(dc,s,x,y);return TextOutW(dc,x,y,s.c_str(),static_cast<int>(s.size()));}
int WINAPI drawText(HDC dc,LPWSTR text,int length,LPRECT rect,UINT flags){if(flags&DT_MODIFYSTRING)return DrawTextW(dc,text,length,rect,flags);auto s=Translate(wide(text,length));return DrawTextW(dc,s.data(),static_cast<int>(s.size()),rect,flags);}
BOOL WINAPI extentA(HDC dc,LPCSTR text,int length,LPSIZE size){auto s=DecodeDisplay(text,length);return GetTextExtentPoint32W(dc,s.c_str(),static_cast<int>(s.size()),size);}
BOOL WINAPI extentW(HDC dc,LPCWSTR text,int length,LPSIZE size){auto s=Translate(wide(text,length));return GetTextExtentPoint32W(dc,s.c_str(),static_cast<int>(s.size()),size);}
HFONT WINAPI createFont(const LOGFONTA* input) {
    if(!input)return CreateFontIndirectA(input);
    LOGFONTW f{};memcpy(&f,input,offsetof(LOGFONTA,lfFaceName));
    auto name=convert(932,input->lfFaceName,-1);
    if(f.lfCharSet==SHIFTJIS_CHARSET||name.find(L"Gothic")!=name.npos||name.find(L"ゴシック")!=name.npos||name.find(L"明朝")!=name.npos){name=L"Microsoft YaHei";f.lfCharSet=DEFAULT_CHARSET;}
    wcsncpy_s(f.lfFaceName,name.c_str(),_TRUNCATE);return CreateFontIndirectW(&f);
}
struct RectF {float X,Y,Width,Height;};
extern "C" int WINAPI GdipDrawString(void*,const WCHAR*,int,void*,const RectF*,void*,void*);
extern "C" int WINAPI GdipMeasureString(void*,const WCHAR*,int,void*,const RectF*,void*,RectF*,int*,int*);
extern "C" int WINAPI GdipAddPathString(void*,const WCHAR*,int,void*,int,float,const RectF*,void*);
extern "C" int WINAPI GdipCreateFontFamilyFromName(const WCHAR*,void*,void**);
int WINAPI gdDraw(void* g,const WCHAR* text,int n,void* f,const RectF* rect,void* format,void* brush){auto raw=wide(text,n);auto s=(raw==L"ランキング戦")?raw:Translate(raw);return GdipDrawString(g,s.c_str(),static_cast<int>(s.size()),f,rect,format,brush);}
int WINAPI gdMeasure(void* g,const WCHAR* text,int n,void* f,const RectF* rect,void* format,RectF* bounds,int* fitted,int* lines){auto raw=wide(text,n);auto s=(raw==L"ランキング戦")?raw:Translate(raw);return GdipMeasureString(g,s.c_str(),static_cast<int>(s.size()),f,rect,format,bounds,fitted,lines);}
int WINAPI gdPath(void* p,const WCHAR* text,int n,void* family,int style,float size,const RectF* rect,void* format){auto raw=wide(text,n);auto s=(raw==L"ランキング戦")?raw:Translate(raw);return GdipAddPathString(p,s.c_str(),static_cast<int>(s.size()),family,style,size,rect,format);}
int WINAPI gdFamily(const WCHAR* name,void* collection,void** family){std::wstring s=wide(name);if(s.find(L"Gothic")!=s.npos||s.find(L"ゴシック")!=s.npos||s.find(L"明朝")!=s.npos)s=L"Microsoft YaHei";return GdipCreateFontFamilyFromName(s.c_str(),collection,family);}
// Thread-local post-message hook sees complete resource dialogs and property
// pages after their own WM_INITDIALOG handler. It never intercepts user input.
BOOL CALLBACK translateControl(HWND w,LPARAM) {
    wchar_t cls[80]{};GetClassNameW(w,cls,80);
    if(!_wcsicmp(cls,L"Edit")||!_wcsicmp(cls,L"RichEdit20W"))return TRUE;
    int n=GetWindowTextLengthW(w);if(n>0&&n<8192){std::wstring s(n+1,L'\0');GetWindowTextW(w,s.data(),n+1);s.resize(n);auto t=Translate(s);if(t!=s)SetWindowTextW(w,t.c_str());}
    if(!_wcsicmp(cls,WC_TABCONTROLW)) {
        int count=static_cast<int>(SendMessageW(w,TCM_GETITEMCOUNT,0,0));
        for(int i=0;i<count;++i){wchar_t buf[512]{};TCITEMW item{};item.mask=TCIF_TEXT;item.pszText=buf;item.cchTextMax=512;if(SendMessageW(w,TCM_GETITEMW,i,reinterpret_cast<LPARAM>(&item))){auto text=Translate(buf);item.pszText=text.data();SendMessageW(w,TCM_SETITEMW,i,reinterpret_cast<LPARAM>(&item));}}
    }
    return TRUE;
}
LRESULT CALLBACK dialogObserver(int code,WPARAM wp,LPARAM lp) {
    if(code>=0) {
        auto message=reinterpret_cast<CWPRETSTRUCT*>(lp);
        if(message->message==WM_INITDIALOG) {
            translateControl(message->hwnd,0);EnumChildWindows(message->hwnd,translateControl,0);
            if(HWND parent=GetParent(message->hwnd)){translateControl(parent,0);EnumChildWindows(parent,translateControl,0);}
        } else if(message->message==WM_NOTIFY&&message->lParam) {
            auto hdr=reinterpret_cast<NMHDR*>(message->lParam);
            if(hdr->code==LVN_GETDISPINFOW) {
                auto di=reinterpret_cast<NMLVDISPINFOW*>(message->lParam);
                if((di->item.mask&LVIF_TEXT)&&isString(di->item.pszText)) {
                    auto t=Translate(di->item.pszText);
                    if(t!=di->item.pszText){thread_local std::wstring lvBuf[16];thread_local size_t lvIdx=0;auto& slot=lvBuf[lvIdx++&15];slot=std::move(t);di->item.pszText=slot.data();}
                }
            }
        }
    }
    return CallNextHookEx(nullptr,code,wp,lp);
}
void ensureDialogObserver(){thread_local HHOOK hook=nullptr;if(!hook)hook=SetWindowsHookExW(WH_CALLWNDPROCRET,dialogObserver,nullptr,GetCurrentThreadId());}
INT_PTR WINAPI dialogA(HINSTANCE m,LPCSTR name,HWND w,DLGPROC proc,LPARAM param){ensureDialogObserver();return DialogBoxParamA(m,name,w,proc,param);}
HWND WINAPI modelessA(HINSTANCE m,LPCSTR name,HWND w,DLGPROC proc,LPARAM param){ensureDialogObserver();return CreateDialogParamA(m,name,w,proc,param);}
INT_PTR WINAPI properties(LPCPROPSHEETHEADERW header){ensureDialogObserver();return PropertySheetW(header);}
BOOL fileDialog(OPENFILENAMEA* a,bool save) {
    if(!a||a->Flags&OFN_ENABLEHOOK)return save?GetSaveFileNameA(a):GetOpenFileNameA(a);
    OPENFILENAMEW w{};w.lStructSize=sizeof(w);w.hwndOwner=a->hwndOwner;w.hInstance=a->hInstance;
    // Paths keep ACP semantics; only display labels use CP932 + translation.
    std::vector<wchar_t> file(std::max<DWORD>(a->nMaxFile,1)),title(std::max<DWORD>(a->nMaxFileTitle,1));
    auto initial=convert(CP_ACP,a->lpstrFile,-1);if(initial.size()<file.size())wcscpy_s(file.data(),file.size(),initial.c_str());
    std::wstring filter;if(a->lpstrFilter){for(const char* p=a->lpstrFilter;*p;p+=strlen(p)+1){filter+=DecodeDisplay(p);filter+=L'\0';}filter+=L'\0';}
    auto caption=DecodeDisplay(a->lpstrTitle),dir=convert(CP_ACP,a->lpstrInitialDir,-1),ext=convert(CP_ACP,a->lpstrDefExt,-1);
    w.lpstrFilter=a->lpstrFilter?filter.c_str():nullptr;w.nFilterIndex=a->nFilterIndex;w.lpstrFile=file.data();w.nMaxFile=static_cast<DWORD>(file.size());
    w.lpstrFileTitle=a->lpstrFileTitle?title.data():nullptr;w.nMaxFileTitle=a->nMaxFileTitle;w.lpstrInitialDir=a->lpstrInitialDir?dir.c_str():nullptr;
    w.lpstrTitle=a->lpstrTitle?caption.c_str():nullptr;w.Flags=a->Flags;w.lpstrDefExt=a->lpstrDefExt?ext.c_str():nullptr;
    if(!(save?GetSaveFileNameW(&w):GetOpenFileNameW(&w)))return FALSE;
    // Preserve embedded-NUL multi-selection buffers, with byte-based offsets.
    size_t count=wcslen(file.data())+1;
    if(a->Flags&OFN_ALLOWMULTISELECT)while(count<file.size()&&file[count])count+=wcslen(file.data()+count)+1;
    if(count<file.size())++count;
    BOOL used=FALSE;int bytes=WideCharToMultiByte(CP_ACP,WC_NO_BEST_FIT_CHARS,file.data(),static_cast<int>(count),nullptr,0,nullptr,&used);
    if(used||bytes>static_cast<int>(a->nMaxFile)){SetLastError(ERROR_INSUFFICIENT_BUFFER);return FALSE;}
    WideCharToMultiByte(CP_ACP,0,file.data(),static_cast<int>(count),a->lpstrFile,a->nMaxFile,nullptr,nullptr);
    if(a->lpstrFileTitle)WideCharToMultiByte(CP_ACP,0,title.data(),-1,a->lpstrFileTitle,a->nMaxFileTitle,nullptr,nullptr);
    a->nFileOffset=static_cast<WORD>(WideCharToMultiByte(CP_ACP,0,file.data(),w.nFileOffset,nullptr,0,nullptr,nullptr));
    a->nFileExtension=static_cast<WORD>(WideCharToMultiByte(CP_ACP,0,file.data(),w.nFileExtension,nullptr,0,nullptr,nullptr));
    a->nFilterIndex=w.nFilterIndex;a->Flags=w.Flags;return TRUE;
}
BOOL WINAPI openFile(OPENFILENAMEA* a){return fileDialog(a,false);}BOOL WINAPI saveFile(OPENFILENAMEA* a){return fileDialog(a,true);}
struct Hook {const char* name;void* fn;};
#define HOOK(n,f) {n,reinterpret_cast<void*>(f)}
const Hook hooks[]={
HOOK("MultiByteToWideChar",mbToWide),HOOK("MessageBoxA",messageA),HOOK("MessageBoxW",messageW),HOOK("AppendMenuA",appendA),HOOK("AppendMenuW",appendW),
HOOK("SetWindowTextA",titleA),HOOK("SetWindowTextW",titleW),HOOK("SetDlgItemTextA",itemA),HOOK("SetDlgItemTextW",itemW),
HOOK("SendMessageA",sendA),HOOK("SendMessageW",sendW),HOOK("SendDlgItemMessageA",sendItemA),HOOK("SendDlgItemMessageW",sendItemW),HOOK("CreateWindowExA",createA),
HOOK("TextOutW",textOut),HOOK("DrawTextW",drawText),HOOK("GetTextExtentPoint32A",extentA),HOOK("GetTextExtentPoint32W",extentW),HOOK("CreateFontIndirectA",createFont),
HOOK("GdipDrawString",gdDraw),HOOK("GdipMeasureString",gdMeasure),HOOK("GdipAddPathString",gdPath),HOOK("GdipCreateFontFamilyFromName",gdFamily),
HOOK("DialogBoxParamA",dialogA),HOOK("CreateDialogParamA",modelessA),HOOK("PropertySheetW",properties),HOOK("GetOpenFileNameA",openFile),HOOK("GetSaveFileNameA",saveFile),HOOK("SysAllocString",allocBstr)};
#undef HOOK
}
unsigned Install(HMODULE module) {
    dictionary();unsigned count=0;
    PeImports::Visit(module,[&](const PeImports::Import& entry){
        if(!entry.name){
            if(!_stricmp(entry.library,"OLEAUT32.dll")&&entry.ordinal==2&&
               PeImports::Replace(entry.slot,reinterpret_cast<void*>(allocBstr)))++count;
            return true;
        }
        for(const auto& hook:hooks)if(!strcmp(entry.name,hook.name)){
            if(PeImports::Replace(entry.slot,hook.fn))++count;
            break;
        }
        return true;
    });
    return count;
}
}
