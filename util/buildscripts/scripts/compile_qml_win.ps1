param(
  [Parameter(Mandatory = $true)]
  [string]$QtPrefix64,

  [ValidateSet('Development', 'Release')]
  [string]$Configuration = 'Development',

  [ValidateSet('x64')]
  [string]$Platform = 'x64'
)

$ErrorActionPreference = 'Stop'

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..\..')).Path
$qtPrefix = (Resolve-Path $QtPrefix64).Path

$requiredFiles = @(
  (Join-Path $qtPrefix 'bin\moc.exe'),
  (Join-Path $qtPrefix 'include\QtQml\QQmlContext'),
  (Join-Path $qtPrefix 'include\QtQuick'),
  (Join-Path $qtPrefix 'lib\Qt5Qml.lib'),
  (Join-Path $qtPrefix 'lib\Qt5Quick.lib'),
  (Join-Path $qtPrefix 'bin\Qt5Qml.dll'),
  (Join-Path $qtPrefix 'bin\Qt5Quick.dll')
)

$missing = $requiredFiles | Where-Object { -not (Test-Path $_) }
if($missing)
{
  throw "Qt QML prefix is incomplete. Missing: $($missing -join ', ')"
}

$msbuildCandidates = @(
  (Get-Command msbuild.exe -ErrorAction SilentlyContinue).Source,
  'C:\Program Files\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe',
  'C:\Program Files\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe'
) | Where-Object { $_ -and (Test-Path $_) }

if(-not $msbuildCandidates)
{
  throw 'MSBuild.exe was not found. Install Visual Studio Build Tools with the C++ workload.'
}

$msbuild = $msbuildCandidates | Select-Object -First 1
$solution = Join-Path $repoRoot 'renderdoc.sln'

$msbuildArguments = @(
  $solution,
  '/m',
  "/p:Configuration=$Configuration",
  "/p:Platform=$Platform",
  '/p:PlatformToolset=v143',
  '/p:RENDERDOC_QML_UI=1',
  "/p:RENDERDOC_QT_PREFIX64=$qtPrefix"
)

& $msbuild @msbuildArguments

if($LASTEXITCODE -ne 0)
{
  exit $LASTEXITCODE
}
