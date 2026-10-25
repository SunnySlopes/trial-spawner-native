[CmdletBinding()]
param([switch]$Test)
$ErrorActionPreference = 'Stop'
$here = Split-Path -Parent $MyInvocation.MyCommand.Path
$buildDir = Join-Path $here 'build'
foreach ($tool in @('cmake', 'ninja')) {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) { throw "$tool is required on PATH" }
}
$testing = if ($Test) { 'ON' } else { 'OFF' }
& cmake -S $here -B $buildDir -G Ninja -DCMAKE_BUILD_TYPE=Release "-DBUILD_TESTING=$testing"
if ($LASTEXITCODE -ne 0) { throw 'CMake configuration failed' }
& cmake --build $buildDir --parallel
if ($LASTEXITCODE -ne 0) { throw 'Native build failed' }
if ($Test) {
    & ctest --test-dir $buildDir --output-on-failure
    if ($LASTEXITCODE -ne 0) { throw 'Native tests failed' }
}
Write-Host (Join-Path $buildDir 'trial-spawner.exe')
