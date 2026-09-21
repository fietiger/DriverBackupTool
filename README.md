# DriverBackupTool (Windows 驱动备份与一键还原工具)

纯 C 语言 + Win32 原生 API 开发的 Windows 驱动备份与批量注入还原工具。

- 零第三方依赖、零流氓捆绑、体积仅 260KB；
- 内置 UAC 管理员提权清单（requireAdministrator）；
- 核心引擎基于微软官方 `DISM`（驱动导出）与 `PnPUtil`（驱动批量递归安装）；
- 自动滤除微软官方通用驱动，仅提取属于当前主板、显卡、网卡、声卡、蓝牙及特定工控板卡的第三方硬件驱动。

---

## 🚀 核心特性

1. **纯正 Win32 GUI 界面**：采用系统原生公共控件（Common Controls v6），支持现代化视觉样式与暗淡滚动高亮。
2. **一键备份**：自动调用底层 DISM 引擎，将第三方驱动完整提取为原始安装包（`.inf` / `.sys` / `.cat` 等）。
3. **一键还原**：自动调用 PnPUtil 引擎递归扫描选定文件夹，一键自动批量安装所选硬件驱动，消除黄色感叹号。
4. **实时双向管道**：内置无阻塞多线程管道，实时回显底层执行进度与详细硬件匹配日志。

---

## 🛠️ 本地编译构建

### Linux 交叉编译（MinGW-w64）
```bash
sudo apt-get install -y mingw-w64
x86_64-w64-mingw32-windres app.rc -O coff -o app.res
x86_64-w64-mingw32-gcc -O2 -municode -mwindows main.c app.res -o DriverBackupTool.exe -lcomctl32 -lshell32 -lole32
```

### Windows 本地构建（MSVC 或 GCC）
* 使用 Visual Studio 开发者命令行或 MinGW 编译即可。

---

## 📦 自动化 CI/CD
本项目配置了 GitHub Actions 自动化工作流：
* **自动测试**：检查 PE 架构、导入表与子系统依赖；
* **自动构建**：全自动编译出纯净 x64 原生单文件可执行程序；
* **自动发布**：Push 到 `main` 分支或打 `v*` 标签时自动创建 GitHub Release 并附带编译产物供直接下载。
