# HSR-4.4-MUMU RenderDoc

This branch is a RenderDoc build dedicated to capturing and replaying
Honkai: Star Rail (HSR) running inside MuMu. It is not the HSR asset extractor;
its output is the `.srrdc` capture used for later HSR resource and shader
analysis.

![HSR RenderDoc Mesh Preview](docs/images/hsr-renderdoc-mesh-preview.png)

## What Changed

- Keeps EGL hooks enabled for MuMu's host EGL/GLES bridge, which is required to
  capture the real HSR rendering instead of an empty host shell.
- Uses the `.srrdc` extension and embeds the technical `SR` variant marker for
  compatibility with existing captures.
- Rejects captures without the matching embedded variant marker before replay.
- Updates the RenderDoc UI and command-line tools to open and save `.srrdc`
  files.
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

## Mesh Preview Stability

The Vulkan `VK_ERROR_DEVICE_LOST` crash that could occur when opening Mesh
Preview or switching DRAW events has been fixed in the current build.

For Vulkan captures, the unstable post-output replay path is intentionally
disabled in Mesh Preview. This includes replaying vertex-shader/geometry-shader
post-VS outputs and task/mesh-shader post outputs. The normal input vertex data
path, texture and output viewing, pipeline inspection, and DRAW/event
navigation remain available.

This keeps the capture analysis workflow usable while avoiding the replay
operation that could trigger a GPU watchdog reset on the affected driver and
capture combination.

## Known Limitations

Vulkan post-output mesh preview is not currently available because that replay
path is disabled for stability. Captures should still be opened with the
matching build and important captures should be backed up before analysis.
