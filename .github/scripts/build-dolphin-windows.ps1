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
if (-not (Test-Path (Join-Path $Out "COPYING"))) { Copy-Item (Join-Path $src "COPYING") $Out }
if (-not (Test-Path (Join-Path $Out "Licenses"))) { Copy-Item -Recurse (Join-Path $src "LICENSES") (Join-Path $Out "Licenses") }

$exe = Join-Path $Out "Dolphin.exe"
if (-not (Select-String -Path $exe -Pattern "Project\+ Dolphin v$([regex]::Escape($Version))" -SimpleMatch:$false -Quiet)) {
  throw "Dolphin.exe does not report v$Version"
}
& bash "$repo/.github/scripts/dolphin-manifest.sh" $Out $Version "Dolphin.exe"
if ($LASTEXITCODE -ne 0) { throw "manifest failed" }
