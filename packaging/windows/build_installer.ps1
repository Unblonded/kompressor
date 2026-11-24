# Builds build\windows\installer\KompressorSetup-<version>.exe from an existing Windows build.
# Usage: powershell -ExecutionPolicy Bypass -File packaging\windows\build_installer.ps1 [-BuildDir build\windows]
param([string]$BuildDir = "build\windows")
$ErrorActionPreference = "Stop"
$root = Resolve-Path "$PSScriptRoot\..\.."

$iscc = (Get-Command iscc -ErrorAction SilentlyContinue).Source
if (-not $iscc) {
  $iscc = @("$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe", "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
            "$env:ProgramFiles\Inno Setup 6\ISCC.exe") | Where-Object { Test-Path $_ } | Select-Object -First 1
}
if (-not $iscc) { throw "Inno Setup 6 not found. Install it with: winget install JRSoftware.InnoSetup" }

$version = (Select-String -Path "$root\CMakeLists.txt" -Pattern 'project\(Kompressor VERSION ([0-9.]+)').Matches[0].Groups[1].Value
$build = Resolve-Path (Join-Path $root $BuildDir)
foreach ($exe in "Kompressor.exe", "KompShell.dll", "kompressor-cli.exe") {
  if (-not (Test-Path "$build\$exe")) { throw "$build\$exe is missing - build the 'windows' preset first." }
}
& $iscc /Q "/DAppVersion=$version" "/DBuildDir=$build" "/DOutputDir=$build\installer" "$PSScriptRoot\kompressor.iss"
if ($LASTEXITCODE -ne 0) { throw "ISCC failed with exit code $LASTEXITCODE" }
Get-Item "$build\installer\KompressorSetup-$version.exe"
