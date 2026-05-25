<#
.SYNOPSIS
Removes TransferUDT Windows services.

.DESCRIPTION
Stops and deletes the Agent, Server and/or Dashboard Windows services. Installed binaries
are preserved by default. Pass -RemoveFiles to remove the install directory and
-RemoveData to remove runtime data and configuration files.
#>

[CmdletBinding()]
param(
    [ValidateSet("Agent", "Server", "Dashboard", "Both", "ServerDashboard", "All")]
    [string]$Component = "Both",

    [string]$InstallRoot = "$env:ProgramFiles\TransferUDT",
    [string]$DataRoot = "$env:ProgramData\TransferUDT",

    [switch]$RemoveFiles,
    [switch]$RemoveData
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$AgentServiceName = "RadarAgentUDTService"
$ServerServiceName = "ServerUDTService"
$DashboardServiceName = "TransferUDTDashboardService"

function Test-ComponentSelected {
    param([Parameter(Mandatory = $true)][string]$Name)

    switch ($Name) {
        "Agent" { return $Component -eq "Agent" -or $Component -eq "Both" -or $Component -eq "All" }
        "Server" { return $Component -eq "Server" -or $Component -eq "Both" -or $Component -eq "ServerDashboard" -or $Component -eq "All" }
        "Dashboard" { return $Component -eq "Dashboard" -or $Component -eq "ServerDashboard" -or $Component -eq "All" }
    }

    return $false
}

function Assert-Administrator {
    $identity = [Security.Principal.WindowsIdentity]::GetCurrent()
    $principal = New-Object Security.Principal.WindowsPrincipal($identity)
    $isAdmin = $principal.IsInRole([Security.Principal.WindowsBuiltInRole]::Administrator)
    if (-not $isAdmin) {
        throw "Run this uninstaller from an elevated PowerShell session."
    }
}

function Wait-ServiceDeleted {
    param([Parameter(Mandatory = $true)][string]$Name)

    $deadline = (Get-Date).AddSeconds(30)
    while ((Get-Date) -lt $deadline) {
        $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if (-not $service) {
            return
        }
        Start-Sleep -Milliseconds 500
    }

    throw "Service $Name was not deleted within 30 seconds."
}

function Remove-TransferService {
    param([Parameter(Mandatory = $true)][string]$Name)

    $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if (-not $service) {
        Write-Host "Service $Name is not installed."
        return
    }

    if ($service.Status -ne "Stopped") {
        Write-Host "Stopping service $Name"
        Stop-Service -Name $Name -Force
    }

    Write-Host "Deleting service $Name"
    $output = & "$env:SystemRoot\System32\sc.exe" delete $Name 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe delete $Name failed: $output"
    }

    Wait-ServiceDeleted -Name $Name
}

function Remove-DirectoryIfRequested {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$RootLabel
    )

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $resolved = (Resolve-Path -LiteralPath $Path).ProviderPath
    if ($resolved.Length -lt 10) {
        throw "Refusing to remove suspiciously short $RootLabel path: $resolved"
    }

    Write-Host "Removing $RootLabel directory $resolved"
    Remove-Item -LiteralPath $resolved -Recurse -Force
}

Assert-Administrator

if (Test-ComponentSelected "Agent") {
    Remove-TransferService -Name $AgentServiceName
}

if (Test-ComponentSelected "Server") {
    Remove-TransferService -Name $ServerServiceName
}

if (Test-ComponentSelected "Dashboard") {
    Remove-TransferService -Name $DashboardServiceName
}

if ($RemoveFiles) {
    Remove-DirectoryIfRequested -Path $InstallRoot -RootLabel "install"
}

if ($RemoveData) {
    Remove-DirectoryIfRequested -Path $DataRoot -RootLabel "data"
}

Write-Host "TransferUDT uninstall completed."
