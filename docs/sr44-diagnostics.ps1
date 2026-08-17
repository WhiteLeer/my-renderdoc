[CmdletBinding()]
param(
  [ValidateSet('Collect', 'Baseline', 'Capture')]
  [string]$Mode = 'Collect',
  [string]$GameRoot = 'D:\Unpack_Workspace\Games\SR_4.4',
  [string]$ToolRoot = 'D:\Unpack_Workspace\Base_RenderDoc\my-renderdoc\x64\Development',
  [string]$OutputRoot = 'D:\Unpack_Workspace\Games\SR_4.4\Validation\sr44-diagnostics',
  [int]$ObserveSeconds = 30,
  [int]$PollMilliseconds = 250,
  [switch]$EnableWer,
  [switch]$EnableWpr
)

$ErrorActionPreference = 'Stop'
$game = Join-Path $GameRoot 'StarRail.exe'
$tool = Join-Path $ToolRoot 'rendertestcmd.exe'
$playerLog = Join-Path $env:USERPROFILE 'AppData\LocalLow\Cognosphere\Star Rail\Player.log'
$run = Join-Path $OutputRoot (Get-Date -Format 'yyyyMMdd_HHmmss')
$capturePrefix = Join-Path $run 'sr44'
$jsonl = Join-Path $run 'launcher.jsonl'
$textLog = Join-Path $run 'rendertest.log'
$stdout = Join-Path $run 'launcher.stdout.log'
$stderr = Join-Path $run 'launcher.stderr.log'
$snapshot = Join-Path $run 'process-snapshot.jsonl'
$wprFile = Join-Path $run 'system.etl'
$werDump = Join-Path $run 'dumps'
$runStarted = Get-Date
$trackedProcesses = @{}
$launcherExitRecorded = $false

New-Item -ItemType Directory -Force -Path $run | Out-Null
New-Item -ItemType Directory -Force -Path $werDump | Out-Null

function Write-Record([string]$Stage, [object]$Data) {
  [pscustomobject]@{
    timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
    stage = $Stage
    data = $Data
  } | ConvertTo-Json -Compress -Depth 8 | Add-Content -LiteralPath $snapshot -Encoding utf8
}

function Get-TargetSnapshot {
  $rows = @()
  $procs = Get-Process -Name StarRail,StarRail_BE,rendertestcmd -ErrorAction SilentlyContinue
  foreach($p in $procs) {
    $modules = @()
    try {
      $modules = @($p.Modules | Where-Object {
        $_.ModuleName -match '(?i)rendertest|renderdoc|UnityPlayer|GameAssembly|HoYo|d3d11|dxgi'
      } | ForEach-Object {
        [pscustomobject]@{
          name = $_.ModuleName
          path = $_.FileName
          version = $_.FileVersionInfo.FileVersion
        }
      })
    } catch {
      $modules = @([pscustomobject]@{ error = $_.Exception.Message })
    }
    $rows += [pscustomobject]@{
      pid = $p.Id
      name = $p.ProcessName
      path = $p.Path
      start_time = $p.StartTime
      responding = $p.Responding
      modules = $modules
    }
  }
  return $rows
}

function Configure-Wer {
  # Unity player runs as WindowsPlayer.exe in this build. StarRail.exe is only
  # the launcher/container name, so configure both names plus the known backend.
  $processNames = @('StarRail.exe', 'WindowsPlayer.exe', 'StarRail_BE.exe', 'rendertestcmd.exe')
  $configured = @()
  foreach($processName in $processNames) {
    $key = "HKCU:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\$processName"
    New-Item -Path $key -Force | Out-Null
    New-ItemProperty -Path $key -Name DumpFolder -PropertyType ExpandString -Value $werDump -Force | Out-Null
    New-ItemProperty -Path $key -Name DumpType -PropertyType DWord -Value 2 -Force | Out-Null
    New-ItemProperty -Path $key -Name DumpCount -PropertyType DWord -Value 3 -Force | Out-Null
    $configured += [pscustomobject]@{ process = $processName; key = $key; dump_folder = $werDump }
  }
  Write-Record 'wer_configured' @{ processes = $configured }
}

function Start-WprTrace {
  & wpr.exe -start GeneralProfile -filemode 2>&1 | Add-Content -LiteralPath (Join-Path $run 'wpr-start.log')
  if($LASTEXITCODE -ne 0) { throw "wpr.exe -start failed with exit code $LASTEXITCODE" }
  Write-Record 'wpr_started' @{ profile = 'GeneralProfile'; output = $wprFile }
}

