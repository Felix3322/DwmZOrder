# DwmZOrder

原生 Win32 DWM 合成窗口排序工具。它直接在当前 DWM 进程中加载同目录的 `DwmZOrder.dll`，支持跨 Band 的合成顺序调整和持续保持。

## 使用

1. 将 `DwmOrderTool.exe` 与 `DwmZOrder.dll` 放在同一目录并运行。
2. 实际调整需要管理员权限。点击“管理员重启 / Elevate”，或以管理员身份运行 EXE。
3. 选择目标窗口；需要相对排序时再选择参照窗口。位置支持合成最前、合成最后、参照上方和参照下方。
4. 点击“应用顺序”。勾选“持续保持”后，DWM 更新场景时会继续维护该目标。用“恢复目标”恢复 Windows 当前顺序，用“停止全部”停止本会话代理的保持。

改变的是 DWM 合成遮挡顺序，窗口原始 Band、鼠标命中和键盘焦点仍由 Windows 管理。隐藏、最小化、独占呈现和安全桌面不会因排序自动可见。超时或“结果未知”时先查询状态，不要重复应用。

支持范围和模型适配边界见 [SUPPORT.md](SUPPORT.md)。不匹配的完整模型会拒绝修改。

## 构建与打包

需要 Windows x64、Visual Studio 2022 / Build Tools 的 C++ 桌面工具、Windows SDK 和 CMake 3.24+：

```powershell
powershell -ExecutionPolicy Bypass -File .\Build.ps1
powershell -ExecutionPolicy Bypass -File .\Package.ps1 -Destination '..\DwmZOrder-release'
```

构建输出位于 `build/bin/Release/`。打包脚本会校验源文件清单和构建回执，生成可运行的 `DwmZOrder-x64.zip` 及对应源码包；包内不含 PDB、系统 DLL 或构建缓存。

## 许可证

本程序由 KSword 版权所有者另行授权，采用 **GPL-3.0-only**，因此采用不同于 KSword 主项目的许可证。此授权仅适用于 DwmZOrder，不改变 KSword 主项目的许可证。

版权与授权声明见 [NOTICE.md](NOTICE.md)，完整许可条款见 [LICENSE](LICENSE)。

如果你想尝试我们完整的高级系统维护工具，包含更多这样的功能，可以尝试 [KSword](https://github.com/KSwordDEV/KSword)。
