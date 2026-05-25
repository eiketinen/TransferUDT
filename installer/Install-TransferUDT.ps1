<#
.SYNOPSIS
Installs TransferUDT Agent, Server and Dashboard as Windows services.

.DESCRIPTION
Copies binaries and configuration files to stable Windows locations, creates or
updates the services, and writes service-specific environment variables under
HKLM:\SYSTEM\CurrentControlSet\Services\<service>\Environment.

The service environment is loaded when the service starts, so updating the
configuration path does not require a machine reboot. Restart only the affected
service.
#>

[CmdletBinding()]
param(
    [ValidateSet("Agent", "Server", "Dashboard", "Both", "ServerDashboard", "All")]
    [string]$Component = "Both",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",

    [string]$InstallRoot = "$env:ProgramFiles\TransferUDT",
    [string]$DataRoot = "$env:ProgramData\TransferUDT",

    [string]$AgentExe = "",
    [string]$ServerExe = "",
    [string]$DashboardSource = "",

    [string]$AgentConfig = "",
    [string]$ServerConfig = "",
    [int]$DashboardPort = 8443,
    [string]$DashboardCertificate = "",
    [string]$DashboardCertificatePassword = "",
    [string]$DashboardOperatorPassword = "",
    [string]$DashboardAgentClientId = "",
    [string]$DashboardAgentPublicKey = "",

    [switch]$ForceConfig,
    [switch]$SkipAclHardening,
    [switch]$NoStart,
    [switch]$TrustDashboardCertificate
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$ScriptRoot = if (-not [string]::IsNullOrWhiteSpace($PSScriptRoot)) {
    $PSScriptRoot
}
else {
    Split-Path -Parent $MyInvocation.MyCommand.Path
}
$RepoRoot = (Resolve-Path -LiteralPath (Join-Path $ScriptRoot "..")).Path
Import-Module (Join-Path $RepoRoot "scripts\TransferUDT.Build.psm1") -Force

function Get-DefaultExecutablePath {
    param(
        [Parameter(Mandatory = $true)]
        [ValidateSet("Agent", "Server", "Dashboard")]
        [string]$Kind
    )

    $resolved = Resolve-TransferUDTExecutable -RepoRoot $RepoRoot -Configuration $Configuration -Architecture $Architecture -Kind $Kind
    if ($resolved) {
        return $resolved
    }

    $candidates = @(Get-TransferUDTExecutableCandidates -RepoRoot $RepoRoot -Configuration $Configuration -Architecture $Architecture -Kind $Kind)
    if ($candidates.Count -eq 0) {
        throw "No default candidate exists for $Kind $Configuration $Architecture."
    }
    return $candidates[0]
}

if ([string]::IsNullOrWhiteSpace($AgentExe)) {
    $AgentExe = Get-DefaultExecutablePath -Kind Agent
}
if ([string]::IsNullOrWhiteSpace($ServerExe)) {
    $ServerExe = Get-DefaultExecutablePath -Kind Server
}
if ([string]::IsNullOrWhiteSpace($DashboardSource)) {
    $DashboardSource = Get-TransferUDTDashboardPublishDirectory -RepoRoot $RepoRoot -Configuration $Configuration -Architecture $Architecture
}
if ([string]::IsNullOrWhiteSpace($AgentConfig)) {
    $AgentConfig = Join-Path $ScriptRoot "..\AgentUDTC++_v7.2\config.properties"
}
if ([string]::IsNullOrWhiteSpace($ServerConfig)) {
    $ServerConfig = Join-Path $ScriptRoot "..\ServerUDTC++_v3\config.properties"
}

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
        throw "Run this installer from an elevated PowerShell session."
    }
}

function Resolve-RequiredPath {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Label
    )

    $resolved = Resolve-Path -LiteralPath $Path -ErrorAction SilentlyContinue
    if (-not $resolved) {
        throw "$Label not found: $Path"
    }

    return $resolved.ProviderPath
}

function New-Directory {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path)) {
        New-Item -ItemType Directory -Path $Path | Out-Null
    }
}

