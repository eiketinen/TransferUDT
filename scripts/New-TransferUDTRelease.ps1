<#
.SYNOPSIS
Creates an official TransferUDT Windows release package.

.DESCRIPTION
Optionally rebuilds the installer, optionally runs Release tests, copies
documentation, builds a sanitized source archive, and writes SHA256 checksums.
#>

[CmdletBinding()]
param(
    [ValidatePattern('^[0-9]+\.[0-9]+\.[0-9]+([-.][A-Za-z0-9]+)?$')]
    [string]$Version = "1.0.0",

    [ValidateSet("Debug", "Release")]
    [string]$Configuration = "Release",

    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",

    [switch]$Build,

    [switch]$RunTests,

    [switch]$ReuseValidatedBuild,

    [switch]$ReuseValidatedTests
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot "..")).Path
$buildModule = Join-Path $repoRoot "scripts\TransferUDT.Build.psm1"
Import-Module $buildModule -Force

$distRoot = Join-Path $repoRoot "dist"
$releaseBase = Join-Path $distRoot "releases"
$stagingBase = Join-Path ([System.IO.Path]::GetTempPath()) "TransferUDT-release-staging"
$architectureInfo = Resolve-TransferUDTArchitecture -Architecture $Architecture
$platformLabel = $architectureInfo.PlatformLabel
$releaseName = Get-TransferUDTReleaseName -Version $Version -Architecture $Architecture
$releaseRoot = Join-Path $releaseBase $releaseName
$stagingRoot = Join-Path $stagingBase $releaseName

function Get-FullPath {
    param([Parameter(Mandatory = $true)][string]$Path)
    return [System.IO.Path]::GetFullPath($Path)
}

function Assert-UnderRoot {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$Root
    )

    $fullPath = Get-FullPath $Path
    $fullRoot = Get-FullPath $Root
    if (-not $fullRoot.EndsWith([System.IO.Path]::DirectorySeparatorChar)) {
        $fullRoot += [System.IO.Path]::DirectorySeparatorChar
    }

    if (-not $fullPath.StartsWith($fullRoot, [System.StringComparison]::OrdinalIgnoreCase)) {
        throw "Refusing to operate outside expected root. Path='$fullPath' Root='$fullRoot'"
    }
}

function Reset-Directory {
    param(
        [Parameter(Mandatory = $true)][string]$Path,
        [Parameter(Mandatory = $true)][string]$AllowedRoot
    )

    Assert-UnderRoot -Path $Path -Root $AllowedRoot
    if (Test-Path -LiteralPath $Path) {
        Remove-Item -LiteralPath $Path -Recurse -Force
    }
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
}

function Copy-FileToDirectory {
    param(
        [Parameter(Mandatory = $true)][string]$Source,
        [Parameter(Mandatory = $true)][string]$DestinationDirectory,
        [string]$DestinationName
    )

    if (-not (Test-Path -LiteralPath $Source)) {
        throw "Required file is missing: $Source"
    }
    New-Item -ItemType Directory -Path $DestinationDirectory -Force | Out-Null
    $name = if ($DestinationName) { $DestinationName } else { Split-Path -Leaf $Source }
    Copy-Item -LiteralPath $Source -Destination (Join-Path $DestinationDirectory $name) -Force
}

function Test-SourceFileExcluded {
    param(
        [Parameter(Mandatory = $true)][string]$RelativePath,
        [Parameter(Mandatory = $true)][System.IO.FileInfo]$File
    )

    $normalized = $RelativePath -replace '/', '\'
    $segments = $normalized -split '\\'
    $excludedSegments = @(
        ".git", ".vs", ".claude", "bin", "obj", "x64", "x86", "Win32", "Debug", "Release",
        "vcpkg_installed", "dist", "data", "storage", "reconstructed", "logs",
        "log", "e2e-runs"
    )
    foreach ($segment in $segments) {
        if ($excludedSegments -contains $segment) {
            return $true
        }
    }

    $excludedExtensions = @(
        ".exe", ".dll", ".lib", ".exp", ".pdb", ".ilk", ".obj", ".idb",
        ".tlog", ".recipe", ".log", ".db", ".sqlite", ".sqlite3", ".pem",
        ".key", ".pfx", ".p12", ".resolved"
    )
    if ($excludedExtensions -contains $File.Extension.ToLowerInvariant()) {
        return $true
    }

    if ($File.Name -eq "config.properties") {
        return $true
    }
    if ($File.Name -like "*.local.properties") {
        return $true
    }
    if ($normalized -like "installer\TransferUDT-*-Test.*") {
        return $true
    }

    return $false
}

