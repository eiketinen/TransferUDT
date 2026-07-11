param(
    [string]$RunRoot = "",
    [int]$Port = 18443,
    [switch]$UseHttpForLocalTest
)

$ErrorActionPreference = "Stop"

function Get-RepoRoot {
    return (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
}

function New-Directory([string]$Path) {
    New-Item -ItemType Directory -Path $Path -Force | Out-Null
    return (Resolve-Path $Path).Path
}

function Convert-ToPem([string]$Label, [byte[]]$Der) {
    $b64 = [Convert]::ToBase64String($Der)
    $lines = New-Object System.Collections.Generic.List[string]
    $lines.Add("-----BEGIN $Label-----")
    for ($i = 0; $i -lt $b64.Length; $i += 64) {
        $lines.Add($b64.Substring($i, [Math]::Min(64, $b64.Length - $i)))
    }
    $lines.Add("-----END $Label-----")
    return [string]::Join([Environment]::NewLine, $lines)
}

function Wait-HttpReady([string]$Url, [int]$TimeoutSeconds) {
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        & curl.exe -k -s -o NUL --max-time 2 $Url
        if ($LASTEXITCODE -eq 0) {
            return
        }
        Start-Sleep -Milliseconds 300
    }
    throw "Dashboard did not become ready at $Url"
}

$repoRoot = Get-RepoRoot
if ([string]::IsNullOrWhiteSpace($RunRoot)) {
    $RunRoot = Join-Path $repoRoot ("e2e-runs\dashboard-https-" + (Get-Date -Format "yyyyMMdd-HHmmss"))
}
$RunRoot = New-Directory $RunRoot
$certPath = Join-Path $RunRoot "dashboard.pfx"
$dbPath = Join-Path $RunRoot "dashboard.db"
$serverDbPath = Join-Path $RunRoot "server.db"
$serverLogPath = Join-Path $RunRoot "server.log"
$publicKeyPath = Join-Path $RunRoot "agent-default.pub"
$privateKeyPath = Join-Path $RunRoot "agent-default.key"
$cookiePath = Join-Path $RunRoot "cookies.txt"
$certPassword = "dashboard-test-password"
$operatorPassword = "operator-test-password"

$helperDir = New-Directory (Join-Path $RunRoot "CryptoHelper")
$helperProject = Join-Path $helperDir "CryptoHelper.csproj"
$helperProgram = Join-Path $helperDir "Program.cs"
@"
<Project Sdk="Microsoft.NET.Sdk">
  <PropertyGroup>
    <OutputType>Exe</OutputType>
    <TargetFramework>net8.0</TargetFramework>
    <ImplicitUsings>enable</ImplicitUsings>
    <Nullable>enable</Nullable>
  </PropertyGroup>
</Project>
"@ | Set-Content -Path $helperProject -Encoding UTF8
@"
using System.Security.Cryptography;
using System.Text;
using System.Net;
using System.Net.Http;
using System.Net.Http.Json;
using System.Text.Json;
using System.Security.Cryptography.X509Certificates;
using System.Security.Authentication;

static string Pem(string label, byte[] der)
{
    var b64 = Convert.ToBase64String(der);
    var sb = new StringBuilder();
    sb.AppendLine($"-----BEGIN {label}-----");
    for (var i = 0; i < b64.Length; i += 64) sb.AppendLine(b64.Substring(i, Math.Min(64, b64.Length - i)));
    sb.AppendLine($"-----END {label}-----");
    return sb.ToString();
}

if (args[0] == "generate")
{
    using var rsa = RSA.Create(3072);
    File.WriteAllText(args[1], Pem("PRIVATE KEY", rsa.ExportPkcs8PrivateKey()), Encoding.ASCII);
    File.WriteAllText(args[2], Pem("PUBLIC KEY", rsa.ExportSubjectPublicKeyInfo()), Encoding.ASCII);
    return;
}

if (args[0] == "cert")
{
    using var rsa = RSA.Create(2048);
    var request = new CertificateRequest("CN=localhost", rsa, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1);
    request.CertificateExtensions.Add(new X509BasicConstraintsExtension(false, false, 0, false));
    request.CertificateExtensions.Add(new X509KeyUsageExtension(X509KeyUsageFlags.DigitalSignature | X509KeyUsageFlags.KeyEncipherment, false));
    request.CertificateExtensions.Add(new X509EnhancedKeyUsageExtension(new OidCollection { new Oid("1.3.6.1.5.5.7.3.1") }, false));
    var san = new SubjectAlternativeNameBuilder();
    san.AddDnsName("localhost");
    san.AddIpAddress(IPAddress.Parse("127.0.0.1"));
    request.CertificateExtensions.Add(san.Build());
    using var cert = request.CreateSelfSigned(DateTimeOffset.UtcNow.AddMinutes(-5), DateTimeOffset.UtcNow.AddDays(2));
    using var exportable = cert.HasPrivateKey ? cert : cert.CopyWithPrivateKey(rsa);
    File.WriteAllBytes(args[1], exportable.Export(X509ContentType.Pfx, args[2]));
    return;
}

