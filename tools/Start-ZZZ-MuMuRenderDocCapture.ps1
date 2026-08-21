[CmdletBinding()]
param(
  [string]$RenderDocBin,
  [string]$MuMuRoot,
  [int]$VmIndex = 0,
  [string]$OutputDir
)

$ErrorActionPreference = 'Stop'

function Resolve-RenderDocBin([string]$Path, [string]$DefaultPath) {
  if([string]::IsNullOrWhiteSpace($Path)) { $Path = $env:ZZZ_RENDERDOC_BIN }
  if([string]::IsNullOrWhiteSpace($Path)) { $Path = $DefaultPath }
  $candidate = (Resolve-Path -LiteralPath $Path -ErrorAction Stop).Path
  if(!(Get-Item -LiteralPath $candidate).PSIsContainer) {
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
  if(!(Get-Item -LiteralPath $candidate).PSIsContainer) {
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
  try { return Get-Content -LiteralPath $Path -Raw | ConvertFrom-Json } catch { return $null }
}

function Save-LauncherSettings([string]$Path, [string]$MuMuRoot, [string]$RenderDocBin) {
  $parent = Split-Path -Parent $Path
  New-Item -ItemType Directory -Force -Path $parent | Out-Null
  [pscustomobject]@{
    MuMuRoot = $MuMuRoot
    RenderDocBin = $RenderDocBin
  } | ConvertTo-Json | Set-Content -LiteralPath $Path -Encoding UTF8
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

function Show-PathDialog([string]$InitialMuMuPath, [string]$InitialRenderDocPath) {
  Add-Type -AssemblyName System.Windows.Forms
  Add-Type -AssemblyName System.Drawing
  [System.Windows.Forms.Application]::EnableVisualStyles()

  Add-Type @'
using System;
using System.Runtime.InteropServices;
public static class ZzzRenderDocDpi {
  [DllImport("user32.dll")] public static extern bool SetProcessDPIAware();
}
'@
  [ZzzRenderDocDpi]::SetProcessDPIAware() | Out-Null

  $form = New-Object System.Windows.Forms.Form
  $form.Text = 'ZZZ RenderDoc + MuMu'
  $form.StartPosition = 'CenterScreen'
  $form.AutoScaleMode = [System.Windows.Forms.AutoScaleMode]::Dpi
  $form.MinimumSize = New-Object System.Drawing.Size(760, 190)
  $form.ClientSize = New-Object System.Drawing.Size(860, 220)

  $layout = New-Object System.Windows.Forms.TableLayoutPanel
  $layout.Dock = [System.Windows.Forms.DockStyle]::Fill
  $layout.Padding = New-Object System.Windows.Forms.Padding(12)
  $layout.ColumnCount = 3
  [void]$layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))
  [void]$layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::Percent, 100)))
  [void]$layout.ColumnStyles.Add((New-Object System.Windows.Forms.ColumnStyle([System.Windows.Forms.SizeType]::AutoSize)))

  $mumuLabel = New-Object System.Windows.Forms.Label
  $mumuLabel.Text = 'MuMuNxMain.exe'
  $mumuLabel.AutoSize = $true
  $mumuLabel.Margin = New-Object System.Windows.Forms.Padding(0, 7, 10, 3)
  $mumuBox = New-Object System.Windows.Forms.TextBox
  $mumuBox.Text = $InitialMuMuPath
  $mumuBox.Dock = [System.Windows.Forms.DockStyle]::Fill
  $mumuBox.Margin = New-Object System.Windows.Forms.Padding(0, 3, 8, 3)
  $mumuBrowse = New-Object System.Windows.Forms.Button
  $mumuBrowse.Text = 'Browse...'
  $mumuBrowse.AutoSize = $true
  $mumuBrowse.MinimumSize = New-Object System.Drawing.Size(85, 28)
  $mumuBrowse.Add_Click({
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select MuMuNxMain.exe'
    $dialog.Filter = 'MuMuNxMain.exe|MuMuNxMain.exe|Executable files|*.exe'
    if($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $mumuBox.Text = $dialog.FileName }
  })

  $renderDocLabel = New-Object System.Windows.Forms.Label
  $renderDocLabel.Text = 'ZZZ renderdoccmd.exe'
  $renderDocLabel.AutoSize = $true
  $renderDocLabel.Margin = New-Object System.Windows.Forms.Padding(0, 7, 10, 3)
  $renderDocBox = New-Object System.Windows.Forms.TextBox
  $renderDocBox.Text = $InitialRenderDocPath
  $renderDocBox.Dock = [System.Windows.Forms.DockStyle]::Fill
  $renderDocBox.Margin = New-Object System.Windows.Forms.Padding(0, 3, 8, 3)
  $renderDocBrowse = New-Object System.Windows.Forms.Button
  $renderDocBrowse.Text = 'Browse...'
  $renderDocBrowse.AutoSize = $true
  $renderDocBrowse.MinimumSize = New-Object System.Drawing.Size(85, 28)
  $renderDocBrowse.Add_Click({
    $dialog = New-Object System.Windows.Forms.OpenFileDialog
    $dialog.Title = 'Select ZZZ renderdoccmd.exe'
    $dialog.Filter = 'renderdoccmd.exe|renderdoccmd.exe|Executable files|*.exe'
    if($dialog.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $renderDocBox.Text = $dialog.FileName }
  })

  $hint = New-Object System.Windows.Forms.Label
  $hint.Text = 'Paths are validated and remembered for this Windows user.'
  $hint.AutoSize = $true
  $hint.ForeColor = [System.Drawing.SystemColors]::GrayText
  $hint.Margin = New-Object System.Windows.Forms.Padding(0, 5, 0, 5)
  $layout.Controls.Add($mumuLabel, 0, 0)
  $layout.Controls.Add($mumuBox, 1, 0)
  $layout.Controls.Add($mumuBrowse, 2, 0)
  $layout.Controls.Add($renderDocLabel, 0, 1)
  $layout.Controls.Add($renderDocBox, 1, 1)
  $layout.Controls.Add($renderDocBrowse, 2, 1)
  $layout.Controls.Add($hint, 1, 2)

  $startButton = New-Object System.Windows.Forms.Button
  $startButton.Text = 'Start capture'
  $startButton.AutoSize = $true
  $startButton.DialogResult = [System.Windows.Forms.DialogResult]::OK
  $startButton.Add_Click({
    try {
      $resolvedMuMu = Resolve-MuMuRoot $mumuBox.Text $InitialMuMuPath
      $resolvedRenderDoc = Resolve-RenderDocBin $renderDocBox.Text $InitialRenderDocPath
      $form.Tag = [pscustomobject]@{ MuMuRoot = $resolvedMuMu; RenderDocBin = $resolvedRenderDoc }
    } catch {
      $_ | Out-String | Write-Host
      $form.DialogResult = [System.Windows.Forms.DialogResult]::None
      [System.Windows.Forms.MessageBox]::Show($form, $_.Exception.Message, 'Invalid path', 'OK', 'Error') | Out-Null
    }
  })
  $cancelButton = New-Object System.Windows.Forms.Button
  $cancelButton.Text = 'Cancel'
  $cancelButton.AutoSize = $true
  $cancelButton.DialogResult = [System.Windows.Forms.DialogResult]::Cancel
  $buttons = New-Object System.Windows.Forms.FlowLayoutPanel
  $buttons.FlowDirection = [System.Windows.Forms.FlowDirection]::RightToLeft
  $buttons.Dock = [System.Windows.Forms.DockStyle]::Fill
  [void]$buttons.Controls.Add($cancelButton)
  [void]$buttons.Controls.Add($startButton)
  $layout.Controls.Add($buttons, 1, 3)

  $form.AcceptButton = $startButton
  $form.CancelButton = $cancelButton
  [void]$form.Controls.Add($layout)
  [void]$form.ShowDialog()
  if($form.Tag) { return $form.Tag }
  return $null
}

