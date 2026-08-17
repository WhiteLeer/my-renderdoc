[CmdletBinding()]
param(
  [int]$VmIndex = 0,
  [string]$RenderDocBin,
  [string]$MuMuRoot,
  [string]$OutputDir,
  [string]$OtherRenderDocBin,
  [switch]$NoGui
)

$ErrorActionPreference = 'Stop'

function Resolve-RenderDocBin([string]$Path, [string]$DefaultPath) {
  if([string]::IsNullOrWhiteSpace($Path)) { $Path = $env:SR_RENDERDOC_BIN }
  if([string]::IsNullOrWhiteSpace($Path)) { $Path = $DefaultPath }

  $candidate = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
  if((Get-Item -LiteralPath $candidate).PSIsContainer -eq $false) {
    $candidate = Split-Path -Parent $candidate
  }
  if(Test-Path -LiteralPath (Join-Path $candidate 'renderdoccmd.exe')) { return $candidate }
  if(Test-Path -LiteralPath (Join-Path $candidate 'x64\Development\renderdoccmd.exe')) {
    return Join-Path $candidate 'x64\Development'
  }
  throw "RenderDoc bin directory is invalid: $Path"
}

function Resolve-MuMuRoot([string]$Path, [string]$DefaultPath) {
  if([string]::IsNullOrWhiteSpace($Path)) { $Path = $env:MUMU_ROOT }
  if([string]::IsNullOrWhiteSpace($Path)) { $Path = $DefaultPath }

  $candidate = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
  if((Get-Item -LiteralPath $candidate).PSIsContainer -eq $false) {
    $candidate = Split-Path -Parent $candidate
  }
  if((Split-Path -Leaf $candidate) -ieq 'nx_main') {
    $candidate = Split-Path -Parent $candidate
  }
  if((Test-Path -LiteralPath (Join-Path $candidate 'nx_main\mumu-cli.exe')) -and
     (Test-Path -LiteralPath (Join-Path $candidate 'nx_main\MuMuNxMain.exe'))) {
    return $candidate
  }
  throw "MuMu root is invalid: $Path"
}

function Read-LauncherSettings([string]$Path) {
  if(!(Test-Path -LiteralPath $Path)) { return $null }
  try {
    return (Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json)
  } catch {
    return $null
  }
}

function Save-LauncherSettings([string]$Path, [string]$MuMuRoot, [string]$RenderDocBin) {
  try {
    $parent = Split-Path -Parent $Path
    New-Item -ItemType Directory -Force -Path $parent | Out-Null
    [ordered]@{
      MuMuRoot = $MuMuRoot
      RenderDocBin = $RenderDocBin
    } | ConvertTo-Json | Set-Content -LiteralPath $Path -Encoding UTF8
  } catch {
    Write-Warning "Could not save launcher settings: $($_.Exception.Message)"
  }
}

function Get-RememberedExecutablePath([string]$Path, [string]$RelativeExecutable) {
  if([string]::IsNullOrWhiteSpace($Path)) { return $Path }
  $item = Get-Item -LiteralPath $Path -ErrorAction SilentlyContinue
  if($item -and $item.PSIsContainer) {
    $candidate = Join-Path $item.FullName $RelativeExecutable
    if(Test-Path -LiteralPath $candidate) { return $candidate }
  }
  return $Path
}

