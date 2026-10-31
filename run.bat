@echo off
setlocal
cd /d "%~dp0"
if not exist "build\trial-spawner.exe" (
    echo Build first: pwsh -File build.ps1
    exit /b 1
)
if exist "finder.json" (
    "build\trial-spawner.exe" --config finder.json %*
) else (
    "build\trial-spawner.exe" %*
)
exit /b %errorlevel%
