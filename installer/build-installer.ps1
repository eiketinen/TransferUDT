<#
.SYNOPSIS
Builds the TransferUDT graphical Windows installer.
#>

[CmdletBinding()]
param(
    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Debug",

    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",

    [string]$DashboardTestPassword = "TransferUDT-Test-123!",

    [switch]$Incremental
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$buildModule = Join-Path $repoRoot "scripts\TransferUDT.Build.psm1"
Import-Module $buildModule -Force

function Find-MSBuild {
    $command = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($command) {
        return $command.Source
    }

    $candidatePaths = @(
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\2022\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles}\Microsoft Visual Studio\18\Community\MSBuild\Current\Bin\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe",
        "${env:ProgramFiles(x86)}\Microsoft Visual Studio\2022\BuildTools\MSBuild\Current\Bin\MSBuild.exe"
    )

    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path -LiteralPath $candidatePath) {
            return $candidatePath
        }
    }

    $vswhere = "${env:ProgramFiles(x86)}\Microsoft Visual Studio\Installer\vswhere.exe"
    if (Test-Path -LiteralPath $vswhere) {
        $installationPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($installationPath) {
            $candidatePath = Join-Path $installationPath "MSBuild\Current\Bin\amd64\MSBuild.exe"
            if (Test-Path -LiteralPath $candidatePath) {
                return $candidatePath
            }
        }
    }

    throw "MSBuild was not found. Install Visual Studio Build Tools with the C++ workload."
}

function Invoke-MSBuildSolution {
    param(
        [Parameter(Mandatory = $true)][string]$MSBuildPath,
        [Parameter(Mandatory = $true)][string]$SolutionPath,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$Platform
    )

    $target = if ($Incremental) { "Build" } else { "Rebuild" }
    Write-Host "$target $(Split-Path -Leaf $SolutionPath) $Configuration|$Platform"
    & $MSBuildPath $SolutionPath "/m" "/t:$target" "/p:Configuration=$Configuration" "/p:Platform=$Platform"
    if ($LASTEXITCODE -ne 0) {
        throw "MSBuild failed for $SolutionPath with exit code $LASTEXITCODE."
    }
}

function Resolve-VisualStudioInstallRoot {
    param([Parameter(Mandatory = $true)][string]$MSBuildPath)

    $directory = (Get-Item -LiteralPath $MSBuildPath).Directory
    while ($directory -and $directory.Name -ne "MSBuild") {
        $directory = $directory.Parent
    }
    if ($directory -and $directory.Parent) {
        return $directory.Parent.FullName
    }
    return $null
}

function Test-BytePattern {
    param(
        [Parameter(Mandatory = $true)][byte[]]$Haystack,
        [Parameter(Mandatory = $true)][byte[]]$Needle
    )

    if ($Needle.Length -eq 0 -or $Haystack.Length -lt $Needle.Length) {
        return $false
    }

    for ($i = 0; $i -le $Haystack.Length - $Needle.Length; $i++) {
        $matched = $true
        for ($j = 0; $j -lt $Needle.Length; $j++) {
            if ($Haystack[$i + $j] -ne $Needle[$j]) {
                $matched = $false
                break
            }
        }
        if ($matched) {
            return $true
        }
    }
    return $false
}

function Assert-BinaryContainsText {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Text
    )

    $bytes = [IO.File]::ReadAllBytes($Path)
    $needle = [Text.Encoding]::ASCII.GetBytes($Text)
    if (-not (Test-BytePattern -Haystack $bytes -Needle $needle)) {
        throw "Binary validation failed. '$Path' does not contain expected marker '$Text'. Rebuild before packaging."
    }
}

