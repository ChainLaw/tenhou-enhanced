#pragma once
#include <windows.h>

// Recovery uses the same connection contract for the native socket and any
// optional transport. Backends register before the client's UI thread starts.
namespace LobbyConnection {
namespace Detail {
inline bool (*connected)()=nullptr;
inline void (*releaseWindow)(HWND)=nullptr;
inline bool (*fallbackToNative)()=nullptr;
}
inline void UseBackend(bool (*connected)(),void (*releaseWindow)(HWND),bool (*fallbackToNative)()=nullptr){
    Detail::connected=connected;Detail::releaseWindow=releaseWindow;Detail::fallbackToNative=fallbackToNative;
}
inline bool Ready(DWORD socket){return socket!=MAXDWORD&&(!Detail::connected||Detail::connected());}
inline void ReleaseWindow(HWND window){if(Detail::releaseWindow)Detail::releaseWindow(window);}
inline bool FallbackToNative(){return Detail::fallbackToNative&&Detail::fallbackToNative();}
}