if (args[0] == "sign")
{
    using var rsa = RSA.Create();
    rsa.ImportFromPem(File.ReadAllText(args[1]));
    var data = Encoding.UTF8.GetBytes(args[2]);
    Console.Write(Convert.ToHexString(rsa.SignData(data, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1)).ToLowerInvariant());
    return;
}

if (args[0] == "exercise")
{
    var baseUrl = args[1].TrimEnd('/');
    var password = args[2];
    var privateKeyPath = args[3];
    var cookies = new CookieContainer();
    var handler = new HttpClientHandler
    {
        CookieContainer = cookies,
        SslProtocols = SslProtocols.Tls12,
        ServerCertificateCustomValidationCallback = HttpClientHandler.DangerousAcceptAnyServerCertificateValidator
    };
    using var http = new HttpClient(handler) { Timeout = TimeSpan.FromSeconds(5) };

    Exception? lastReadyError = null;
    for (var attempt = 0; attempt < 50; attempt++)
    {
        try
        {
            using var ready = await http.GetAsync(baseUrl + "/health/live");
            if (ready.IsSuccessStatusCode) break;
        }
        catch (Exception ex)
        {
            lastReadyError = ex;
        }
        await Task.Delay(300);
        if (attempt == 49) throw new Exception("Dashboard did not become ready: " + lastReadyError?.Message, lastReadyError);
    }

    using (var protectedHealth = await http.GetAsync(baseUrl + "/api/health"))
    {
        if (protectedHealth.StatusCode != HttpStatusCode.Unauthorized)
            throw new Exception("Detailed health endpoint must require authentication.");
    }

    using var login = await http.PostAsJsonAsync(baseUrl + "/api/login", new { password });
    login.EnsureSuccessStatusCode();

    using var rsa = RSA.Create();
    rsa.ImportFromPem(File.ReadAllText(privateKeyPath));

    async Task SendHeartbeatAsync(object heartbeat)
    {
        var body = JsonSerializer.Serialize(heartbeat, new JsonSerializerOptions(JsonSerializerDefaults.Web));
        var timestamp = DateTimeOffset.UtcNow.ToUnixTimeSeconds().ToString();
        var nonce = Convert.ToHexString(RandomNumberGenerator.GetBytes(32)).ToLowerInvariant();
        var canonical = timestamp + "\n" + nonce + "\n" + body;
        var signature = Convert.ToHexString(rsa.SignData(Encoding.UTF8.GetBytes(canonical), HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1)).ToLowerInvariant();
        using var request = new HttpRequestMessage(HttpMethod.Post, baseUrl + "/api/agents/heartbeat");
        request.Content = new StringContent(body, Encoding.UTF8, "application/json");
        request.Headers.Add("X-Client-Id", "agent-default");
        request.Headers.Add("X-Timestamp", timestamp);
        request.Headers.Add("X-Nonce", nonce);
        request.Headers.Add("X-Signature", signature);
        using var response = await http.SendAsync(request);
        response.EnsureSuccessStatusCode();
    }

    var heartbeat = new
    {
        clientId = "agent-default",
        hostname = "agent-e2e",
        agentVersion = "AgentUDTC++",
        serviceStatus = "running",
        uptimeSeconds = 12,
        watchedDirs = new[] { "C:/TransferUDT/Agent/incoming" },
        pendingFiles = 1,
        processedFiles = 2,
        failedFiles = 0,
        diskFreeBytes = 123456789,
        lastTransferAt = (string?)null,
        recentErrors = Array.Empty<string>(),
        recentLogs = new[] { "[INFO] heartbeat e2e" }
    };
    await SendHeartbeatAsync(heartbeat);

    var overview = await http.GetStringAsync(baseUrl + "/api/overview");
    var agents = await http.GetStringAsync(baseUrl + "/api/agents");
    var health = await http.GetStringAsync(baseUrl + "/api/health");
    var metrics = await http.GetStringAsync(baseUrl + "/api/metrics?windowHours=24");
    var filteredLogs = await http.GetStringAsync(baseUrl + "/api/logs?source=agent&level=INFO&q=heartbeat&limit=20");
    if (!overview.Contains("\"agentsOnline\":1")) throw new Exception("Expected one online agent in overview.");
    if (!agents.Contains("\"clientId\":\"agent-default\"")) throw new Exception("Expected agent-default in agents API.");
    if (!health.Contains("\"name\":\"agents\",\"status\":\"healthy\"")) throw new Exception("Expected healthy Agent component.");
    if (!metrics.Contains("\"heartbeatSamples\":1")) throw new Exception("Expected one heartbeat sample in metrics.");
    if (!metrics.Contains("\"pendingFiles\":1")) throw new Exception("Expected pending file gauge in metrics.");
    if (!metrics.Contains("\"processedFiles\":2")) throw new Exception("Expected processed file gauge in metrics.");
    if (!metrics.Contains("\"reportingAgents\":1")) throw new Exception("Expected Agent trend point.");
    if (!filteredLogs.Contains("heartbeat e2e")) throw new Exception("Expected filtered Agent log.");

    var completedAt = DateTimeOffset.UtcNow;
    await SendHeartbeatAsync(new
    {
        clientId = "agent-default",
        hostname = "agent-e2e",
        agentVersion = "AgentUDTC++",
        serviceStatus = "running",
        uptimeSeconds = 20,
        watchedDirs = new[] { "C:/TransferUDT/Agent/incoming" },
        pendingFiles = 0,
        processedFiles = 3,
        failedFiles = 0,
        diskFreeBytes = 123456789,
        lastTransferAt = completedAt,
        recentErrors = Array.Empty<string>(),
        recentLogs = new[] { "[INFO] heartbeat e2e transfer completed" }
    });

    var completedMetrics = await http.GetStringAsync(baseUrl + "/api/metrics?windowHours=24");
    var completedAgents = await http.GetStringAsync(baseUrl + "/api/agents");
    if (!completedMetrics.Contains("\"heartbeatSamples\":2")) throw new Exception("Expected two heartbeat samples after lifecycle transition.");
    if (!completedMetrics.Contains("\"pendingFiles\":0")) throw new Exception("Expected pending file gauge to return to zero.");
    if (!completedMetrics.Contains("\"processedFiles\":3")) throw new Exception("Expected processed file gauge to increment after completion.");
    if (!completedAgents.Contains("\"lastTransferAt\":")) throw new Exception("Expected last transfer timestamp after completion.");
    Console.WriteLine("PASS");
}
"@ | Set-Content -Path $helperProgram -Encoding UTF8
if (-not $UseHttpForLocalTest) {
    dotnet run --project $helperProject -- cert $certPath $certPassword | Out-Null
}
dotnet run --project $helperProject -- generate $privateKeyPath $publicKeyPath | Out-Null

