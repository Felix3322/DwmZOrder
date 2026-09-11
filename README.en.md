# DwmZOrder

[简体中文](README.md)

Native Win32 DWM composition window ordering utility. It loads `DwmZOrder.dll` from the same directory into the current DWM process and supports cross-band composition ordering with optional maintenance.

## Use

1. Put `DwmOrderTool.exe` and `DwmZOrder.dll` in the same directory, then run the program.
2. Changing order requires administrator privileges. Click **Restart as administrator** or run the EXE as administrator.
3. Select the target window. Select a reference window when you need relative ordering. You can place the target at the front, at the back, above the reference, or below the reference.
4. Click **Apply**. Enable **Maintain this order** to reapply the selected order when DWM updates the scene. Use **Restore** to restore the current Windows order, or **Stop all maintenance** to stop all maintenance by this agent in the current session.

**Read order** only reads the current order; it does not move the window. Results identify the selected window, its position in the DWM list, and whether maintenance is enabled. Position 1 is frontmost. The list count is not the number of windows visible on screen. The application uses Chinese for a Chinese Windows interface language and English for other interface languages.

This changes DWM composition occlusion order. Windows continues to manage the original band, mouse hit testing, and keyboard focus. Hidden, minimized, exclusive-presentation, and secure-desktop windows do not become visible merely because their order changes. When a result is unknown or times out, read the current order before applying another change.

See [SUPPORT.md](SUPPORT.md) for supported component versions and model limits. An unsupported complete model refuses to make changes.

## Build and package

Windows x64, Visual Studio 2022 / Build Tools with C++ desktop tools, Windows SDK, and CMake 3.24 or later are required:

```powershell
powershell -ExecutionPolicy Bypass -File .\Build.ps1
powershell -ExecutionPolicy Bypass -File .\Package.ps1 -Destination '..\DwmZOrder-release'
```

Build output is in `build/bin/Release/`. The packaging script verifies the source manifest and build receipt, then produces `DwmZOrder-x64.zip` and its matching source archive. Packages do not contain PDBs, system DLLs, or build caches.

## License

This program is separately authorized by the KSword copyright holders and is licensed under **GPL-3.0-only**. This authorization covers DwmZOrder only and does not alter the license of the KSword main project.

See [NOTICE.md](NOTICE.md) for the copyright and authorization statement, and [LICENSE](LICENSE) for the complete license text.

For a complete advanced system maintenance tool with more features such as this, try [KSword](https://github.com/KSwordDEV/KSword).
