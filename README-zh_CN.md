# Tenhou Enhanced

简体中文 | [English](README.md)

天鳳 Windows 版的非官方修改版。项目参考官方 4K 版界面，使用 WebView2 重新构建登录页面与大厅，新增原生牌桌余牌数显示和简体中文汉化，并修复了一些已知问题。

4K 大厅通过原版 Windows 客户端登录，原生牌桌负责画面、牌桌逻辑及用户操作。可选的实验性 Web 模式仅在匹配及其后续对局、掉线续局时使用 Web 版连接，并将原生出牌、立直和鸣牌指令转换为 Web 协议；取消匹配或对局结束后恢复原生大厅连接。

> [!WARNING]
> **免责声明**：本项目为个人制作的非官方项目，与天鳳（Tenhou）及其运营方**无任何关联**，也未获得其认可或支持。「天鳳」及所有相关名称、素材与商标均归其各自权利人所有。
>
> 本项目会在运行时修改并扩展原版客户端，使用风险自负；因使用本项目造成的任何损失（包括但不限于账号受限或被封禁），作者不承担责任。本项目不会读取未公开牌面或其他玩家手牌，也不得用于谋取不正当优势。如运营方提出异议，本项目可能会被移除。

## 功能

| 功能 | 说明 |
| --- | --- |
| 4K 大厅 | 基于官方 4K 界面重新构建了页面与大厅；Windows 版调用了 Web 版和 4K 版的部分接口，并调整了界面适配；支持简体中文、日文和英文 |
| 原生牌桌余牌数显示 | 在场风盘显示余牌数；依据摸牌事件和原生牌山状态计数，不读取未公开牌面或其他玩家手牌 |
| 简体中文界面 | 汉化原生登录页、大厅、菜单、设置及对局相关提示；4K 大厅语言由 `vendor/tenhou4/` 中的语言文件单独处理 |
| 优化重连机制 | 优化掉线后的连接恢复与对局结束返回大厅流程 |

## 安装

将完整运行目录放在可写位置。启动器、`tenhou_patch.dll`、原版 `launcher.exe` 和 `tenhou.exe` 需保持同级；使用 4K 大厅还需要同级的 `WebView2Loader.dll`、`web/` 资源，以及系统已安装的 `Microsoft Edge WebView2 Runtime`，自行编译会自动生成此目录。

运行路径建议使用纯英文路径。首次运行或清理原版资源后，牌桌材质、阴影和音效等会联网下载或重新生成；本地大厅的图片、字体和脚本随构建部署，登录、匹配及在线数据查询仍需联网。

## 使用

在运行目录中启动 `TenhouEnhanced.exe`。

- **设置入口**：4K 大厅齿轮提供常用设置；完整原版设置位于窗口标题栏右键菜单的「设置」。
- **匹配**：Web 连接模式下，一般、上級、特上及鳳凰卓统一使用 Web 版预约；段位、R、会员期限和规则开放情况由服务器最终判定。设置 `web3=0` 时，匹配沿用原版 Windows 客户端的入场条件。
- **对局操作**：右键跳过 / 摸切默认开启；左键双击跳过 / 摸切、ESC 跳过 / 摸切默认关闭，可在齿轮设置或原版设置的「其他」中调整。
- **音量与提示音**：保留鸣牌提示音开关；鸣牌 / SE 音效及 BGM 可在原版设置中的「SE」和「BGM」中自定义。
- **背景与牌面**：桌布仅影响大厅背景，牌面、局内牌桌可在原版设置的「牌面」和「牌桌」中修改。
- **观战与牌谱**：列表由原生网络服务获取或本地读取，点击后进入原生观战 / 牌谱界面；账号 / 设备牌谱筛选沿用 4K 版说明。

## 配置

编辑运行目录中的 `enhanced.ini`，修改后重新启动即可切换原版 / WebView2 4K 大厅：

```ini
[lobby]
webview2=1
```

`0` 为 Windows 原版大厅，`1` 为 WebView2 4K 大厅（默认）。

### 实验性功能

在 `enhanced.ini` 中修改匹配方式，修改后重新启动即可生效：

```ini
[network]
web3=0
```

`0` 为原版 Windows 客户端匹配（默认），`1` 为 Web 版匹配配合 Windows 原生牌桌对局（实验性）。部署更新时会保留已有的显式配置。

## 编译

### 环境要求

- **操作系统**：Windows
- **开发环境 / 工具链**：Visual Studio 或 Visual Studio Build Tools，安装「使用 C++ 的桌面开发」工作负载（含 MSVC x86 工具链、Windows SDK 及 CMake 组件）
- **CMake**：3.20 及以上
- **语言 / 构建设置**：Win32 (x86) / C++17，静态链接 C/C++ 运行库
- **Microsoft Edge WebView2 Runtime**：运行 4K 大厅（默认大厅）时需要
- **Python 3**（可选）：仅在重新生成汉化数据时需要

### 编译与启动

```powershell
.\scripts\build.ps1
.\tenhou\TenhouEnhanced.exe
```

部署已有产物：

```powershell
.\scripts\deploy.ps1 -Destination "<部署目标目录>"
```

### 修改资源

修改 `src/web/index.html`、`src/web/adapter.js` 或 `vendor/tenhou4/` 中的界面素材后，重新执行 `build.ps1` 即可生成并部署大厅。也可单独生成网页：

```powershell
.\scripts\build_lobby.ps1
```

修改原版汉化词典 `src/localization/localization.md` 后，先使用 Python 3 更新生成文件，再编译：

```powershell
python .\scripts\build_localization.py
.\scripts\build.ps1
```

词典源文件与 `src/localization/generated/localization_data.inc` 均需保留。生成器读取 A～I 编号的五列表格，部分编号含展开规则，修改时保留编号及格式；4K 大厅语言由 `scripts/build_lobby.ps1` 和 `vendor/tenhou4/` 中的语言文件处理。

## 项目结构

```text
tenhou-enhanced/
├── CMakeLists.txt
├── scripts/            # 构建、部署与词典生成脚本
├── src/
│   ├── common/         # 注入与 PE 导入表等公共代码
│   ├── launcher/       # 启动器，负责注入补丁 DLL
│   ├── patch/          # 补丁 DLL 入口
│   ├── lobby/          # WebView2 大厅、大厅协议、本地牌谱
│   ├── native/         # 余牌计数与输入策略
│   ├── localization/   # 原版汉化词典及生成数据
│   └── web/            # 4K 大厅页面与适配脚本
└── vendor/
    ├── client/         # 原版 launcher.exe 与 tenhou.exe
    ├── tenhou4/        # 4K 大厅素材与语言文件
    └── webview2/       # WebView2Loader.dll
```
