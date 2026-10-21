# One-click setup for the Encarta 97 static recompilation. Run it by
# double-clicking Setup.cmd in the repo folder; docs\GETTING-STARTED.md is the
# same thing by hand.
#
# It checks the tools, copies your CD1 into content\ (the app into analysis\),
# finds every function in ENC97.EXE, lifts it to C, builds the video codec and
# the harness, and leaves "Encarta 97.cmd" in this folder. Each step is skipped
# when its output already exists (-Force redoes them), so a rerun after a
# failure resumes. Everything it does goes to setup.log.
#
#   -GameDir <drive, folder, .iso, or a folder holding the .iso>   skip the question (-Game works too)
#   -Yes                                                          answer yes to every install question
#
# The toolkit is ..\pcrecomp (PCRECOMP overrides it); only the ISO reader and
# the video codec's 16-bit lifter come from it. Under recomp-launcher
# (RECOMP_LAUNCHER=1) it never waits for Enter.
param([switch]$Force, [switch]$Yes, [Alias('Game')][string]$GameDir = "")
$Game = $GameDir

$ErrorActionPreference = 'Stop'
$Root = Split-Path -Parent $PSScriptRoot
$Toolkit = $env:PCRECOMP
if (-not $Toolkit) { $Toolkit = Join-Path (Split-Path -Parent $Root) 'pcrecomp' }
$env:PCRECOMP = $Toolkit
$Log = Join-Path $Root 'setup.log'
Set-Location $Root
function Log($t) { Add-Content -Path $Log -Value $t -Encoding UTF8 }
Log "==== setup $(Get-Date -Format s)"

$Steps = 7
function Say($t, $c = 'Gray') { Write-Host $t -ForegroundColor $c; Log $t }
function Step($n, $t) { Write-Host ""; Say "[$n/$Steps] $t" 'Cyan' }
function Pause-End { if (-not $Yes -and $env:RECOMP_LAUNCHER -ne '1') { Read-Host "Press Enter to close" | Out-Null } }
function Fail($t) {
  Say ""; Say "Setup stopped: $t" 'Red'
  Say "The details are in $Log. Fix that and run Setup.cmd again; finished steps are skipped." 'Yellow'
  Pause-End; exit 1
}
trap { Fail "$_" }
function Ask($q) { if ($Yes) { Say "$q yes"; return $true }; $a = Read-Host "$q [Y/n]"; return -not ($a -match '^[nN]') }
function Refresh-Path {
  $env:Path = [Environment]::GetEnvironmentVariable('Path', 'Machine') + ';' + [Environment]::GetEnvironmentVariable('Path', 'User')
}
# Run a program with its output in the log. Windows PowerShell turns a native
# program's stderr into errors, so 'Stop' is off while it runs.
function Exec([string[]]$cmd) {
  $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
  $rest = @($cmd | Select-Object -Skip 1)   # @(): a one-element slice would splat as characters
  & $cmd[0] @rest 2>&1 | ForEach-Object { Log "$_" }
  $code = $LASTEXITCODE
  $ErrorActionPreference = $old
  return $code
}
function Run($what, [string[]]$cmd) {
  Say "  $what..."
  $code = Exec $cmd
  if ($code -ne 0) { Fail "$what failed (exit code $code)." }
}
function Done($path) { (Test-Path $path) -and -not $Force }

if ($env:RECOMP_LAUNCHER -ne '1') { Clear-Host }
Say "Encarta 97: static recompilation setup" 'White'
Say "You need your own Encarta 97 Encyclopedia CD1 (the disc, a copy of it, or its .iso) and about 2 GB free."

