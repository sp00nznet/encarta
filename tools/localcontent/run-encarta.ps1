<#
.SYNOPSIS
    Run Encarta 97 from a local content directory, with the recompiled Indeo
    codec registered so video plays.

.DESCRIPTION
    Three things have to be arranged, and each is one environment variable the
    harness reads:

    ENC97_REDIRECT   The app opens absolute paths on the drive it decided its
                     content lives on - it never asks for BookPath, so pointing
                     that at a copy does nothing. Rewriting the prefix on every
                     file open is what actually moves the content.

    ENC97_CDROM      Encarta checks that its content is on a CD-ROM. This makes
                     one drive letter answer as one, with the label CD1 of
                     Encarta 97 actually carries.

    IR32_DLL         Where the 16-bit Indeo driver is, for the video bridge.
                     It is recompiled rather than loaded, but its constant data
                     is read from the file.

    ENC97_PROFILE    CodePath and DATPath, which the app refuses to start
                     without and which normally live in a registry key Setup
                     would have written.

.EXAMPLE
    .\run-encarta.ps1 -Content G:\encarta97

.EXAMPLE
    # keep it open to click around, and log every file it touches
    .\run-encarta.ps1 -Content G:\encarta97 -Hold -FileLog all
#>
[CmdletBinding()]
param(
    # the local mirror made by mirror-cd.ps1
    [Parameter(Mandatory = $true)][string]$Content,
    # the drive letter the app looks for its CD on; the mirror stands in for it
    [string]$CdDrive = "H:",
    # ENC97.EXE and its DLLs (AM16/AMF16 must be here too - see the README)
    [string]$AppDir,
    [int]$TimeoutMs = 900000,
    [switch]$Hold,
    [ValidateSet("", "1", "all")][string]$FileLog = "",
    [switch]$NoVideo
)

$ErrorActionPreference = "Stop"
$repo = Split-Path (Split-Path $PSScriptRoot -Parent) -Parent
if (-not $AppDir) { $AppDir = Join-Path $repo "analysis" }

$harness = Join-Path $repo "build\tools\recomp\Release\recomp_enc97_run.exe"
if (-not (Test-Path $harness)) {
    Write-Error "No harness at $harness - build it first:`n" +
                "  cmake -B build -G ""Visual Studio 17 2022"" -A Win32`n" +
                "  cmake --build build --config Release --target recomp_enc97_run"
    exit 1
}
$exe = Join-Path $AppDir "ENC97.EXE"
if (-not (Test-Path $exe)) { Write-Error "No ENC97.EXE in $AppDir"; exit 1 }
if (-not (Test-Path (Join-Path $AppDir "AM16.DLL"))) {
    Write-Warning "AM16.DLL/AMF16.DLL are not in $AppDir - article pictures will fail."
    Write-Warning "They ship on CD1 at AAMSSTP\SYSTEM32\; copy them next to ENC97.EXE."
}

$Content = $Content.TrimEnd('\')
if (-not (Test-Path (Join-Path $Content "ENCYC97\ENCARTA.M20"))) {
    Write-Error "$Content does not look like a mirrored CD (no ENCYC97\ENCARTA.M20). Run mirror-cd.ps1 first."
    exit 1
}

# The trailing separators matter: the app concatenates these with a file name
# and no separator of its own, so "…\analysis" + "ENCART97.DAT" becomes one
# nonexistent path and it reports a missing CD.
$appPath = $AppDir.TrimEnd('\') + '\'
$env:ENC97_PROFILE  = "CodePath=$appPath;DATPath=$appPath;BookPath=$Content\ENCYC97\"
$env:ENC97_REDIRECT = "$($CdDrive.TrimEnd('\'))\=$Content\"
$env:ENC97_CDROM    = $Content.Substring(0, 1)
$env:MSGBOX_LOG     = "1"     # answer the startup dialogs instead of blocking
$env:NO_PRINTDLG    = "1"     # the printer query can stall on a machine with none
if ($Hold)     { $env:HOLD = "1" }     else { Remove-Item Env:HOLD -ErrorAction SilentlyContinue }
if ($FileLog)  { $env:FILE_LOG = $FileLog } else { Remove-Item Env:FILE_LOG -ErrorAction SilentlyContinue }

if ($NoVideo) {
    $env:NO_VIDEO = "1"
} else {
    Remove-Item Env:NO_VIDEO -ErrorAction SilentlyContinue
    $ir32 = Join-Path $Content "AAMSSTP\SYSTEM16\IR32.DLL"
    if (Test-Path $ir32) {
        $env:IR32_DLL = $ir32
    } else {
        Write-Warning "No IR32.DLL at $ir32 - video will not play. It is on CD1."
    }
    $bridge = Join-Path $repo "tools\indeo\runtime\build\ir32vfw.dll"
    $beside = Join-Path (Split-Path $harness -Parent) "ir32vfw.dll"
    if ((Test-Path $bridge) -and -not (Test-Path $beside)) {
        Copy-Item $bridge $beside -Force
    }
    if (-not (Test-Path $beside)) {
        Write-Warning "ir32vfw.dll is not beside the harness - video will not play."
        Write-Warning "Build it: tools\indeo\runtime\build.bat <path to IR32.DLL>"
    }
}

Write-Host "content : $Content  (as $CdDrive, answering as CD-ROM)"
Write-Host "app     : $AppDir"
Write-Host "video   : $(if ($NoVideo) { 'disabled' } else { 'IV32 via the recompiled codec' })"
Write-Host ""
& $harness $exe $TimeoutMs
