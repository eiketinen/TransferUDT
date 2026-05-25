Set-StrictMode -Version Latest

function Resolve-TransferUDTArchitecture {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "x86")]
        [string]$Architecture
    )

    switch ($Architecture) {
        "x86" {
            return [pscustomobject]@{
                Architecture = "x86"
                PlatformLabel = "windows-x86"
                InstallerArchitectureSuffix = "-x86"
                VcPlatform = "Win32"
                RuntimeIdentifier = "win-x86"
                PeMachine = 0x014c
                PeMachineName = "I386"
            }
        }
        "x64" {
            return [pscustomobject]@{
                Architecture = "x64"
                PlatformLabel = "windows-x64"
                InstallerArchitectureSuffix = ""
                VcPlatform = "x64"
                RuntimeIdentifier = "win-x64"
                PeMachine = 0x8664
                PeMachineName = "AMD64"
            }
        }
    }
}

function Get-TransferUDTReleaseName {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Version,

        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "x86")]
        [string]$Architecture
    )

    $arch = Resolve-TransferUDTArchitecture -Architecture $Architecture
    return "TransferUDT-$Version-$($arch.PlatformLabel)"
}

function Get-TransferUDTInstallerFileName {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("Debug", "Release")]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "x86")]
        [string]$Architecture,

        [switch]$Complete
    )

    $arch = Resolve-TransferUDTArchitecture -Architecture $Architecture
    $completeSuffix = if ($Complete) { "-Complete" } else { "" }
    return "TransferUDT-Setup-$Configuration$($arch.InstallerArchitectureSuffix)$completeSuffix.exe"
}

function Get-TransferUDTDashboardPublishDirectory {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [ValidateSet("Debug", "Release")]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "x86")]
        [string]$Architecture
    )

    $arch = Resolve-TransferUDTArchitecture -Architecture $Architecture
    return Join-Path $RepoRoot "DashboardWeb\bin\$Configuration\net8.0\$($arch.RuntimeIdentifier)\publish"
}

function Get-TransferUDTExecutableCandidates {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [ValidateSet("Debug", "Release")]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "x86")]
        [string]$Architecture,

        [Parameter(Mandatory = $true)]
        [ValidateSet("Agent", "Server", "AgentTests", "ServerTests", "Dashboard")]
        [string]$Kind
    )

    $arch = Resolve-TransferUDTArchitecture -Architecture $Architecture
    $candidates = New-Object System.Collections.Generic.List[string]

    switch ($Kind) {
        "Agent" {
            if ($Architecture -eq "x86") {
                $candidates.Add((Join-Path $RepoRoot "AgentUDTC++_v7.2\bin\Win32\$Configuration\AgentUDTC++.exe"))
            }
            else {
                $candidates.Add((Join-Path $RepoRoot "AgentUDTC++_v7.2\bin\x64\$Configuration\AgentUDTC++.exe"))
                $candidates.Add((Join-Path $RepoRoot "AgentUDTC++_v7.2\bin\$Configuration\AgentUDTC++.exe"))
            }
        }
        "Server" {
            $candidates.Add((Join-Path $RepoRoot "ServerUDTC++_v3\bin\$($arch.VcPlatform)\$Configuration\ServerUDTC++.exe"))
            if ($Architecture -eq "x64") {
                $candidates.Add((Join-Path $RepoRoot "ServerUDTC++_v3\bin\$Configuration\ServerUDTC++.exe"))
            }
        }
        "AgentTests" {
            if ([string]::IsNullOrWhiteSpace($env:LOCALAPPDATA)) {
                break
            }
            if ($Architecture -eq "x86") {
                $candidates.Add((Join-Path $env:LOCALAPPDATA "AgentUDTCppTests\bin\Win32\$Configuration\AgentUDTC++Tests.exe"))
            }
            else {
                $candidates.Add((Join-Path $env:LOCALAPPDATA "AgentUDTCppTests\bin\$Configuration\AgentUDTC++Tests.exe"))
            }
        }
        "ServerTests" {
            $candidates.Add((Join-Path $RepoRoot "ServerUDTC++_v3\tests\bin\$($arch.VcPlatform)\$Configuration\ServerUDTC++Tests.exe"))
            if ($Architecture -eq "x64") {
                $candidates.Add((Join-Path $RepoRoot "ServerUDTC++_v3\tests\bin\$Configuration\ServerUDTC++Tests.exe"))
            }
        }
        "Dashboard" {
            $publishDir = Get-TransferUDTDashboardPublishDirectory -RepoRoot $RepoRoot -Configuration $Configuration -Architecture $Architecture
            $candidates.Add((Join-Path $publishDir "DashboardWeb.exe"))
        }
    }

    return @($candidates)
}

