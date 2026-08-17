@echo off
setlocal
fltmc >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
  echo Requesting administrator privileges...
  set "SR_CAPTURE_ARGS=%*"
  PowerShell.exe -NoLogo -NoProfile -Command "$arguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0Start-SR-MuMuRenderDocCapture.ps1'); if($env:SR_CAPTURE_ARGS){$arguments += $env:SR_CAPTURE_ARGS}; $process = Start-Process -FilePath 'PowerShell.exe' -Verb RunAs -ArgumentList $arguments -Wait -PassThru; exit $process.ExitCode"
  exit /b %ERRORLEVEL%
)
PowerShell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-SR-MuMuRenderDocCapture.ps1" %*
set "exitCode=%ERRORLEVEL%"
echo.
if not "%exitCode%"=="0" echo SR RenderDoc launcher failed with exit code %exitCode%.
pause
exit /b %exitCode%
