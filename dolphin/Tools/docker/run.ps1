# Run a command in the Linux build/test container with the workspace mounted (Windows host,
# Docker Desktop). Paths default to the D:\code\pm_rollback layout.
#
#   Tools\docker\run.ps1 build                       # configure + build, binaries -> run\linux-build\<distro>\Binaries
#   Tools\docker\run.ps1 test                        # quick harness tests (no game needed)
#   Tools\docker\run.ps1 -- test -m dolphin tests/test_rollback.py
#   Tools\docker\run.ps1 -Distro arch build
#   Tools\docker\run.ps1 bash                        # interactive shell
#
# Put `--` before the container command whenever it has dash options: PowerShell would otherwise
# match them against this script's parameters (-m -> -MainGitDir, -v -> -Verbose).
#
# The build tree lives in a Docker volume (pplus-build-<distro>) because building on a Windows
# bind mount is very slow (CMake's configure alone took ~20 min); only Binaries/ is copied to
# run\linux-build\<distro> on D:. -BindBuild builds directly on the D: dir instead.
# Remove the volume with: docker volume rm pplus-build-<distro>
[CmdletBinding(PositionalBinding = $false)]
param(
  [ValidateSet("ubuntu", "arch")] [string]$Distro = "ubuntu",
  [string]$Workspace = (Resolve-Path "$PSScriptRoot\..\..\..").Path,
  [string]$Source = (Resolve-Path "$PSScriptRoot\..\..").Path,
  [string]$MainGitDir = "",
  [switch]$BindBuild,
  [string[]]$DockerArgs = @(),
  [Parameter(ValueFromRemainingArguments = $true)] [string[]]$Command
)
$ErrorActionPreference = "Stop"
$image = if ($Distro -eq "arch") { "pplus-dolphin-linux:arch" } else { "pplus-dolphin-linux:ubuntu24.04" }
$out = Join-Path $Workspace "run\linux-build\$Distro"
New-Item -ItemType Directory -Force $out | Out-Null

$mounts = @(
  "-v", "${Source}:/src:ro",
  "-v", "${out}:/out",
  "-v", "$(Join-Path $Workspace 'harness'):/work/harness:ro",
  "-v", "$(Join-Path $Workspace 'game\SSBB_NTSC.iso'):/work/game/SSBB_NTSC.iso:ro",
  "-v", "$(Join-Path $Workspace 'run\template-user'):/work/run/template-user:ro",
  "--tmpfs", "/instances:size=12g,exec",
  "--shm-size", "1g"
)
if ($BindBuild) {
  $mounts += @("-v", "${out}:/build")
} else {
  $mounts += @("-v", "pplus-build-${Distro}:/build", "-e", "OUT=/out")
}
# A git worktree's .git file points at the main repo with a host path; mount the main repo's .git
# so the build can still read the revision (Dolphin's netplay compares it between peers).
$gitFile = Join-Path $Source ".git"
$wtName = ""
if (-not $MainGitDir -and (Test-Path $gitFile -PathType Leaf)) {
  $gd = ((Get-Content $gitFile -Raw) -replace '^gitdir:\s*', '').Trim()
  $MainGitDir = Split-Path (Split-Path $gd -Parent) -Parent   # <main>/.git/worktrees/<name> -> <main>/.git
  $wtName = Split-Path $gd -Leaf
}
$gitEnv = @()
if ($MainGitDir -and $wtName) {
  $mounts += @("-v", "${MainGitDir}:/gitcommon:ro")
  $gitEnv = @("-e", "GIT_DIR=/gitcommon/worktrees/$wtName", "-e", "GIT_WORK_TREE=/src",
              "-e", "GIT_CONFIG_COUNT=1", "-e", "GIT_CONFIG_KEY_0=safe.directory", "-e", "GIT_CONFIG_VALUE_0=*")
}
$tty = if ([Console]::IsInputRedirected -or [Console]::IsOutputRedirected) { @() } else { @("-it") }
if (-not $Command) { $Command = @("bash") }
# --init: a real PID 1 that reaps orphans (the harness checks that killed drivers' Dolphins are gone).
docker run --rm --init @tty @mounts @gitEnv @DockerArgs $image @Command
exit $LASTEXITCODE
