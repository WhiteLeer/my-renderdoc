[CmdletBinding()]
param(
  [string]$OutputFile,
  [int]$PollMilliseconds = 100,
  [int]$ObserveSeconds = 180,
  [int]$ParentPid = 0
)

$ErrorActionPreference = 'Stop'
if(-not $OutputFile) { throw '必须指定 -OutputFile' }
$parentPids = @{}
$seen = @{}
$deadline = (Get-Date).AddSeconds($ObserveSeconds)
$stopSource = "SR44ProcessStop_$PID"
$stopTraceEnabled = $false

function Write-Row([string]$Stage, $Data) {
  [pscustomobject]@{
    timestamp_utc = (Get-Date).ToUniversalTime().ToString('o')
    stage = $Stage
    data = $Data
  } | ConvertTo-Json -Compress -Depth 8 | Add-Content -LiteralPath $OutputFile -Encoding utf8
}

try {
  Register-WmiEvent -Class Win32_ProcessStopTrace -SourceIdentifier $stopSource -ErrorAction Stop | Out-Null
  $stopTraceEnabled = $true
} catch {
  Write-Row 'monitor_setup_error' @{ component = 'Win32_ProcessStopTrace'; message = $_.Exception.Message }
}

while((Get-Date) -lt $deadline) {
  if($stopTraceEnabled) {
    foreach($event in @(Get-Event -SourceIdentifier $stopSource -ErrorAction SilentlyContinue)) {
      $stop = $event.SourceEventArgs.NewEvent
      if($stop.ProcessName -match '^(?i)(StarRail|StarRail_BE|WindowsPlayer|rendertestcmd)\.exe$') {
        Write-Row 'process_stop_trace' @{
          pid = [int]$stop.ProcessID
          name = $stop.ProcessName
          parent_pid = [int]$stop.ParentProcessID
          exit_status = [uint32]$stop.ExitStatus
        }
      }
      Remove-Event -EventIdentifier $event.EventIdentifier -ErrorAction SilentlyContinue
    }
  }
  $rows = @(Get-CimInstance Win32_Process -Filter "Name = 'WindowsPlayer.exe' OR Name = 'StarRail.exe' OR Name = 'StarRail_BE.exe' OR Name = 'rendertestcmd.exe'" -ErrorAction SilentlyContinue)
  $active = @{}
  foreach($row in $rows) {
    $active[[int]$row.ProcessId] = $row
    if(-not $seen.ContainsKey([int]$row.ProcessId)) {
      $seen[[int]$row.ProcessId] = $true
      Write-Row 'process_seen' @{
        pid = [int]$row.ProcessId
        name = $row.Name
        parent_pid = [int]$row.ParentProcessId
        command_line = $row.CommandLine
        executable_path = $row.ExecutablePath
        creation_date = $row.CreationDate
        matches_parent = ($ParentPid -gt 0 -and [int]$row.ParentProcessId -eq $ParentPid)
      }
    }
  }

  foreach($seenPid in @($seen.Keys)) {
    if(-not $active.ContainsKey([int]$seenPid) -and -not $parentPids.ContainsKey([int]$seenPid)) {
      $parentPids[[int]$seenPid] = $true
      $exitCode = $null
      try {
        $proc = Get-CimInstance Win32_Process -Filter "ProcessId = $seenPid" -ErrorAction SilentlyContinue
        if($proc) { $exitCode = $proc.TerminationDate }
      } catch { }
      Write-Row 'process_disappeared' @{ pid = [int]$seenPid; exit_code = $exitCode; note = 'Process no longer present; Windows does not expose a reliable exit code after disappearance through this polling path.' }
    }
  }
  Start-Sleep -Milliseconds $PollMilliseconds
}
if($stopTraceEnabled) {
  foreach($event in @(Get-Event -SourceIdentifier $stopSource -ErrorAction SilentlyContinue)) {
    $stop = $event.SourceEventArgs.NewEvent
    if($stop.ProcessName -match '^(?i)(StarRail|StarRail_BE|WindowsPlayer|rendertestcmd)\.exe$') {
      Write-Row 'process_stop_trace' @{
        pid = [int]$stop.ProcessID
        name = $stop.ProcessName
        parent_pid = [int]$stop.ParentProcessID
        exit_status = [uint32]$stop.ExitStatus
      }
    }
    Remove-Event -EventIdentifier $event.EventIdentifier -ErrorAction SilentlyContinue
  }
  Unregister-Event -SourceIdentifier $stopSource -ErrorAction SilentlyContinue
}
Write-Row 'monitor_finished' @{ observe_seconds = $ObserveSeconds }
