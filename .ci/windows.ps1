$ErrorActionPreference = "Stop"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "No Visual Studio with C++ tools found" }
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object { if ($_ -match "^([^=]+)=(.*)$") { Set-Item -Path "env:$($matches[1])" -Value $matches[2] } }
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $vs "Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe" }
& $cmake -S . -B build-win
if ($LASTEXITCODE -ne 0) { throw "configure failed" }
& $cmake --build build-win --config Release --parallel
if ($LASTEXITCODE -ne 0) { throw "build failed" }
Push-Location build-win
ctest -C Release --output-on-failure
$ok = $LASTEXITCODE -eq 0
Pop-Location
New-Item -ItemType Directory -Force -Path dist | Out-Null
Get-ChildItem -Path build-win -Recurse -Filter luma.exe | Select-Object -First 1 | Copy-Item -Destination dist\
if (-not $ok) { throw "tests failed" }
