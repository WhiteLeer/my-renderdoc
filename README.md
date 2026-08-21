# my-renderdoc

Personal RenderDoc fork for SR capture and replay validation.

## Upstream baseline

- Upstream repository: https://github.com/baldurk/renderdoc.git
- Imported upstream commit: `2fc0bc04cb95499635f63986a55bc6f67849dd9f`
- Imported upstream version: `v1.45`
- SR branch: `SR-4.4-MUMU`

`main` is kept as the unmodified baseline reference. SR-specific changes belong
on `SR-4.4-MUMU` or a branch derived from it.

## SR customizations

This branch is not a general-purpose RenderDoc release. Its changes are for
capturing the SR Android client running inside MuMu:

- Keeps EGL hooks enabled for the MuMu host EGL/GLES bridge even when MuMu sets
  `RENDERDOC_HOOK_EGL=0`.
- Uses the `.srrdc` extension for captures produced by this build.
- Embeds an `SR` capture-variant marker and rejects `.zzzrdc` captures, captures
  without the marker, and captures produced by another custom build.
- Updates the RenderDoc UI, replay code, command-line tools, capture filters,
  thumbnails, and save/export paths to use `.srrdc`.
- Adds the `--open-remote-manager` diagnostic startup option and skips null
  remote-device protocol controllers during device enumeration.
- Keeps Android build compatibility with JDK 9+ and treats Android shaderc as
  optional when the local NDK does not provide it.

The branch does not contain the SR asset extractor. It only provides the
RenderDoc build and capture path used to obtain `.srrdc` files for later SR
analysis.

## Build output

After building the Windows development configuration, the launcher expects the
following files under `x64\Development`:

- `renderdoccmd.exe`
- `renderdoc.dll`
- `renderdoc.json`

The graphical RenderDoc executable is built in the same configuration and can
open only SR captures produced by this branch.

## MuMu capture launcher

Launcher files:

- `tools\Start-SR-MuMuRenderDocCapture.bat`
- `tools\Start-SR-MuMuRenderDocCapture.ps1`

Double-click the BAT file. It requests administrator privileges when needed,
then opens a path dialog:

1. Select `MuMuNxMain.exe` from the MuMu installation.
2. Select `renderdoccmd.exe` from this SR RenderDoc build, normally under
   `x64\Development`.
3. Click `Start capture`.
4. Enter the SR 3D scene and use the normal RenderDoc capture hotkey, such as
   `F12` or `PrtScr`.

The launcher restarts MuMu, registers only the selected SR Vulkan implicit
layer, starts the host through `renderdoccmd capture`, and enables child-process
hooking and reference-all-resources capture options. Captures are written by
default to:

`D:\Unpack_Workspace\RenderDocTests\mumu-host\mumu-host_sr_<frame>.srrdc`

The dialog accepts either the executable itself or its containing directory.
The last successful selection is saved for the current Windows user at:

`%LOCALAPPDATA%\SR-RenderDoc\launcher-settings.json`

The launcher can also be used without the dialog:

```powershell
PowerShell.exe -NoProfile -ExecutionPolicy Bypass -File `
  .\tools\Start-SR-MuMuRenderDocCapture.ps1 `
  -NoGui `
  -MuMuRoot 'D:\Unpack_Workspace\Games_MUMU\MuMuPlayerGlobal\nx_main\MuMuNxMain.exe' `
  -RenderDocBin 'D:\Unpack_Workspace\Base_RenderDoc\my-renderdoc\x64\Development\renderdoccmd.exe'
```

`-OutputDir` can be used to override the capture output directory. Environment
variables `MUMU_ROOT`, `SR_RENDERDOC_BIN`, `RENDERDOC_TEST_OUTPUT_DIR`, and
`ZZZ_RENDERDOC_BIN` are also supported.

## Capture compatibility

Use the matching custom RenderDoc build for each capture family:

- SR build: `.srrdc`
- ZZZ build: `.zzzrdc`

Do not rename a capture to bypass the extension check. The embedded variant
marker is the authoritative check, and the SR and ZZZ replay implementations
must remain separate.
