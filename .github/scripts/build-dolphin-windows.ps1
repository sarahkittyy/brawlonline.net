# Builds our Dolphin for Windows (MSVC, Ninja, the ninja-release-x64 preset) and stages it for the
# launcher package (extraResources "dolphin").
#
#   .github/scripts/build-dolphin-windows.ps1 -Version 0.1.42 -Out launcher/release/dolphin
#
# Expects ccache on PATH (the workflow installs it) and stamp-version.sh run first.
param(
  [Parameter(Mandatory = $true)][string]$Version,
  [Parameter(Mandatory = $true)][string]$Out
)
$ErrorActionPreference = "Stop"
$repo = (Resolve-Path "$PSScriptRoot/../..").Path
$src = Join-Path $repo "dolphin"
$build = Join-Path $src "build/release/x64"

# MSVC environment (x64 host and target), taken from vcvars64.bat.
$vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
$vs = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
if (-not $vs) { throw "Visual Studio with the C++ tools was not found" }
Write-Host "Visual Studio: $vs"
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul && set" | ForEach-Object {
  if ($_ -match '^([^=]+)=(.*)$') { [Environment]::SetEnvironmentVariable($Matches[1], $Matches[2]) }
}

Push-Location $src
try {
  # DOLPHIN_MSVC_PCH=OFF: pch.h as a plain forced include, no /Yu, so ccache can cache every compile.
  cmake --preset ninja-release-x64 `
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache `
    -DCMAKE_DISABLE_PRECOMPILE_HEADERS=ON -DDOLPHIN_MSVC_PCH=OFF `
    -DENABLE_AUTOUPDATE=OFF -DENABLE_ANALYTICS=OFF -DUSE_DISCORD_PRESENCE=OFF `
    -DENABLE_TESTS=OFF -DENABLE_NOGUI=OFF "-DDISTRIBUTOR=brawlonline.net"  # quoted: PowerShell splits -D...=x.net at the dot
  if ($LASTEXITCODE -ne 0) { throw "cmake configure failed" }
  ccache -z | Out-Null
  cmake --build $build --target project-plus-dolphin dolphin-tool
  if ($LASTEXITCODE -ne 0) { throw "build failed" }
  ccache -s -v   # verbose: shows why calls are uncacheable
} finally {
  Pop-Location
}

$bin = Join-Path $build "Binaries"
if (Test-Path $Out) { Remove-Item -Recurse -Force $Out }
New-Item -ItemType Directory -Force $Out | Out-Null
# Everything Dolphin needs at run time; not the test runner, the P+ updater, NoGUI or debug files.
Get-ChildItem $bin | Where-Object {
  $_.Name -notin @("Tests", "Updater.exe", "DolphinNoGUI.exe", "portable.txt") -and $_.Extension -notin @(".pdb", ".ilk", ".exp", ".lib")
} | Copy-Item -Destination $Out -Recurse
# The Visual C++ runtime DLLs Dolphin, Qt and the runtime itself import (msvcp140.dll,
# vcruntime140.dll, ...; 6 of the 10), beside Dolphin.exe, from the toolset that built it: Windows
# doesn't come with them, so without them Dolphin doesn't start (exit 0xC0000135, "Required DLLs for
# launching Dolphin are missing"), and an older installed runtime than the toolset may not work.
$crt = Get-ChildItem (Join-Path $env:VCToolsRedistDir "x64") -Directory -Filter "Microsoft.VC*.CRT" |
  Sort-Object Name | Select-Object -Last 1
if (-not $crt) { throw "the Visual C++ runtime was not found in $env:VCToolsRedistDir" }
$scan = @(Get-ChildItem $Out -Recurse -File -Include *.exe, *.dll)
while ($scan.Count) {
  $names = $scan | ForEach-Object { & dumpbin /nologo /dependents $_.FullName } |
    Where-Object { $_ -match '^\s+((msvcp|vcruntime|concrt|vccorlib)140[\w]*\.dll)\s*$' } | ForEach-Object { $Matches[1] }
  $scan = @()
  foreach ($name in ($names | Sort-Object -Unique)) {
    if (Test-Path (Join-Path $Out $name)) { continue }
    $dll = Get-ChildItem $crt.FullName -Filter $name   # the file's own name (imports are upper case)
    if (-not $dll) { throw "$name is imported but not in $($crt.FullName)" }
    Copy-Item $dll.FullName $Out
    $scan += Get-Item (Join-Path $Out $dll.Name)
  }
}
Write-Host "Visual C++ runtime from $($crt.FullName): $((Get-ChildItem $Out -Filter *140*.dll).Name -join ', ')"
if (-not (Test-Path (Join-Path $Out "COPYING"))) { Copy-Item (Join-Path $src "COPYING") $Out }
if (-not (Test-Path (Join-Path $Out "Licenses"))) { Copy-Item -Recurse (Join-Path $src "LICENSES") (Join-Path $Out "Licenses") }

$exe = Join-Path $Out "Dolphin.exe"
if (-not (Select-String -Path $exe -Pattern "Project\+ Dolphin v$([regex]::Escape($Version))" -SimpleMatch:$false -Quiet)) {
  throw "Dolphin.exe does not report v$Version"
}
& bash "$repo/.github/scripts/dolphin-manifest.sh" $Out $Version "Dolphin.exe"
if ($LASTEXITCODE -ne 0) { throw "manifest failed" }
