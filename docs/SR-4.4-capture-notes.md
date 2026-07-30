# SR 4.4 capture notes

Baseline: RenderDoc v1.45, upstream commit `2fc0bc04cb95499635f63986a55bc6f67849dd9f`.

## Observed behavior

- Launching `StarRail.exe` directly remains alive and reaches the SDK login phase.
- Launching through the stock RenderDoc v1.45 capture command creates the process, but the
  process exits near the game's security initialization and no capture is produced.
- Attaching stock RenderDoc v1.45 after startup returns `ResultCode::InjectionFailed` while the
  game process remains alive.
- The diagnostic build identifies the attach failure at `VirtualAllocEx`, which returns Win32
  error 5 (`ERROR_ACCESS_DENIED`) before the DLL path is written or `LoadLibraryW` is called.
- Changing the temporary DLL-path allocation from `PAGE_EXECUTE_READWRITE` to `PAGE_READWRITE`
  produces the same error, so executable-page policy is not the cause.
- Windows reports `BlockDynamicCode=OFF` for the running target process.

## Branch changes

- Report the exact remote-load failure stage through `RDResult` instead of returning only a
  generic injection failure.
- Use `PAGE_READWRITE` for the remote DLL-path string because the buffer is data, not code.

The current evidence indicates that late attachment is denied before RenderDoc can load its DLL.
The early launch path and late-attach path therefore fail at different stages.

## Bounded launch-capture verification (2026-07-27)

Command, run from `D:\Unpack_Workspace\Games\SR_4.4`:

`D:\Unpack_Workspace\Base_RenderDoc\my-renderdoc\x64\Development\rendertestcmd.exe capture -c sr44_launch_capture D:\Unpack_Workspace\Games\SR_4.4\StarRail.exe`

Environment:

- `RENDERTEST_SR44_LAUNCH_DIAGNOSTICS=1`
- `RENDERTEST_SR44_LAUNCH_LOG=D:\Unpack_Workspace\Games\SR_4.4\Validation\renderdoc_diagnostics\sr44_launch.log`

The command reports `Launched as ID 38920`. RenderDoc also uses this target-control
ident as the successful `renderdoccmd` process status; it is not the exit code of
`StarRail.exe`.

The preserved launch log is:

`D:\Unpack_Workspace\Games\SR_4.4\Validation\renderdoc_diagnostics\sr44_exitstatus_snapshot.log`

It proves that `CreateProcessW`, `OpenProcess`, remote DLL loading, all four
configuration calls, and both `ResumeThread` calls succeed. The injected RenderDoc
DLL registers the Win32 and graphics hooks, observes two successful D3D11 device
creations, and reports the target as `STILL_ACTIVE` after the two-second observation
window.

A separate `Win32_ProcessStopTrace` probe recorded PID `2180` exiting with status
`999` about 7.1 seconds after launch. The corresponding Unity `Player.log` reaches
D3D11 initialisation, Wwise initialisation, and then stops immediately after:

`[Security] call WriteTextureStatisticUserData in static library`

The active RenderDoc log is removed when this target shuts down, so the validation
command copies it to the snapshot path before the process exits. An empty or missing
live log after shutdown must not be interpreted as evidence that launch injection
did not run.

No `.rdc` file was produced. This launch command configures the capture filename and
target-control listener but does not itself request a frame capture, so absence of an
`.rdc` is not an injection failure.

## One-shot diagnostics

The repository includes `docs/sr44-diagnostics.ps1`. It never loops or retries a target.
`Collect` only archives existing logs. `Baseline` starts `StarRail.exe` once without
RenderTest. `Capture` starts `rendertestcmd.exe capture` once and records the launcher stages.

Collect existing evidence:

`powershell -ExecutionPolicy Bypass -File docs/sr44-diagnostics.ps1 -Mode Collect`

Run one native baseline:

`powershell -ExecutionPolicy Bypass -File docs/sr44-diagnostics.ps1 -Mode Baseline -ObserveSeconds 30`

Run one explicit capture test with structured launcher logging:

`powershell -ExecutionPolicy Bypass -File docs/sr44-diagnostics.ps1 -Mode Capture -ObserveSeconds 30 -EnableWer -EnableWpr`

Each run writes to a timestamped directory under
`D:\Unpack_Workspace\Games\SR_4.4\Validation\sr44-diagnostics`. Capture runs produce
`launcher.jsonl`, `rendertest.log`, `launcher.stdout.log`, `launcher.stderr.log`,
`process-snapshot.jsonl`, and `Player.log`. WER dumps, when generated, are placed in
the run's `dumps` subdirectory; WPR produces `system.etl`.

The JSONL stages distinguish process creation, remote load, configuration calls,
resume, observation, and target exit. A missing dump with a target exit is consistent
with an intentional exit or an external termination; it is not by itself proof of a
specific protection check.

For comparison, the ordinary D3D11 sample used:

`D:\Unpack_Workspace\Base_RenderDoc\my-renderdoc\x64\Development\rendertestcmd.exe capture -c demo_capture demos_x64.exe D3D11_Simple_Triangle`

Its log shows successful `CreateProcessW`, `OpenProcess`, remote DLL load, all four
configuration calls, and both `ResumeThread` calls. No `.rdc` file was found in the
SR 4.4 directory after that sample run.
