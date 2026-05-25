param(
    [string]$Configuration = "Debug",
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [int]$Port = 0,
    [string]$RunRoot = "",
    [int]$TimeoutSeconds = 90,
    [int64]$PayloadBytes = 524288,
    [string]$PayloadRelativePath = "signed-handshake-e2e.bin",
    [switch]$VerifyChangedContentResend,
    [switch]$KeepRunRoot
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function New-FreeTcpPort {
    for ($attempt = 0; $attempt -lt 100; $attempt++) {
        $candidate = Get-Random -Minimum 51000 -Maximum 60000
        $listener = [System.Net.Sockets.TcpListener]::new([System.Net.IPAddress]::Loopback, $candidate)
        try {
            $listener.Start()
            return $candidate
        }
        catch {
            Start-Sleep -Milliseconds 20
        }
        finally {
            $listener.Stop()
        }
    }
    throw "Could not find an available local test port."
}

function Convert-ToConfigPath([string]$Path) {
    if (Test-Path $Path) {
        return (Resolve-Path $Path).Path
    }
    return [System.IO.Path]::GetFullPath($Path)
}

function New-Directory([string]$Path) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    return (Resolve-Path $Path).Path
}

function New-RsaPemKeyPair([string]$PrivateKeyPath, [string]$PublicKeyPath, [string]$WorkDir) {
    $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnet) {
        throw "dotnet is required to generate temporary PEM keys for this e2e test."
    }

    $projectDir = New-Directory (Join-Path $WorkDir "keygen")
    $csprojPath = Join-Path $projectDir "KeyGen.csproj"
    $programPath = Join-Path $projectDir "Program.cs"

    @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net9.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
</Project>
"@ | Set-Content -Path $csprojPath -Encoding UTF8

    @"
using System.Security.Cryptography;
using System.Text;

static void WritePem(string path, string label, byte[] der)
{
    var b64 = Convert.ToBase64String(der);
    using var writer = new StreamWriter(path, false, Encoding.ASCII);
    writer.WriteLine($"-----BEGIN {label}-----");
    for (var i = 0; i < b64.Length; i += 64)
    {
        writer.WriteLine(b64.Substring(i, Math.Min(64, b64.Length - i)));
    }
    writer.WriteLine($"-----END {label}-----");
}

if (args.Length != 2)
{
    Console.Error.WriteLine("usage: KeyGen <private-key-path> <public-key-path>");
    return 2;
}

using var rsa = RSA.Create(3072);
WritePem(args[0], "PRIVATE KEY", rsa.ExportPkcs8PrivateKey());
WritePem(args[1], "PUBLIC KEY", rsa.ExportSubjectPublicKeyInfo());
return 0;
"@ | Set-Content -Path $programPath -Encoding UTF8

    $previousDotnetCliHome = $env:DOTNET_CLI_HOME
    $previousDotnetNoLogo = $env:DOTNET_NOLOGO
    $env:DOTNET_CLI_HOME = New-Directory (Join-Path $WorkDir ".dotnet")
    $env:DOTNET_NOLOGO = "1"
    try {
        & $dotnet.Source run --project $csprojPath -- $PrivateKeyPath $PublicKeyPath | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to generate RSA PEM key pair with dotnet."
        }
    }
    finally {
        $env:DOTNET_CLI_HOME = $previousDotnetCliHome
        $env:DOTNET_NOLOGO = $previousDotnetNoLogo
    }
}

function Install-LocalConfig([string]$Name, [string]$ExePath, [string]$SourceConfigPath, [string]$BackupDir) {
    $targetConfigPath = Join-Path (Split-Path -Parent (Resolve-Path $ExePath).Path) "config.properties"
    $backupPath = Join-Path $BackupDir "$Name.config.properties.bak"
    $hadOriginal = Test-Path $targetConfigPath
    if ($hadOriginal) {
        Copy-Item -LiteralPath $targetConfigPath -Destination $backupPath -Force
    }
    Copy-Item -LiteralPath $SourceConfigPath -Destination $targetConfigPath -Force
    return [pscustomobject]@{
        Target = $targetConfigPath
        Backup = $backupPath
        HadOriginal = $hadOriginal
    }
}

