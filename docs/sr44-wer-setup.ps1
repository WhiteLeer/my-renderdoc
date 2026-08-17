[CmdletBinding()]
param(
  [string]$DumpFolder = 'D:\Unpack_Workspace\Games\SR_4.4\Validation\sr44-diagnostics\wer-dumps',
  [int]$DumpCount = 3
)

$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force -Path $DumpFolder | Out-Null
$names = @('StarRail.exe', 'WindowsPlayer.exe', 'StarRail_BE.exe', 'rendertestcmd.exe')
$rows = foreach($name in $names) {
  $key = "HKCU:\Software\Microsoft\Windows\Windows Error Reporting\LocalDumps\$name"
  New-Item -Path $key -Force | Out-Null
  New-ItemProperty -Path $key -Name DumpFolder -PropertyType ExpandString -Value $DumpFolder -Force | Out-Null
  New-ItemProperty -Path $key -Name DumpType -PropertyType DWord -Value 2 -Force | Out-Null
  New-ItemProperty -Path $key -Name DumpCount -PropertyType DWord -Value $DumpCount -Force | Out-Null
  [pscustomobject]@{ process = $name; key = $key; dump_folder = $DumpFolder; dump_type = 2; dump_count = $DumpCount }
}
$rows | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $DumpFolder 'wer-config.json') -Encoding utf8
$rows | Format-Table -AutoSize