function Protect-DirectoryAcl {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Name
    )

    if ($SkipAclHardening) {
        return
    }

    if (-not (Test-Path -LiteralPath $Path)) {
        return
    }

    $administrators = (New-Object Security.Principal.SecurityIdentifier("S-1-5-32-544")).Translate([Security.Principal.NTAccount])
    $system = (New-Object Security.Principal.SecurityIdentifier("S-1-5-18")).Translate([Security.Principal.NTAccount])
    $inheritance = [Security.AccessControl.InheritanceFlags]"ContainerInherit,ObjectInherit"
    $propagation = [Security.AccessControl.PropagationFlags]"None"
    $access = [Security.AccessControl.FileSystemRights]"FullControl"

    $acl = Get-Acl -LiteralPath $Path
    $acl.SetAccessRuleProtection($true, $false)

    foreach ($rule in @($acl.Access)) {
        [void]$acl.RemoveAccessRuleAll($rule)
    }

    $acl.SetAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($administrators, $access, $inheritance, $propagation, "Allow")))
    $acl.SetAccessRule((New-Object Security.AccessControl.FileSystemAccessRule($system, $access, $inheritance, $propagation, "Allow")))
    Set-Acl -LiteralPath $Path -AclObject $acl
    Write-Host "$Name ACL hardened: Administrators and SYSTEM only"
}

function Copy-ConfigFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $sourceFullPath = [IO.Path]::GetFullPath($Source)
    $destinationFullPath = [IO.Path]::GetFullPath($Destination)

    if ($sourceFullPath -ieq $destinationFullPath) {
        Write-Host "$Name config already in place: $Destination"
        return
    }

    if ((Test-Path -LiteralPath $Destination) -and -not $ForceConfig) {
        Write-Host "$Name config already exists; preserving $Destination"
        return
    }

    Copy-Item -LiteralPath $Source -Destination $Destination -Force
    Write-Host "$Name config installed at $Destination"
}

function Copy-NativeDependencies {
    param(
        [Parameter(Mandatory = $true)][string]$SourceExe,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $sourceDirectory = Split-Path -Parent $SourceExe
    $sourceFullPath = [IO.Path]::GetFullPath($sourceDirectory)
    $destinationFullPath = [IO.Path]::GetFullPath($DestinationDirectory)
    if ($sourceFullPath -ieq $destinationFullPath) {
        return
    }

    $dependencies = @(Get-ChildItem -LiteralPath $sourceDirectory -Filter "*.dll" -File -ErrorAction SilentlyContinue)
    foreach ($dependency in $dependencies) {
        Copy-Item -LiteralPath $dependency.FullName -Destination $DestinationDirectory -Force
    }

    if ($dependencies.Count -gt 0) {
        Write-Host "$Name native dependencies copied: $($dependencies.Count)"
    }
}

function Wait-ServiceState {
    param(
        [Parameter(Mandatory = $true)][string]$Name,
        [Parameter(Mandatory = $true)][string]$Status,
        [int]$TimeoutSeconds = 30
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
        if ($service -and $service.Status.ToString() -eq $Status) {
            return
        }
        Start-Sleep -Milliseconds 500
    }

    throw "Service $Name did not reach status $Status within $TimeoutSeconds seconds."
}

function Stop-ServiceIfRunning {
    param([Parameter(Mandatory = $true)][string]$Name)

    $service = Get-Service -Name $Name -ErrorAction SilentlyContinue
    if ($service -and $service.Status -ne "Stopped") {
        Write-Host "Stopping service $Name"
        Stop-Service -Name $Name -Force
        Wait-ServiceState -Name $Name -Status "Stopped"
    }
}

function Invoke-Sc {
    param([Parameter(Mandatory = $true)][string[]]$Arguments)

    $output = & "$env:SystemRoot\System32\sc.exe" @Arguments 2>&1
    if ($LASTEXITCODE -ne 0) {
        throw "sc.exe $($Arguments -join ' ') failed: $output"
    }
}

function Set-ServiceEnvironment {
    param(
        [Parameter(Mandatory = $true)][string]$ServiceName,
        [Parameter(Mandatory = $true)][string[]]$Environment
    )

    $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    New-ItemProperty -Path $serviceKey -Name "Environment" -PropertyType MultiString -Value $Environment -Force | Out-Null
    Write-Host "$ServiceName environment set: $($Environment -join '; ')"
}

function Install-TransferService {
    param(
        [Parameter(Mandatory = $true)][string]$ServiceName,
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][string]$BinaryPath,
        [Parameter(Mandatory = $true)][string]$ConfigPath,
        [Parameter(Mandatory = $true)][string]$ConfigVariable
    )

    Stop-ServiceIfRunning -Name $ServiceName

    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    $quotedBinary = '"' + $BinaryPath + '"'
    if ($service) {
        Write-Host "Updating service $ServiceName"
        Invoke-Sc -Arguments @("config", $ServiceName, "binPath=", $quotedBinary, "start=", "auto", "DisplayName=", $DisplayName)
    }
    else {
        Write-Host "Creating service $ServiceName"
        Invoke-Sc -Arguments @("create", $ServiceName, "binPath=", $quotedBinary, "start=", "auto", "DisplayName=", $DisplayName)
    }

    Invoke-Sc -Arguments @("description", $ServiceName, $Description)
    Invoke-Sc -Arguments @("failure", $ServiceName, "reset=", "86400", "actions=", "restart/60000/restart/60000/none/60000")
    Set-ServiceEnvironment -ServiceName $ServiceName -Environment @("$ConfigVariable=$ConfigPath")

    if (-not $NoStart) {
        Write-Host "Starting service $ServiceName"
        Start-Service -Name $ServiceName
        Wait-ServiceState -Name $ServiceName -Status "Running"
    }
}

