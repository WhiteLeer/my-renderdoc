[CmdletBinding()]
param(
  [string]$ExecutablePath = 'D:\Unpack_Workspace\Games\SR_4.4\StarRail.exe',
  [string]$WorkingDirectory = '',
  [string[]]$Argument = @(),
  [string]$OutputRoot = 'D:\Unpack_Workspace\Games\SR_4.4\Validation\sr44-diagnostics',
  [string]$CdbPath = '',
  [int]$TimeoutSeconds = 60,
  [switch]$LoadMicrosoftSymbols,
  [int]$MonitorSeconds = 180,
  [int]$MonitorGraceSeconds = 30,
  [switch]$EnableWpr,
  [switch]$NoDump
)

$ErrorActionPreference = 'Stop'

function Find-Cdb {
  if($CdbPath -and (Test-Path -LiteralPath $CdbPath)) { return (Resolve-Path -LiteralPath $CdbPath).Path }

  $candidates = @(
    'D:\Unpack_Workspace\Tools\WinDbg\amd64\cdb.exe',
    'C:\Program Files\Windows Kits\10\Debuggers\x64\cdb.exe',
    'C:\Program Files (x86)\Windows Kits\10\Debuggers\x64\cdb.exe'
  )
  $winDbgRoot = 'C:\Program Files\WindowsApps'
  if(Test-Path -LiteralPath $winDbgRoot) {
    $candidates += Get-ChildItem -LiteralPath $winDbgRoot -Directory -Filter 'Microsoft.WinDbg_*_x64__*' -ErrorAction SilentlyContinue |
      Sort-Object Name -Descending |
      ForEach-Object { Join-Path $_.FullName 'amd64\cdb.exe' }
  }
  foreach($candidate in $candidates) {
    if(Test-Path -LiteralPath $candidate) { return (Resolve-Path -LiteralPath $candidate).Path }
  }
  throw '找不到 cdb.exe。请通过 -CdbPath 指定路径。'
}

function Quote-CdbString([string]$Value) {
  return $Value.Replace('\\', '\\').Replace('"', '\\"')
}

if(-not (Test-Path -LiteralPath $ExecutablePath)) {
  throw "目标程序不存在: $ExecutablePath"
}
$ExecutablePath = (Resolve-Path -LiteralPath $ExecutablePath).Path
if(-not $WorkingDirectory) { $WorkingDirectory = Split-Path -Parent $ExecutablePath }
$WorkingDirectory = (Resolve-Path -LiteralPath $WorkingDirectory).Path
$cdb = Find-Cdb

$run = Join-Path $OutputRoot ('cdb-exit-' + (Get-Date -Format 'yyyyMMdd_HHmmss'))
New-Item -ItemType Directory -Force -Path $run | Out-Null
$log = Join-Path $run 'cdb.log'
$commands = Join-Path $run 'cdb.commands.txt'
$dump = Join-Path $run 'exit.dmp'
$meta = Join-Path $run 'probe.json'
$monitor = Join-Path $run 'process-monitor.jsonl'
$wprFile = Join-Path $run 'sr44-process.etl'
$wprStartLog = Join-Path $run 'wpr-start.log'
$wprStopLog = Join-Path $run 'wpr-stop.log'
$wprCsv = Join-Path $run 'sr44-process.csv'
$launchLog = Join-Path $run 'rendertest-launch.log'
$launchJson = Join-Path $run 'rendertest-launch.jsonl'
$wprProfile = Join-Path $PSScriptRoot 'sr44-process.wprp'
$logmanSession = 'SR44Process_' + (Get-Date -Format 'yyyyMMdd_HHmmss')

$dumpCommand = if($NoDump) { '.echo [DUMP] disabled' } else {
  # OutputRoot is normally on D:\Unpack_Workspace and contains no spaces.
  # Do not nest quotes inside the breakpoint command string; CDB treats them
  # as the end of the command action.
  ".dump /ma $dump"
}

