@echo off
setlocal
fltmc >nul 2>&1
if not "%ERRORLEVEL%"=="0" (
  echo Requesting administrator privileges...
  set "HSR_CAPTURE_ARGS=%*"
  PowerShell.exe -NoLogo -NoProfile -Command "$arguments = @('-NoLogo','-NoProfile','-ExecutionPolicy','Bypass','-File','%~dp0Start-HSR-MuMuRenderDocCapture.ps1'); if($env:HSR_CAPTURE_ARGS){$arguments += $env:HSR_CAPTURE_ARGS}; $process = Start-Process -FilePath 'PowerShell.exe' -Verb RunAs -ArgumentList $arguments -Wait -PassThru; exit $process.ExitCode"
  exit /b %ERRORLEVEL%
)
PowerShell.exe -NoLogo -NoProfile -ExecutionPolicy Bypass -File "%~dp0Start-HSR-MuMuRenderDocCapture.ps1" %*
set "exitCode=%ERRORLEVEL%"
echo.
if not "%exitCode%"=="0" echo HSR RenderDoc launcher failed with exit code %exitCode%.
pause
exit /b %exitCode%