function Install-DashboardService {
    param(
        [Parameter(Mandatory = $true)][string]$ServiceName,
        [Parameter(Mandatory = $true)][string]$DisplayName,
        [Parameter(Mandatory = $true)][string]$Description,
        [Parameter(Mandatory = $true)][string]$DashboardExe,
        [Parameter(Mandatory = $true)][string]$ContentRoot,
        [Parameter(Mandatory = $true)][string[]]$Environment
    )

    Stop-ServiceIfRunning -Name $ServiceName

    $service = Get-Service -Name $ServiceName -ErrorAction SilentlyContinue
    $quotedBinary = '"' + $DashboardExe + '" --contentRoot "' + $ContentRoot + '"'
    $serviceKey = "HKLM:\SYSTEM\CurrentControlSet\Services\$ServiceName"
    if ($service) {
        Write-Host "Updating service $ServiceName"
    }
    else {
        Write-Host "Creating service $ServiceName"
        New-Service `
            -Name $ServiceName `
            -BinaryPathName $DashboardExe `
            -DisplayName $DisplayName `
            -StartupType Automatic | Out-Null
    }

    New-ItemProperty -Path $serviceKey -Name "ImagePath" -PropertyType ExpandString -Value $quotedBinary -Force | Out-Null
    New-ItemProperty -Path $serviceKey -Name "DisplayName" -PropertyType String -Value $DisplayName -Force | Out-Null
    New-ItemProperty -Path $serviceKey -Name "Description" -PropertyType String -Value $Description -Force | Out-Null
    Set-ItemProperty -Path $serviceKey -Name "Start" -Value 2
    Invoke-Sc -Arguments @("failure", $ServiceName, "reset=", "86400", "actions=", "restart/60000/restart/60000/none/60000")
    Set-ServiceEnvironment -ServiceName $ServiceName -Environment $Environment

    if (-not $NoStart) {
        Write-Host "Starting service $ServiceName"
        Start-Service -Name $ServiceName
        Wait-ServiceState -Name $ServiceName -Status "Running"
    }
}

function Copy-ExecutableFile {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$Destination,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $sourceFullPath = [IO.Path]::GetFullPath($Source)
    $destinationFullPath = [IO.Path]::GetFullPath($Destination)
    if ($sourceFullPath -ieq $destinationFullPath) {
        Write-Host "$Name executable already in place: $Destination"
        return
    }

    Copy-Item -LiteralPath $Source -Destination $Destination -Force
}

