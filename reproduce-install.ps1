# reproduce-install.ps1  — pinned reproduction for PIC32CM PL10 Curiosity Nano
# Generated automatically from a verified installation.
#
# Portable by design: builds the workspace in whatever directory this script itself
# lives in (via $PSScriptRoot), NOT a hardcoded path. To reproduce this setup, copy
# this script plus requirements-lock.txt and the app\ folder into an empty target
# directory (see README.md "Reproducing this setup elsewhere"), then run it from there.
$ErrorActionPreference = "Stop"
if (-not $PSScriptRoot) {
  throw "This script must be run as a file (e.g. 'powershell -File reproduce-install.ps1'), not pasted into a console - `$PSScriptRoot is only set when running from a saved .ps1 file."
}
$WS = $PSScriptRoot

# $ErrorActionPreference = "Stop" only catches PowerShell-native errors - a failing
# external command (west, git, pip, pyocd) just sets $LASTEXITCODE and lets the script
# keep going, cascading into confusing unrelated errors further down. Wrap every external
# call in this function so a real failure stops the script immediately, at its source.
function Invoke-Checked {
  param([Parameter(Mandatory)][ScriptBlock]$Command)
  & $Command
  if ($LASTEXITCODE -ne 0) {
    throw "Command failed with exit code $($LASTEXITCODE): $Command"
  }
}

# --- pinned values (harvested from a verified installation) ---
$ZEPHYR_REV      = "454f92597639d56513a75c3b4300bf958a2baf2f"
$WEST_VER        = "1.5.0"
$SDK_VER         = "1.0.1"
$BOARD           = "pic32cm_pl10_cnano"
$PYOCD_TARGET    = "pic32cm6408pl10048"
$PYOCD_VER       = "0.43.0"   # IMPORTANT: 0.44.1 is broken for this board (SWD "FAULT ACK"/"No ACK");
                              # see C:\work\Bukarest\3_Mi_CMSIS\Notizen.md, verified 2026-07-11.
$GROUP_FILTER    = "-optional,-babblesim,-ci"
$PROJECT_FILTER  = "-.*,+hal_microchip,+cmsis,+cmsis_6,+picolibc"
$PYOCD_FREQUENCY = "2000000"  # raised from the board.cmake default of 100000; verified stable
                              # on this board across flash + multi-cycle debug sessions, 2026-07-12

# --- prerequisites ---
foreach ($t in 'python','git','ninja','cmake') {
  if (-not (Get-Command $t -ErrorAction SilentlyContinue)) { throw "$t is missing from PATH" }
}

# 7-Zip is required by the Zephyr SDK setup strictly via PATH (not just installed).
# Without it, 'west sdk install' aborts while registering the CMake package.
$sevenZip = 'C:\Program Files\7-Zip'
if ((Test-Path "$sevenZip\7z.exe") -and ($env:PATH -notlike "*$sevenZip*")) {
  $env:PATH = "$sevenZip;$env:PATH"
} elseif (-not (Get-Command 7z -ErrorAction SilentlyContinue)) {
  throw "7-Zip (7z.exe) is missing from PATH - required by the Zephyr SDK setup. Please install 7-Zip."
}

# --- venv + pinned tools ---
New-Item -ItemType Directory -Force -Path $WS | Out-Null
Invoke-Checked { python -m venv $WS\.venv }
& $WS\.venv\Scripts\Activate.ps1
Invoke-Checked { python -m pip install --upgrade pip }
Invoke-Checked { pip install "west==$WEST_VER" }

# --- workspace, then pin the manifest repo (zephyr/) to the exact commit ---
# 'west init -m <url> --mr <SHA>' does NOT work: west runs 'git clone --branch <mr>',
# and git's --branch only resolves ref NAMES (branches/tags) - never a raw commit SHA,
# even when that SHA happens to be a branch's current tip (verified empirically against
# a throwaway test repo). The correct pattern is to clone the default branch (a full,
# non-shallow clone - west's init never passes --depth for the manifest repo, so the
# complete history is already there) and then check out the pinned commit explicitly.
Invoke-Checked { west init -m https://github.com/zephyrproject-rtos/zephyr $WS }
Set-Location $WS\zephyr
Invoke-Checked { git checkout $ZEPHYR_REV }
Set-Location $WS
Invoke-Checked { west config manifest.group-filter -- "$GROUP_FILTER" }
Invoke-Checked { west config manifest.project-filter -- "$PROJECT_FILTER" }
Invoke-Checked { west update --narrow --fetch-opt=--depth=1 }

# --- export + pinned Python deps ---
# Deliberately ONLY requirements-lock.txt (not 'west packages pip --install' /
# the full requirements.txt): the latter also pulls in test/compliance packages
# (e.g. opencv, spsdk), of which 'hidapi' in certain version combinations forces
# an MSVC build tools build on Windows, which we deliberately avoid.
$env:CMAKE_GENERATOR = "Ninja"
Invoke-Checked { west zephyr-export }
Invoke-Checked { pip install -r $WS\requirements-lock.txt }

# --- SDK pinned, ARM only, without extra host tools ---
Set-Location $WS\zephyr
Invoke-Checked { west sdk install -t arm-zephyr-eabi -H --version $SDK_VER }

# --- the application ships alongside this script ($WS\app) ---
# Verification build + flash:
Invoke-Checked { west build -p always -b $BOARD -d $WS\build $WS\app }
Invoke-Checked { pyocd flash -t $PYOCD_TARGET -f $PYOCD_FREQUENCY $WS\build\zephyr\zephyr.hex }
Write-Host "Reproduction complete." -ForegroundColor Green