function Copy-SourceTree {
    param(
        [Parameter(Mandatory = $true)][string]$SourceRoot,
        [Parameter(Mandatory = $true)][string]$DestinationRoot
    )

    if (-not (Test-Path -LiteralPath $SourceRoot)) {
        return
    }

    $rootFull = Get-FullPath $SourceRoot
    Get-ChildItem -LiteralPath $SourceRoot -Recurse -File -Force | ForEach-Object {
        $relative = $_.FullName.Substring($rootFull.Length).TrimStart('\', '/')
        $sourceRelative = Join-Path (Split-Path -Leaf $SourceRoot) $relative
        if (Test-SourceFileExcluded -RelativePath $sourceRelative -File $_) {
            return
        }

        $destination = Join-Path $DestinationRoot $sourceRelative
        New-Item -ItemType Directory -Path (Split-Path -Parent $destination) -Force | Out-Null
        Copy-Item -LiteralPath $_.FullName -Destination $destination -Force
    }
}

function Write-Sha256Sums {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$OutputPath
    )

    $baseFull = Get-FullPath $BaseDirectory
    $outputFull = Get-FullPath $OutputPath
    $lines = New-Object System.Collections.Generic.List[string]

    Get-ChildItem -LiteralPath $BaseDirectory -Recurse -File | Where-Object {
        (Get-FullPath $_.FullName) -ne $outputFull
    } | Sort-Object FullName | ForEach-Object {
        $relative = $_.FullName.Substring($baseFull.Length).TrimStart('\', '/') -replace '\\', '/'
        $hash = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash
        $lines.Add("$hash  $relative")
    }

    Set-Content -LiteralPath $OutputPath -Value $lines -Encoding ASCII
}

function Assert-RequiredFile {
    param([Parameter(Mandatory = $true)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        throw "Required file is missing: $Path"
    }
}

function Assert-SourceArchiveClean {
    param([Parameter(Mandatory = $true)][string]$ZipPath)

    Assert-RequiredFile -Path $ZipPath
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $excludedSegments = @(
        ".git", ".vs", ".claude", "bin", "obj", "x64", "x86", "Win32", "Debug", "Release",
        "vcpkg_installed", "dist", "data", "storage", "reconstructed", "logs",
        "log", "e2e-runs"
    )
    $excludedExtensions = @(
        ".exe", ".dll", ".lib", ".exp", ".pdb", ".ilk", ".obj", ".idb",
        ".tlog", ".recipe", ".log", ".db", ".sqlite", ".sqlite3", ".pem",
        ".key", ".pfx", ".p12", ".resolved"
    )

    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $fileCount = 0
        foreach ($entry in $archive.Entries) {
            if ([string]::IsNullOrEmpty($entry.Name)) {
                continue
            }

            $fileCount++
            $normalized = $entry.FullName -replace '/', '\'
            $segments = $normalized -split '\\'
            foreach ($segment in $segments) {
                if ($excludedSegments -contains $segment) {
                    throw "Source archive contains excluded segment '$segment': $($entry.FullName)"
                }
            }

            $extension = [System.IO.Path]::GetExtension($entry.Name).ToLowerInvariant()
            if ($excludedExtensions -contains $extension) {
                throw "Source archive contains excluded extension '$extension': $($entry.FullName)"
            }

            if ($entry.Name -eq "config.properties" -or $entry.Name -like "*.local.properties") {
                throw "Source archive contains local configuration: $($entry.FullName)"
            }
            if ($normalized -like "installer\TransferUDT-*-Test.*") {
                throw "Source archive contains generated installer test asset: $($entry.FullName)"
            }
        }

        if ($fileCount -eq 0) {
            throw "Source archive is empty: $ZipPath"
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-Sha256SumsFile {
    param(
        [Parameter(Mandatory = $true)][string]$BaseDirectory,
        [Parameter(Mandatory = $true)][string]$SumsPath
    )

    Assert-RequiredFile -Path $SumsPath
    $lines = Get-Content -LiteralPath $SumsPath
    if (@($lines).Count -eq 0) {
        throw "Checksum file is empty: $SumsPath"
    }

    foreach ($line in $lines) {
        if ([string]::IsNullOrWhiteSpace($line)) {
            continue
        }
        if ($line -notmatch '^\s*(?<Hash>[A-Fa-f0-9]{64})\s+\*?(?<RelativePath>.+?)\s*$') {
            throw "Invalid checksum line in '$SumsPath': $line"
        }

        $expectedHash = $Matches.Hash.ToUpperInvariant()
        $relativePath = $Matches.RelativePath -replace '/', '\'
        if ([System.IO.Path]::IsPathRooted($relativePath)) {
            throw "Checksum path must be relative in '$SumsPath': $relativePath"
        }

        $targetPath = Join-Path $BaseDirectory $relativePath
        Assert-UnderRoot -Path $targetPath -Root $BaseDirectory
        Assert-RequiredFile -Path $targetPath
        $actualHash = (Get-FileHash -LiteralPath $targetPath -Algorithm SHA256).Hash.ToUpperInvariant()
        if ($actualHash -ne $expectedHash) {
            throw "Checksum mismatch for '$targetPath'. Expected=$expectedHash Actual=$actualHash"
        }
    }
}

function Assert-ReleaseManifest {
    param(
        [Parameter(Mandatory = $true)][string]$ManifestPath,
        [Parameter(Mandatory = $true)][string]$ReleaseRoot,
        [Parameter(Mandatory = $true)][string]$Version,
        [Parameter(Mandatory = $true)][string]$PlatformLabel,
        [Parameter(Mandatory = $true)][string]$InstallerName,
        [Parameter(Mandatory = $true)][string]$SourceZipName,
        [Parameter(Mandatory = $true)][bool]$BuildExecuted,
        [Parameter(Mandatory = $true)][bool]$TestsExecuted
    )

    Assert-RequiredFile -Path $ManifestPath
    $manifest = Get-Content -LiteralPath $ManifestPath -Raw | ConvertFrom-Json
    $expectedInstaller = "bin/$InstallerName"
    $expectedSource = "source/$SourceZipName"

    if ($manifest.product -ne "TransferUDT") {
        throw "Manifest product mismatch: $($manifest.product)"
    }
    if ($manifest.version -ne $Version) {
        throw "Manifest version mismatch. Expected=$Version Actual=$($manifest.version)"
    }
    if ($manifest.platform -ne $PlatformLabel) {
        throw "Manifest platform mismatch. Expected=$PlatformLabel Actual=$($manifest.platform)"
    }
    if ($manifest.installer -ne $expectedInstaller) {
        throw "Manifest installer mismatch. Expected=$expectedInstaller Actual=$($manifest.installer)"
    }
    if ($manifest.source -ne $expectedSource) {
        throw "Manifest source mismatch. Expected=$expectedSource Actual=$($manifest.source)"
    }
    if ([bool]$manifest.buildExecuted -ne $BuildExecuted) {
        throw "Manifest buildExecuted mismatch. Expected=$BuildExecuted Actual=$($manifest.buildExecuted)"
    }
    if ([bool]$manifest.testsExecuted -ne $TestsExecuted) {
        throw "Manifest testsExecuted mismatch. Expected=$TestsExecuted Actual=$($manifest.testsExecuted)"
    }

    Assert-RequiredFile -Path (Join-Path $ReleaseRoot ($expectedInstaller -replace '/', '\'))
    Assert-RequiredFile -Path (Join-Path $ReleaseRoot ($expectedSource -replace '/', '\'))
}

function Assert-ReleaseZipContents {
    param(
        [Parameter(Mandatory = $true)][string]$ZipPath,
        [Parameter(Mandatory = $true)][string[]]$ExpectedEntries
    )

    Assert-RequiredFile -Path $ZipPath
    Add-Type -AssemblyName System.IO.Compression.FileSystem

    $archive = [System.IO.Compression.ZipFile]::OpenRead($ZipPath)
    try {
        $entries = @($archive.Entries | ForEach-Object { $_.FullName -replace '\\', '/' })
        foreach ($expectedEntry in $ExpectedEntries) {
            if ($entries -notcontains $expectedEntry) {
                throw "Release zip is missing expected entry '$expectedEntry': $ZipPath"
            }
        }
    }
    finally {
        $archive.Dispose()
    }
}

function Assert-ReleaseBuildOutputs {
    param(
        [Parameter(Mandatory = $true)][string]$RepoRoot,
        [Parameter(Mandatory = $true)][string]$Configuration,
        [Parameter(Mandatory = $true)][string]$Architecture
    )

    foreach ($kind in @("Agent", "Server", "Dashboard")) {
        $exe = Resolve-TransferUDTExecutable -RepoRoot $RepoRoot -Configuration $Configuration -Architecture $Architecture -Kind $kind -Required
        Assert-TransferUDTPEMachine -Path $exe -Architecture $Architecture | Out-Null
    }
}

New-Item -ItemType Directory -Path $distRoot, $releaseBase, $stagingBase -Force | Out-Null

if ($Build) {
    $buildScript = Join-Path $repoRoot "installer\build-installer.ps1"
    & powershell.exe -NoProfile -ExecutionPolicy Bypass -File $buildScript -Configuration $Configuration -Architecture $Architecture
    if ($LASTEXITCODE -ne 0) {
        throw "Installer build failed with exit code $LASTEXITCODE."
    }
}

if ($RunTests) {
    $agentTests = Resolve-TransferUDTExecutable -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture -Kind AgentTests -Required
    $serverTests = Resolve-TransferUDTExecutable -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture -Kind ServerTests -Required

    foreach ($testExe in @($agentTests, $serverTests)) {
        if (-not (Test-Path -LiteralPath $testExe)) {
            throw "Test executable is missing: $testExe"
        }
        & $testExe
        if ($LASTEXITCODE -ne 0) {
            throw "Test run failed: $testExe"
        }
    }
}

Reset-Directory -Path $releaseRoot -AllowedRoot $releaseBase
Reset-Directory -Path $stagingRoot -AllowedRoot $stagingBase

$installerSource = Join-Path $distRoot (Get-TransferUDTInstallerFileName -Configuration $Configuration -Architecture $Architecture -Complete)
$installerName = "TransferUDT-$Version-$platformLabel-setup.exe"
$installerOut = Join-Path $releaseRoot "bin\$installerName"
Copy-FileToDirectory -Source $installerSource -DestinationDirectory (Split-Path -Parent $installerOut) -DestinationName $installerName

$releaseDocsDir = Join-Path $releaseRoot "docs"
$guideFileName = "TransferUDT-$Version-user-guide.html"
$configReferenceFileName = "TransferUDT-$Version-config-reference.html"
$docFiles = @(
    "docs\html\$guideFileName",
    "docs\html\$configReferenceFileName"
)

foreach ($doc in $docFiles) {
    $path = Join-Path $repoRoot $doc
    Copy-FileToDirectory -Source $path -DestinationDirectory $releaseDocsDir
}

$htmlGuide = Join-Path $releaseDocsDir $guideFileName
Copy-Item -LiteralPath $htmlGuide -Destination (Join-Path $releaseRoot "README.html") -Force
$htmlConfigReference = Join-Path $releaseDocsDir $configReferenceFileName
Copy-Item -LiteralPath $htmlConfigReference -Destination (Join-Path $releaseRoot $configReferenceFileName) -Force

foreach ($rootFile in @("LICENSE", "SECURITY.md", "CONTRIBUTING.md")) {
    Copy-FileToDirectory -Source (Join-Path $repoRoot $rootFile) -DestinationDirectory $releaseRoot
}

$sourceRoot = Join-Path $stagingRoot "TransferUDT-$Version-source"
New-Item -ItemType Directory -Path $sourceRoot -Force | Out-Null

foreach ($rootFile in @(".editorconfig", ".gitattributes", ".gitignore", "README.md", "LICENSE", "SECURITY.md", "CONTRIBUTING.md")) {
    $path = Join-Path $repoRoot $rootFile
    if (Test-Path -LiteralPath $path) {
        Copy-FileToDirectory -Source $path -DestinationDirectory $sourceRoot
    }
}

foreach ($sourceDir in @(
    "AgentUDTC++_v7.2",
    "ServerUDTC++_v3",
    "TransferCore",
    "DashboardWeb",
    "installer",
    "scripts",
    "docs",
    ".github"
)) {
    Copy-SourceTree -SourceRoot (Join-Path $repoRoot $sourceDir) -DestinationRoot $sourceRoot
}

$sourcePackageDir = Join-Path $releaseRoot "source"
New-Item -ItemType Directory -Path $sourcePackageDir -Force | Out-Null
$sourceZip = Join-Path $sourcePackageDir "TransferUDT-$Version-source.zip"
$sourceZipTemp = Join-Path $stagingRoot "TransferUDT-$Version-source.zip"
if (Test-Path -LiteralPath $sourceZip) {
    Remove-Item -LiteralPath $sourceZip -Force
}
if (Test-Path -LiteralPath $sourceZipTemp) {
    Remove-Item -LiteralPath $sourceZipTemp -Force
}
Compress-Archive -Path (Join-Path $sourceRoot "*") -DestinationPath $sourceZipTemp -Force
Copy-Item -LiteralPath $sourceZipTemp -Destination $sourceZip -Force

$manifest = [ordered]@{
    product = "TransferUDT"
    version = $Version
    platform = $platformLabel
    configuration = $Configuration
    createdAt = (Get-Date).ToString("o")
    installer = "bin/$installerName"
    source = "source/TransferUDT-$Version-source.zip"
    buildExecuted = [bool]($Build -or $ReuseValidatedBuild)
    testsExecuted = [bool]($RunTests -or $ReuseValidatedTests)
}
$manifestPath = Join-Path $releaseRoot "release-manifest.json"
$manifest | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath $manifestPath -Encoding ASCII

$internalSums = Join-Path $releaseRoot "SHA256SUMS.txt"
Write-Sha256Sums -BaseDirectory $releaseRoot -OutputPath $internalSums

Write-Host "Validating release directory..."
Assert-ReleaseBuildOutputs -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture
Get-TransferUDTPEMachine -Path $installerOut | Out-Null
Assert-SourceArchiveClean -ZipPath $sourceZip
Assert-ReleaseManifest `
    -ManifestPath $manifestPath `
    -ReleaseRoot $releaseRoot `
    -Version $Version `
    -PlatformLabel $platformLabel `
    -InstallerName $installerName `
    -SourceZipName (Split-Path -Leaf $sourceZip) `
    -BuildExecuted ([bool]($Build -or $ReuseValidatedBuild)) `
    -TestsExecuted ([bool]($RunTests -or $ReuseValidatedTests))
Assert-Sha256SumsFile -BaseDirectory $releaseRoot -SumsPath $internalSums

$releaseZip = Join-Path $releaseBase "$releaseName.zip"
$releaseZipTemp = Join-Path $stagingRoot "$releaseName.zip"
if (Test-Path -LiteralPath $releaseZip) {
    Remove-Item -LiteralPath $releaseZip -Force
}
if (Test-Path -LiteralPath $releaseZipTemp) {
    Remove-Item -LiteralPath $releaseZipTemp -Force
}
Compress-Archive -Path (Join-Path $releaseRoot "*") -DestinationPath $releaseZipTemp -Force
Copy-Item -LiteralPath $releaseZipTemp -Destination $releaseZip -Force

$externalSums = Join-Path $releaseBase "$releaseName-SHA256SUMS.txt"
$externalLines = @(
    "$((Get-FileHash -LiteralPath $releaseZip -Algorithm SHA256).Hash)  $(Split-Path -Leaf $releaseZip)",
    "$((Get-FileHash -LiteralPath $installerOut -Algorithm SHA256).Hash)  $releaseName/bin/$installerName",
    "$((Get-FileHash -LiteralPath $sourceZip -Algorithm SHA256).Hash)  $releaseName/source/$(Split-Path -Leaf $sourceZip)"
)
Set-Content -LiteralPath $externalSums -Value $externalLines -Encoding ASCII

Write-Host "Validating release zip..."
Assert-ReleaseZipContents -ZipPath $releaseZip -ExpectedEntries @(
    "README.html",
    "$configReferenceFileName",
    "SHA256SUMS.txt",
    "release-manifest.json",
    "bin/$installerName",
    "docs/$configReferenceFileName",
    "source/$(Split-Path -Leaf $sourceZip)"
)
Assert-Sha256SumsFile -BaseDirectory $releaseBase -SumsPath $externalSums

Write-Host "Release directory: $releaseRoot"
Write-Host "Release zip: $releaseZip"
Write-Host "Checksums: $externalSums"
