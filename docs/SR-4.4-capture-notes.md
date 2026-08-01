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