function Copy-DirectoryContents {
    param(
        [Parameter(Mandatory = $true)][string]$SourceDirectory,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory,
        [Parameter(Mandatory = $true)][string]$Name
    )

    New-Directory -Path $DestinationDirectory
    $sourceFullPath = [IO.Path]::GetFullPath($SourceDirectory)
    $destinationFullPath = [IO.Path]::GetFullPath($DestinationDirectory)
    if ($sourceFullPath -ieq $destinationFullPath) {
        Write-Host "$Name files already in place: $DestinationDirectory"
        return
    }
    Copy-Item -Path (Join-Path $SourceDirectory "*") -Destination $DestinationDirectory -Recurse -Force
    Write-Host "$Name files copied to $DestinationDirectory"
}

function Copy-FileIfDifferent {
    param(
        [Parameter(Mandatory = $true)][string]$SourcePath,
        [Parameter(Mandatory = $true)][string]$DestinationPath,
        [Parameter(Mandatory = $true)][string]$Name
    )

    $sourceFullPath = [IO.Path]::GetFullPath($SourcePath)
    $destinationFullPath = [IO.Path]::GetFullPath($DestinationPath)
    if ($sourceFullPath -ieq $destinationFullPath) {
        Write-Host "$Name already in place: $DestinationPath"
        return
    }

    Copy-Item -LiteralPath $sourceFullPath -Destination $destinationFullPath -Force
}

function Convert-ToJsonPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return ([IO.Path]::GetFullPath($Path)).Replace("\", "/")
}

function Assert-PfxCertificatePassword {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Password
    )

    $cert = $null
    try {
        $flags = [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::MachineKeySet `
            -bor [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::PersistKeySet `
            -bor [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new($Path, $Password, $flags)
        if (-not $cert.HasPrivateKey) {
            throw "The PFX does not contain a private key."
        }
    }
    catch {
        throw "Dashboard HTTPS certificate could not be opened with the provided PFX password: $Path. $($_.Exception.Message)"
    }
    finally {
        if ($cert) {
            $cert.Dispose()
        }
    }
}

function Install-TrustedDashboardCertificate {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Password
    )

    $cert = $null
    try {
        $cert = [System.Security.Cryptography.X509Certificates.X509Certificate2]::new(
            $Path,
            $Password,
            [System.Security.Cryptography.X509Certificates.X509KeyStorageFlags]::Exportable)
        $cerPath = Join-Path $env:TEMP "transferudt-dashboard-local.cer"
        [IO.File]::WriteAllBytes(
            $cerPath,
            $cert.Export([System.Security.Cryptography.X509Certificates.X509ContentType]::Cert))
        Import-Certificate -FilePath $cerPath -CertStoreLocation "Cert:\LocalMachine\Root" | Out-Null
        Write-Host "Dashboard HTTPS certificate trusted for local test access."
    }
    finally {
        if ($cert) {
            $cert.Dispose()
        }
    }
}

function Install-Agent {
    $sourceExe = Resolve-RequiredPath -Path $AgentExe -Label "Agent executable"
    $sourceConfig = Resolve-RequiredPath -Path $AgentConfig -Label "Agent config"
    Stop-ServiceIfRunning -Name $AgentServiceName

    $targetDir = Join-Path $InstallRoot "Agent"
    $configDir = Join-Path $DataRoot "Agent"
    $runtimeDirs = @(
        $targetDir,
        $configDir,
        (Join-Path $DataRoot "Agent\logs"),
        (Join-Path $DataRoot "Agent\db"),
        (Join-Path $DataRoot "Agent\watch")
    )
    foreach ($dir in $runtimeDirs) {
        New-Directory -Path $dir
    }
    Protect-DirectoryAcl -Path $configDir -Name "Agent data"

    $targetExe = Join-Path $targetDir (Split-Path -Leaf $sourceExe)
    $targetConfig = Join-Path $configDir "config.properties"
    Copy-ExecutableFile -Source $sourceExe -Destination $targetExe -Name "Agent"
    Copy-NativeDependencies -SourceExe $sourceExe -DestinationDirectory $targetDir -Name "Agent"
    Copy-ConfigFile -Source $sourceConfig -Destination $targetConfig -Name "Agent"

    Install-TransferService `
        -ServiceName $AgentServiceName `
        -DisplayName "TransferUDT Agent" `
        -Description "TransferUDT file transfer agent service" `
        -BinaryPath $targetExe `
        -ConfigPath $targetConfig `
        -ConfigVariable "AGENT_CONFIG_PATH"
}

