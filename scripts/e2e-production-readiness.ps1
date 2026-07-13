param(
    [string]$Configuration = "Debug",
    [ValidateSet("x64", "x86")]
    [string]$Architecture = "x64",
    [string]$RunRoot = "",
    [int]$DefaultTimeoutSeconds = 180,
    [string[]]$ScenarioName = @(),
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
        $udp = [System.Net.Sockets.UdpClient]::new([System.Net.Sockets.AddressFamily]::InterNetwork)
        try {
            $listener.Start()
            $udp.Client.Bind([System.Net.IPEndPoint]::new([System.Net.IPAddress]::Loopback, $candidate))
            return $candidate
        }
        catch {
            Start-Sleep -Milliseconds 20
        }
        finally {
            $listener.Stop()
            $udp.Dispose()
        }
    }
    throw "Could not find a local test port available for both TCP and UDP."
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

function New-RepeatedDelayPattern([int[]]$Pattern, [int]$Count) {
    if (-not $Pattern -or $Pattern.Count -eq 0 -or $Count -le 0) {
        return ""
    }

    $values = New-Object System.Collections.Generic.List[string]
    for ($i = 0; $i -lt $Count; $i++) {
        $values.Add([string]$Pattern[$i % $Pattern.Count])
    }
    return [string]::Join(",", $values)
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
    <TargetFramework>net8.0</TargetFramework>
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

function New-CertificateTestPki([string]$OutputDir, [string]$ServerIdentity, [string]$ClientIdentity, [string]$WorkDir) {
    $dotnet = Get-Command dotnet -ErrorAction SilentlyContinue
    if (-not $dotnet) {
        throw "dotnet is required to generate temporary X.509 certificates for this e2e test."
    }

    $projectDir = New-Directory (Join-Path $WorkDir "certgen")
    $csprojPath = Join-Path $projectDir "CertGen.csproj"
    $programPath = Join-Path $projectDir "Program.cs"

    @"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
</Project>
"@ | Set-Content -Path $csprojPath -Encoding UTF8

    @"
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;

static void WritePem(string path, string label, byte[] der)
{
    var b64 = Convert.ToBase64String(der);
    using var writer = new StreamWriter(path, false, Encoding.ASCII);
    writer.WriteLine($"-----BEGIN {label}-----");
    for (var i = 0; i < b64.Length; i += 64)
        writer.WriteLine(b64.Substring(i, Math.Min(64, b64.Length - i)));
    writer.WriteLine($"-----END {label}-----");
}

static X509Certificate2 CreateCa(string name, RSA key)
{
    var request = new CertificateRequest($"CN={name}", key,
        HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
    request.CertificateExtensions.Add(new X509BasicConstraintsExtension(true, false, 0, true));
    request.CertificateExtensions.Add(new X509KeyUsageExtension(
        X509KeyUsageFlags.KeyCertSign | X509KeyUsageFlags.CrlSign, true));
    request.CertificateExtensions.Add(new X509SubjectKeyIdentifierExtension(request.PublicKey, false));
    return request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5),
                                    DateTimeOffset.UtcNow.AddDays(2));
}

static X509Certificate2 CreateLeaf(string identity, RSA key, X509Certificate2 ca,
                                   bool server)
{
    var request = new CertificateRequest($"CN={identity}", key,
        HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
    request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, true));
    request.CertificateExtensions.Add(new X509KeyUsageExtension(
        X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, true));
    var eku = new OidCollection {
        new Oid(server ? "1.3.6.1.5.5.7.3.1" : "1.3.6.1.5.5.7.3.2")
    };
    request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(eku, false));
    var san = new SubjectAlternativeNameBuilder();
    san.AddDnsName(identity);
    request.CertificateExtensions.Add(san.Build());
    request.CertificateExtensions.Add(new X509SubjectKeyIdentifierExtension(request.PublicKey, false));
    var serial = RandomNumberGenerator.GetBytes(16);
    serial[0] &= 0x7f;
    var issued = request.Create(ca, DateTimeOffset.UtcNow.AddMinutes(-5),
                                DateTimeOffset.UtcNow.AddDays(1), serial);
    return issued.CopyWithPrivateKey(key);
}

if (args.Length != 3) return 2;
Directory.CreateDirectory(args[0]);
using var caKey = RSA.Create(3072);
using var ca = CreateCa("TransferUDT E2E CA", caKey);
using var wrongCaKey = RSA.Create(3072);
using var wrongCa = CreateCa("TransferUDT Wrong E2E CA", wrongCaKey);
using var serverKey = RSA.Create(3072);
using var server = CreateLeaf(args[1], serverKey, ca, true);
using var clientKey = RSA.Create(3072);
using var client = CreateLeaf(args[2], clientKey, ca, false);

var cleanCrlBuilder = new CertificateRevocationListBuilder();
var cleanCrl = cleanCrlBuilder.Build(
    ca, 1, DateTimeOffset.UtcNow.AddDays(1), HashAlgorithmName.SHA256,
    RSASignaturePadding.Pkcs1, DateTimeOffset.UtcNow.AddMinutes(-5));
var revokedClientCrlBuilder = new CertificateRevocationListBuilder();
revokedClientCrlBuilder.AddEntry(client, DateTimeOffset.UtcNow.AddMinutes(-1),
                                 X509RevocationReason.KeyCompromise);
var revokedClientCrl = revokedClientCrlBuilder.Build(
    ca, 2, DateTimeOffset.UtcNow.AddDays(1), HashAlgorithmName.SHA256,
    RSASignaturePadding.Pkcs1, DateTimeOffset.UtcNow.AddMinutes(-5));