# ---------------------------------------------------------------- tools
Step 1 "Checking the tools"
# Encarta opens its files with OpenFile, which takes 127 characters at most.
$longest = Join-Path $Root 'analysis\ENCART97.DAT'
if ($longest.Length -gt 127) {
  Fail "this folder's path is too long for Encarta ($($longest.Length) characters to its data file, 127 at most). Move it somewhere shorter, such as C:\recomp\encarta."
}
# A Python that answers "Python 3.x". The Store's placeholder "python" (the
# one that opens the Store) answers nothing, so asking is the reliable test.
function Find-Python {
  $user = @(Get-ChildItem "$env:LOCALAPPDATA\Programs\Python\Python3*\python.exe" -ErrorAction SilentlyContinue | Sort-Object FullName -Descending | ForEach-Object { ,@($_.FullName) })
  foreach ($c in (@(@('py', '-3'), @('python'), @('python3')) + $user)) {
    if (-not (Get-Command $c[0] -ErrorAction SilentlyContinue)) { continue }
    $old = $ErrorActionPreference; $ErrorActionPreference = 'Continue'
    $rest = @($c | Select-Object -Skip 1)
    $v = (& $c[0] @rest --version 2>&1 | Out-String).Trim()
    $ErrorActionPreference = $old
    if ($v -match '^Python 3\.(\d+)' -and [int]$Matches[1] -ge 10) { return ,$c }
  }
  return $null
}
$pyargs = Find-Python
if (-not $pyargs) {
  Say "  Python 3.10 or newer is not installed."
  if (-not (Ask "  Install Python 3.12 now (winget, for your user only, about 30 MB)?")) { Fail "Python 3 is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) {
    Fail "winget is missing. Install 'App Installer' from the Microsoft Store, or install Python yourself."
  }
  Exec @('winget', 'install', '-e', '--id', 'Python.Python.3.12', '--scope', 'user', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
  Refresh-Path
  $pyargs = Find-Python
  if (-not $pyargs) { Fail "Python installed, but it can't be found. Install Python 3.12 from python.org yourself." }
}
Say "  Python: $($pyargs -join ' ')"
# The video codec's build.bat runs this Python rather than "py".
$env:PYTHON = (& $pyargs[0] @($pyargs | Select-Object -Skip 1) -c 'import sys; print(sys.executable)' | Out-String).Trim()

if ((Exec ($pyargs + @('-c', 'import pefile, capstone'))) -ne 0) {
  Say "  The Python packages pefile and capstone are missing (about 20 MB)."
  if (-not (Ask "  Install them now (pip, for your user only)?")) { Fail "pefile and capstone are required." }
  Run "Installing pefile and capstone" ($pyargs + @('-m', 'pip', 'install', '--user', 'pefile', 'capstone'))
}

if (-not (Test-Path (Join-Path $Toolkit 'tools\assets\iso_peek.py'))) {
  Say "  The pcrecomp toolkit is not beside this folder ($Toolkit)."
  if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    if (-not (Ask "  git is needed to fetch it. Install Git for Windows now (winget, about 60 MB)?")) { Fail "git is needed to fetch pcrecomp." }
    if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Git for Windows yourself." }
    Exec @('winget', 'install', '-e', '--id', 'Git.Git', '--accept-package-agreements', '--accept-source-agreements') | Out-Null
    Refresh-Path
    if (-not (Get-Command git -ErrorAction SilentlyContinue)) { Fail "Git did not install. Install Git for Windows yourself." }
  }
  if (-not (Ask "  Download it now (git, about 20 MB)?")) { Fail "pcrecomp is required at $Toolkit." }
  Run "Cloning pcrecomp" @('git', 'clone', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $Toolkit)
}
Say "  pcrecomp: $Toolkit"

# Visual Studio 2022 or newer, or its Build Tools, with the C++ x86 compiler
# and CMake (both in the C++ workload).
function Find-VS {
  foreach ($p in @("${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe",
                   "$env:ProgramFiles\Microsoft Visual Studio\Installer\vswhere.exe")) {
    if (Test-Path $p) {
      $vs = & $p -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
      if ($vs) { return $vs }
    }
  }
  return $null
}
if (-not (Find-VS)) {
  Say "  Visual Studio with the C++ tools is not installed. The recompiled app is C, and this is the compiler."
  if (-not (Ask "  Install the Visual Studio 2022 Build Tools with the C++ workload now (winget, about 3 GB)?")) { Fail "a C compiler (Visual Studio C++) is required." }
  if (-not (Get-Command winget -ErrorAction SilentlyContinue)) { Fail "winget is missing. Install Visual Studio 2022 Build Tools with 'Desktop development with C++' yourself." }
  Exec @('winget', 'install', '-e', '--id', 'Microsoft.VisualStudio.2022.BuildTools', '--accept-package-agreements', '--accept-source-agreements',
         '--override', '--quiet --wait --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended') | Out-Null
  if (-not (Find-VS)) { Fail "the Build Tools did not install. Install Visual Studio 2022 with 'Desktop development with C++' yourself." }
}
$VS = Find-VS
$vcvars = Join-Path $VS 'VC\Auxiliary\Build\vcvars32.bat'
$cmake = (Get-Command cmake -ErrorAction SilentlyContinue).Source
if (-not $cmake) { $cmake = Join-Path $VS 'Common7\IDE\CommonExtensions\Microsoft\CMake\CMake\bin\cmake.exe' }
if (-not (Test-Path $cmake)) { Fail "CMake was not found: add 'C++ CMake tools for Windows' in the Visual Studio Installer, or install CMake." }
Say "  C compiler: $VS"