function Restore-LocalConfigs($InstalledConfigs) {
    foreach ($entry in @($InstalledConfigs)) {
        if ($entry.HadOriginal -and (Test-Path $entry.Backup)) {
            Copy-Item -LiteralPath $entry.Backup -Destination $entry.Target -Force
        }
        elseif (Test-Path $entry.Target) {
            Remove-Item -LiteralPath $entry.Target -Force -ErrorAction SilentlyContinue
        }
    }
}

function Start-TransferProcess([string]$Name, [string]$ExePath) {
    if (-not (Test-Path $ExePath)) {
        throw "$Name executable not found: $ExePath"
    }

    $psi = [System.Diagnostics.ProcessStartInfo]::new()
    $psi.FileName = (Resolve-Path $ExePath).Path
    $psi.Arguments = "/debug"
    $psi.WorkingDirectory = Split-Path -Parent $psi.FileName
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true

    $process = [System.Diagnostics.Process]::new()
    $process.StartInfo = $psi
    if (-not $process.Start()) {
        throw "Failed to start $Name."
    }
    return $process
}

function Stop-TransferProcess($Process) {
    if ($null -ne $Process -and -not $Process.HasExited) {
        Stop-Process -Id $Process.Id -Force -ErrorAction SilentlyContinue
        try {
            $Process.WaitForExit(5000) | Out-Null
        }
        catch {
            # Best effort cleanup.
        }
    }
}

function Wait-ForLogText([string]$LogPath, [string]$Pattern, [int]$TimeoutSeconds, $Process, [string]$Name) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        if ($null -ne $Process -and $Process.HasExited) {
            throw "$Name exited before writing expected log '$Pattern'. ExitCode=$($Process.ExitCode)"
        }
        if (Test-Path $LogPath) {
            $content = Get-Content -Path $LogPath -Raw -ErrorAction SilentlyContinue
            if ($content -match $Pattern) {
                return
            }
        }
        Start-Sleep -Milliseconds 250
    }
    throw "Timed out waiting for '$Pattern' in $LogPath"
}