function Resolve-TransferUDTExecutable {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$RepoRoot,

        [Parameter(Mandatory = $true)]
        [ValidateSet("Debug", "Release")]
        [string]$Configuration,

        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "x86")]
        [string]$Architecture,

        [Parameter(Mandatory = $true)]
        [ValidateSet("Agent", "Server", "AgentTests", "ServerTests", "Dashboard")]
        [string]$Kind,

        [switch]$Required
    )

    $candidates = @(Get-TransferUDTExecutableCandidates -RepoRoot $RepoRoot -Configuration $Configuration -Architecture $Architecture -Kind $Kind)
    foreach ($candidate in $candidates) {
        if (Test-Path -LiteralPath $candidate) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    if ($Required) {
        $candidateText = if ($candidates.Count -gt 0) { [string]::Join("; ", $candidates) } else { "<none>" }
        throw "$Kind executable not found for configuration '$Configuration' and architecture '$Architecture'. Candidates: $candidateText"
    }

    return $null
}

function Get-TransferUDTPEMachine {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        throw "PE file does not exist: $Path"
    }

    $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Open, [System.IO.FileAccess]::Read, [System.IO.FileShare]::ReadWrite)
    $reader = $null
    try {
        $reader = [System.IO.BinaryReader]::new($stream)
        if ($stream.Length -lt 0x40) {
            throw "File is too small to be a PE executable: $Path"
        }

        $mz = $reader.ReadUInt16()
        if ($mz -ne 0x5a4d) {
            throw "File does not start with MZ header: $Path"
        }

        $stream.Seek(0x3c, [System.IO.SeekOrigin]::Begin) | Out-Null
        $peOffset = $reader.ReadInt32()
        if ($peOffset -lt 0 -or ($peOffset + 6) -gt $stream.Length) {
            throw "File has an invalid PE header offset: $Path"
        }

        $stream.Seek($peOffset, [System.IO.SeekOrigin]::Begin) | Out-Null
        $signature = $reader.ReadUInt32()
        if ($signature -ne 0x00004550) {
            throw "File does not contain a PE signature: $Path"
        }

        $machine = $reader.ReadUInt16()
        $machineName = switch ($machine) {
            0x014c { "I386" }
            0x8664 { "AMD64" }
            0x01c0 { "ARM" }
            0xaa64 { "ARM64" }
            default { "0x{0:x4}" -f $machine }
        }

        return [pscustomobject]@{
            Path = (Resolve-Path -LiteralPath $Path).Path
            Machine = $machine
            MachineName = $machineName
        }
    }
    finally {
        if ($reader) {
            $reader.Dispose()
        }
        else {
            $stream.Dispose()
        }
    }
}

function Assert-TransferUDTPEMachine {
    [CmdletBinding()]
    param(
        [Parameter(Mandatory = $true)]
        [string]$Path,

        [Parameter(Mandatory = $true)]
        [ValidateSet("x64", "x86")]
        [string]$Architecture
    )

    $arch = Resolve-TransferUDTArchitecture -Architecture $Architecture
    $actual = Get-TransferUDTPEMachine -Path $Path
    if ($actual.Machine -ne $arch.PeMachine) {
        throw "Unexpected PE machine for '$Path'. Expected $($arch.PeMachineName) for $Architecture, got $($actual.MachineName)."
    }

    return $actual
}

Export-ModuleMember -Function `
    Resolve-TransferUDTArchitecture, `
    Get-TransferUDTReleaseName, `
    Get-TransferUDTInstallerFileName, `
    Get-TransferUDTDashboardPublishDirectory, `
    Get-TransferUDTExecutableCandidates, `
    Resolve-TransferUDTExecutable, `
    Get-TransferUDTPEMachine, `
    Assert-TransferUDTPEMachine