$scriptToolsDir = Split-Path -Parent $MyInvocation.MyCommand.Path
$repoRoot = Split-Path -Parent $scriptToolsDir
$workspace = Split-Path -Parent (Split-Path -Parent $repoRoot)
$defaultMuMuRoot = Join-Path $workspace 'Games_MUMU\MuMuPlayerGlobal'
if(Test-Path -LiteralPath (Join-Path $repoRoot 'renderdoc.sln')) {
  $defaultRenderDocBin = Join-Path $repoRoot 'x64\Development'
} else {
  $defaultRenderDocBin = Join-Path $workspace 'Base_RenderDoc\my-renderdoc\x64\Development'
}
$defaultMuMuExecutable = Join-Path $defaultMuMuRoot 'nx_main\MuMuNxMain.exe'
$defaultRenderDocExecutable = Join-Path $defaultRenderDocBin 'renderdoccmd.exe'
$localAppData = if($env:LOCALAPPDATA) { $env:LOCALAPPDATA } else { Join-Path $HOME 'AppData\Local' }
$settingsPath = Join-Path $localAppData 'ZZZ-RenderDoc\launcher-settings.json'
$settings = Read-LauncherSettings $settingsPath

$initialMuMuPath = if($settings -and $settings.MuMuRoot) {
  Get-RememberedExecutablePath ([string]$settings.MuMuRoot) 'nx_main\MuMuNxMain.exe'
} else { $defaultMuMuExecutable }
$initialRenderDocPath = if($settings -and $settings.RenderDocBin) {
  Get-RememberedExecutablePath ([string]$settings.RenderDocBin) 'renderdoccmd.exe'
} else { $defaultRenderDocExecutable }

if([string]::IsNullOrWhiteSpace($RenderDocBin) -and [string]::IsNullOrWhiteSpace($MuMuRoot)) {
  $selection = Show-PathDialog $initialMuMuPath $initialRenderDocPath
  if(!$selection) { exit 0 }
  $MuMuRoot = $selection.MuMuRoot
  $RenderDocBin = $selection.RenderDocBin
  Save-LauncherSettings $settingsPath $MuMuRoot $RenderDocBin
}

