@echo off
setlocal
chcp 65001 >nul
cd /d "%~dp0"
if not exist "build\trial-spawner.exe" (
  echo Build first: pwsh -File build.ps1
  pause
  exit /b 1
)
"build\trial-spawner.exe" --config finder.properties %*
if errorlevel 1 pause