WritePem(Path.Combine(args[0], "ca.pem"), "CERTIFICATE", ca.Export(X509ContentType.Cert));
WritePem(Path.Combine(args[0], "wrong-ca.pem"), "CERTIFICATE", wrongCa.Export(X509ContentType.Cert));
WritePem(Path.Combine(args[0], "clean.crl.pem"), "X509 CRL", cleanCrl);
WritePem(Path.Combine(args[0], "revoked-agent.crl.pem"), "X509 CRL", revokedClientCrl);
WritePem(Path.Combine(args[0], "server.key"), "PRIVATE KEY", serverKey.ExportPkcs8PrivateKey());
WritePem(Path.Combine(args[0], "server.pem"), "CERTIFICATE", server.Export(X509ContentType.Cert));
WritePem(Path.Combine(args[0], "agent.key"), "PRIVATE KEY", clientKey.ExportPkcs8PrivateKey());
WritePem(Path.Combine(args[0], "agent.pem"), "CERTIFICATE", client.Export(X509ContentType.Cert));
return 0;
"@ | Set-Content -Path $programPath -Encoding UTF8

    $previousDotnetCliHome = $env:DOTNET_CLI_HOME
    $previousDotnetNoLogo = $env:DOTNET_NOLOGO
    $env:DOTNET_CLI_HOME = New-Directory (Join-Path $WorkDir ".dotnet-certgen")
    $env:DOTNET_NOLOGO = "1"
    try {
        & $dotnet.Source run --project $csprojPath -- $OutputDir $ServerIdentity $ClientIdentity | Out-Null
        if ($LASTEXITCODE -ne 0) {
            throw "Failed to generate temporary X.509 test PKI with dotnet."
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

function Convert-ToExtendedPath([string]$Path) {
    $runningOnWindows = [System.IO.Path]::DirectorySeparatorChar -eq '\'
    if ([string]::IsNullOrWhiteSpace($Path) -or -not $runningOnWindows) {
        return $Path
    }
    if ($Path.StartsWith("\\?\")) {
        return $Path
    }
    if ($Path.StartsWith("\\")) {
        return "\\?\UNC\" + $Path.Substring(2)
    }
    return "\\?\" + $Path
}

function Convert-ToComparablePath([string]$Path) {
    if ([string]::IsNullOrWhiteSpace($Path)) {
        return ""
    }
    $fullPath = [System.IO.Path]::GetFullPath($Path)
    if ($fullPath.StartsWith("\\?\UNC\")) {
        $fullPath = "\\" + $fullPath.Substring(8)
    }
    elseif ($fullPath.StartsWith("\\?\")) {
        $fullPath = $fullPath.Substring(4)
    }
    return $fullPath.TrimEnd('\').ToLowerInvariant()
}

function Get-SafeSHA256([string]$Path) {
    try {
        return (Get-FileHash -Algorithm SHA256 -LiteralPath (Convert-ToExtendedPath $Path) -ErrorAction Stop).Hash
    }
    catch {
        return $null
    }
}

function Wait-ForReconstructedFile([string]$Root, [string]$FileName, [int64]$ExpectedLength, [string]$ExpectedHash, [int]$TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastHash = ""
    while ((Get-Date) -lt $deadline) {
        $candidate = Get-ChildItem -Path $Root -Recurse -File -Filter $FileName -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($candidate -and $candidate.Length -eq $ExpectedLength) {
            $lastHash = Get-SafeSHA256 $candidate.FullName
            if ($lastHash -eq $ExpectedHash) {
                return $candidate.FullName
            }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($lastHash) {
        throw "Timed out waiting for '$FileName' to match SHA256. LastHash=$lastHash ExpectedHash=$ExpectedHash"
    }
    throw "Timed out waiting for reconstructed file '$FileName' under $Root"
}

function Wait-ForReconstructedHashUnderRoot([string]$Root, [int64]$ExpectedLength, [string]$ExpectedHash, [int]$TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastHash = ""
    while ((Get-Date) -lt $deadline) {
        $candidates = Get-ChildItem -Path $Root -Recurse -File -ErrorAction SilentlyContinue | Where-Object { $_.Length -eq $ExpectedLength }
        foreach ($candidate in @($candidates)) {
            $lastHash = Get-SafeSHA256 $candidate.FullName
            if ($lastHash -eq $ExpectedHash) {
                return $candidate.FullName
            }
        }
        Start-Sleep -Milliseconds 500
    }
    if ($lastHash) {
        throw "Timed out waiting for reconstructed content to match SHA256. LastHash=$lastHash ExpectedHash=$ExpectedHash"
    }
    throw "Timed out waiting for reconstructed content under $Root"
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

function Test-ReconstructedHashExists([string]$Root, [string]$FileName, [int64]$ExpectedLength, [string]$ExpectedHash) {
    $candidate = Get-ChildItem -Path $Root -Recurse -File -Filter $FileName -ErrorAction SilentlyContinue | Select-Object -First 1
    if (-not $candidate -or $candidate.Length -ne $ExpectedLength) {
        return $false
    }
    return ((Get-SafeSHA256 $candidate.FullName) -eq $ExpectedHash)
}

function New-PayloadFile([string]$Path, [int64]$SizeBytes, [int]$TimeoutSeconds = 30) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    $lastError = $null

    while ($true) {
        $rng = $null
        $stream = $null
        try {
            $rng = [System.Security.Cryptography.RandomNumberGenerator]::Create()
            $buffer = New-Object byte[] (1024 * 1024)
            $stream = [System.IO.File]::Open($Path, [System.IO.FileMode]::Create, [System.IO.FileAccess]::Write, [System.IO.FileShare]::None)

            $remaining = $SizeBytes
            while ($remaining -gt 0) {
                $count = [Math]::Min($buffer.Length, [int]$remaining)
                if ($count -ne $buffer.Length) {
                    $chunk = New-Object byte[] $count
                    $rng.GetBytes($chunk)
                    $stream.Write($chunk, 0, $count)
                }
                else {
                    $rng.GetBytes($buffer)
                    $stream.Write($buffer, 0, $buffer.Length)
                }
                $remaining -= $count
            }

            return
        }
        catch [System.IO.IOException] {
            $lastError = $_.Exception
        }
        catch [System.UnauthorizedAccessException] {
            $lastError = $_.Exception
        }
        finally {
            if ($stream) { $stream.Dispose() }
            if ($rng) { $rng.Dispose() }
        }

        if ((Get-Date) -ge $deadline) {
            throw "Timed out waiting to write payload '$Path': $($lastError.Message)"
        }
        Start-Sleep -Milliseconds 250
    }
}

function Get-Matches([string]$Text, [string]$Pattern) {
    return [regex]::Matches($Text, $Pattern)
}

function New-ScenarioReport($Result) {
    $status = if ($Result.Passed) { "PASS" } else { "FAIL" }
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("# E2E Scenario Report - $($Result.Name)")
    $lines.Add("")
    $lines.Add("- Status: $status")
    $lines.Add("- Description: $($Result.Description)")
    $lines.Add("- Expected outcome: $($Result.ExpectedOutcome)")
    $lines.Add("- Duration seconds: $([Math]::Round($Result.DurationSeconds, 2))")
    $lines.Add("- Payload bytes: $($Result.PayloadBytes)")
    $lines.Add("- File count: $($Result.FileCount)")
    $lines.Add("- Payload relative path: $($Result.PayloadRelativePath)")
    $lines.Add("- Payload SHA256: $($Result.ExpectedHash)")
    $lines.Add("- Output SHA256: $($Result.ActualHash)")
    $lines.Add("- Changed-content resend: $($Result.ChangedContentResend)")
    $lines.Add("- Changed-content server policy: $($Result.ChangedFilesServerPolicy)")
    $lines.Add("- Replacement payload bytes: $($Result.ReplacementPayloadBytes)")
    $lines.Add("- Replacement payload SHA256: $($Result.ReplacementExpectedHash)")
    $lines.Add("- Replacement output SHA256: $($Result.ReplacementActualHash)")
    $lines.Add("- Server replacement evidence: $($Result.ServerReplacementEvidence)")
    $lines.Add("- Server versioning evidence: $($Result.ServerVersioningEvidence)")
    $lines.Add("- Adaptive chunks: $($Result.AdaptiveEnabled)")
    $lines.Add("- ACK delay pattern ms: $($Result.AckDelayPattern)")
    $lines.Add("- Server certificate handshake evidence: $($Result.ServerCertificateHandshake)")
    $lines.Add("- Agent certificate handshake evidence: $($Result.AgentCertificateHandshake)")
    $lines.Add("- Server all-chunks-complete evidence: $($Result.ServerCompleted)")
    $lines.Add("- Agent file-complete evidence: $($Result.AgentCompleted)")
    $lines.Add("- Agent restart executed: $($Result.AgentRestarted)")
    $lines.Add("- Idempotent recovery evidence: $($Result.RecoveryEvidence)")
    $lines.Add("- Stored chunk lines: $($Result.StoredChunkLines)")
    $lines.Add("- Adaptive telemetry lines: $($Result.AdaptiveTelemetryLines)")
    $lines.Add("- Adaptive recommendation min bytes: $($Result.AdaptiveRecommendationMin)")
    $lines.Add("- Adaptive recommendation max bytes: $($Result.AdaptiveRecommendationMax)")
    $lines.Add("- Adaptive unique recommendation count: $($Result.AdaptiveRecommendationUniqueCount)")
    $lines.Add("- ACK min ms: $($Result.AckMinMs)")
    $lines.Add("- ACK max ms: $($Result.AckMaxMs)")
    $lines.Add("- Negative evidence: $($Result.NegativeEvidence)")
    if (-not $Result.Passed) {
        $lines.Add("- Error: $($Result.Error)")
    }
    $lines.Add("")
    $lines.Add("## Artifacts")
    $lines.Add("")
    $lines.Add("- Run directory: $($Result.RunRoot)")
    $lines.Add("- Server log: $($Result.ServerLog)")
    $lines.Add("- Agent log: $($Result.AgentLog)")
    $lines.Add("- Input file: $($Result.InputPath)")
    $lines.Add("- Output file: $($Result.OutputPath)")
    $lines.Add("")
    $lines.Add("## Production Readiness Notes")
    $lines.Add("")
    if ($Result.Passed -and $Result.ExpectedOutcome -eq "success") {
        $lines.Add("- Integrity: passed by end-to-end SHA-256 comparison.")
        $lines.Add("- Authentication: certificate handshake was observed on both server and agent logs.")
        if ($Result.AdaptiveEnabled) {
            if ($Result.AdaptiveRecommendationUniqueCount -gt 1) {
                $lines.Add("- Adaptive behavior: chunk recommendation changed during transfer.")
            }
            else {
                $lines.Add("- Adaptive behavior: telemetry was present, but recommendation did not vary materially in this scenario.")
            }
        }
        if ($Result.ChangedContentResend) {
            if ($Result.ChangedFilesServerPolicy -eq "versioned") {
                $lines.Add("- Changed-content resend: same watched path was rewritten and the Server preserved the original output while creating a versioned output.")
            }
            else {
                $lines.Add("- Changed-content resend: same watched path was rewritten and the managed server output was replaced.")
            }
        }
    }
    elseif ($Result.Passed) {
        $lines.Add("- Expected rejection was observed and no valid reconstructed output was accepted.")
    }
    else {
        $lines.Add("- This scenario is not production-ready until the failure above is corrected.")
    }
    return [string]::Join([Environment]::NewLine, $lines)
}

function Resolve-Executable([string]$RepoRoot, [string]$Configuration, [string]$Architecture, [string]$Kind) {
    $resolvedKind = if ($Kind -eq "server") { "Server" } else { "Agent" }
    return Resolve-TransferUDTExecutable -RepoRoot $RepoRoot -Configuration $Configuration -Architecture $Architecture -Kind $resolvedKind -Required
}

function Invoke-E2EScenario($Scenario, [string]$SuiteRoot, [string]$RepoRoot, [string]$Configuration, [string]$Architecture, [int]$DefaultTimeoutSeconds) {
    $scenarioStart = Get-Date
    $scenarioRoot = New-Directory (Join-Path $SuiteRoot $Scenario.Name)
    $port = New-FreeTcpPort
    $clientId = "agent-$($Scenario.Name)"
    $safeClientId = $clientId -replace "[^A-Za-z0-9_.@-]", "-"
    $keysDir = New-Directory (Join-Path $scenarioRoot "keys")
    $agentRoot = New-Directory (Join-Path $scenarioRoot "agent")
    $serverRoot = New-Directory (Join-Path $scenarioRoot "server")
    $watchDir = New-Directory (Join-Path $agentRoot "watch")
    $serverStorage = New-Directory (Join-Path $serverRoot "storage")
    $serverReconstructed = New-Directory (Join-Path $serverRoot "reconstructed")
    $agentLogs = New-Directory (Join-Path $agentRoot "logs")
    $serverLogs = New-Directory (Join-Path $serverRoot "logs")
    $agentDb = New-Directory (Join-Path $agentRoot "db")
    $serverDb = New-Directory (Join-Path $serverRoot "db")
    $configBackups = New-Directory (Join-Path $scenarioRoot "original-configs")

    $serverIdentity = "transfer-server.test"
    New-CertificateTestPki $keysDir $serverIdentity $safeClientId $scenarioRoot
    $caBundle = Join-Path $keysDir "ca.pem"
    $wrongCaBundle = Join-Path $keysDir "wrong-ca.pem"
    $serverPrivateKey = Join-Path $keysDir "server.key"
    $serverCertificate = Join-Path $keysDir "server.pem"
    $agentPrivateKey = Join-Path $keysDir "agent.key"
    $agentCertificate = Join-Path $keysDir "agent.pem"
    $cleanCrl = Join-Path $keysDir "clean.crl.pem"
    $revokedAgentCrl = Join-Path $keysDir "revoked-agent.crl.pem"

    $serverConfig = Join-Path $scenarioRoot "server.properties"
    $agentConfig = Join-Path $scenarioRoot "agent.properties"
    $serverLog = Join-Path $serverLogs "server.log"
    $agentLog = Join-Path $agentLogs "agent.log"
    $ackDelayPattern = if ($Scenario.AckPattern) { New-RepeatedDelayPattern $Scenario.AckPattern 160 } else { "" }
    $timeoutSeconds = if ($Scenario.TimeoutSeconds) { [int]$Scenario.TimeoutSeconds } else { $DefaultTimeoutSeconds }
    $adaptiveEnabledText = if ($Scenario.AdaptiveEnabled) { "true" } else { "false" }
    $chunkKb = if ($Scenario.ChunkKb) { [int]$Scenario.ChunkKb } else { 64 }
    $adaptiveMinKb = if ($Scenario.AdaptiveMinKb) { [int]$Scenario.AdaptiveMinKb } else { 64 }
    $adaptiveMaxKb = if ($Scenario.AdaptiveMaxKb) { [int]$Scenario.AdaptiveMaxKb } else { 1024 }
    $adaptiveInitialKb = if ($Scenario.AdaptiveInitialKb) { [int]$Scenario.AdaptiveInitialKb } else { $adaptiveMinKb }
    $adaptiveTargetAckMs = if ($Scenario.AdaptiveTargetAckMs) { [int]$Scenario.AdaptiveTargetAckMs } else { 200 }
    $expectedOutcome = if ($Scenario.ExpectedOutcome) { [string]$Scenario.ExpectedOutcome } else { "success" }
    $fileCount = if ($Scenario.FileCount) { [int]$Scenario.FileCount } else { 1 }
    $changedContentResend = [bool]$Scenario.VerifyChangedContentResend
    $restartAgentAfterFirstChunk = [bool]$Scenario.RestartAgentAfterFirstChunk
    $changedFilesServerPolicy = if ($Scenario.ChangedFilesServerPolicy) { [string]$Scenario.ChangedFilesServerPolicy } else { "overwrite" }
    $changedFilesAgentEnabled = "true"
    if ($Scenario.PSObject.Properties.Name -contains "ChangedFilesAgentEnabled") {
        $changedFilesAgentEnabled = if ([bool]$Scenario.ChangedFilesAgentEnabled) { "true" } else { "false" }
    }
    if ($changedContentResend -and ($expectedOutcome -ne "success" -or $fileCount -ne 1)) {
        throw "VerifyChangedContentResend requires one success scenario file."
    }
    if ($restartAgentAfterFirstChunk -and ($expectedOutcome -ne "success" -or $fileCount -ne 1)) {
        throw "RestartAgentAfterFirstChunk requires one success scenario file."
    }
    $serverMaxFileSizeMb = if ($Scenario.ServerMaxFileSizeMb) { [int]$Scenario.ServerMaxFileSizeMb } else { 512 }
    $serverAllowedClientId = if ($Scenario.ServerAllowedClientId) { [string]$Scenario.ServerAllowedClientId } else { $safeClientId }
    $agentCaBundlePath = if ($Scenario.UseWrongCaBundle) { $wrongCaBundle } else { $caBundle }
    $serverCrlPath = if ($Scenario.UseRevokedClientCrl) { $revokedAgentCrl } else { $cleanCrl }

    @"
server.port = $port
server.bind_address = 127.0.0.1
server.allowed_clients = 127.0.0.1
server.storage_path = $(Convert-ToConfigPath $serverStorage)
server.reconstructed_path = $(Convert-ToConfigPath $serverReconstructed)
server.max_file_size_mb = $serverMaxFileSizeMb
server.max_client_storage_mb = 1024
max.connection = 32
work.threads = 4
work.max_pending_tasks = 256
circuitbreaker.failure.threshold = 10
circuitbreaker.reset.timeout.seconds = 10
log.filepath = $(Convert-ToConfigPath $serverLog)
db.filepath = $(Join-Path $serverDb "server.db")
log.max_size_mb = 25
log.backup_count = 2
log.flush_level = always
max.band = 50
seg.len = 1350
snd.buf = 64
rcv.buf = 64
snd.timeout = 30
rcv.timeout = 30
ling.on = 0
ling.time = 5
test.ack_delay_pattern_ms = $ackDelayPattern
resend.changed_files.server_policy = $changedFilesServerPolicy
security.enabled = true
security.handshake.enabled = true
security.allow_insecure = false
security.identity.mode = certificate_handshake
security.allowed_client_ids = $serverAllowedClientId
security.server_private_key_path = $(Convert-ToConfigPath $serverPrivateKey)
security.server_certificate_path = $(Convert-ToConfigPath $serverCertificate)
security.ca_bundle_path = $(Convert-ToConfigPath $caBundle)
security.revocation.mode = crl
security.crl_path = $(Convert-ToConfigPath $serverCrlPath)
security.certificate_expiry_warning_days = 30
"@ | Set-Content -Path $serverConfig -Encoding ASCII

    @"
server.targets = 127.0.0.1:$port
chunk.size = $chunkKb
chunk.adaptive.enabled = $adaptiveEnabledText
chunk.adaptive.min_kb = $adaptiveMinKb
chunk.adaptive.max_kb = $adaptiveMaxKb
chunk.adaptive.initial_kb = $adaptiveInitialKb
chunk.adaptive.target_ack_ms = $adaptiveTargetAckMs
data.dirs = $(Convert-ToConfigPath $watchDir)
resend.changed_files.enabled = $changedFilesAgentEnabled
resend.changed_files.identity = sha256
max.band = 50
seg.len = 1350
snd.buf = 64
rcv.buf = 64
snd.timeout = 30
rcv.timeout = 30
max.retries = 4
max.retries.abandon = 12
retry.jitter.max.ms = 25
work.threads = 4
stability.check.interval.seconds = 1
stability.check.count = 1
watcher.check.interval.seconds = 1
pending.check.interval.seconds = 2
circuitbreaker.failure.threshold = 10
circuitbreaker.reset.timeout.seconds = 10
memory.usage.percent.limit = 95
log.filepath = $(Convert-ToConfigPath $agentLog)
log.max_size_mb = 25
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
security.identity.mode = certificate_handshake
security.client_id = $safeClientId
security.client_private_key_path = $(Convert-ToConfigPath $agentPrivateKey)
security.client_certificate_path = $(Convert-ToConfigPath $agentCertificate)
security.ca_bundle_path = $(Convert-ToConfigPath $agentCaBundlePath)
security.revocation.mode = crl
security.crl_path = $(Convert-ToConfigPath $cleanCrl)
security.certificate_expiry_warning_days = 30
security.server_identity = $serverIdentity
"@ | Set-Content -Path $agentConfig -Encoding ASCII

    $serverExe = Resolve-Executable $RepoRoot $Configuration $Architecture "server"
    $agentExe = Resolve-Executable $RepoRoot $Configuration $Architecture "agent"
    $serverProcess = $null
    $agentProcess = $null
    $installedConfigs = @()
    $result = [ordered]@{
        Name = $Scenario.Name
        Description = $Scenario.Description
        RunRoot = $scenarioRoot
        ServerLog = $serverLog
        AgentLog = $agentLog
        InputPath = ""
        OutputPath = ""
        PayloadBytes = ([int64]$Scenario.PayloadBytes) * $fileCount
        PayloadRelativePath = ""
        FileCount = $fileCount
        ExpectedOutcome = $expectedOutcome
        ExpectedHash = ""
        ActualHash = ""
        ChangedContentResend = $changedContentResend
        ChangedFilesServerPolicy = $changedFilesServerPolicy
        ReplacementPayloadBytes = 0
        ReplacementExpectedHash = ""
        ReplacementActualHash = ""
        ServerReplacementEvidence = $false
        ServerVersioningEvidence = $false
        AdaptiveEnabled = [bool]$Scenario.AdaptiveEnabled
        AckDelayPattern = if ($ackDelayPattern) { ($Scenario.AckPattern -join ",") + " repeated" } else { "none" }
        ServerCertificateHandshake = $false
        AgentCertificateHandshake = $false
        ServerCompleted = $false
        AgentCompleted = $false
        AgentRestarted = $false
        RecoveryEvidence = $false
        StoredChunkLines = 0
        AdaptiveTelemetryLines = 0
        AdaptiveRecommendationMin = 0
        AdaptiveRecommendationMax = 0
        AdaptiveRecommendationUniqueCount = 0
        AckMinMs = 0
        AckMaxMs = 0
        NegativeEvidence = ""
        DurationSeconds = 0
        Passed = $false
        Error = ""
        ReportPath = ""
    }

    try {
        $installedConfigs += Install-LocalConfig "server" $serverExe $serverConfig $configBackups
        $installedConfigs += Install-LocalConfig "agent" $agentExe $agentConfig $configBackups

        $serverProcess = Start-TransferProcess "server" $serverExe
        Wait-ForLogText $serverLog "Server running|Server listening|initialized successfully|started" 20 $serverProcess "server"
        $agentProcess = Start-TransferProcess "agent" $agentExe
        Wait-ForLogText $agentLog "Agent started|Starting main processing loop|initialized successfully" 20 $agentProcess "agent"

        $inputPaths = New-Object System.Collections.Generic.List[string]
        $outputPaths = New-Object System.Collections.Generic.List[string]
        $expectedHashes = New-Object System.Collections.Generic.List[string]
        $actualHashes = New-Object System.Collections.Generic.List[string]

        for ($fileIndex = 1; $fileIndex -le $fileCount; $fileIndex++) {
            $payloadName = if ($fileCount -eq 1) { "$($Scenario.Name).bin" } else { "$($Scenario.Name)-$fileIndex.bin" }
            $payloadRelativePath = if ($Scenario.PayloadRelativePath -and $fileCount -eq 1) { [string]$Scenario.PayloadRelativePath } else { $payloadName }
            $payloadPath = Resolve-SafePayloadPath $watchDir $payloadRelativePath
            New-Item -ItemType Directory -Path (Split-Path -Parent $payloadPath) -Force | Out-Null
            New-PayloadFile $payloadPath ([int64]$Scenario.PayloadBytes)
            $payloadName = [System.IO.Path]::GetFileName($payloadPath)
            $expectedHash = (Get-FileHash -Algorithm SHA256 -Path $payloadPath).Hash
            $inputPaths.Add($payloadPath)
            $expectedHashes.Add($expectedHash)
            $result.PayloadRelativePath = $payloadRelativePath

            if ($restartAgentAfterFirstChunk -and $fileIndex -eq 1) {
                Wait-ForLogText $serverLog "Stored chunk|Successfully saved chunk 0" 30 $serverProcess "server"
                Stop-TransferProcess $agentProcess
                $agentProcess = $null
                Start-Sleep -Milliseconds 500
                $agentProcess = Start-TransferProcess "agent-restarted" $agentExe
                Start-Sleep -Milliseconds 1000
                if ($agentProcess.HasExited) {
                    throw "Restarted Agent exited unexpectedly. ExitCode=$($agentProcess.ExitCode)"
                }
                $result.AgentRestarted = $true
            }

            if ($expectedOutcome -eq "success") {
                $outputPath = Wait-ForReconstructedFile $serverReconstructed $payloadName ([int64]$Scenario.PayloadBytes) $expectedHash $timeoutSeconds
                $actualHash = Get-SafeSHA256 $outputPath
                $outputPaths.Add($outputPath)
                $actualHashes.Add($actualHash)

                if ($Scenario.PayloadRelativePath -and $fileCount -eq 1) {
                    $expectedReconstructedPath = Join-Path (Join-Path $serverReconstructed ("id_" + $safeClientId)) $payloadRelativePath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
                    if ((Convert-ToComparablePath $outputPath) -ne (Convert-ToComparablePath $expectedReconstructedPath)) {
                        throw "Reconstructed file did not preserve the expected relative path. Expected=$expectedReconstructedPath Actual=$outputPath"
                    }
                }

                if ($changedContentResend) {
                    $replacementBytes = if ($Scenario.ReplacementPayloadBytes) { [int64]$Scenario.ReplacementPayloadBytes } else { ([int64]$Scenario.PayloadBytes) + 4096 }
                    New-PayloadFile $payloadPath $replacementBytes
                    $replacementHash = (Get-FileHash -Algorithm SHA256 -Path $payloadPath).Hash
                    if ($changedFilesServerPolicy -eq "versioned") {
                        $replacementOutputPath = Wait-ForReconstructedHashUnderRoot $serverReconstructed $replacementBytes $replacementHash $timeoutSeconds
                    }
                    else {
                        $replacementOutputPath = Wait-ForReconstructedFile $serverReconstructed $payloadName $replacementBytes $replacementHash $timeoutSeconds
                    }
                    $replacementActualHash = Get-SafeSHA256 $replacementOutputPath
                    if ($replacementActualHash -ne $replacementHash) {
                        throw "Replacement hash mismatch. Expected=$replacementHash Actual=$replacementActualHash File=$replacementOutputPath"
                    }
                    if ($changedFilesServerPolicy -eq "versioned") {
                        if ((Convert-ToComparablePath $replacementOutputPath) -eq (Convert-ToComparablePath $outputPath)) {
                            throw "Versioned changed-content resend overwrote the original output."
                        }
                        $originalHashAfterVersion = Get-SafeSHA256 $outputPath
                        if ($originalHashAfterVersion -ne $expectedHash) {
                            throw "Versioned changed-content resend did not preserve the original output. ExpectedOriginal=$expectedHash ActualOriginal=$originalHashAfterVersion"
                        }
                    }
                    elseif ($Scenario.PayloadRelativePath -and $fileCount -eq 1) {
                        $expectedReconstructedPath = Join-Path (Join-Path $serverReconstructed ("id_" + $safeClientId)) $payloadRelativePath.Replace('/', [System.IO.Path]::DirectorySeparatorChar)
                        if ((Convert-ToComparablePath $replacementOutputPath) -ne (Convert-ToComparablePath $expectedReconstructedPath)) {
                            throw "Replacement reconstructed file did not preserve the expected relative path. Expected=$expectedReconstructedPath Actual=$replacementOutputPath"
                        }
                    }

                    $expectedHashes.Add($replacementHash)
                    $actualHashes.Add($replacementActualHash)
                    $outputPaths.Add($replacementOutputPath)
                    $result.PayloadBytes += $replacementBytes
                    $result.ReplacementPayloadBytes = $replacementBytes
                    $result.ReplacementExpectedHash = $replacementHash
                    $result.ReplacementActualHash = $replacementActualHash
                }
            }
        }

        $result.InputPath = [string]::Join("; ", $inputPaths)
        $result.ExpectedHash = [string]::Join("; ", $expectedHashes)

        if ($expectedOutcome -eq "success") {
            $result.OutputPath = [string]::Join("; ", $outputPaths)
            $result.ActualHash = [string]::Join("; ", $actualHashes)
            Wait-ForLogText $serverLog "All chunks received for file" 20 $serverProcess "server"
            Wait-ForLogText $agentLog "File processing completed" 20 $agentProcess "agent"
        }
        elseif ($expectedOutcome -eq "auth-reject") {
            Wait-ForLogText $serverLog "Certificate authentication rejected|Certificate authentication failed|Authentication handshake failed" 30 $serverProcess "server"
            $result.NegativeEvidence = "Server rejected certificate authentication."
        }
        elseif ($expectedOutcome -eq "server-proof-reject") {
            Wait-ForLogText $agentLog "Server certificate chain or identity is invalid|Server certificate proof verification failed|Certificate authentication failed" 30 $agentProcess "agent"
            $result.NegativeEvidence = "Agent rejected a Server certificate issued by an unexpected CA."
        }
        elseif ($expectedOutcome -eq "too-large-reject") {
            Wait-ForLogText $serverLog "server.max_file_size_mb|Rejecting file|exceeds configured" 30 $serverProcess "server"
            $result.NegativeEvidence = "Server rejected file larger than configured maximum."
        }

        $serverText = Get-Content -Path $serverLog -Raw -ErrorAction SilentlyContinue
        $agentText = Get-Content -Path $agentLog -Raw -ErrorAction SilentlyContinue
        $result.ServerCertificateHandshake = $serverText -match "Certificate authentication succeeded"
        $result.AgentCertificateHandshake = $agentText -match "Certificate authentication succeeded"
        $result.ServerCompleted = $serverText -match "All chunks received for file"
        $result.AgentCompleted = $agentText -match "File processing completed"
        $result.RecoveryEvidence = $serverText -match "already stored successfully.*duplicate without updating counters"
        $result.ServerReplacementEvidence = $serverText -match "Replacing managed output"
        $result.ServerVersioningEvidence = $serverText -match "Versioning managed output|Versioning incoming output"
        $result.StoredChunkLines = (Get-Matches $serverText "Stored chunk|Stored adaptive").Count
        $result.AdaptiveTelemetryLines = (Get-Matches $agentText "Adaptive chunk telemetry").Count

        $recommendations = New-Object System.Collections.Generic.List[int64]
        foreach ($match in (Get-Matches $agentText "recommended_next_chunk_bytes=(\d+)")) {
            $recommendations.Add([int64]$match.Groups[1].Value)
        }
        if ($recommendations.Count -gt 0) {
            $result.AdaptiveRecommendationMin = ($recommendations | Measure-Object -Minimum).Minimum
            $result.AdaptiveRecommendationMax = ($recommendations | Measure-Object -Maximum).Maximum
            $result.AdaptiveRecommendationUniqueCount = @($recommendations | Sort-Object -Unique).Count
        }

        $ackValues = New-Object System.Collections.Generic.List[int64]
        foreach ($match in (Get-Matches $agentText "ack_ms=(\d+)")) {
            $ackValues.Add([int64]$match.Groups[1].Value)
        }
        if ($ackValues.Count -gt 0) {
            $result.AckMinMs = ($ackValues | Measure-Object -Minimum).Minimum
            $result.AckMaxMs = ($ackValues | Measure-Object -Maximum).Maximum
        }

        $adaptiveOk = $true
        if ($Scenario.AdaptiveEnabled -and $Scenario.RequireAdaptiveVariation) {
            $adaptiveOk = $result.AdaptiveTelemetryLines -gt 0 -and $result.AdaptiveRecommendationUniqueCount -gt 1
        }
        $changedContentOk = $true
        if ($changedContentResend) {
            $serverPolicyEvidence = if ($changedFilesServerPolicy -eq "versioned") {
                $result.ServerVersioningEvidence
            }
            else {
                $result.ServerReplacementEvidence
            }
            $changedContentOk =
                $result.ReplacementPayloadBytes -gt 0 -and
                $result.ReplacementExpectedHash -eq $result.ReplacementActualHash -and
                $serverPolicyEvidence
        }
        $recoveryOk = (-not $restartAgentAfterFirstChunk) -or
            ($result.AgentRestarted -and $result.RecoveryEvidence)

        if ($expectedOutcome -eq "success") {
            $hashOk = $actualHashes.Count -eq $expectedHashes.Count
            if ($hashOk) {
                for ($i = 0; $i -lt $expectedHashes.Count; $i++) {
                    if ($actualHashes[$i] -ne $expectedHashes[$i]) {
                        $hashOk = $false
                        break
                    }
                }
            }

            $result.Passed =
                $hashOk -and
                $result.ServerCertificateHandshake -and
                $result.AgentCertificateHandshake -and
                $result.ServerCompleted -and
                $result.AgentCompleted -and
                $adaptiveOk -and
                $changedContentOk -and
                $recoveryOk
            if (-not $result.Passed -and $Scenario.AdaptiveEnabled -and $Scenario.RequireAdaptiveVariation -and -not $adaptiveOk) {
                $result.Error = "Adaptive telemetry did not show required chunk recommendation variation."
            }
            elseif (-not $result.Passed -and $changedContentResend -and -not $changedContentOk) {
                $result.Error = "Changed-content resend did not replace the managed output with server evidence."
            }
            elseif (-not $result.Passed -and $restartAgentAfterFirstChunk -and -not $recoveryOk) {
                $result.Error = "Agent restart completed without idempotent duplicate-chunk recovery evidence."
            }
            elseif (-not $result.Passed -and -not $hashOk) {
                $result.Error = "One or more reconstructed files did not match the expected SHA-256."
            }
        }
        else {
            $unexpectedOutput = $false
            for ($i = 0; $i -lt $inputPaths.Count; $i++) {
                $fileName = Split-Path -Leaf $inputPaths[$i]
                if (Test-ReconstructedHashExists $serverReconstructed $fileName ([int64]$Scenario.PayloadBytes) $expectedHashes[$i]) {
                    $unexpectedOutput = $true
                    break
                }
            }

            $evidenceOk = -not [string]::IsNullOrWhiteSpace($result.NegativeEvidence)
            $authContextOk = $true
            if ($expectedOutcome -eq "too-large-reject") {
                $authContextOk = $result.ServerCertificateHandshake -and $result.AgentCertificateHandshake
            }

            $result.Passed = $evidenceOk -and (-not $unexpectedOutput) -and $authContextOk
            if (-not $result.Passed) {
                if ($unexpectedOutput) {
                    $result.Error = "A negative scenario produced a valid reconstructed output."
                }
                elseif (-not $evidenceOk) {
                    $result.Error = "Expected rejection evidence was not observed."
                }
                elseif (-not $authContextOk) {
                    $result.Error = "Too-large rejection did not occur after a certificate handshake."
                }
            }
        }
    }
    catch {
        $result.Error = $_.Exception.Message
        $result.Passed = $false
    }
    finally {
        $result.DurationSeconds = ((Get-Date) - $scenarioStart).TotalSeconds
        Stop-TransferProcess $agentProcess
        Stop-TransferProcess $serverProcess
        Restore-LocalConfigs $installedConfigs
        $scenarioReport = New-ScenarioReport ([pscustomobject]$result)
        $scenarioReportPath = Join-Path $scenarioRoot "report.md"
        $scenarioReport | Set-Content -Path $scenarioReportPath -Encoding UTF8
        $result["ReportPath"] = $scenarioReportPath
    }

    return [pscustomobject]$result
}

$repoRoot = Get-RepoRoot
Import-Module (Join-Path $repoRoot "scripts\TransferUDT.Build.psm1") -Force
if ([string]::IsNullOrWhiteSpace($RunRoot)) {
    $RunRoot = Join-Path $repoRoot ("e2e-runs\production-readiness-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$RunRoot = New-Directory $RunRoot

$scenarios = @(
    [pscustomobject]@{
        Name = "tiny-good-fixed"
        Description = "Tiny file over good network, fixed chunk mode."
        PayloadBytes = 8KB
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        TimeoutSeconds = 60
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "small-good-fixed"
        Description = "Small file over good network, fixed 64 KB chunks."
        PayloadBytes = 512KB
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        TimeoutSeconds = 90
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "medium-slow-fixed"
        Description = "Medium file with stable but slow ACKs, fixed 128 KB chunks."
        PayloadBytes = 2MB
        ChunkKb = 128
        AdaptiveEnabled = $false
        AckPattern = @(180)
        TimeoutSeconds = 120
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "large-good-adaptive"
        Description = "Large file over good network, adaptive chunks should increase."
        PayloadBytes = 12MB
        ChunkKb = 128
        AdaptiveEnabled = $true
        AdaptiveMinKb = 64
        AdaptiveInitialKb = 128
        AdaptiveMaxKb = 1024
        AdaptiveTargetAckMs = 200
        AckPattern = @()
        TimeoutSeconds = 150
        RequireAdaptiveVariation = $true
    },
    [pscustomobject]@{
        Name = "large-poor-adaptive"
        Description = "Large file with consistently poor ACK latency, adaptive chunks should shrink."
        PayloadBytes = 12MB
        ChunkKb = 512
        AdaptiveEnabled = $true
        AdaptiveMinKb = 256
        AdaptiveInitialKb = 1024
        AdaptiveMaxKb = 1024
        AdaptiveTargetAckMs = 150
        AckPattern = @(700)
        TimeoutSeconds = 220
        RequireAdaptiveVariation = $true
    },
    [pscustomobject]@{
        Name = "jittery-adaptive"
        Description = "Jittery network alternating fast and slow ACKs, adaptive chunks should vary both directions."
        PayloadBytes = 6MB
        ChunkKb = 256
        AdaptiveEnabled = $true
        AdaptiveMinKb = 128
        AdaptiveInitialKb = 512
        AdaptiveMaxKb = 1024
        AdaptiveTargetAckMs = 220
        AckPattern = @(30, 650, 45, 800, 60, 500)
        TimeoutSeconds = 180
        RequireAdaptiveVariation = $true
    },
    [pscustomobject]@{
        Name = "many-small-good-fixed"
        Description = "Burst of multiple small files over good network, fixed 64 KB chunks."
        PayloadBytes = 64KB
        FileCount = 8
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        TimeoutSeconds = 120
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "agent-restart-resume"
        Description = "Agent is terminated after the first persisted chunk and must resume idempotently from its existing database."
        PayloadBytes = 8MB
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @(75)
        RestartAgentAfterFirstChunk = $true
        TimeoutSeconds = 150
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "same-path-changed-content-resend"
        Description = "Same watched path is rewritten with different content and must replace the managed server output."
        PayloadBytes = 1MB
        ReplacementPayloadBytes = 1052672
        PayloadRelativePath = "XXX/XXXX/same-path-changed-content-resend.bin"
        VerifyChangedContentResend = $true
        ChangedFilesServerPolicy = "overwrite"
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        TimeoutSeconds = 150
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "same-path-changed-content-versioned"
        Description = "Same watched path is rewritten with different content and must create a versioned server output."
        PayloadBytes = 256KB
        ReplacementPayloadBytes = 300KB
        PayloadRelativePath = "XXX/XXXX/same-path-changed-content-versioned.bin"
        VerifyChangedContentResend = $true
        ChangedFilesServerPolicy = "versioned"
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        TimeoutSeconds = 120
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "unauthorized-client-rejected"
        Description = "Agent presents a certificate identity that is not allowed by server policy."
        ExpectedOutcome = "auth-reject"
        PayloadBytes = 64KB
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        ServerAllowedClientId = "agent-not-authorized"
        TimeoutSeconds = 60
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "revoked-client-rejected"
        Description = "Server CRL revokes the Agent certificate before a secure session can be issued."
        ExpectedOutcome = "auth-reject"
        PayloadBytes = 64KB
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        UseRevokedClientCrl = $true
        TimeoutSeconds = 60
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "wrong-server-ca-rejected"
        Description = "Agent trusts the wrong CA bundle and must reject the Server certificate."
        ExpectedOutcome = "server-proof-reject"
        PayloadBytes = 64KB
        ChunkKb = 64
        AdaptiveEnabled = $false
        AckPattern = @()
        UseWrongCaBundle = $true
        TimeoutSeconds = 60
        RequireAdaptiveVariation = $false
    },
    [pscustomobject]@{
        Name = "too-large-file-rejected"
        Description = "Authenticated agent sends a file larger than server.max_file_size_mb and must be rejected."
        ExpectedOutcome = "too-large-reject"
        PayloadBytes = 2MB
        ChunkKb = 256
        AdaptiveEnabled = $false
        AckPattern = @()
        ServerMaxFileSizeMb = 1
        TimeoutSeconds = 60
        RequireAdaptiveVariation = $false
    }
)

if ($ScenarioName.Count -gt 0) {
    foreach ($requestedName in $ScenarioName) {
        if (-not ($scenarios | Where-Object { $_.Name -eq $requestedName })) {
            throw "Unknown E2E scenario '$requestedName'."
        }
    }
    $scenarios = @($scenarios | Where-Object { $ScenarioName -contains $_.Name })
}

$results = New-Object System.Collections.Generic.List[object]
Write-Host "Production readiness E2E suite root: $RunRoot"
foreach ($scenario in $scenarios) {
    Write-Host "Running scenario: $($scenario.Name)"
    $result = Invoke-E2EScenario $scenario $RunRoot $repoRoot $Configuration $Architecture $DefaultTimeoutSeconds
    $results.Add($result)
    $status = if ($result.Passed) { "PASS" } else { "FAIL" }
    Write-Host "[$status] $($scenario.Name) duration=$([Math]::Round($result.DurationSeconds, 2))s report=$($result.ReportPath)"
}

$passedCount = @($results | Where-Object { $_.Passed }).Count
$failedCount = $results.Count - $passedCount
$summaryLines = New-Object System.Collections.Generic.List[string]
$summaryLines.Add("# TransferUDT E2E Production Readiness Report")
$summaryLines.Add("")
$summaryLines.Add("- Suite root: $RunRoot")
$summaryLines.Add("- Started: $(Get-Date -Format o)")
$summaryLines.Add("- Scenarios: $($results.Count)")
$summaryLines.Add("- Passed: $passedCount")
$summaryLines.Add("- Failed: $failedCount")
$summaryLines.Add("")
$summaryLines.Add("| Scenario | Status | Expected | Files | Size bytes | Changed resend | Server policy | Adaptive | ACK pattern | Duration s | Report |")
$summaryLines.Add("| --- | --- | --- | ---: | ---: | --- | --- | --- | --- | ---: | --- |")
foreach ($result in $results) {
    $status = if ($result.Passed) { "PASS" } else { "FAIL" }
    $summaryLines.Add("| $($result.Name) | $status | $($result.ExpectedOutcome) | $($result.FileCount) | $($result.PayloadBytes) | $($result.ChangedContentResend) | $($result.ChangedFilesServerPolicy) | $($result.AdaptiveEnabled) | $($result.AckDelayPattern) | $([Math]::Round($result.DurationSeconds, 2)) | [$($result.Name)]($($result.ReportPath)) |")
}
$summaryLines.Add("")
$summaryLines.Add("## Readiness Assessment")
$summaryLines.Add("")
if ($failedCount -eq 0) {
    $summaryLines.Add("All E2E production-readiness scenarios passed. The build is a candidate for controlled production pilot, subject to operational checks outside this suite: service installer validation, certificate/CRL distribution, monitoring/alerting, disk quota policy, and restore/retry drills.")
}
else {
    $summaryLines.Add("One or more scenarios failed. Treat this build as not ready for production until the failed reports are reviewed and remediated.")
}

$summaryPath = Join-Path $RunRoot "production-readiness-report.md"
$summaryJsonPath = Join-Path $RunRoot "production-readiness-summary.json"
[string]::Join([Environment]::NewLine, $summaryLines) | Set-Content -Path $summaryPath -Encoding UTF8
$results | ConvertTo-Json -Depth 6 | Set-Content -Path $summaryJsonPath -Encoding UTF8

Write-Host "Summary report: $summaryPath"
Write-Host "Summary JSON: $summaryJsonPath"

if ($failedCount -gt 0) {
    exit 1
}

if (-not $KeepRunRoot) {
    Write-Host "Artifacts kept in $RunRoot"
}