function Show-PathDialog([string]$InitialMuMuRoot, [string]$InitialRenderDocBin) {
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type -AssemblyName System.Drawing
  [System.Windows.Forms.Application]::EnableVisualStyles()
  try {
    Add-Type -TypeDefinition @'
using System;
using System.Runtime.InteropServices;
public static class SrRenderDocDpi {
  [DllImport("user32.dll")]
  public static extern bool SetProcessDPIAware();
}
'@ -ErrorAction Stop
    [SrRenderDocDpi]::SetProcessDPIAware() | Out-Null
  } catch {
    # Older Windows versions may not expose the DPI API; the form remains usable.
  }

  $form = New-Object System.Windows.Forms.Form
  $form.Text = 'SR RenderDoc + MuMu'
  $form.StartPosition = 'CenterScreen'
  $form.FormBorderStyle = 'FixedDialog'
  $form.MaximizeBox = $false
  $form.MinimizeBox = $false
  $form.TopMost = $true
  $form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
  $form.Font = New-Object System.Drawing.Font('Segoe UI', 10)
  $form.ClientSize = New-Object System.Drawing.Size(900, 220)
  $form.Padding = New-Object System.Windows.Forms.Padding(12)

  $mumuLabel = New-Object System.Windows.Forms.Label
  $mumuLabel.Text = 'MuMuNxMain.exe'
  $mumuLabel.AutoSize = $true
  $mumuLabel.Margin = New-Object System.Windows.Forms.Padding(0, 5, 10, 3)

  $mumuBox = New-Object System.Windows.Forms.TextBox
  $mumuBox.Text = $InitialMuMuRoot
  $mumuBox.Dock = [System.Windows.Forms.DockStyle]::Fill
  $mumuBox.Margin = New-Object System.Windows.Forms.Padding(0, 3, 8, 3)

  $mumuBrowse = New-Object System.Windows.Forms.Button
  $mumuBrowse.Text = 'Browse...'
  $mumuBrowse.AutoSize = $true
  $mumuBrowse.MinimumSize = New-Object System.Drawing.Size(85, 28)
  $mumuBrowse.Margin = New-Object System.Windows.Forms.Padding(0, 1, 0, 1)
  $mumuBrowse.Add_Click(({
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select MuMuNxMain.exe'
    $dialog.Filter = 'MuMuNxMain.exe|MuMuNxMain.exe|Executable files|*.exe'
    $dialog.CheckFileExists = $true
    $current = Get-Item -LiteralPath $mumuBox.Text -ErrorAction SilentlyContinue
    if($current) {
      $dialog.InitialDirectory = if($current.PSIsContainer) { $current.FullName } else { $current.DirectoryName }
    }
    if($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
      $mumuBox.Text = $dialog.FileName
    }
  }.GetNewClosure()))

  $renderDocLabel = New-Object System.Windows.Forms.Label
  $renderDocLabel.Text = 'SR renderdoccmd.exe'
  $renderDocLabel.AutoSize = $true
  $renderDocLabel.Margin = New-Object System.Windows.Forms.Padding(0, 5, 10, 3)

  $renderDocBox = New-Object System.Windows.Forms.TextBox
  $renderDocBox.Text = $InitialRenderDocBin
  $renderDocBox.Dock = [System.Windows.Forms.DockStyle]::Fill
  $renderDocBox.Margin = New-Object System.Windows.Forms.Padding(0, 3, 8, 3)

  $renderDocBrowse = New-Object System.Windows.Forms.Button
  $renderDocBrowse.Text = 'Browse...'
  $renderDocBrowse.AutoSize = $true
  $renderDocBrowse.MinimumSize = New-Object System.Drawing.Size(85, 28)
  $renderDocBrowse.Margin = New-Object System.Windows.Forms.Padding(0, 1, 0, 1)
  $renderDocBrowse.Add_Click(({
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select SR renderdoccmd.exe'
    $dialog.Filter = 'renderdoccmd.exe|renderdoccmd.exe|Executable files|*.exe'
    $dialog.CheckFileExists = $true
    $current = Get-Item -LiteralPath $renderDocBox.Text -ErrorAction SilentlyContinue
    if($current) {
      $dialog.InitialDirectory = if($current.PSIsContainer) { $current.FullName } else { $current.DirectoryName }
    }
    if($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) {
      $renderDocBox.Text = $dialog.FileName
    }
  }.GetNewClosure()))

  $hint = New-Object System.Windows.Forms.Label
  $hint.Text = 'Select the EXE itself; its containing folder is also accepted.'
  $hint.AutoSize = $true
  $hint.ForeColor = [System.Drawing.Color]::DimGray
  $hint.Margin = New-Object System.Windows.Forms.Padding(0, 4, 0, 6)

  $startButton = New-Object System.Windows.Forms.Button
  $startButton.Text = 'Start capture'
  $startButton.AutoSize = $true
  $startButton.MinimumSize = New-Object System.Drawing.Size(105, 30)
  $startButton.Anchor = [System.Windows.Forms.AnchorStyles]::Right
  $startButton.Margin = New-Object System.Windows.Forms.Padding(0, 4, 8, 0)
  $startButton.Add_Click(({
    try {
      $resolvedMuMuRoot = Resolve-MuMuRoot -Path $mumuBox.Text -DefaultPath $InitialMuMuRoot
      $resolvedRenderDocBin = Resolve-RenderDocBin -Path $renderDocBox.Text -DefaultPath $InitialRenderDocBin
      $form.Tag = @{
        MuMuRoot = Join-Path $resolvedMuMuRoot 'nx_main\MuMuNxMain.exe'
        RenderDocBin = Join-Path $resolvedRenderDocBin 'renderdoccmd.exe'
      }
      $form.DialogResult = [System.Windows.Forms.DialogResult]::OK
      $form.Close()
    } catch {
      [System.Windows.Forms.MessageBox]::Show(
        $form,
        $_.Exception.Message,
        'Invalid path',
        [System.Windows.Forms.MessageBoxButtons]::OK,
        [System.Windows.Forms.MessageBoxIcon]::Error
      ) | Out-Null
    }
  }.GetNewClosure()))

  $cancelButton = New-Object System.Windows.Forms.Button
  $cancelButton.Text = 'Cancel'
  $cancelButton.AutoSize = $true
  $cancelButton.MinimumSize = New-Object System.Drawing.Size(100, 30)
  $cancelButton.Anchor = [System.Windows.Forms.AnchorStyles]::Right
  $cancelButton.Margin = New-Object System.Windows.Forms.Padding(0, 4, 0, 0)
  $cancelButton.DialogResult = [System.Windows.Forms.DialogResult]::Cancel

  $form.AcceptButton = $startButton
  $form.CancelButton = $cancelButton

  $layout = New-Object System.Windows.Forms.TableLayoutPanel
  $layout.Dock = [System.Windows.Forms.DockStyle]::Fill
  $layout.ColumnCount = 3
  $layout.RowCount = 4
  $layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
  $layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 100)))
  $layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
  for($row = 0; $row -lt 4; $row++) {
    $layout.RowStyles.Add((New-Object System.Windows.Forms.RowStyle([System.Windows.Forms.SizeType]::AutoSize)))
  }
  $layout.Controls.Add($mumuLabel, 0, 0)
  $layout.Controls.Add($mumuBox, 1, 0)
  $layout.Controls.Add($mumuBrowse, 2, 0)
  $layout.Controls.Add($renderDocLabel, 0, 1)
  $layout.Controls.Add($renderDocBox, 1, 1)
  $layout.Controls.Add($renderDocBrowse, 2, 1)
  $layout.Controls.Add($hint, 0, 2)
  $layout.SetColumnSpan($hint, 3)
  $layout.Controls.Add($startButton, 1, 3)
  $layout.Controls.Add($cancelButton, 2, 3)
  $form.Controls.Add($layout)

  $result = $form.ShowDialog()
  if($result -ne [System.Windows.Forms.DialogResult]::OK) { return $null }
  return $form.Tag
}