$mumuRoot = Resolve-MuMuRoot $MuMuRoot $defaultMuMuRoot
$renderDocBin = Resolve-RenderDocBin $RenderDocBin $defaultRenderDocBin
if([string]::IsNullOrWhiteSpace($OutputDir)) { $OutputDir = Join-Path $workspace 'RenderDocTests\mumu-host' }
$mumuCli = Join-Path $mumuRoot 'nx_main\mumu-cli.exe'
$mumuHost = Join-Path $mumuRoot 'nx_main\MuMuNxMain.exe'
$mumuWorkDir = Split-Path $mumuHost
$renderDocCmd = Join-Path $renderDocBin 'renderdoccmd.exe'
$renderDocDll = Join-Path $renderDocBin 'renderdoc.dll'
$renderDocJson = Join-Path $renderDocBin 'renderdoc.json'
$captureTemplate = Join-Path $OutputDir 'mumu-host_zzz'

if(!(Test-Path -LiteralPath $mumuCli) -or !(Test-Path -LiteralPath $mumuHost)) { throw "MuMu installation was not found under $mumuRoot." }
if(!(Test-Path -LiteralPath $renderDocCmd) -or !(Test-Path -LiteralPath $renderDocDll) -or !(Test-Path -LiteralPath $renderDocJson)) { throw "ZZZ RenderDoc build is incomplete under $renderDocBin." }
New-Item -ItemType Directory -Force -Path $OutputDir | Out-Null

function Invoke-MuMu([string[]]$Arguments) {
  Write-Host ('MuMu: ' + ($Arguments -join ' '))
  & $mumuCli @Arguments
  if($LASTEXITCODE -ne 0) { throw "MuMu command failed with exit code $LASTEXITCODE." }
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
  @($process.Modules | Where-Object { $_.ModuleName -ieq 'renderdoc.dll' } | ForEach-Object { $_.FileName })
}

# Replace all local RenderDoc implicit-layer registrations with the selected build.
$vulkanLayerKey = 'HKLM:\SOFTWARE\Khronos\Vulkan\ImplicitLayers'
try {
  $properties = (Get-ItemProperty -Path $vulkanLayerKey -ErrorAction SilentlyContinue).PSObject.Properties |
    Where-Object { $_.Name -like '*renderdoc.json' }
  foreach($property in @($properties)) {
    Remove-ItemProperty -Path $vulkanLayerKey -Name $property.Name -ErrorAction SilentlyContinue
  }
  New-ItemProperty -Path $vulkanLayerKey -Name $renderDocJson -PropertyType DWord -Value 0 -Force | Out-Null
} catch {
  throw "Cannot update the 64-bit Vulkan layer registration. Run this BAT as Administrator. $($_.Exception.Message)"
}

Invoke-MuMu @('control', '--vmindex', $VmIndex, 'shutdown')
Invoke-MuMu @('main', 'close')
Start-Sleep -Seconds 6

Get-CimInstance Win32_Process -Filter "Name='renderdoccmd.exe'" -ErrorAction SilentlyContinue |
  Where-Object { $_.ExecutablePath -and $_.ExecutablePath.StartsWith($workspace, [StringComparison]::OrdinalIgnoreCase) } |
  ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }

$env:RENDERDOC_CAPTURE_OFFSCREEN_SUBMIT = '1'
Remove-Item Env:VK_LOADER_LAYERS_DISABLE -ErrorAction SilentlyContinue
$arguments = @('capture', '-w', '-d', $mumuWorkDir, '-c', $captureTemplate,
  '--opt-hook-children', '--opt-ref-all-resources', $mumuHost)
$renderDocProcess = Start-Process -FilePath $renderDocCmd -WorkingDirectory $mumuWorkDir -ArgumentList $arguments -PassThru
Write-Host "Started ZZZ renderdoccmd.exe (PID $($renderDocProcess.Id))."
Start-Sleep -Seconds 5

$main = Wait-ForProcess 'MuMuNxMain.exe'
if(!$main) { throw 'MuMuNxMain.exe did not start under ZZZ RenderDoc.' }
Invoke-MuMu @('control', '--vmindex', $VmIndex, '--version', '15', 'launch')
$device = Wait-ForProcess 'MuMuNxDevice.exe' 45
if(!$device) { throw 'MuMuNxDevice.exe did not start.' }
Start-Sleep -Seconds 5

$allModules = @()
foreach($process in @($main) + @($device)) { $allModules += Get-RenderDocModulePaths $process.ProcessId }
$wrongModule = @($allModules | Where-Object { $_ -and !$_.StartsWith($renderDocBin, [StringComparison]::OrdinalIgnoreCase) })
if($wrongModule.Count -gt 0) { throw "A different RenderDoc DLL is loaded: $($wrongModule -join ', ')" }

Write-Host 'ZZZ RenderDoc injection is ready. Enter the ZZZ 3D scene and capture normally.'
Write-Host "Capture files: $captureTemplate`_<NUMBER>.zzzrdc"