# ---------------------------------------------------------------- the disc
Step 2 "Copying your CD1 into content (about 645 MB)"
if ((Done 'content\ENCYC97\ENCARTA.M20') -and (Test-Path 'analysis\ENC97.EXE')) {
  Say "  Already there (skipping)."
} else {
  $Game = $Game.Trim('"', ' ')
  if (-not $Game) {
    # A drive holding CD1 answers for itself.
    foreach ($d in (Get-PSDrive -PSProvider FileSystem)) {
      if (Test-Path (Join-Path $d.Root 'ENCYC97\ENCARTA.M20')) { $Game = $d.Root; Say "  Found CD1 in $Game"; break }
    }
  }
  if (-not $Game) {
    foreach ($d in @($Root, (Split-Path -Parent $Root), (Join-Path $env:USERPROFILE 'Downloads'))) {
      if (Get-ChildItem $d -File -Filter '*.iso' -ErrorAction SilentlyContinue) { $Game = $d; Say "  Looking for CD1 among the .iso files in $d"; break }
    }
  }
  if (-not $Game -and $Yes) { Fail "Encarta 97's CD1 wasn't found. Give its drive, a copy of it or its .iso with -GameDir." }
  while (-not $Game) {
    $Game = (Read-Host "  Where is Encarta 97's CD1? A drive (E:\), a copy of the disc, or its .iso").Trim('"', ' ')
  }
  Run "Copying the disc" ($pyargs + @('tools\content.py', $Game))
}

# ---------------------------------------------------------------- function map
Step 3 "Finding every function in ENC97.EXE (under a minute)"
if (Done 'tools\recomp\funcs.txt') { Say "  Already done (skipping)." }
else { Run "Disassembling" ($pyargs + @('tools\recomp\funcmap.py', 'analysis\ENC97.EXE', 'tools\recomp\funcs.txt')) }

# ---------------------------------------------------------------- lift
Step 4 "Recompiling ENC97.EXE to C (about a minute)"
if (Done 'tools\recomp\enc97_full.c') { Say "  Already done (skipping)." }
else { Run "Lifting" ($pyargs + @('tools\recomp\lift.py', 'analysis\ENC97.EXE', 'tools\recomp\funcs.txt', 'tools\recomp\enc97_full.c')) }

# ---------------------------------------------------------------- video
Step 5 "Recompiling the video codec (a few minutes)"
if (Done 'tools\indeo\runtime\build\ir32vfw.dll') { Say "  Already done (skipping)." }
else {
  # pcrecomp deccd6a (2026-09-13) walks a jump table until a dword is not an
  # address in the image, which an NE segment's small offsets never stop: the
  # codec then decodes 31 of 68 clips exactly. Its parent decodes all 68.
  $pin = 'c5376ff63e6e37d52880373c3c584f458dfa2ff0'
  $kit = $Toolkit
  if (Select-String -Path (Join-Path $Toolkit 'tools\lift\lift32_cpu.py') -Pattern 'if self.in_image(t):' -SimpleMatch -Quiet) {
    $kit = Join-Path $Root 'build\pcrecomp-video'
    if (-not (Test-Path (Join-Path $kit 'tools\lift\lift32_cpu.py'))) {
      Say "  Your pcrecomp's lifter gets this codec wrong; fetching the commit before that change for it."
      New-Item -ItemType Directory -Force $kit | Out-Null
      Run "Fetching pcrecomp $($pin.Substring(0, 7))" @('git', '-C', $kit, 'init', '-q')
      Run "Fetching pcrecomp $($pin.Substring(0, 7))" @('git', '-C', $kit, 'fetch', '-q', '--depth', '1', 'https://github.com/sp00nznet/pcrecomp', $pin)
      Run "Fetching pcrecomp $($pin.Substring(0, 7))" @('git', '-C', $kit, 'checkout', '-q', 'FETCH_HEAD')
    }
  }
  $env:PCRECOMP = $kit
  Run "Building the Indeo codec" @('cmd', '/d', '/c', "call `"$vcvars`" >nul && `"$Root\tools\indeo\runtime\build.bat`" `"$Root\content\AAMSSTP\SYSTEM16\IR32.DLL`"")
  $env:PCRECOMP = $Toolkit
}

# ---------------------------------------------------------------- build
Step 6 "Building the app (a few minutes)"
if (Done 'build\tools\recomp\Release\recomp_enc97_run.exe') { Say "  Already built (skipping)." }
else {
  # A cache another project's configure overwrote (GETTING-STARTED) is not worth keeping.
  if ($Force -or ((Test-Path 'build\CMakeCache.txt') -and -not (Select-String -Path 'build\CMakeCache.txt' -Pattern 'CMAKE_PROJECT_NAME' -Quiet))) {
    Remove-Item -Recurse -Force build -ErrorAction SilentlyContinue
  }
  Run "Configuring" @($cmake, '-B', 'build', '-A', 'Win32')
  Run "Compiling" @($cmake, '--build', 'build', '--config', 'Release', '--target', 'recomp_enc97_run')
}

# ---------------------------------------------------------------- launcher
Step 7 "Making the launcher"
$cmd = Join-Path $Root 'Encarta 97.cmd'
Set-Content -Path $cmd -Encoding ASCII -Value @"
@echo off
rem Runs the recompiled Encarta 97 from content\ (made by Setup.cmd). Close
rem Encarta and this window goes too.
powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0tools\localcontent\run-encarta.ps1" -Content "%~dp0content" -Hold -TimeoutMs 2147483647 %*
"@
Say "  $cmd"
Write-Host ""
Say "Done. Double-click 'Encarta 97' in this folder to run it." 'Green'
Pause-End
exit 0
