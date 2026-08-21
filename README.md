# ZZZ-3.0-MUMU RenderDoc

This branch is a game-specific RenderDoc build for capturing and replaying
Zenless Zone Zero (ZZZ) running inside MuMu. It is intended for later frame,
shader, and rendering-resource analysis, not for extracting game assets.

## What Changed

- Keeps the MuMu EGL hooks required to capture the renderer instead of the
  empty host shell.
- Captures MuMu's offscreen Vulkan submissions through
  `RENDERDOC_CAPTURE_OFFSCREEN_SUBMIT=1`, which is the key difference from the
  HSR capture path.
- Filters MuMu utility and crash-reporting child processes while preserving
  injection into the rendering processes.
- Uses `.zzzrdc` files and embeds the `ZZZ` capture-variant marker so captures
  from incompatible game-specific builds are not silently replayed.
- Provides a configurable MuMu launcher that selects the ZZZ RenderDoc build,
  registers its Vulkan layer, restarts MuMu, and verifies the loaded DLL.
- Removes temporary diagnostic file logging from the capture path.

## Build Requirements

Build the Windows development configuration and make sure these files exist in
`x64\Development`:

- `renderdoccmd.exe`
- `renderdoc.dll`
- `renderdoc.json`

MuMu must also contain:

- `nx_main\MuMuNxMain.exe`
- `nx_main\mumu-cli.exe`

## Capture ZZZ

1. Double-click `tools\Start-ZZZ-MuMuRenderDocCapture.bat`.
2. Select `MuMuNxMain.exe` in the first field.
3. Select this branch's `x64\Development\renderdoccmd.exe` in the second field.
4. Click `Start capture` and wait for the launcher to report that injection is
   ready.
5. Enter a ZZZ 3D scene and press the normal RenderDoc capture hotkey, such as
   `F12` or `PrtScr`.

The launcher requests administrator privileges when necessary, replaces the
local Vulkan implicit-layer registration with the selected build, restarts
MuMu, hooks its child processes, and writes captures to:

`D:\Unpack_Workspace\RenderDocTests\mumu-host\mumu-host_zzz_<frame>.zzzrdc`

Selected paths are remembered for the current Windows user at
`%LOCALAPPDATA%\ZZZ-RenderDoc\launcher-settings.json`.

For unattended use, the PowerShell launcher accepts an executable or its
containing directory:

```powershell
PowerShell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\Start-ZZZ-MuMuRenderDocCapture.ps1 `
  -NoGui `
  -MuMuRoot 'D:\Unpack_Workspace\Games_MUMU\MuMuPlayerGlobal\nx_main\MuMuNxMain.exe' `
  -RenderDocBin 'D:\Unpack_Workspace\Base_RenderDoc_ZZZ\my-renderdoc\x64\Development\renderdoccmd.exe'
```

Open `.zzzrdc` files with the matching ZZZ build. Renaming a file does not
change its embedded variant marker.

## Known Limitations

Replay stability is still under investigation. Switching frames rapidly after
opening a `.zzzrdc` capture can trigger a GPU device-lost error or crash.
Slower navigation reduces the risk but does not eliminate it, and some
captures may still fail depending on their contents. Keep backups of useful
captures and avoid unnecessary frame switching during analysis.