function Stop-WprTrace {
  & wpr.exe -stop $wprFile 2>&1 | Add-Content -LiteralPath (Join-Path $run 'wpr-stop.log')
  Write-Record 'wpr_stopped' @{ output = $wprFile; exit_code = $LASTEXITCODE }
}

function ConvertTo-WindowsArgument([string]$Value) {
  # This script also runs under Windows PowerShell 5.1, which lacks ArgumentList.
  if($Value -notmatch '[\s"]') { return $Value }
  return '"' + $Value.Replace('"', '\"') + '"'
}

function Write-LauncherOutput {
  if($outTask) {
    try {
      if($launcher.HasExited -or $outTask.IsCompleted) {
        [IO.File]::WriteAllText($stdout, $outTask.GetAwaiter().GetResult())
      } else {
        Write-Record 'stdout_pending' @{ message = 'Launcher is still running; stdout is not closed yet' }
      }
    } catch { Write-Record 'stdout_error' @{ message = $_.Exception.Message } }
  }
  if($errTask) {
    try {
      if($launcher.HasExited -or $errTask.IsCompleted) {
        [IO.File]::WriteAllText($stderr, $errTask.GetAwaiter().GetResult())
      } else {
        Write-Record 'stderr_pending' @{ message = 'Launcher is still running; stderr is not closed yet' }
      }
    } catch { Write-Record 'stderr_error' @{ message = $_.Exception.Message } }
  }
}

function Get-ProcessDetails($Process) {
  $path = $null
  $startTime = $null
  try { $path = $Process.Path } catch { }
  try { $startTime = $Process.StartTime.ToString('o') } catch { }
  return @{
    pid = $Process.Id
    name = $Process.ProcessName
    path = $path
    start_time = $startTime
  }
}

function Track-TargetProcesses {
  $processes = @(Get-Process -Name StarRail,StarRail_BE -ErrorAction SilentlyContinue)
  foreach($process in $processes) {
    if(-not $trackedProcesses.ContainsKey($process.Id)) {
      $trackedProcesses[$process.Id] = $process
      Write-Record 'target_process_seen' (Get-ProcessDetails $process)
    }
  }

  foreach($processId in @($trackedProcesses.Keys)) {
    $process = $trackedProcesses[$processId]
    try {
      if($process.HasExited) {
        $exitCode = $null
        try {
          $process.Refresh()
          $exitCode = $process.ExitCode
        } catch { }
        $details = Get-ProcessDetails $process
        $details['exit_code'] = $exitCode
        $details['exit_code_hex'] = if($null -eq $exitCode) { $null } else { '0x{0:X8}' -f ([uint32]$exitCode) }
        $details['exit_code_source'] = 'powershell_process_handle'
        Write-Record 'target_process_exit' $details
        $trackedProcesses.Remove($processId)
      }
    } catch {
      Write-Record 'target_process_exit_query_error' @{ pid = $processId; message = $_.Exception.Message }
      $trackedProcesses.Remove($processId)
    }
  }
}

function Export-RecentWindowsEvents {
  $eventFile = Join-Path $run 'windows-events.jsonl'
  $end = Get-Date
  foreach($logName in @('Application', 'System')) {
    try {
      Get-WinEvent -FilterHashtable @{ LogName = $logName; StartTime = $runStarted.AddSeconds(-2); EndTime = $end } -ErrorAction Stop |
        Where-Object {
          $_.ProviderName -match '(?i)Windows Error Reporting|Application Error|.NET Runtime' -or
          $_.Message -match '(?i)StarRail|StarRail_BE|rendertest|renderdoc|faulting application|terminated unexpectedly'
        } |
        ForEach-Object {
          [pscustomobject]@{
            log = $logName
            time = $_.TimeCreated.ToUniversalTime().ToString('o')
            provider = $_.ProviderName
            id = $_.Id
            level = $_.LevelDisplayName
            message = $_.Message
          } | ConvertTo-Json -Compress -Depth 8 | Add-Content -LiteralPath $eventFile -Encoding utf8
        }
    } catch {
      Write-Record 'windows_event_query_error' @{ log = $logName; message = $_.Exception.Message }
    }
  }
  if(Test-Path $eventFile) { Write-Record 'windows_events_exported' @{ output = $eventFile } }
}

if($EnableWer) { Configure-Wer }
if($EnableWpr) { Start-WprTrace }

Write-Record 'collection_started' @{ mode = $Mode; game = $game; tool = $tool }
Get-TargetSnapshot | ForEach-Object { Write-Record 'initial_process_snapshot' $_ }