$symbolCommands = if($LoadMicrosoftSymbols) {
  @('.symfix', '.sympath+ srv*C:\Symbols*https://msdl.microsoft.com/download/symbols')
} else {
  @('.sympath')
}

# Use bm (match) instead of bu: bu resolves against the parent process ntdll/kernelbase
# base. StarRail.exe is a child with its own ASLR slide, so parent-resolved bu addresses
# never fire in the child. bm re-matches by symbol name in each process.
# Dump commands include | / || so the log shows which process actually faulted.
$bpSuffix = '; |; ||; .lastevent; kv 40; ' + $dumpCommand
$commandLines = @(
  $symbolCommands
  # rendertestcmd creates StarRail.exe. Keep the debugger attached to that child.
  '.childdbg 1'
  '.echo ==== SR44 child debugging enabled ===='
  'sxd 80000003'
  'sxd 80000002'
  '.echo ==== SR44 exit probe started ===='
  '.echo Target: ' + $ExecutablePath
  '.echo PID: ${$pid}'
  # Do not use gc here. A hit must leave CDB stopped with the faulting context intact.
  'bm ntdll!NtTerminateProcess ".echo [EXIT] ntdll!NtTerminateProcess; r rcx; r rdx' + $bpSuffix + '"'
  'bm ntdll!RtlExitUserProcess ".echo [EXIT] ntdll!RtlExitUserProcess; r rcx' + $bpSuffix + '"'
  'bm ntdll!RtlFailFast2 ".echo [EXIT] ntdll!RtlFailFast2' + $bpSuffix + '"'
  'bm kernelbase!TerminateProcess ".echo [EXIT] kernelbase!TerminateProcess; r rcx; r rdx' + $bpSuffix + '"'
  'bm kernelbase!RaiseFailFastException ".echo [EXIT] kernelbase!RaiseFailFastException' + $bpSuffix + '"'
  'bm ntdll!ZwTerminateProcess ".echo [EXIT] ntdll!ZwTerminateProcess; r rcx; r rdx' + $bpSuffix + '"'
  # Exception events (apply to current + subsequent child processes with .childdbg)
  'sxe -c ".echo [EXCEPTION] access violation' + $bpSuffix + '" c0000005'
  'sxe -c ".echo [EXCEPTION] fail fast /GS' + $bpSuffix + '" c0000409'
  'sxe -c ".echo [EXCEPTION] illegal instruction' + $bpSuffix + '" c000001d'
  'g'
  '.echo ==== SR44 exit probe finished ===='
  'q'
)
[IO.File]::WriteAllLines($commands, $commandLines, [Text.UTF8Encoding]::new($false))

if($EnableWpr) {
  # WPRP is kept as documentation, but the inbox WPR on this machine rejects
  # custom provider references. logman starts the same lightweight kernel
  # process provider reliably and produces a small ETL.
  & logman.exe stop $logmanSession -ets 2>&1 | Out-Null
  & logman.exe delete $logmanSession 2>&1 | Out-Null
  & logman.exe create trace $logmanSession -o $wprFile -f bincirc -max 256 -nb 16 64 -p 'Microsoft-Windows-Kernel-Process' 0x70 5 -ets 2>&1 |
    Set-Content -LiteralPath $wprStartLog -Encoding utf8
  if($LASTEXITCODE -ne 0) { throw "logman create failed: $LASTEXITCODE" }
}

$monitorScript = Join-Path $PSScriptRoot 'sr44-process-monitor.ps1'
$monitorProcess = $null
if(Test-Path -LiteralPath $monitorScript) {
  $monitorArgs = @('-NoProfile', '-ExecutionPolicy', 'Bypass', '-File', $monitorScript,
                   '-OutputFile', $monitor, '-ObserveSeconds', $MonitorSeconds)
  $monitorError = Join-Path $run 'process-monitor.stderr.log'
  $monitorProcess = Start-Process -FilePath 'powershell.exe' -ArgumentList $monitorArgs -WindowStyle Hidden -PassThru -RedirectStandardError $monitorError
}

