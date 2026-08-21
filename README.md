# HSR-4.4-MUMU RenderDoc

This branch is a RenderDoc build dedicated to capturing and replaying
Honkai: Star Rail (HSR) running inside MuMu. It is not the HSR asset extractor;
its output is the `.srrdc` capture used for later HSR resource and shader
analysis.

## What Changed

- Keeps EGL hooks enabled for MuMu's host EGL/GLES bridge, which is required to
  capture the real HSR rendering instead of an empty host shell.
- Uses the `.srrdc` extension and embeds the technical `SR` variant marker for
  compatibility with existing captures.
- Rejects captures without the matching embedded variant marker before replay.
- Updates the RenderDoc UI and command-line tools to open, save, and export
  `.srrdc` files.
- Adds MuMu capture diagnostics and avoids null remote-device controllers during
  device enumeration.

## Build Requirements

Build the Windows development configuration and make sure these files exist in
`x64\Development`:

- `renderdoccmd.exe`
- `renderdoc.dll`
- `renderdoc.json`

MuMu must also be installed and contain `nx_main\MuMuNxMain.exe` and
`nx_main\mumu-cli.exe`.

## Capture HSR

1. Double-click `tools\Start-SR-MuMuRenderDocCapture.bat`.
2. In the dialog, select `MuMuNxMain.exe`.
3. Select this build's `x64\Development\renderdoccmd.exe`.
4. Click `Start capture` and wait for the launcher to report that injection is
   ready.
5. Enter the HSR 3D scene and press the normal RenderDoc capture hotkey, such as
   `F12` or `PrtScr`.

The launcher requests administrator privileges when required, restarts MuMu,
registers the selected HSR Vulkan layer, and starts the MuMu host through
RenderDoc. It enables child-process hooking and reference-all-resources capture
options.

Captures are written by default to:

`D:\Unpack_Workspace\RenderDocTests\mumu-host\mumu-host_sr_<frame>.srrdc`

The selected paths are remembered for the current Windows user at
`%LOCALAPPDATA%\SR-RenderDoc\launcher-settings.json`.

The launcher also accepts a directory instead of an EXE. For unattended use:

```powershell
PowerShell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\Start-SR-MuMuRenderDocCapture.ps1 `
  -NoGui `
  -MuMuRoot 'D:\Unpack_Workspace\Games_MUMU\MuMuPlayerGlobal\nx_main\MuMuNxMain.exe' `
  -RenderDocBin 'D:\Unpack_Workspace\Base_RenderDoc\my-renderdoc\x64\Development\renderdoccmd.exe'
```

Use the HSR build to open `.srrdc` files. Renaming a capture does not change its
embedded variant marker; captures must be opened with the matching build.

## Known Limitations

Replay stability is still under investigation. Switching frames rapidly after
opening an `.srrdc` capture can trigger a GPU device-lost error or crash. Slower
navigation reduces the risk but does not eliminate it.

The issue is not present in every capture. Some captures open normally and
provide the complete rendering data needed for analysis, which is why this build
and selected captures are published for research and validation. Keep a backup
of important captures and avoid rapid frame switching until the replay issue is
resolved.