$launcher = $null
try {
  if($Mode -eq 'Baseline') {
    if(-not (Test-Path $game)) { throw "Game executable not found: $game" }
    $launcher = Start-Process -FilePath $game -WorkingDirectory $GameRoot -PassThru
    Write-Record 'baseline_started' @{ pid = $launcher.Id }
  } elseif($Mode -eq 'Capture') {
    if(-not (Test-Path $tool)) { throw "RenderTest command executable not found: $tool" }
    if(-not (Test-Path $game)) { throw "Game executable not found: $game" }
    $psi = [Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = $tool
    $psi.WorkingDirectory = $GameRoot
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $arguments = @('capture', '-w', '-c', $capturePrefix, $game) |
      ForEach-Object { ConvertTo-WindowsArgument $_ }
    $psi.Arguments = $arguments -join ' '
    $psi.Environment['RENDERTEST_SR44_LAUNCH_DIAGNOSTICS'] = '1'
    $psi.Environment['RENDERTEST_SR44_LAUNCH_LOG'] = $textLog
    $psi.Environment['RENDERTEST_SR44_LAUNCH_JSONL'] = $jsonl
    $psi.Environment['RENDERTEST_SR44_LAUNCH_OBSERVE_MS'] = '10000'
    $launcher = [Diagnostics.Process]::Start($psi)
    if(-not $launcher) { throw 'Process.Start returned no process object' }
    Write-Record 'capture_started' @{ pid = $launcher.Id; command = "$tool capture -w -c $capturePrefix $game" }
    $outTask = $launcher.StandardOutput.ReadToEndAsync()
    $errTask = $launcher.StandardError.ReadToEndAsync()
  }

  if($Mode -ne 'Collect') {
    $deadline = (Get-Date).AddSeconds($ObserveSeconds)
    while((Get-Date) -lt $deadline) {
      Track-TargetProcesses
      Get-TargetSnapshot | ForEach-Object { Write-Record 'process_snapshot' $_ }
      if($Mode -eq 'Capture' -and $launcher -and $launcher.HasExited -and -not $launcherExitRecorded) {
        Write-LauncherOutput
        Write-Record 'capture_launcher_exit' @{ pid = $launcher.Id; exit_code = $launcher.ExitCode }
        $launcherExitRecorded = $true
      }
      Start-Sleep -Milliseconds $PollMilliseconds
    }

    if($Mode -eq 'Capture' -and $launcher) {
      Track-TargetProcesses
      if($launcher.HasExited -and -not $launcherExitRecorded) {
        Write-LauncherOutput
        Write-Record 'capture_launcher_exit' @{ pid = $launcher.Id; exit_code = $launcher.ExitCode }
        $launcherExitRecorded = $true
      } else {
        Write-Record 'capture_observation_timeout' @{ pid = $launcher.Id; observe_seconds = $ObserveSeconds; still_running = -not $launcher.HasExited }
      }
    }
  }
} catch {
  Write-Record 'collection_error' @{
    message = $_.Exception.Message
    type = $_.Exception.GetType().FullName
    script_stack = $_.ScriptStackTrace
  }
  Write-LauncherOutput
  throw
} finally {
  if($Mode -eq 'Capture' -and $launcher) { Write-LauncherOutput }
  Track-TargetProcesses
  Get-TargetSnapshot | ForEach-Object { Write-Record 'final_process_snapshot' $_ }
  if($EnableWpr) { Stop-WprTrace }
  if(Test-Path $playerLog) { Copy-Item -LiteralPath $playerLog -Destination (Join-Path $run 'Player.log') -Force }
  $existingDiagnostics = Join-Path $GameRoot 'Validation\renderdoc_diagnostics'
  if(Test-Path $existingDiagnostics) {
    $existingOutput = Join-Path $run 'existing-renderdoc-diagnostics'
    New-Item -ItemType Directory -Force -Path $existingOutput | Out-Null
    Get-ChildItem -LiteralPath $existingDiagnostics -File -ErrorAction SilentlyContinue |
      Copy-Item -Destination $existingOutput -Force -ErrorAction SilentlyContinue
  }
  $captureDir = Join-Path $GameRoot 'Validation\RenderDoc'
  if(Test-Path $captureDir) {
    Get-ChildItem -LiteralPath $captureDir -File -Filter '*.rdc' -ErrorAction SilentlyContinue |
      Copy-Item -Destination $run -Force -ErrorAction SilentlyContinue
  }
  Export-RecentWindowsEvents
  Write-Record 'collection_finished' @{ output = $run }
  Write-Output $run
}