function Wait-ForReconstructedFile([string]$Root, [string]$FileName, [int]$ExpectedLength, [string]$ExpectedHash, [int]$TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastHash = ""
    while ((Get-Date) -lt $deadline) {
        $candidate = Get-ChildItem -Path $Root -Recurse -File -Filter $FileName -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate -and $candidate.Length -eq $ExpectedLength) {
            $lastHash = (Get-FileHash -Algorithm SHA256 -Path $candidate.FullName).Hash
            if ($lastHash -eq $ExpectedHash) {
                return $candidate.FullName
            }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($lastHash) {
        throw "Timed out waiting for reconstructed file '$FileName' under $Root to match SHA256. LastHash=$lastHash ExpectedHash=$ExpectedHash"
    }
    throw "Timed out waiting for reconstructed file '$FileName' under $Root"
}

function Resolve-SafePayloadPath([string]$Root, [string]$RelativePath) {
    if ([string]::IsNullOrWhiteSpace($RelativePath)) {
        throw "PayloadRelativePath cannot be empty."
    }

    $normalized = $RelativePath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    if ([System.IO.Path]::IsPathRooted($normalized)) {
        throw "PayloadRelativePath must be relative."
    }

    $parts = $normalized.Split([System.IO.Path]::DirectorySeparatorChar, [System.StringSplitOptions]::RemoveEmptyEntries)
    if ($parts.Count -eq 0) {
        throw "PayloadRelativePath must include a file name."
    }
    foreach ($part in $parts) {
        if ($part -eq "." -or $part -eq "..") {
            throw "PayloadRelativePath cannot contain traversal segments."
        }
    }

    return Join-Path $Root $normalized
}

function Write-AllBytesWithRetry([string]$Path, [byte[]]$Bytes, [int]$TimeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastError = $null

    while ($true) {
        try {
            [System.IO.File]::WriteAllBytes($Path, $Bytes)
            return
        }
        catch [System.IO.IOException] {
            $lastError = $_.Exception
        }
        catch [System.UnauthorizedAccessException] {
            $lastError = $_.Exception
        }

        if ((Get-Date) -ge $deadline) {
            throw "Timed out waiting to write payload '$Path': $($lastError.Message)"
        }
        Start-Sleep -Milliseconds 250
    }
}

$repoRoot = Get-RepoRoot
Import-Module (Join-Path $repoRoot "scripts\TransferUDT.Build.psm1") -Force
if ($Port -eq 0) {
    $Port = New-FreeTcpPort
}
if ([string]::IsNullOrWhiteSpace($RunRoot)) {
    $RunRoot = Join-Path $repoRoot ("e2e-runs\transferudt-e2e-signed-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}

$RunRoot = New-Directory $RunRoot
$keysDir = New-Directory (Join-Path $RunRoot "keys")
$agentRoot = New-Directory (Join-Path $RunRoot "agent")
$serverRoot = New-Directory (Join-Path $RunRoot "server")
$watchDir = New-Directory (Join-Path $agentRoot "watch")
$serverStorage = New-Directory (Join-Path $serverRoot "storage")
$serverReconstructed = New-Directory (Join-Path $serverRoot "reconstructed")
$agentLogs = New-Directory (Join-Path $agentRoot "logs")
$serverLogs = New-Directory (Join-Path $serverRoot "logs")
$agentDb = New-Directory (Join-Path $agentRoot "db")
$serverDb = New-Directory (Join-Path $serverRoot "db")

$serverPrivateKey = Join-Path $keysDir "server.key"
$serverPublicKey = Join-Path $keysDir "server.pub"
$agentPrivateKey = Join-Path $keysDir "agent.key"
$agentPublicKey = Join-Path $keysDir "agent.pub"
New-RsaPemKeyPair $serverPrivateKey $serverPublicKey $RunRoot
New-RsaPemKeyPair $agentPrivateKey $agentPublicKey $RunRoot

$serverConfig = Join-Path $RunRoot "server.properties"
$agentConfig = Join-Path $RunRoot "agent.properties"
$serverLog = Join-Path $serverLogs "server.log"
$agentLog = Join-Path $agentLogs "agent.log"
$clientId = "agent-e2e"

@"
server.port = $Port
server.bind_address = 127.0.0.1
server.allowed_clients = 127.0.0.1
server.storage_path = $(Convert-ToConfigPath $serverStorage)
server.reconstructed_path = $(Convert-ToConfigPath $serverReconstructed)
server.max_file_size_mb = 1024
server.max_client_storage_mb = 2048
max.connection = 16
work.threads = 2
work.max_pending_tasks = 64
circuitbreaker.failure.threshold = 5
circuitbreaker.reset.timeout.seconds = 30
log.filepath = $(Convert-ToConfigPath $serverLog)
db.filepath = $(Join-Path $serverDb "server.db")
log.max_size_mb = 10
log.backup_count = 2
log.flush_level = always
max.band = 10
seg.len = 1350
snd.buf = 64
rcv.buf = 64
snd.timeout = 20
rcv.timeout = 20
ling.on = 0
ling.time = 5
test.ack_delay_pattern_ms =
resend.changed_files.server_policy = overwrite
security.enabled = true
security.handshake.enabled = true
security.allow_insecure = false
security.identity.mode = signed_handshake
security.allowed_client_ids = $clientId
security.server_private_key_path = $(Convert-ToConfigPath $serverPrivateKey)
security.client_public_key.$clientId = $(Convert-ToConfigPath $agentPublicKey)
"@ | Set-Content -Path $serverConfig -Encoding ASCII

@"
server.targets = 127.0.0.1:$Port
chunk.size = 64
chunk.adaptive.enabled = false
data.dirs = $(Convert-ToConfigPath $watchDir)
resend.changed_files.enabled = true
resend.changed_files.identity = sha256
max.band = 10
seg.len = 1350
snd.buf = 64
rcv.buf = 64
snd.timeout = 20
rcv.timeout = 20
max.retries = 3
max.retries.abandon = 10
retry.jitter.max.ms = 25
work.threads = 2
stability.check.interval.seconds = 1
stability.check.count = 1
watcher.check.interval.seconds = 1
pending.check.interval.seconds = 2
circuitbreaker.failure.threshold = 5
circuitbreaker.reset.timeout.seconds = 10
memory.usage.percent.limit = 95
log.filepath = $(Convert-ToConfigPath $agentLog)
log.max_size_mb = 10
log.backup_count = 2
log.move.rotate = true
log.flush_level = always
watcher.exclude.files = agent.log
db.filepath = $(Join-Path $agentDb "agent.db")
pool.size = 1
udt.require.greeting = true
udt.expected.greeting = READY
udt.greeting.timeout.ms = 5000
udt.keepalive.enabled = true
udt.keepalive.interval.seconds = 30
udt.keepalive.payload = PING
max.file.processing.attempts = 3
security.enabled = true
security.handshake.enabled = true
security.allow_insecure = false
security.identity.mode = signed_handshake
security.client_id = $clientId
security.client_private_key_path = $(Convert-ToConfigPath $agentPrivateKey)
security.server_public_key_path = $(Convert-ToConfigPath $serverPublicKey)
"@ | Set-Content -Path $agentConfig -Encoding ASCII

$serverExe = Resolve-TransferUDTExecutable -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture -Kind Server -Required
$agentExe = Resolve-TransferUDTExecutable -RepoRoot $repoRoot -Configuration $Configuration -Architecture $Architecture -Kind Agent -Required

$serverProcess = $null
$agentProcess = $null
$installedConfigs = @()
$succeeded = $false
try {
    Write-Host "Run root: $RunRoot"
    Write-Host "Port: $Port"
    $configBackups = New-Directory (Join-Path $RunRoot "original-configs")
    $installedConfigs += Install-LocalConfig "server" $serverExe $serverConfig $configBackups
    $installedConfigs += Install-LocalConfig "agent" $agentExe $agentConfig $configBackups

    Write-Host "Starting server..."
    $serverProcess = Start-TransferProcess "server" $serverExe
    Wait-ForLogText $serverLog "Server running|Server listening|initialized successfully|started" 20 $serverProcess "server"

    Write-Host "Starting agent..."
    $agentProcess = Start-TransferProcess "agent" $agentExe
    Wait-ForLogText $agentLog "Agent started|Starting main processing loop|initialized successfully" 20 $agentProcess "agent"

    if ($PayloadBytes -le 0 -or $PayloadBytes -gt [int64]::MaxValue) {
        throw "PayloadBytes must be positive."
    }
    if ($PayloadBytes -gt [int64][int]::MaxValue) {
        throw "PayloadBytes is too large for this test harness."
    }

    $payloadPath = Resolve-SafePayloadPath $watchDir $PayloadRelativePath
    New-Item -ItemType Directory -Path (Split-Path -Parent $payloadPath) -Force | Out-Null
    $payloadBuffer = New-Object byte[] ([int]$PayloadBytes)
    $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
    try {
        $rng.GetBytes($payloadBuffer)
    }
    finally {
        $rng.Dispose()
    }
    Write-AllBytesWithRetry $payloadPath $payloadBuffer
    $expectedHash = (Get-FileHash -Algorithm SHA256 -Path $payloadPath).Hash

    Write-Host "Waiting for reconstruction..."
    $reconstructedPath = Wait-ForReconstructedFile $serverReconstructed ([System.IO.Path]::GetFileName($payloadPath)) $payloadBuffer.Length $expectedHash $TimeoutSeconds
    $actualHash = (Get-FileHash -Algorithm SHA256 -Path $reconstructedPath).Hash
    if ($actualHash -ne $expectedHash) {
        throw "Hash mismatch. Expected=$expectedHash Actual=$actualHash File=$reconstructedPath"
    }

    $expectedReconstructedPath = Join-Path (Join-Path $serverReconstructed ("id_" + $clientId)) $PayloadRelativePath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
    if ((Resolve-Path $reconstructedPath).Path -ne [System.IO.Path]::GetFullPath($expectedReconstructedPath)) {
        throw "Reconstructed file did not preserve the expected relative path. Expected=$expectedReconstructedPath Actual=$reconstructedPath"
    }

    $replacementHash = ""
    $replacementBytes = 0
    $finalOutputHash = $actualHash
    $finalOutputBytes = $payloadBuffer.Length
    if ($VerifyChangedContentResend) {
        $replacementBytes = $PayloadBytes + 4096
        if ($replacementBytes -gt [int64][int]::MaxValue) {
            $replacementBytes = $PayloadBytes
        }

        $replacementBuffer = New-Object byte[] ([int]$replacementBytes)
        $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
        try {
            $rng.GetBytes($replacementBuffer)
        }
        finally {
            $rng.Dispose()
        }

        Write-AllBytesWithRetry $payloadPath $replacementBuffer
        $replacementHash = (Get-FileHash -Algorithm SHA256 -Path $payloadPath).Hash

        Write-Host "Waiting for reconstruction after changed same-path content..."
        $replacementReconstructedPath = Wait-ForReconstructedFile $serverReconstructed ([System.IO.Path]::GetFileName($payloadPath)) $replacementBuffer.Length $replacementHash $TimeoutSeconds
        $replacementActualHash = (Get-FileHash -Algorithm SHA256 -Path $replacementReconstructedPath).Hash
        if ($replacementActualHash -ne $replacementHash) {
            throw "Replacement hash mismatch. Expected=$replacementHash Actual=$replacementActualHash File=$replacementReconstructedPath"
        }
        if ((Resolve-Path $replacementReconstructedPath).Path -ne [System.IO.Path]::GetFullPath($expectedReconstructedPath)) {
            throw "Replacement reconstructed file did not preserve the expected relative path. Expected=$expectedReconstructedPath Actual=$replacementReconstructedPath"
        }
        $reconstructedPath = $replacementReconstructedPath
        $finalOutputHash = $replacementActualHash
        $finalOutputBytes = $replacementBuffer.Length
        Write-Host "Changed same-path content resend verified."
    }

    $serverLogText = Get-Content -Path $serverLog -Raw -ErrorAction SilentlyContinue
    $agentLogText = Get-Content -Path $agentLog -Raw -ErrorAction SilentlyContinue
    if ($serverLogText -notmatch "signed_handshake|AUTH_SIGNED|Signed") {
        throw "Server log does not show signed handshake activity."
    }
    if ($agentLogText -notmatch "signed_handshake|AUTH_SIGNED|Signed") {
        throw "Agent log does not show signed handshake activity."
    }

    Write-Host "E2E signed handshake transfer succeeded."
    Write-Host "Input: $payloadPath"
    Write-Host "Output: $reconstructedPath"
    Write-Host "Bytes: $finalOutputBytes"
    Write-Host "SHA256: $finalOutputHash"
    $resultJsonPath = Join-Path $RunRoot "signed-handshake-result.json"
    [ordered]@{
        status = "PASS"
        architecture = $Architecture
        configuration = $Configuration
        runRoot = $RunRoot
        clientId = $clientId
        inputPath = $payloadPath
        outputPath = $reconstructedPath
        payloadRelativePath = $PayloadRelativePath
        payloadBytes = $payloadBuffer.Length
        sha256 = $actualHash
        finalOutputBytes = $finalOutputBytes
        finalOutputSha256 = $finalOutputHash
        changedContentResendVerified = [bool]$VerifyChangedContentResend
        replacementPayloadBytes = $replacementBytes
        replacementSha256 = $replacementHash
        serverExecutable = $serverExe
        agentExecutable = $agentExe
        createdAt = (Get-Date).ToString("o")
    } | ConvertTo-Json -Depth 5 | Set-Content -Path $resultJsonPath -Encoding UTF8
    Write-Host "Result JSON: $resultJsonPath"
    $succeeded = $true
}
finally {
    Stop-TransferProcess $agentProcess
    Stop-TransferProcess $serverProcess
    Restore-LocalConfigs $installedConfigs
    if ($KeepRunRoot -or -not $succeeded) {
        Write-Host "Artifacts kept in $RunRoot"
    }
    elseif (Test-Path $RunRoot) {
        Remove-Item -LiteralPath $RunRoot -Recurse -Force -ErrorAction SilentlyContinue
    }
}