$scriptToolsDir = $PSScriptRoot
$repoRoot = Split-Path -Parent $scriptToolsDir
$workspace = Split-Path -Parent (Split-Path -Parent $repoRoot)
$defaultRenderDocBin = Join-Path $workspace 'Base_RenderDoc\my-renderdoc\x64\Development'
$defaultMuMuRoot = Join-Path $workspace 'Games_MUMU\MuMuPlayerGlobal'
$defaultRenderDocExecutable = Join-Path $defaultRenderDocBin 'renderdoccmd.exe'
$defaultMuMuExecutable = Join-Path $defaultMuMuRoot 'nx_main\MuMuNxMain.exe'
$localAppData = if([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
  Join-Path $HOME 'AppData\Local'
} else {
  $env:LOCALAPPDATA
}
$settingsPath = Join-Path $localAppData 'SR-RenderDoc\launcher-settings.json'
$settings = Read-LauncherSettings $settingsPath

$initialMuMuRoot = if(![string]::IsNullOrWhiteSpace($env:MUMU_ROOT)) {
  $env:MUMU_ROOT
} elseif($settings -and ![string]::IsNullOrWhiteSpace([string]$settings.MuMuRoot)) {
  Get-RememberedExecutablePath ([string]$settings.MuMuRoot) 'nx_main\MuMuNxMain.exe'
} else {
  $defaultMuMuExecutable
}
$initialRenderDocBin = if(![string]::IsNullOrWhiteSpace($env:SR_RENDERDOC_BIN)) {
  $env:SR_RENDERDOC_BIN
} elseif($settings -and ![string]::IsNullOrWhiteSpace([string]$settings.RenderDocBin)) {
  Get-RememberedExecutablePath ([string]$settings.RenderDocBin) 'renderdoccmd.exe'
} else {
  $defaultRenderDocExecutable
}

$showPathDialog = [string]::IsNullOrWhiteSpace($RenderDocBin) -and
  [string]::IsNullOrWhiteSpace($MuMuRoot) -and
  -not $NoGui
if($showPathDialog) {
  $selection = Show-PathDialog $initialMuMuRoot $initialRenderDocBin
  if($null -eq $selection) { exit 2 }
  $MuMuRoot = $selection.MuMuRoot
  $RenderDocBin = $selection.RenderDocBin
  Save-LauncherSettings $settingsPath $MuMuRoot $RenderDocBin
}

$mumuRoot = Resolve-MuMuRoot $MuMuRoot $defaultMuMuRoot
$renderDocBin = Resolve-RenderDocBin $RenderDocBin $defaultRenderDocBin

if([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = $env:RENDERDOC_TEST_OUTPUT_DIR }
$outputDir = if([string]::IsNullOrWhiteSpace($OutputDir)) {
  Join-Path $workspace 'RenderDocTests\mumu-host'
} else {
  [Environment]::ExpandEnvironmentVariables($OutputDir)
}

$otherDefault = Join-Path $workspace 'Base_RenderDoc_ZZZ\my-renderdoc\x64\Development'
if([string]::IsNullOrWhiteSpace($OtherRenderDocBin)) { $OtherRenderDocBin = $env:ZZZ_RENDERDOC_BIN }
if([string]::IsNullOrWhiteSpace($OtherRenderDocBin) -and (Test-Path -LiteralPath $otherDefault)) {
  $OtherRenderDocBin = $otherDefault
}

$mumuCli = Join-Path $mumuRoot 'nx_main\mumu-cli.exe'
$mumuHost = Join-Path $mumuRoot 'nx_main\MuMuNxMain.exe'
$mumuWorkDir = Split-Path $mumuHost
$renderDocCmd = Join-Path $renderDocBin 'renderdoccmd.exe'
$renderDocDll = Join-Path $renderDocBin 'renderdoc.dll'
$renderDocJson = Join-Path $renderDocBin 'renderdoc.json'
$captureTemplate = Join-Path $outputDir 'mumu-host_sr'
$logPath = Join-Path $outputDir 'launcher-sr.log'

function Write-LauncherLog([string]$Message) {
  $line = '[{0}] {1}' -f (Get-Date -Format 'yyyy-MM-dd HH:mm:ss'), $Message
  Add-Content -LiteralPath $logPath -Value $line
  Write-Host $line
}

function Invoke-MuMu([string[]]$Arguments) {
  $result = (& $mumuCli @Arguments 2>&1 | Out-String).Trim()
  if($result) { Write-LauncherLog $result }
}

function Wait-ForProcess([string]$Name, [int]$TimeoutSeconds = 30) {
  $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
  do {
    $process = Get-CimInstance Win32_Process -Filter "Name='$Name'" -ErrorAction SilentlyContinue |
      Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($mumuRoot, [StringComparison]::OrdinalIgnoreCase) }
    if($process) { return $process }
    Start-Sleep -Milliseconds 500
  } while((Get-Date) -lt $deadline)
  return $null
}

function Get-RenderDocModulePaths($ProcessId) {
  $process = Get-Process -Id $ProcessId -ErrorAction Stop
  @($process.Modules |
    Where-Object { $_.ModuleName -ieq 'renderdoc.dll' } |
    ForEach-Object { $_.FileName })
}

if(!(Test-Path -LiteralPath $mumuCli) -or !(Test-Path -LiteralPath $mumuHost)) {
  throw "MuMu installation was not found under $mumuRoot."
}
if(!(Test-Path -LiteralPath $renderDocCmd) -or !(Test-Path -LiteralPath $renderDocDll) -or
   !(Test-Path -LiteralPath $renderDocJson)) {
  throw "SR RenderDoc build is incomplete under $renderDocBin."
}

New-Item -ItemType Directory -Force -Path $outputDir | Out-Null
Set-Content -LiteralPath $logPath -Value 'MuMu RenderDoc launcher: SR'
Write-LauncherLog "Using $renderDocBin"

# Replace only the two local RenderDoc layer entries; all other Vulkan layers stay intact.
$vulkanLayerKey = 'HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers'
$zzzJson = if([string]::IsNullOrWhiteSpace($OtherRenderDocBin)) {
  $null
} else {
  Join-Path (Resolve-RenderDocBin $OtherRenderDocBin $otherDefault) 'renderdoc.json'
}
try {
  if($zzzJson -and (Get-ItemProperty -Path $vulkanLayerKey -Name $zzzJson -ErrorAction SilentlyContinue)) {
    Remove-ItemProperty -Path $vulkanLayerKey -Name $zzzJson -ErrorAction SilentlyContinue
  }
  if(Get-ItemProperty -Path $vulkanLayerKey -Name $renderDocJson -ErrorAction SilentlyContinue) {
    Remove-ItemProperty -Path $vulkanLayerKey -Name $renderDocJson -ErrorAction SilentlyContinue
  }
  New-ItemProperty -Path $vulkanLayerKey -Name $renderDocJson -PropertyType DWord -Value 0 -Force | Out-Null
  Write-LauncherLog "Registered SR Vulkan layer: $renderDocJson"
} catch {
  throw "Cannot update the 64-bit Vulkan layer registration. Run this BAT as Administrator. $($_.Exception.Message)"
}

# MuMu must be restarted so no process retains another RenderDoc DLL.
Invoke-MuMu @('control', '--vmindex', $VmIndex, 'shutdown')
Invoke-MuMu @('main', 'close')
Start-Sleep -Seconds 6

$renderDocBins = @($renderDocBin)
if($zzzJson) { $renderDocBins += Split-Path -Parent $zzzJson }

# Stop only RenderDoc command processes from the selected local builds.
Get-CimInstance Win32_Process -Filter "Name='renderdoccmd.exe'" -ErrorAction SilentlyContinue |
  Where-Object {
    $path = $_.ExecutablePath
    $renderDocBins | Where-Object {
      $path -and $path.StartsWith($_, [StringComparison]::OrdinalIgnoreCase)
    }
  } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }
Start-Sleep -Seconds 2

$env:RENDERDOC_CAPTURE_OFFSCREEN_SUBMIT = '1'
Remove-Item Env:VK_LOADER_LAYERS_DISABLE -ErrorAction SilentlyContinue

$arguments = @(
  'capture', '-w', '-d', $mumuWorkDir, '-c', $captureTemplate,
  '--opt-hook-children', '--opt-ref-all-resources', $mumuHost
)
$renderDocProcess = Start-Process -FilePath $renderDocCmd -WorkingDirectory $mumuWorkDir -ArgumentList $arguments -PassThru
Write-LauncherLog "Started SR renderdoccmd.exe (PID $($renderDocProcess.Id))"
Start-Sleep -Seconds 5

$main = Wait-ForProcess 'MuMuNxMain.exe'
if(!$main) { throw 'MuMuNxMain.exe did not start under SR RenderDoc.' }
Write-LauncherLog "MuMu main process started (PID $($main.ProcessId))."

Invoke-MuMu @('control', '--vmindex', $VmIndex, '--version', '15', 'launch')
$device = Wait-ForProcess 'MuMuNxDevice.exe' 45
if(!$device) { throw 'MuMuNxDevice.exe did not start.' }
Start-Sleep -Seconds 5

$allModules = @()
foreach($process in @($main) + @($device)) {
  $paths = Get-RenderDocModulePaths $process.ProcessId
  $allModules += $paths
  Write-LauncherLog "PID $($process.ProcessId) renderdoc.dll: $($paths -join ', ')"
}

$wrongModule = @($allModules | Where-Object {
  $_ -and !$_.StartsWith($renderDocBin, [StringComparison]::OrdinalIgnoreCase)
})
if($wrongModule.Count -gt 0) {
  throw "A non-SR RenderDoc DLL is loaded: $($wrongModule -join ', ')"
}

Write-LauncherLog "SR injection is ready. Capture template: $captureTemplate"
Write-Host ''
Write-Host 'SR RenderDoc injection is ready. Enter the SR 3D scene and capture normally.'
Write-Host "Capture files: $captureTemplate`_<NUMBER>.srrdc"
