# install-prerequisites.ps1  — host-tool prerequisites for PIC32CM PL10 Curiosity Nano
#
# reproduce-install.ps1 only CHECKS for Python, Git, Ninja, CMake and 7-Zip on PATH and
# stops if one is missing - it never installs them. This script fills that gap: for each
# tool it checks whether it is already present (any version -> left untouched), and only
# if it is missing installs the EXACT version this project was verified with, via winget.
#
# Run this once on a fresh machine BEFORE reproduce-install.ps1:
#   powershell -ExecutionPolicy Bypass -File install-prerequisites.ps1
#
# Machine-scoped installs (Git, 7-Zip, CMake) trigger a UAC elevation prompt - accept it,
# or start this from an elevated PowerShell. A new shell is needed afterwards for freshly
# added PATH entries to be visible (this script refreshes PATH for its own final check).
$ErrorActionPreference = "Stop"

# --- pinned host-tool versions (harvested from the verified reference machine, 2026-07-14) ---
# Only used when a tool is ABSENT; an already-installed tool of any version is kept as-is.
# winget package IDs verified against the community repo. Bump a version here (and confirm
# the exact version still exists in winget) if the reference machine's toolchain moves.
$TOOLS = @(
  @{ Name = 'Python'; Exe = 'python'; Id = 'Python.Python.3.14'; Version = '3.14.6'; Custom = 'PrependPath=1' }
  @{ Name = 'Git';    Exe = 'git';    Id = 'Git.Git';            Version = '2.51.1'; Custom = $null }
  @{ Name = 'Ninja';  Exe = 'ninja';  Id = 'Ninja-build.Ninja';  Version = '1.13.1'; Custom = $null }
  @{ Name = 'CMake';  Exe = 'cmake';  Id = 'Kitware.CMake';      Version = '4.1.2';  Custom = 'ADD_CMAKE_TO_PATH=User' }
  @{ Name = '7-Zip';  Exe = '7z';     Id = '7zip.7zip';          Version = '26.01';  Custom = $null }
)

# --- helpers ---

# winget updates the persisted Machine/User PATH, but not this already-running process.
# Re-read both scopes so a tool installed earlier in THIS run is visible to later checks.
function Update-SessionPath {
  $machine = [Environment]::GetEnvironmentVariable('Path', 'Machine')
  $user    = [Environment]::GetEnvironmentVariable('Path', 'User')
  $env:PATH = @($machine, $user | Where-Object { $_ }) -join ';'
}

# Returns the found tool's version string, or $null if the tool is absent.
# 7-Zip is special-cased: its installer does NOT add itself to PATH, so also probe the
# default install location (mirrors reproduce-install.ps1's own 7-Zip check).
function Get-InstalledVersion {
  param([hashtable]$Tool)
  $cmd = Get-Command $Tool.Exe -ErrorAction SilentlyContinue
  $found = $null -ne $cmd
  if (-not $found -and $Tool.Name -eq '7-Zip' -and (Test-Path 'C:\Program Files\7-Zip\7z.exe')) {
    $found = $true
  }
  if (-not $found) { return $null }
  # Best-effort version string for the report; presence is what actually matters.
  try {
    switch ($Tool.Name) {
      'Python' { return (python --version 2>&1).ToString().Replace('Python ', '').Trim() }
      'Git'    { return (git --version).ToString().Replace('git version ', '').Trim() }
      'Ninja'  { return (ninja --version).ToString().Trim() }
      'CMake'  { return ((cmake --version) | Select-Object -First 1).ToString().Replace('cmake version ', '').Trim() }
      '7-Zip'  {
        $exe = (Get-Command 7z -ErrorAction SilentlyContinue).Source
        if (-not $exe) { $exe = 'C:\Program Files\7-Zip\7z.exe' }
        $banner = (& $exe | Where-Object { $_ -match '^7-Zip' } | Select-Object -First 1)
        if ($banner -match '7-Zip\s+(\S+)') { return $Matches[1] } else { return 'present' }
      }
      default  { return 'present' }
    }
  } catch { return 'present' }
}

# --- winget must be available (ships with App Installer on Windows 10 1809+/Windows 11) ---
if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
  throw "winget is not available. Install 'App Installer' from the Microsoft Store (or update Windows), then re-run. Alternatively install Python 3.14.6, Git 2.51.1, Ninja 1.13.1, CMake 4.1.2 and 7-Zip 26.01 by hand and put them on PATH."
}

# --- check each tool; install the pinned version only if absent ---
$installed = @()
foreach ($t in $TOOLS) {
  $have = Get-InstalledVersion -Tool $t
  if ($have) {
    Write-Host ("[skip]    {0,-7} already present ({1}) - left untouched" -f $t.Name, $have) -ForegroundColor DarkGray
    continue
  }

  Write-Host ("[install] {0,-7} missing - installing pinned {1}" -f $t.Name, $t.Version) -ForegroundColor Cyan
  $wgArgs = @(
    'install', '--id', $t.Id, '--version', $t.Version, '--exact',
    '--silent', '--accept-package-agreements', '--accept-source-agreements'
  )
  if ($t.Custom) { $wgArgs += @('--custom', $t.Custom) }

  & winget @wgArgs
  if ($LASTEXITCODE -ne 0) {
    throw "winget failed to install $($t.Name) $($t.Version) (id $($t.Id), exit $LASTEXITCODE). The pinned version may no longer be in the winget repo - check 'winget show $($t.Id)' for available versions."
  }
  Update-SessionPath
  $installed += $t.Name
}

# --- summary ---
Write-Host ""
if ($installed.Count -eq 0) {
  Write-Host "All prerequisites were already present. Nothing installed." -ForegroundColor Green
} else {
  Write-Host ("Installed: {0}" -f ($installed -join ', ')) -ForegroundColor Green
  Write-Host "Open a NEW terminal so the updated PATH takes effect, then run reproduce-install.ps1." -ForegroundColor Yellow
}
