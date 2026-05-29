<#
.SYNOPSIS
Static Windows 7 compatibility gate for built binaries (RFC-0002 §4.5).

.DESCRIPTION
Recursively scans .exe/.dll under -Path and fails if any imports a documented
post-Windows-7 API (normal OR delay-import table) or if the PE subsystem
version is newer than Windows 7 (> 6.01). This is a CI screen, NOT a proof:
it cannot see LoadLibrary/GetProcAddress dynamic dispatch, so the authoritative
check remains a real Windows 7 SP1 smoke test.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Path,
    [double]$MaxSubsystemVersion = 6.01
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# Documented post-Win7 APIs. Importing any of these means the binary will not
# load (or will fail) on Windows 7 SP1.
$blocklist = @(
    "GetSystemTimePreciseAsFileTime",   # Windows 8
    "PathCchCanonicalizeEx",            # Windows 8
    "PathCchCombineEx",                 # Windows 8
    "SetThreadDescription",             # Windows 10 1607
    "GetThreadDescription",             # Windows 10 1607
    "DiscardVirtualMemory",             # Windows 8.1
    "PrefetchVirtualMemory",            # Windows 8
    "SetProcessMitigationPolicy",       # Windows 8
    "CompareStringEx2"
)

function Find-DumpBin {
    $cmd = Get-Command dumpbin.exe -ErrorAction SilentlyContinue
    if ($cmd) { return $cmd.Source }
    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path $vswhere) {
        $root = & $vswhere -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath
        if ($root) {
            $hit = Get-ChildItem -Path (Join-Path $root "VC\Tools\MSVC") -Recurse -Filter dumpbin.exe -ErrorAction SilentlyContinue | Select-Object -First 1
            if ($hit) { return $hit.FullName }
        }
    }
    throw "dumpbin.exe not found. Run from a VS Developer environment or install the C++ tools."
}

$dumpbin = Find-DumpBin
$targets = @(Get-ChildItem -LiteralPath $Path -Recurse -Include *.exe, *.dll -File -ErrorAction SilentlyContinue)
if ($targets.Count -eq 0) {
    throw "No .exe/.dll found under '$Path'."
}

$violations = @()
foreach ($bin in $targets) {
    $imports = & $dumpbin /imports /dependents /nologo $bin.FullName 2>&1 | Out-String
    foreach ($api in $blocklist) {
        if ($imports -match [regex]::Escape($api)) {
            $violations += "POST-WIN7 IMPORT: $($bin.Name) imports $api"
        }
    }

    $headers = & $dumpbin /headers /nologo $bin.FullName 2>&1 | Out-String
    if ($headers -match "(?m)^\s*([0-9]+\.[0-9]+)\s+subsystem version") {
        $subsys = [double]$Matches[1]
        if ($subsys -gt $MaxSubsystemVersion) {
            $violations += "SUBSYSTEM: $($bin.Name) subsystem version $subsys > $MaxSubsystemVersion"
        }
        Write-Host ("OK subsystem {0}: {1}" -f $subsys, $bin.Name)
    }

    # api-ms-win-* dependents are surfaced for manual review (some are Win7-present).
    foreach ($line in ($imports -split "`r?`n")) {
        if ($line -match "api-ms-win-[\w-]+\.dll") {
            Write-Host ("REVIEW api-set in {0}: {1}" -f $bin.Name, $Matches[0])
        }
    }
}

if ($violations.Count -gt 0) {
    Write-Error ("Windows 7 compatibility check FAILED:`n" + ($violations -join "`n"))
    exit 1
}

Write-Host "Windows 7 static compatibility check passed for $($targets.Count) binary(ies)."
Write-Host "NOTE: static analysis only. Authoritative validation is a Windows 7 SP1 smoke test."