function New-InstallerAssets {
    param(
        [Parameter(Mandatory = $true)][string]$OutputDirectory,
        [Parameter(Mandatory = $true)][string]$DashboardTestPassword
    )

    $assetGenDir = Join-Path $PSScriptRoot "..\dist\installer-asset-generator"
    New-Item -ItemType Directory -Path $assetGenDir -Force | Out-Null

    $assetProject = Join-Path $assetGenDir "InstallerAssetGenerator.csproj"
    $assetProgram = Join-Path $assetGenDir "Program.cs"

    @'
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
</Project>
'@ | Set-Content -LiteralPath $assetProject -Encoding UTF8

    @'
using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

static string Pem(string label, byte[] der)
{
    var b64 = Convert.ToBase64String(der);
    var sb = new StringBuilder();
    sb.AppendLine($"-----BEGIN {label}-----");
    for (var i = 0; i < b64.Length; i += 64)
    {
        sb.AppendLine(b64.Substring(i, Math.Min(64, b64.Length - i)));
    }
    sb.AppendLine($"-----END {label}-----");
    return sb.ToString();
}

var dashboardPfxPath = args[0];
var dashboardPfxPassword = args[1];
var agentPrivateKeyPath = args[2];
var agentPublicKeyPath = args[3];

Directory.CreateDirectory(Path.GetDirectoryName(dashboardPfxPath)!);
Directory.CreateDirectory(Path.GetDirectoryName(agentPrivateKeyPath)!);
Directory.CreateDirectory(Path.GetDirectoryName(agentPublicKeyPath)!);

using (var rsa = RSA.Create(2048))
{
    var request = new CertificateRequest("CN=localhost", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
    request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
    request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, false));
    request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection { new Oid("1.3.6.1.5.5.7.3.1") }, false));
    var san = new SubjectAlternativeNameBuilder();
    san.AddDnsName("localhost");
    san.AddIpAddress(IPAddress.Parse("127.0.0.1"));
    request.CertificateExtensions.Add(san.Build());
    using var cert = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddYears(2));
    using var exportable = cert.HasPrivateKey ? cert : cert.CopyWithPrivateKey(rsa);
    File.WriteAllBytes(dashboardPfxPath, exportable.Export(X509ContentType.Pfx, dashboardPfxPassword));
}

using (var rsa = RSA.Create(3072))
{
    File.WriteAllText(agentPrivateKeyPath, Pem("PRIVATE KEY", rsa.ExportPkcs8PrivateKey()), Encoding.ASCII);
    File.WriteAllText(agentPublicKeyPath, Pem("PUBLIC KEY", rsa.ExportSubjectPublicKeyInfo()), Encoding.ASCII);
}
'@ | Set-Content -LiteralPath $assetProgram -Encoding UTF8

    $dashboardPfx = Join-Path $OutputDirectory "TransferUDT-Dashboard-Test.pfx"
    $agentPrivateKey = Join-Path $OutputDirectory "TransferUDT-Agent-Test.key"
    $agentPublicKey = Join-Path $OutputDirectory "TransferUDT-Agent-Test.pub"

    $assets = @($dashboardPfx, $agentPrivateKey, $agentPublicKey)
    $existingAssets = @($assets | Where-Object { Test-Path -LiteralPath $_ })
    if ($existingAssets.Count -eq $assets.Count) {
        Write-Host "Reusing existing default wizard assets"
        return
    }
    if ($existingAssets.Count -gt 0) {
        throw "Default wizard assets are incomplete. Remove all three test assets before regenerating them together: $($assets -join ', ')"
    }

    Write-Host "Generating default wizard assets"
    dotnet run --project $assetProject -- $dashboardPfx $DashboardTestPassword $agentPrivateKey $agentPublicKey
    if ($LASTEXITCODE -ne 0) {
        throw "Installer asset generation failed with exit code $LASTEXITCODE."
    }

    foreach ($asset in $assets) {
        if (-not (Test-Path -LiteralPath $asset)) {
            throw "Installer asset was not generated: $asset"
        }
    }
}

$isccCommand = Get-Command iscc.exe -ErrorAction SilentlyContinue
$isccPath = $null
if ($isccCommand) {
    $isccPath = $isccCommand.Source
}
else {
    $candidatePaths = @(
        "${env:ProgramFiles(x86)}\Inno Setup 6\ISCC.exe",
        "${env:ProgramFiles}\Inno Setup 6\ISCC.exe",
        "$env:LOCALAPPDATA\Programs\Inno Setup 6\ISCC.exe"
    )

    foreach ($candidatePath in $candidatePaths) {
        if (Test-Path -LiteralPath $candidatePath) {
            $isccPath = $candidatePath
            break
        }
    }
}

