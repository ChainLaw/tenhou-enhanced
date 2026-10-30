# Tenhou Enhanced

English | [简体中文](README-zh_CN.md)

An unofficial modified edition of the Tenhou Windows client. Modeled on the official 4K interface, it rebuilds the login page and lobby with WebView2, adds a native remaining-tile counter and a Simplified Chinese localization, and fixes some known issues.

The 4K lobby uses the original Windows client for login. The native table handles rendering, table logic and user controls. The optional experimental Web mode uses the web edition's connection only for matchmaking, the resulting game and game recovery, translating native discards, riichi and calls into the Web protocol. Cancelling matchmaking or finishing a game restores the native lobby connection.

> [!WARNING]
> **Disclaimer.** This is an unofficial, personal project. It is **not** affiliated with, endorsed by, or supported by Tenhou or its operators. "Tenhou" and all related names, assets and trademarks belong to their respective owners.
>
> This project modifies and extends the original client at runtime. Use it at your own risk; the authors accept no liability for any loss, including account restrictions or suspension, arising from its use. It does not read hidden tiles or other players' hands, and must not be used to gain an unfair advantage. If the operators object to this project, it may be removed.

## Features

| Feature | Description |
| --- | --- |
| 4K lobby | Based on the official 4K interface; the page and lobby are rebuilt. The Windows client calls some interfaces of the web edition and the 4K edition, with UI adjustments to match. Supports Simplified Chinese, Japanese and English |
| Native remaining-tile counter | Shows the remaining tile count on the wind indicator. It counts from draw events and the native wall state, and never reads hidden tiles or other players' hands |
| Simplified Chinese UI | Localizes the native login page, lobby, menus, settings and in-game prompts. The 4K lobby's languages are handled separately by the language files in `vendor/tenhou4/` |
| Improved reconnection mechanism | Improves connection recovery after disconnects and the return to the lobby after a game ends |

## Installation

Put the complete runtime directory in a writable location. The launcher, `tenhou_patch.dll`, the original `launcher.exe` and `tenhou.exe` must stay in the same directory. The 4K lobby additionally needs `WebView2Loader.dll` and the `web/` resources alongside them, plus the system-installed `Microsoft Edge WebView2 Runtime`. Building from source generates this directory automatically.

An ASCII-only path is recommended. On first run, or after the original resources are cleaned, table textures, shadows and sound effects are downloaded or regenerated online. The local lobby's images, fonts and scripts are deployed with the build; login, matchmaking and online data queries still require network access.

## Usage

Launch `TenhouEnhanced.exe` from the runtime directory.

- **Settings**: the gear icon in the 4K lobby offers common settings; the full original settings are under "Settings" in the title-bar right-click menu.
- **Matchmaking**: in web connection mode, Normal, Advanced, Expert and Phoenix tables use web reservations. The server checks rank, rating, membership and rule availability. With `web3=0`, matchmaking follows the original Windows client's requirements.
- **Gameplay controls**: right-click skip / tsumogiri is on by default; left double-click skip / tsumogiri and ESC skip / tsumogiri are off by default. Change them in the gear settings or under "Other" in the original settings.
- **Audio**: the call-sound toggle is kept; call / SE sounds and BGM can be customized under "SE" and "BGM" in the original settings.
- **Background and tiles**: the tablecloth only affects the lobby background; tile faces and the in-game table are changed under "Tiles" and "Table" in the original settings.
- **Spectating and replays**: lists come from the native network service or are read locally, and clicking opens the native spectate / replay screen. Account / device replay filtering follows the 4K edition's behavior.

## Configuration

Edit `enhanced.ini` in the runtime directory and restart to switch between the original lobby and the WebView2 4K lobby:

```ini
[lobby]
webview2=1
```

`0` is the original Windows lobby; `1` is the WebView2 4K lobby (default).

### Experimental features

Edit the matchmaking mode in `enhanced.ini` and restart for changes to take effect:

```ini
[network]
web3=0
```

`0` uses the original Windows client's matchmaking (default); `1` uses web edition matchmaking with gameplay on the native Windows table (experimental). Existing explicit settings are preserved when deploying an update.

## Building

### Requirements

- **OS**: Windows
- **IDE / toolchain**: Visual Studio or Visual Studio Build Tools with the "Desktop development with C++" workload (MSVC x86 toolchain, Windows SDK and the CMake component)
- **CMake**: 3.20 or later
- **Language / build settings**: Win32 (x86) / C++17, with the C/C++ runtime statically linked
- **Microsoft Edge WebView2 Runtime**: required to run the 4K lobby (the default lobby)
- **Python 3** (optional): only needed to regenerate the localization data

### Build and launch

```powershell
.\scripts\build.ps1
.\tenhou\TenhouEnhanced.exe
```

Deploy existing build output:

```powershell
.\scripts\deploy.ps1 -Destination "<destination-directory>"
```

### Modifying resources

After editing `src/web/index.html`, `src/web/adapter.js` or the UI assets in `vendor/tenhou4/`, run `build.ps1` again to generate and deploy the lobby. You can also build the web part on its own:

```powershell
.\scripts\build_lobby.ps1
```

After editing the original-client localization dictionary `src/localization/localization.md`, regenerate the data with Python 3, then build:

```powershell
python .\scripts\build_localization.py
.\scripts\build.ps1
```

Keep both the dictionary source and `src/localization/generated/localization_data.inc`. The generator reads five-column tables numbered A to I; some entries carry expansion rules, so preserve the numbering and format when editing. The 4K lobby's languages are handled by `scripts/build_lobby.ps1` and the language files in `vendor/tenhou4/`.

## Project Structure

```text
tenhou-enhanced/
├── CMakeLists.txt
├── scripts/            # Build, deploy and dictionary generation scripts
├── src/
│   ├── common/         # Shared code for injection and PE imports
│   ├── launcher/       # Launcher that injects the patch DLL
│   ├── patch/          # Patch DLL entry point
│   ├── lobby/          # WebView2 lobby, lobby protocol, local replays
│   ├── native/         # Tile counter and input policy
│   ├── localization/   # Original-client localization dictionary and generated data
│   └── web/            # 4K lobby page and adapter script
└── vendor/
    ├── client/         # Original launcher.exe and tenhou.exe
    ├── tenhou4/        # 4K lobby assets and language files
    └── webview2/       # WebView2Loader.dll
```