function Install-Server {
    $sourceExe = Resolve-RequiredPath -Path $ServerExe -Label "Server executable"
    $sourceConfig = Resolve-RequiredPath -Path $ServerConfig -Label "Server config"
    Stop-ServiceIfRunning -Name $ServerServiceName

    $targetDir = Join-Path $InstallRoot "Server"
    $configDir = Join-Path $DataRoot "Server"
    $runtimeDirs = @(
        $targetDir,
        $configDir,
        (Join-Path $DataRoot "Server\logs"),
        (Join-Path $DataRoot "Server\db"),
        (Join-Path $DataRoot "Server\storage"),
        (Join-Path $DataRoot "Server\reconstructed")
    )
    foreach ($dir in $runtimeDirs) {
        New-Directory -Path $dir
    }
    Protect-DirectoryAcl -Path $configDir -Name "Server data"

    $targetExe = Join-Path $targetDir (Split-Path -Leaf $sourceExe)
    $targetConfig = Join-Path $configDir "config.properties"
    Copy-ExecutableFile -Source $sourceExe -Destination $targetExe -Name "Server"
    Copy-NativeDependencies -SourceExe $sourceExe -DestinationDirectory $targetDir -Name "Server"
    Copy-ConfigFile -Source $sourceConfig -Destination $targetConfig -Name "Server"

    Install-TransferService `
        -ServiceName $ServerServiceName `
        -DisplayName "TransferUDT Server" `
        -Description "TransferUDT receiver service" `
        -BinaryPath $targetExe `
        -ConfigPath $targetConfig `
        -ConfigVariable "SERVER_CONFIG_PATH"
}