$psi = [Diagnostics.ProcessStartInfo]::new()
$psi.FileName = $cdb
$psi.WorkingDirectory = $WorkingDirectory
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$psi.RedirectStandardOutput = $true
$psi.RedirectStandardError = $true
$psi.Environment['RENDERTEST_SR44_LAUNCH_DIAGNOSTICS'] = '1'
$psi.Environment['RENDERTEST_SR44_LAUNCH_LOG'] = $launchLog
$psi.Environment['RENDERTEST_SR44_LAUNCH_JSONL'] = $launchJson
$psi.Environment['RENDERTEST_SR44_LAUNCH_OBSERVE_MS'] = '15000'
$argList = @('-o', '-G', '-cf', $commands, '-logo', $log, $ExecutablePath) + $Argument
if($psi.PSObject.Properties.Name -contains 'ArgumentList') {
  foreach($arg in $argList) { [void]$psi.ArgumentList.Add($arg) }
} else {
  $psi.Arguments = ($argList | ForEach-Object {
    if($_ -notmatch '[\s"]') { $_ } else { '"' + $_.Replace('"', '\\"') + '"' }
  }) -join ' '
}

$started = Get-Date
$proc = [Diagnostics.Process]::Start($psi)
if(-not $proc) { throw '无法启动 CDB。' }
$stdoutTask = $proc.StandardOutput.ReadToEndAsync()
$stderrTask = $proc.StandardError.ReadToEndAsync()
$completed = $proc.WaitForExit($TimeoutSeconds * 1000)
if(-not $completed) {
  $proc.Kill()
  $status = 'timeout'
} else { $status = 'completed' }

$stdout = $stdoutTask.GetAwaiter().GetResult()
$stderr = $stderrTask.GetAwaiter().GetResult()
if($monitorProcess -and -not $monitorProcess.HasExited) {
  # CDB may lose the protected target context before the target exits. Keep
  # the independent lifecycle monitor alive long enough to observe that exit.
  $graceDeadline = (Get-Date).AddSeconds($MonitorGraceSeconds)
  while(-not $monitorProcess.HasExited -and (Get-Date) -lt $graceDeadline) {
    Start-Sleep -Milliseconds 250
  }
  if(-not $monitorProcess.HasExited) {
    Stop-Process -Id $monitorProcess.Id -Force -ErrorAction SilentlyContinue
  }
}
if($EnableWpr) {
  & logman.exe stop $logmanSession -ets 2>&1 | Set-Content -LiteralPath $wprStopLog -Encoding utf8
  if(Test-Path -LiteralPath $wprFile) {
    & tracerpt.exe $wprFile -o $wprCsv -of CSV 2>&1 | Set-Content -LiteralPath (Join-Path $run 'tracerpt.log') -Encoding utf8
  }
  & logman.exe delete $logmanSession 2>&1 | Out-Null
}
[IO.File]::WriteAllText((Join-Path $run 'cdb.stdout.log'), $stdout)
[IO.File]::WriteAllText((Join-Path $run 'cdb.stderr.log'), $stderr)

[pscustomobject]@{
  started_utc = $started.ToUniversalTime().ToString('o')
  finished_utc = (Get-Date).ToUniversalTime().ToString('o')
  status = $status
  cdb = $cdb
  executable = $ExecutablePath
  working_directory = $WorkingDirectory
  arguments = $Argument
  output = $run
  launch_log = if(Test-Path -LiteralPath $launchLog) { $launchLog } else { $null }
  launch_jsonl = if(Test-Path -LiteralPath $launchJson) { $launchJson } else { $null }
  cdb_exit_code = if($completed) { $proc.ExitCode } else { $null }
  dump = if(Test-Path -LiteralPath $dump) { $dump } else { $null }
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $meta -Encoding utf8

Write-Output $run
