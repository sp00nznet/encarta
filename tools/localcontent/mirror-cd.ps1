<#
.SYNOPSIS
    Copy an Encarta 97 CD to a local directory, so the disc is not needed to run.

.DESCRIPTION
    Encarta reads its content straight off the CD. Mirroring the whole disc -
    not just the ENCYC97 folder - keeps everything the app looks for in the
    same shape, including ENC97.CD1, the zero-byte marker it probes to decide
    the disc is present.

    CD1 is about 645 MB. CD2, if you have it, holds the rest of the media and
    can be mirrored into the same directory; the files do not overlap.

.EXAMPLE
    .\mirror-cd.ps1 -Source H:\ -Dest G:\encarta97

.EXAMPLE
    # a second disc, into the same tree
    .\mirror-cd.ps1 -Source H:\ -Dest G:\encarta97
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Dest
)

if (-not (Test-Path $Source)) {
    Write-Error "No such source: $Source  (is the CD inserted?)"
    exit 1
}

$label = (Get-Volume -FilePath $Source -ErrorAction SilentlyContinue).FileSystemLabel
Write-Host "Source : $Source" -NoNewline
if ($label) { Write-Host "  (volume label: $label)" } else { Write-Host "" }
Write-Host "Dest   : $Dest"

New-Item -ItemType Directory -Force -Path $Dest | Out-Null

# /E all subdirectories, /R:1 /W:1 so a bad sector fails fast rather than
# retrying a million times, which is the robocopy default and takes all day.
robocopy $Source $Dest /E /R:1 /W:1 /NFL /NDL /NP /NJH | Out-Null
$rc = $LASTEXITCODE

# robocopy's exit code is a bitmask and anything under 8 is success - 1 simply
# means "files were copied". Treating nonzero as failure is the usual mistake.
if ($rc -ge 8) {
    Write-Error "robocopy failed (exit $rc)"
    exit 1
}

$size = (Get-ChildItem $Dest -Recurse -File -ErrorAction SilentlyContinue |
         Measure-Object -Property Length -Sum).Sum
Write-Host ("Copied : {0:N0} MB" -f ($size / 1MB))

# The marker the app probes. Without it the disc reads as absent however much
# content is there.
$marker = Get-ChildItem $Dest -Filter "ENC97.CD*" -File -ErrorAction SilentlyContinue
if ($marker) {
    Write-Host "Marker : $($marker.Name) present"
} else {
    Write-Warning "No ENC97.CD* marker in $Dest - copy the CD ROOT, not just ENCYC97"
}

$books = Join-Path $Dest "ENCYC97\ENCARTA.M20"
if (Test-Path $books) {
    Write-Host "Books  : ENCYC97\ENCARTA.M20 present"
} else {
    Write-Warning "ENCYC97\ENCARTA.M20 is missing - this may be CD2, or the copy is incomplete"
}

Write-Host ""
Write-Host "Now run it with:"
Write-Host "  .\run-encarta.ps1 -Content $Dest"