function Install-Dashboard {
    $sourceDir = Resolve-RequiredPath -Path $DashboardSource -Label "Dashboard publish directory"
    Stop-ServiceIfRunning -Name $DashboardServiceName

    $targetDir = Join-Path $InstallRoot "Dashboard"
    $configDir = Join-Path $DataRoot "Dashboard"
    $certDir = Join-Path $configDir "certs"
    $runtimeDirs = @(
        $targetDir,
        $configDir,
        $certDir,
        (Join-Path $DataRoot "Dashboard\db"),
        (Join-Path $DataRoot "Dashboard\logs")
    )
    foreach ($dir in $runtimeDirs) {
        New-Directory -Path $dir
    }
    Protect-DirectoryAcl -Path $configDir -Name "Dashboard data"

    Copy-DirectoryContents -SourceDirectory $sourceDir -DestinationDirectory $targetDir -Name "Dashboard"

    $targetCertificate = Join-Path $certDir "dashboard.pfx"
    if (-not [string]::IsNullOrWhiteSpace($DashboardCertificate)) {
        $sourceCertificate = Resolve-RequiredPath -Path $DashboardCertificate -Label "Dashboard HTTPS certificate"
        Copy-FileIfDifferent -SourcePath $sourceCertificate -DestinationPath $targetCertificate -Name "Dashboard HTTPS certificate"
    }
    elseif (-not (Test-Path -LiteralPath $targetCertificate)) {
        throw "Dashboard requires a PFX certificate. Pass -DashboardCertificate or place dashboard.pfx under $certDir."
    }

    if ([string]::IsNullOrWhiteSpace($DashboardCertificatePassword)) {
        throw "Dashboard requires -DashboardCertificatePassword."
    }
    if ([string]::IsNullOrWhiteSpace($DashboardOperatorPassword)) {
        throw "Dashboard requires -DashboardOperatorPassword."
    }
    Assert-PfxCertificatePassword -Path $targetCertificate -Password $DashboardCertificatePassword
    if ($TrustDashboardCertificate) {
        Install-TrustedDashboardCertificate -Path $targetCertificate -Password $DashboardCertificatePassword
    }

    $agentPublicKeysJson = "{}"
    if (-not [string]::IsNullOrWhiteSpace($DashboardAgentClientId) -and -not [string]::IsNullOrWhiteSpace($DashboardAgentPublicKey)) {
        $sourcePublicKey = Resolve-RequiredPath -Path $DashboardAgentPublicKey -Label "Dashboard agent public key"
        $clientsDir = Join-Path $configDir "clients"
        New-Directory -Path $clientsDir
        $targetPublicKey = Join-Path $clientsDir "$DashboardAgentClientId.pub"
        Copy-FileIfDifferent -SourcePath $sourcePublicKey -DestinationPath $targetPublicKey -Name "Dashboard agent public key"
        $agentPublicKeysJson = "{ `"$DashboardAgentClientId`": `"$(Convert-ToJsonPath $targetPublicKey)`" }"
    }

    $appsettings = @"
{
  "Dashboard": {
    "BindAddress": "0.0.0.0",
    "HttpsPort": $DashboardPort,
    "RequireHttps": true,
    "DatabasePath": "$(Convert-ToJsonPath (Join-Path $DataRoot "Dashboard\db\dashboard.db"))",
    "ServerDatabasePath": "$(Convert-ToJsonPath (Join-Path $DataRoot "Server\db\server.db"))",
    "ServerLogPath": "$(Convert-ToJsonPath (Join-Path $DataRoot "Server\logs\server.log"))",
    "LogPath": "$(Convert-ToJsonPath (Join-Path $DataRoot "Dashboard\logs\dashboard.log"))",
    "CertificatePath": "$(Convert-ToJsonPath $targetCertificate)",
    "CertificatePasswordEnv": "TRANSFERUDT_DASHBOARD_CERT_PASSWORD",
    "OperatorPasswordEnv": "TRANSFERUDT_DASHBOARD_OPERATOR_PASSWORD",
    "HeartbeatSkewSeconds": 300,
    "AgentOfflineAfterSeconds": 60,
    "AgentPublicKeys": $agentPublicKeysJson
  }
}
"@
    $appsettingsPath = Join-Path $targetDir "appsettings.json"
    $appsettings | Set-Content -LiteralPath $appsettingsPath -Encoding UTF8
    Write-Host "Dashboard appsettings installed at $appsettingsPath"

    $dashboardExe = Join-Path $targetDir "DashboardWeb.exe"
    if (-not (Test-Path -LiteralPath $dashboardExe)) {
        throw "DashboardWeb.exe not found after copy: $dashboardExe. Publish DashboardWeb self-contained before packaging."
    }

    Install-DashboardService `
        -ServiceName $DashboardServiceName `
        -DisplayName "TransferUDT Dashboard" `
        -Description "TransferUDT HTTPS observability dashboard service" `
        -DashboardExe $dashboardExe `
        -ContentRoot $targetDir `
        -Environment @(
            "TRANSFERUDT_DASHBOARD_CERT_PASSWORD=$DashboardCertificatePassword",
            "TRANSFERUDT_DASHBOARD_OPERATOR_PASSWORD=$DashboardOperatorPassword",
            "ASPNETCORE_ENVIRONMENT=Production"
        )
}

Assert-Administrator
New-Directory -Path $InstallRoot
New-Directory -Path $DataRoot

if (Test-ComponentSelected "Agent") {
    Install-Agent
}

if (Test-ComponentSelected "Server") {
    Install-Server
}

if (Test-ComponentSelected "Dashboard") {
    Install-Dashboard
}

Write-Host "TransferUDT installation completed."
Write-Host "Configuration files are under $DataRoot. Restart the affected service after changing a config file."