"[INFO] dashboard e2e server log" | Set-Content -Path $serverLogPath -Encoding UTF8
$env:TRANSFERUDT_DASHBOARD_CERT_PASSWORD = $certPassword
$env:TRANSFERUDT_DASHBOARD_OPERATOR_PASSWORD = $operatorPassword

$dashboardProject = Join-Path $repoRoot "DashboardWeb\DashboardWeb.csproj"
$dashboardStdout = Join-Path $RunRoot "dashboard.stdout.log"
$dashboardStderr = Join-Path $RunRoot "dashboard.stderr.log"
$argumentLine = @(
    "run",
    "--project `"$dashboardProject`"",
    "--configuration Release",
    "--no-build",
    "--",
    "--Dashboard:HttpsPort=$Port",
    "--Dashboard:BindAddress=127.0.0.1",
    "--Dashboard:RequireHttps=$((-not $UseHttpForLocalTest).ToString().ToLowerInvariant())",
    "--Dashboard:CertificatePath=`"$certPath`"",
    "--Dashboard:DatabasePath=`"$dbPath`"",
    "--Dashboard:ServerDatabasePath=`"$serverDbPath`"",
    "--Dashboard:ServerLogPath=`"$serverLogPath`"",
    "--Dashboard:AgentPublicKeys:agent-default=`"$publicKeyPath`""
) -join " "

$psi = [System.Diagnostics.ProcessStartInfo]::new()
$psi.FileName = "dotnet"
$psi.Arguments = $argumentLine
$psi.WorkingDirectory = $repoRoot
$psi.UseShellExecute = $false
$psi.CreateNoWindow = $true
$process = [System.Diagnostics.Process]::new()
$process.StartInfo = $psi
$null = $process.Start()
try {
    $scheme = if ($UseHttpForLocalTest) { "http" } else { "https" }
    $baseUrl = "${scheme}://127.0.0.1:$Port"
    dotnet run --project $helperProject -- exercise $baseUrl $operatorPassword $privateKeyPath | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Dashboard HTTPS exercise failed."
    }

    $report = @"
# Dashboard E2E Report

- Status: PASS
- Base URL: $baseUrl
- Dashboard DB: $dbPath
- Agent public key: $publicKeyPath
- Validated: public liveness, protected readiness, operator login, signed heartbeat lifecycle from pending to completed, file-level metrics, last transfer time, trends, alerts, overview, agents, and filtered logs.
- Transport mode: $(if ($UseHttpForLocalTest) { "HTTP test mode" } else { "HTTPS" })
"@
    $reportPath = Join-Path $RunRoot "report.md"
    $report | Set-Content -Path $reportPath -Encoding UTF8
    Write-Host "PASS Dashboard E2E"
    Write-Host "Report: $reportPath"
}
finally {
    if ($null -ne $process -and -not $process.HasExited) {
        Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        $process.WaitForExit(5000) | Out-Null
    }
}
