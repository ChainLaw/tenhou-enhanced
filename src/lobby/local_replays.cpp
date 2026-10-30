#include "local_replays.h"
#include "lobby_protocol.h"
#include <windows.h>
#include <wincrypt.h>
#include <algorithm>
#include <filesystem>
#include <regex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace LobbyServices {
ReplayMessages ReadLocalReplays(const std::wstring& directory) {
    const auto ini=directory+L"\\config.ini";
    std::vector<wchar_t> section(65536);GetPrivateProfileSectionW(L"LOG",section.data(),static_cast<DWORD>(section.size()),ini.c_str());
    std::vector<std::pair<std::string,std::string>> history;
    const std::regex logName("[0-9]{10}gm-[0-9a-fA-F]{4}-[0-9]{4}-[0-9a-fA-F]+(?:\\.mjlog)?");
    for(auto line=section.data();*line;line+=wcslen(line)+1){
        std::wstring_view item(line);auto equals=item.find(L'=');int index=0;
        if(equals==item.npos||!LobbyProtocol::Integer(LobbyProtocol::Utf8(std::wstring(item.substr(0,equals))),0,1000000,index))continue;
        auto record=LobbyProtocol::Utf8(std::wstring(item.substr(equals+1)));std::unordered_map<std::string,std::string> fields;
        for(size_t at=0;at<record.size();){auto end=record.find('&',at);if(end==record.npos)end=record.size();auto equal=record.find('=',at);if(equal<end)fields[record.substr(at,equal-at)]=record.substr(equal+1,end-equal-1);at=end+1;}
        auto file=fields.find("file");if(file==fields.end()||!std::regex_match(file->second,logName))continue;
        std::string filtered;for(auto key:{"file","type","oya","un0","un1","un2","un3","sc"}){auto value=fields.find(key);if(value==fields.end())continue;if(!filtered.empty())filtered+='&';filtered+=std::string(key)+'='+value->second;}
        history.emplace_back(file->second,std::move(filtered));
    }
    std::sort(history.begin(),history.end(),[](const auto& a,const auto& b){return a.first>b.first;});
    std::string historyJson="{\"type\":\"logHistory\",\"entries\":[";for(size_t i=0;i<std::min<size_t>(40,history.size());++i){if(i)historyJson+=',';historyJson+=LobbyProtocol::Quote(history[i].second);}historyJson+="]}";
    wchar_t configuredPath[32768]{};GetPrivateProfileStringW(L"LOG",L"PATH",L"./mjlog",configuredPath,32768,ini.c_str());
    std::filesystem::path folder(*configuredPath?configuredPath:L"./mjlog");if(folder.is_relative())folder=std::filesystem::path(directory)/folder;folder=folder.lexically_normal();
    struct File {std::wstring name;ULONGLONG time;};std::vector<File> files;WIN32_FIND_DATAW item{};
    HANDLE find=FindFirstFileW((folder/L"*.mjlog").c_str(),&item);
    if(find!=INVALID_HANDLE_VALUE){do{if(!(item.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY))files.push_back({item.cFileName,(static_cast<ULONGLONG>(item.ftLastWriteTime.dwHighDateTime)<<32)|item.ftLastWriteTime.dwLowDateTime});}while(FindNextFileW(find,&item));FindClose(find);}
    std::sort(files.begin(),files.end(),[](const File& a,const File& b){return a.time>b.time;});
    size_t total=0;std::string json="{\"type\":\"logFiles\",\"files\":[";bool first=true;
    for(size_t i=0;i<std::min<size_t>(40,files.size());++i){
        HANDLE file=CreateFileW((folder/files[i].name).c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);if(file==INVALID_HANDLE_VALUE)continue;
        DWORD size=GetFileSize(file,nullptr);if(size==INVALID_FILE_SIZE||size>2*1024*1024||total+size>16*1024*1024){CloseHandle(file);continue;}
        std::vector<BYTE> bytes(size);DWORD read=0;bool ok=ReadFile(file,bytes.data(),size,&read,nullptr)&&read==size;CloseHandle(file);if(!ok)continue;
        DWORD length=0;CryptBinaryToStringA(bytes.data(),size,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,nullptr,&length);std::string encoded(length,'\0');
        if(!CryptBinaryToStringA(bytes.data(),size,CRYPT_STRING_BASE64|CRYPT_STRING_NOCRLF,encoded.data(),&length))continue;if(!encoded.empty()&&encoded.back()=='\0')encoded.pop_back();
        if(!first)json+=',';first=false;json+="{\"name\":"+LobbyProtocol::Quote(LobbyProtocol::Utf8(files[i].name))+",\"data\":"+LobbyProtocol::Quote(encoded)+"}";total+=size;
    }
    return {std::move(historyJson),json+"]}"};
}
}