if (-not $isccPath) {
    throw "Inno Setup 6 was not found. Install it, then rerun this script."
}

$msbuildPath = Find-MSBuild
$visualStudioRoot = Resolve-VisualStudioInstallRoot -MSBuildPath $msbuildPath
if ($visualStudioRoot) {
    $env:VCPKG_VISUAL_STUDIO_PATH = $visualStudioRoot
    Write-Host "Using Visual Studio root for vcpkg: $visualStudioRoot"
}
$architectureInfo = Resolve-TransferUDTArchitecture -Architecture $Architecture
$vcPlatform = $architectureInfo.VcPlatform
$runtimeIdentifier = $architectureInfo.RuntimeIdentifier

$agentSolution = Join-Path $repoRoot "AgentUDTC++_v7.2\AgentUDTC++.sln"
$serverSolution = Join-Path $repoRoot "ServerUDTC++_v3\ServerUDTC++.sln"
Invoke-MSBuildSolution -MSBuildPath $msbuildPath -SolutionPath $agentSolution -Configuration $Configuration -Platform $vcPlatform
Invoke-MSBuildSolution -MSBuildPath $msbuildPath -SolutionPath $serverSolution -Configuration $Configuration -Platform $vcPlatform

$dashboardProject = Join-Path $repoRoot "DashboardWeb\DashboardWeb.csproj"
$dashboardPublishDir = Get-TransferUDTDashboardPublishDirectory -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture
if (Test-Path -LiteralPath $dashboardProject) {
    Write-Host "Publishing DashboardWeb self-contained $runtimeIdentifier to $dashboardPublishDir"
    dotnet publish $dashboardProject -c $Configuration -r $runtimeIdentifier --self-contained true -o $dashboardPublishDir -p:DebugType=None -p:DebugSymbols=false
    if ($LASTEXITCODE -ne 0) {
        throw "dotnet publish DashboardWeb failed with exit code $LASTEXITCODE."
    }
}

New-InstallerAssets -OutputDirectory $PSScriptRoot -DashboardTestPassword $DashboardTestPassword

$agentExe = Resolve-TransferUDTExecutable -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture -Kind Agent -Required
$serverExe = Resolve-TransferUDTExecutable -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture -Kind Server -Required
$dashboardExe = Resolve-TransferUDTExecutable -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture -Kind Dashboard -Required

foreach ($requiredPath in @($agentExe, $serverExe, $dashboardExe)) {
    if (-not (Test-Path -LiteralPath $requiredPath)) {
        throw "Required build output is missing: $requiredPath"
    }
}

Assert-TransferUDTPEMachine -Path $agentExe -Architecture $Architecture | Out-Null
Assert-TransferUDTPEMachine -Path $serverExe -Architecture $Architecture | Out-Null
Assert-TransferUDTPEMachine -Path $dashboardExe -Architecture $Architecture | Out-Null
Assert-BinaryContainsText -Path $agentExe -Text "Dashboard heartbeat started."

$scriptPath = Join-Path $PSScriptRoot "TransferUDT.iss"
& $isccPath "/DConfiguration=$Configuration" "/DPackageArchitecture=$Architecture" $scriptPath

if ($LASTEXITCODE -ne 0) {
    throw "Inno Setup compiler failed with exit code $LASTEXITCODE."
}

$outputInstallerName = Get-TransferUDTInstallerFileName -Configuration $Configuration -Architecture $Architecture
$completeInstallerName = Get-TransferUDTInstallerFileName -Configuration $Configuration -Architecture $Architecture -Complete
$outputInstaller = Join-Path $repoRoot "dist\$outputInstallerName"
$completeInstaller = Join-Path $repoRoot "dist\$completeInstallerName"
Copy-Item -LiteralPath $outputInstaller -Destination $completeInstaller -Force

Write-Host "Installer generated under dist\$outputInstallerName"
Write-Host "Complete installer copy generated under dist\$completeInstallerName"
