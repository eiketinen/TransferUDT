using System.Collections.Concurrent;
using System.Net;
using System.Security.Cryptography;
using System.Security.Cryptography.X509Certificates;
using System.Text;
using System.Text.Json;
using Microsoft.AspNetCore.HttpOverrides;
using Microsoft.Data.Sqlite;

var builder = WebApplication.CreateBuilder(args);
builder.Host.UseWindowsService(options =>
{
    options.ServiceName = "TransferUDT Dashboard";
});
builder.Logging.ClearProviders();
builder.Logging.AddConsole();
var settings = builder.Configuration.GetSection("Dashboard").Get<DashboardSettings>() ?? new DashboardSettings();
settings.Normalize();

builder.WebHost.ConfigureKestrel(options =>
{
    var address = IPAddress.Parse(settings.BindAddress);
    options.Listen(address, settings.HttpsPort, listen =>
    {
        if (settings.RequireHttps)
        {
            if (string.IsNullOrWhiteSpace(settings.CertificatePath) || !File.Exists(settings.CertificatePath))
            {
                throw new InvalidOperationException($"HTTPS is required, but the certificate was not found: {settings.CertificatePath}");
            }
            var password = Environment.GetEnvironmentVariable(settings.CertificatePasswordEnv);
            listen.UseHttps(new X509Certificate2(
                settings.CertificatePath,
                password,
                X509KeyStorageFlags.MachineKeySet |
                X509KeyStorageFlags.PersistKeySet |
                X509KeyStorageFlags.Exportable));
        }
    });
});

builder.Services.AddSingleton(settings);
builder.Services.AddSingleton<DashboardStore>();
builder.Services.AddSingleton<HeartbeatSignatureVerifier>();
builder.Services.AddSingleton<ServerReadModel>();
builder.Services.AddSingleton<SessionStore>();
builder.Services.AddSingleton<ObservabilityService>();

var app = builder.Build();
app.UseForwardedHeaders(new ForwardedHeadersOptions
{
    ForwardedHeaders = ForwardedHeaders.XForwardedFor | ForwardedHeaders.XForwardedProto
});
app.UseDefaultFiles();
app.UseStaticFiles();

var store = app.Services.GetRequiredService<DashboardStore>();
store.Initialize();

app.Use(async (context, next) =>
{
    if (settings.RequireHttps && !context.Request.IsHttps)
    {
        context.Response.StatusCode = StatusCodes.Status426UpgradeRequired;
        await context.Response.WriteAsJsonAsync(new { error = "HTTPS is required." });
        return;
    }

    var path = context.Request.Path.Value ?? "";
    var publicEndpoint = path.Equals("/api/login", StringComparison.OrdinalIgnoreCase) ||
                         path.Equals("/api/session", StringComparison.OrdinalIgnoreCase) ||
                         path.Equals("/api/agents/heartbeat", StringComparison.OrdinalIgnoreCase) ||
                         !path.StartsWith("/api/", StringComparison.OrdinalIgnoreCase);
    if (publicEndpoint)
    {
        await next();
        return;
    }

    var sessions = context.RequestServices.GetRequiredService<SessionStore>();
    if (!sessions.IsAuthenticated(context.Request.Cookies["transferudt_session"]))
    {
        context.Response.StatusCode = StatusCodes.Status401Unauthorized;
        await context.Response.WriteAsJsonAsync(new { error = "Authentication required." });
        return;
    }

    await next();
});

app.MapPost("/api/login", async (HttpContext context, SessionStore sessions, DashboardSettings cfg) =>
{
    var configuredPassword = Environment.GetEnvironmentVariable(cfg.OperatorPasswordEnv);
    if (string.IsNullOrWhiteSpace(configuredPassword))
    {
        return Results.Problem("Operator password environment variable is not configured.", statusCode: 503);
    }

    var request = await JsonSerializer.DeserializeAsync<LoginRequest>(context.Request.Body, JsonOptions.Default) ?? new LoginRequest();
    var suppliedPasswordBytes = Encoding.UTF8.GetBytes(request.Password ?? "");
    var configuredPasswordBytes = Encoding.UTF8.GetBytes(configuredPassword);
    if (suppliedPasswordBytes.Length != configuredPasswordBytes.Length ||
        !CryptographicOperations.FixedTimeEquals(suppliedPasswordBytes, configuredPasswordBytes))
    {
        return Results.Unauthorized();
    }

    var token = sessions.Create();
    context.Response.Cookies.Append("transferudt_session", token, new CookieOptions
    {
        HttpOnly = true,
        Secure = cfg.RequireHttps,
        SameSite = SameSiteMode.Strict,
        Expires = DateTimeOffset.UtcNow.AddHours(8)
    });
    return Results.Ok(new { authenticated = true });
});

app.MapGet("/api/session", (HttpContext context, SessionStore sessions) =>
    Results.Ok(new { authenticated = sessions.IsAuthenticated(context.Request.Cookies["transferudt_session"]) }));

app.MapGet("/health/live", () => Results.Ok(new
{
    status = "ok",
    generatedAt = DateTimeOffset.UtcNow,
    application = "TransferUDT Dashboard"
}));

app.MapPost("/api/agents/heartbeat", async (
    HttpContext context,
    DashboardSettings cfg,
    HeartbeatSignatureVerifier verifier,
    DashboardStore dashboardStore,
    ILoggerFactory loggerFactory) =>
{
    var logger = loggerFactory.CreateLogger("TransferUDT.Dashboard.Heartbeat");
    var clientId = context.Request.Headers["X-Client-Id"].ToString();
    var timestamp = context.Request.Headers["X-Timestamp"].ToString();
    var nonce = context.Request.Headers["X-Nonce"].ToString();
    var signature = context.Request.Headers["X-Signature"].ToString();

    using var reader = new StreamReader(context.Request.Body, Encoding.UTF8);
    var body = await reader.ReadToEndAsync();
    try
    {
        if (!verifier.Verify(clientId, timestamp, nonce, signature, body, out var error))
        {
            WriteDashboardLog(cfg, $"Heartbeat rejected for '{clientId}': {error}");
            return Results.Json(new { error }, statusCode: StatusCodes.Status401Unauthorized);
        }

        var heartbeat = JsonSerializer.Deserialize<AgentHeartbeat>(body, JsonOptions.Default);
        if (heartbeat is null || !string.Equals(heartbeat.ClientId, clientId, StringComparison.Ordinal))
        {
            return Results.BadRequest(new { error = "Heartbeat clientId must match X-Client-Id." });
        }

        dashboardStore.UpsertHeartbeat(heartbeat, context.Connection.RemoteIpAddress?.ToString() ?? "");
        return Results.Ok(new { accepted = true });
    }
    catch (JsonException ex)
    {
        logger.LogWarning(ex, "Invalid dashboard heartbeat payload for {ClientId}", clientId);
        WriteDashboardLog(cfg, $"Invalid heartbeat payload for '{clientId}'.", ex);
        return Results.BadRequest(new { error = "Invalid heartbeat JSON payload." });
    }
    catch (Exception ex)
    {
        logger.LogError(ex, "Dashboard heartbeat failed for {ClientId}", clientId);
        WriteDashboardLog(cfg, $"Heartbeat processing failed for '{clientId}'.", ex);
        return Results.Problem("Dashboard heartbeat processing failed.", statusCode: StatusCodes.Status500InternalServerError);
    }
});

app.MapGet("/api/overview", (DashboardStore dashboardStore, ServerReadModel server, DashboardSettings cfg) =>
{
    var agents = dashboardStore.GetAgents(cfg.AgentOfflineAfterSeconds);
    return Results.Ok(new
    {
        generatedAt = DateTimeOffset.UtcNow,
        agentsOnline = agents.Count(a => a.IsOnline),
        agentsOffline = agents.Count(a => !a.IsOnline),
        failedFiles = agents.Sum(a => a.FailedFiles),
        pendingFiles = agents.Sum(a => a.PendingFiles),
        server = server.GetStatus(),
        recentAlerts = dashboardStore.GetAlerts(cfg.AgentOfflineAfterSeconds, cfg.LowDiskWarningBytes, 20)
    });
});

app.MapGet("/api/health", (ObservabilityService observability) =>
    Results.Ok(observability.GetHealth()));

app.MapGet("/api/metrics", (int? windowHours, ObservabilityService observability) =>
    Results.Ok(observability.GetMetrics(windowHours)));

app.MapGet("/api/agents", (DashboardStore dashboardStore, DashboardSettings cfg) =>
    Results.Ok(dashboardStore.GetAgents(cfg.AgentOfflineAfterSeconds)));

app.MapGet("/api/agents/{clientId}", (string clientId, DashboardStore dashboardStore, DashboardSettings cfg) =>
{
    var agent = dashboardStore.GetAgent(clientId, cfg.AgentOfflineAfterSeconds);
    return agent is null ? Results.NotFound() : Results.Ok(agent);
});

app.MapGet("/api/transfers", (ServerReadModel server) => Results.Ok(server.GetTransfers(200)));

app.MapGet("/api/server/status", (ServerReadModel server) => Results.Ok(server.GetStatus()));

app.MapGet("/api/logs", (string? source, string? clientId, string? level, string? q, int? limit,
                         DashboardStore dashboardStore, ServerReadModel server, DashboardSettings cfg) =>
{
    var take = Math.Clamp(limit ?? 200, 1, 1000);
    var logs = new List<LogLine>();
    if (string.IsNullOrWhiteSpace(source) || source.Equals("agent", StringComparison.OrdinalIgnoreCase))
    {
        AddLogs("agent", () => dashboardStore.GetAgentLogs(clientId, level, q, take), logs, cfg);
    }
    if (string.IsNullOrWhiteSpace(source) || source.Equals("server", StringComparison.OrdinalIgnoreCase))
    {
        AddLogs("server", () => server.GetServerLogs(level, q, take), logs, cfg);
    }
    return Results.Ok(logs.OrderByDescending(l => l.Timestamp).Take(take));
});

app.Run();

static void AddLogs(string source, Func<List<LogLine>> readLogs, List<LogLine> target, DashboardSettings settings)
{
    try
    {
        target.AddRange(readLogs());
    }
    catch (Exception ex)
    {
        WriteDashboardLog(settings, $"Failed to read {source} logs.", ex);
    }
}

static void WriteDashboardLog(DashboardSettings settings, string message, Exception? exception = null)
{
    try
    {
        Directory.CreateDirectory(Path.GetDirectoryName(settings.LogPath) ?? ".");
        var line = $"[{DateTimeOffset.UtcNow:O}] {message}";
        if (exception is not null)
        {
            line += Environment.NewLine + exception;
        }
        File.AppendAllText(settings.LogPath, line + Environment.NewLine, Encoding.UTF8);
    }
    catch
    {
        // Logging must never make the HTTP endpoint fail.
    }
}

sealed class DashboardSettings
{
    public string BindAddress { get; set; } = "0.0.0.0";
    public int HttpsPort { get; set; } = 8443;
    public bool RequireHttps { get; set; } = true;
    public string DatabasePath { get; set; } = "C:/TransferUDT/Dashboard/db/dashboard.db";
    public string ServerDatabasePath { get; set; } = "C:/TransferUDT/Server/db/server.db";
    public string ServerLogPath { get; set; } = "C:/TransferUDT/Server/log/server.log";
    public string LogPath { get; set; } = "C:/ProgramData/TransferUDT/Dashboard/logs/dashboard.log";
    public string CertificatePath { get; set; } = "C:/ProgramData/TransferUDT/Dashboard/certs/dashboard.pfx";
    public string CertificatePasswordEnv { get; set; } = "TRANSFERUDT_DASHBOARD_CERT_PASSWORD";
    public string OperatorPasswordEnv { get; set; } = "TRANSFERUDT_DASHBOARD_OPERATOR_PASSWORD";
    public int HeartbeatSkewSeconds { get; set; } = 300;
    public int AgentOfflineAfterSeconds { get; set; } = 60;
    public int HeartbeatRetentionDays { get; set; } = 30;
    public int MetricsWindowHours { get; set; } = 24;
    public int MetricsBucketMinutes { get; set; } = 60;
    public long LowDiskWarningBytes { get; set; } = 5L * 1024 * 1024 * 1024;
    public int AutoRefreshSeconds { get; set; } = 30;
    public Dictionary<string, string> AgentPublicKeys { get; set; } = new();

    public void Normalize()
    {
        if (HttpsPort <= 0 || HttpsPort > 65535) HttpsPort = 8443;
        if (HeartbeatSkewSeconds <= 0) HeartbeatSkewSeconds = 300;
        if (AgentOfflineAfterSeconds <= 0) AgentOfflineAfterSeconds = 60;
        HeartbeatRetentionDays = Math.Clamp(HeartbeatRetentionDays, 1, 3650);
        MetricsWindowHours = Math.Clamp(MetricsWindowHours, 1, 168);
        MetricsBucketMinutes = Math.Clamp(MetricsBucketMinutes, 5, 1440);
        if (LowDiskWarningBytes < 0) LowDiskWarningBytes = 0;
        AutoRefreshSeconds = Math.Clamp(AutoRefreshSeconds, 10, 3600);
    }
}

sealed record LoginRequest(string? Password = null);

sealed record AgentHeartbeat(
    string ClientId,
    string Hostname,
    string AgentVersion,
    string ServiceStatus,
    long UptimeSeconds,
    string[] WatchedDirs,
    int PendingFiles,
    int ProcessedFiles,
    int FailedFiles,
    long DiskFreeBytes,
    string? LastTransferAt,
    string[] RecentErrors,
    string[] RecentLogs);

sealed record AgentSummary(
    string ClientId,
    string Hostname,
    string RemoteAddress,
    string AgentVersion,
    string ServiceStatus,
    DateTimeOffset LastHeartbeatAt,
    bool IsOnline,
    long UptimeSeconds,
    int PendingFiles,
    int ProcessedFiles,
    int FailedFiles,
    long DiskFreeBytes,
    string[] WatchedDirs,
    string[] RecentErrors,
    string[] RecentLogs);

sealed record LogLine(DateTimeOffset Timestamp, string Source, string? ClientId, string Level, string Message);

static class JsonOptions
{
    public static readonly JsonSerializerOptions Default = new(JsonSerializerDefaults.Web);
}

sealed class SessionStore
{
    private readonly ConcurrentDictionary<string, DateTimeOffset> _tokens = new();

    public string Create()
    {
        var token = Convert.ToHexString(RandomNumberGenerator.GetBytes(32)).ToLowerInvariant();
        _tokens[token] = DateTimeOffset.UtcNow.AddHours(8);
        return token;
    }

    public bool IsAuthenticated(string? token)
    {
        if (string.IsNullOrWhiteSpace(token)) return false;
        if (!_tokens.TryGetValue(token, out var expiresAt)) return false;
        if (expiresAt >= DateTimeOffset.UtcNow) return true;
        _tokens.TryRemove(token, out _);
        return false;
    }
}

sealed class HeartbeatSignatureVerifier(DashboardSettings settings)
{
    private readonly ConcurrentDictionary<string, DateTimeOffset> _nonces = new();

    public bool Verify(string clientId, string timestamp, string nonce, string signatureHex, string body, out string error)
    {
        error = "";
        if (string.IsNullOrWhiteSpace(clientId) || string.IsNullOrWhiteSpace(timestamp) ||
            string.IsNullOrWhiteSpace(nonce) || string.IsNullOrWhiteSpace(signatureHex))
        {
            error = "Missing heartbeat authentication headers.";
            return false;
        }
        if (!long.TryParse(timestamp, out var unixSeconds))
        {
            error = "Invalid heartbeat timestamp.";
            return false;
        }
        var sentAt = DateTimeOffset.FromUnixTimeSeconds(unixSeconds);
        if (Math.Abs((DateTimeOffset.UtcNow - sentAt).TotalSeconds) > settings.HeartbeatSkewSeconds)
        {
            error = "Heartbeat timestamp is outside the allowed window.";
            return false;
        }
        CleanupNonces();
        var nonceKey = clientId + ":" + nonce;
        if (!_nonces.TryAdd(nonceKey, sentAt.AddSeconds(settings.HeartbeatSkewSeconds)))
        {
            error = "Heartbeat nonce was already used.";
            return false;
        }
        if (!settings.AgentPublicKeys.TryGetValue(clientId, out var publicKeyPath) || !File.Exists(publicKeyPath))
        {
            error = "No configured public key for client.";
            return false;
        }
        if (signatureHex.Length % 2 != 0 || signatureHex.Any(c => !Uri.IsHexDigit(c)))
        {
            error = "Invalid heartbeat signature encoding.";
            return false;
        }

        try
        {
            var canonical = timestamp + "\n" + nonce + "\n" + body;
            using var rsa = RSA.Create();
            rsa.ImportFromPem(File.ReadAllText(publicKeyPath));
            var signature = Convert.FromHexString(signatureHex);
            if (!rsa.VerifyData(Encoding.UTF8.GetBytes(canonical), signature, HashAlgorithmName.SHA256, RSASignaturePadding.Pkcs1))
            {
                error = "Invalid heartbeat signature.";
                return false;
            }
        }
        catch (Exception ex) when (ex is CryptographicException or FormatException or IOException or UnauthorizedAccessException)
        {
            error = "Heartbeat signature verification failed: " + ex.Message;
            return false;
        }
        return true;
    }

    private void CleanupNonces()
    {
        var now = DateTimeOffset.UtcNow;
        foreach (var item in _nonces)
        {
            if (item.Value < now) _nonces.TryRemove(item.Key, out _);
        }
    }
}

sealed class DashboardStore(DashboardSettings settings)
{
    private string ConnectionString => $"Data Source={settings.DatabasePath}";

    public void Initialize()
    {
        Directory.CreateDirectory(Path.GetDirectoryName(settings.DatabasePath) ?? ".");
        using var conn = new SqliteConnection(ConnectionString);
        conn.Open();
        EnsureTable(
            conn,
            "agent_heartbeats",
            """
                CREATE TABLE agent_heartbeats (
                    id INTEGER PRIMARY KEY AUTOINCREMENT,
                    client_id TEXT NOT NULL,
                    hostname TEXT NOT NULL,
                    remote_address TEXT NOT NULL,
                    payload_json TEXT NOT NULL,
                    received_at TEXT NOT NULL
                );
                """,
            requireClientIdPrimaryKey: false);
        EnsureTable(
            conn,
            "agent_latest",
            """
                CREATE TABLE agent_latest (
                    client_id TEXT PRIMARY KEY,
                    hostname TEXT NOT NULL,
                    remote_address TEXT NOT NULL,
                    payload_json TEXT NOT NULL,
                    received_at TEXT NOT NULL
                );
                """,
            requireClientIdPrimaryKey: true);
        ExecuteNonQuery(conn, "CREATE INDEX IF NOT EXISTS idx_agent_heartbeats_client_time ON agent_heartbeats(client_id, received_at);");
        ExecuteNonQuery(conn, "CREATE INDEX IF NOT EXISTS idx_agent_heartbeats_time ON agent_heartbeats(received_at);");
    }

    public void UpsertHeartbeat(AgentHeartbeat heartbeat, string remoteAddress)
    {
        var now = DateTimeOffset.UtcNow;
        var json = JsonSerializer.Serialize(heartbeat, JsonOptions.Default);
        using var conn = new SqliteConnection(ConnectionString);
        conn.Open();
        using var tx = conn.BeginTransaction();
        Execute(conn, tx, """
            INSERT INTO agent_heartbeats(client_id, hostname, remote_address, payload_json, received_at)
            VALUES ($client_id, $hostname, $remote_address, $payload_json, $received_at);
            """, heartbeat.ClientId, heartbeat.Hostname, remoteAddress, json, now);
        Execute(conn, tx, """
            INSERT INTO agent_latest(client_id, hostname, remote_address, payload_json, received_at)
            VALUES ($client_id, $hostname, $remote_address, $payload_json, $received_at)
            ON CONFLICT(client_id) DO UPDATE SET
                hostname = excluded.hostname,
                remote_address = excluded.remote_address,
                payload_json = excluded.payload_json,
                received_at = excluded.received_at;
            """, heartbeat.ClientId, heartbeat.Hostname, remoteAddress, json, now);
        using (var prune = conn.CreateCommand())
        {
            prune.Transaction = tx;
            prune.CommandText = "DELETE FROM agent_heartbeats WHERE received_at < $cutoff;";
            prune.Parameters.AddWithValue("$cutoff", now.AddDays(-settings.HeartbeatRetentionDays).ToString("O"));
            prune.ExecuteNonQuery();
        }
        tx.Commit();
    }

    public List<AgentSummary> GetAgents(int offlineAfterSeconds)
    {
        using var conn = new SqliteConnection(ConnectionString);
        conn.Open();
        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT client_id, hostname, remote_address, payload_json, received_at FROM agent_latest ORDER BY client_id;";
        using var reader = cmd.ExecuteReader();
        var items = new List<AgentSummary>();
        while (reader.Read())
        {
            items.Add(ToSummary(reader.GetString(2), reader.GetString(3), reader.GetString(4), offlineAfterSeconds));
        }
        return items;
    }

    public AgentSummary? GetAgent(string clientId, int offlineAfterSeconds) =>
        GetAgents(offlineAfterSeconds).FirstOrDefault(a => a.ClientId.Equals(clientId, StringComparison.Ordinal));

    public List<LogLine> GetAgentLogs(string? clientId, string? level, string? query, int limit) =>
        GetAgents(int.MaxValue)
            .Where(a => string.IsNullOrWhiteSpace(clientId) || a.ClientId.Equals(clientId, StringComparison.OrdinalIgnoreCase))
            .SelectMany(a => a.RecentLogs.Select(line => ParseLogLine("agent", a.ClientId, line)))
            .Where(l => FilterLog(l, level, query))
            .OrderByDescending(l => l.Timestamp)
            .Take(limit)
            .ToList();

    public List<ObservabilityAlert> GetAlerts(int offlineAfterSeconds, long lowDiskWarningBytes, int limit)
    {
        var alerts = new List<ObservabilityAlert>();
        foreach (var agent in GetAgents(offlineAfterSeconds))
        {
            if (!agent.IsOnline)
                alerts.Add(new("WARN", "agent", agent.ClientId, "Agent offline.", agent.LastHeartbeatAt));
            if (!agent.ServiceStatus.Equals("running", StringComparison.OrdinalIgnoreCase))
                alerts.Add(new("WARN", "agent", agent.ClientId, $"Estado do servico: '{agent.ServiceStatus}'.", agent.LastHeartbeatAt));
            if (agent.FailedFiles > 0)
                alerts.Add(new("ERROR", "agent", agent.ClientId, $"{agent.FailedFiles} arquivo(s) com falha.", agent.LastHeartbeatAt));
            if (lowDiskWarningBytes > 0 && agent.DiskFreeBytes >= 0 && agent.DiskFreeBytes < lowDiskWarningBytes)
                alerts.Add(new("WARN", "agent", agent.ClientId, "Espaco livre em disco abaixo do limite configurado.", agent.LastHeartbeatAt));
        }
        return alerts
            .OrderBy(a => a.Severity == "ERROR" ? 0 : 1)
            .ThenByDescending(a => a.At)
            .Take(limit)
            .ToList();
    }

    public HealthComponent GetHealthComponent()
    {
        try
        {
            using var conn = new SqliteConnection(ConnectionString);
            conn.Open();
            using var cmd = conn.CreateCommand();
            cmd.CommandText = "SELECT 1;";
            cmd.ExecuteScalar();
            return new("dashboardDatabase", "healthy", "Banco do Dashboard disponivel.");
        }
        catch
        {
            return new("dashboardDatabase", "unhealthy", "Banco do Dashboard indisponivel.");
        }
    }

    public long CountHeartbeats(int windowHours)
    {
        using var conn = new SqliteConnection(ConnectionString);
        conn.Open();
        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT COUNT(*) FROM agent_heartbeats WHERE received_at >= $cutoff;";
        cmd.Parameters.AddWithValue("$cutoff", DateTimeOffset.UtcNow.AddHours(-windowHours).ToString("O"));
        return Convert.ToInt64(cmd.ExecuteScalar() ?? 0L);
    }

    public List<AgentTrendPoint> GetTrend(int windowHours, int bucketMinutes)
    {
        var maxPoints = 200;
        var effectiveBucketMinutes = Math.Max(bucketMinutes, (int)Math.Ceiling(windowHours * 60d / maxPoints));
        var cutoff = DateTimeOffset.UtcNow.AddHours(-windowHours);
        using var conn = new SqliteConnection(ConnectionString);
        conn.Open();
        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT payload_json, received_at FROM agent_heartbeats WHERE received_at >= $cutoff ORDER BY received_at;";
        cmd.Parameters.AddWithValue("$cutoff", cutoff.ToString("O"));
        using var reader = cmd.ExecuteReader();
        var samples = new List<(AgentHeartbeat Heartbeat, DateTimeOffset ReceivedAt)>();
        while (reader.Read())
        {
            try
            {
                var heartbeat = JsonSerializer.Deserialize<AgentHeartbeat>(reader.GetString(0), JsonOptions.Default);
                if (heartbeat is not null)
                    samples.Add((heartbeat, DateTimeOffset.Parse(reader.GetString(1))));
            }
            catch (JsonException)
            {
                // Ignore one malformed historical sample without hiding current health.
            }
        }

        var bucketSeconds = effectiveBucketMinutes * 60L;
        return samples
            .GroupBy(sample => sample.ReceivedAt.ToUnixTimeSeconds() / bucketSeconds * bucketSeconds)
            .OrderBy(group => group.Key)
            .Select(group =>
            {
                var latestByAgent = group
                    .GroupBy(sample => sample.Heartbeat.ClientId, StringComparer.Ordinal)
                    .Select(agent => agent.OrderByDescending(sample => sample.ReceivedAt).First().Heartbeat)
                    .ToList();
                var freeDiskValues = latestByAgent.Select(agent => agent.DiskFreeBytes).Where(bytes => bytes >= 0).ToList();
                return new AgentTrendPoint(
                    DateTimeOffset.FromUnixTimeSeconds(group.Key),
                    latestByAgent.Count,
                    latestByAgent.Sum(agent => agent.PendingFiles),
                    latestByAgent.Sum(agent => agent.FailedFiles),
                    freeDiskValues.Count == 0 ? 0 : freeDiskValues.Min());
            })
            .ToList();
    }

    private static void Execute(SqliteConnection conn, SqliteTransaction tx, string sql, string clientId, string hostname, string remoteAddress, string json, DateTimeOffset receivedAt)
    {
        using var cmd = conn.CreateCommand();
        cmd.Transaction = tx;
        cmd.CommandText = sql;
        cmd.Parameters.AddWithValue("$client_id", clientId);
        cmd.Parameters.AddWithValue("$hostname", hostname);
        cmd.Parameters.AddWithValue("$remote_address", remoteAddress);
        cmd.Parameters.AddWithValue("$payload_json", json);
        cmd.Parameters.AddWithValue("$received_at", receivedAt.ToString("O"));
        cmd.ExecuteNonQuery();
    }

    private static void EnsureTable(SqliteConnection conn, string tableName, string createSql, bool requireClientIdPrimaryKey)
    {
        var expectedColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase)
        {
            "client_id",
            "hostname",
            "remote_address",
            "payload_json",
            "received_at"
        };
        if (TableExists(conn, tableName) && !IsCompatibleTable(conn, tableName, expectedColumns, requireClientIdPrimaryKey))
        {
            ExecuteNonQuery(conn, $"DROP TABLE {tableName};");
        }
        if (!TableExists(conn, tableName))
        {
            ExecuteNonQuery(conn, createSql);
        }
    }

    private static bool TableExists(SqliteConnection conn, string tableName)
    {
        using var cmd = conn.CreateCommand();
        cmd.CommandText = "SELECT COUNT(*) FROM sqlite_master WHERE type = 'table' AND name = $name;";
        cmd.Parameters.AddWithValue("$name", tableName);
        return Convert.ToInt32(cmd.ExecuteScalar()) > 0;
    }

    private static bool IsCompatibleTable(SqliteConnection conn, string tableName, HashSet<string> expectedColumns, bool requireClientIdPrimaryKey)
    {
        using var cmd = conn.CreateCommand();
        cmd.CommandText = $"PRAGMA table_info({tableName});";
        using var reader = cmd.ExecuteReader();
        var foundColumns = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var clientIdPrimaryKey = false;
        while (reader.Read())
        {
            var name = reader.GetString(1);
            foundColumns.Add(name);
            if (name.Equals("client_id", StringComparison.OrdinalIgnoreCase) && reader.GetInt32(5) > 0)
            {
                clientIdPrimaryKey = true;
            }
        }
        return expectedColumns.All(foundColumns.Contains) && (!requireClientIdPrimaryKey || clientIdPrimaryKey);
    }

    private static void ExecuteNonQuery(SqliteConnection conn, string sql)
    {
        using var cmd = conn.CreateCommand();
        cmd.CommandText = sql;
        cmd.ExecuteNonQuery();
    }

    private static AgentSummary ToSummary(string remoteAddress, string payloadJson, string receivedAtRaw, int offlineAfterSeconds)
    {
        var heartbeat = JsonSerializer.Deserialize<AgentHeartbeat>(payloadJson, JsonOptions.Default) ?? throw new InvalidDataException("Invalid heartbeat payload.");
        var receivedAt = DateTimeOffset.Parse(receivedAtRaw);
        return new AgentSummary(
            heartbeat.ClientId,
            heartbeat.Hostname,
            remoteAddress,
            heartbeat.AgentVersion,
            heartbeat.ServiceStatus,
            receivedAt,
            DateTimeOffset.UtcNow - receivedAt <= TimeSpan.FromSeconds(offlineAfterSeconds),
            heartbeat.UptimeSeconds,
            heartbeat.PendingFiles,
            heartbeat.ProcessedFiles,
            heartbeat.FailedFiles,
            heartbeat.DiskFreeBytes,
            heartbeat.WatchedDirs,
            heartbeat.RecentErrors,
            heartbeat.RecentLogs);
    }

    internal static LogLine ParseLogLine(string source, string? clientId, string line)
    {
        var level = line.Contains(" error", StringComparison.OrdinalIgnoreCase) || line.Contains("[error", StringComparison.OrdinalIgnoreCase) ? "ERROR" :
                    line.Contains(" warn", StringComparison.OrdinalIgnoreCase) || line.Contains("[warn", StringComparison.OrdinalIgnoreCase) ? "WARN" :
                    "INFO";
        return new LogLine(DateTimeOffset.UtcNow, source, clientId, level, line);
    }

    internal static bool FilterLog(LogLine line, string? level, string? query) =>
        (string.IsNullOrWhiteSpace(level) || line.Level.Equals(level, StringComparison.OrdinalIgnoreCase)) &&
        (string.IsNullOrWhiteSpace(query) || line.Message.Contains(query, StringComparison.OrdinalIgnoreCase));
}

sealed class ServerReadModel(DashboardSettings settings)
{
    public object GetStatus()
    {
        var dbExists = File.Exists(settings.ServerDatabasePath);
        var logExists = File.Exists(settings.ServerLogPath);
        var reconstructed = 0L;
        var chunks = 0L;
        var errors = 0L;
        if (dbExists)
        {
            reconstructed = Scalar("SELECT COUNT(*) FROM reconstructed_files;");
            chunks = Scalar("SELECT COUNT(*) FROM chunks;");
            errors = Scalar("SELECT COUNT(*) FROM error_logs;");
        }
        return new { dbExists, logExists, reconstructedFiles = reconstructed, chunks, errors };
    }

    public List<object> GetTransfers(int limit)
    {
        if (!File.Exists(settings.ServerDatabasePath)) return [];
        using var conn = OpenReadOnly();
        using var cmd = conn.CreateCommand();
        cmd.CommandText = """
            SELECT client_address, filename, reconstructed_path, created_at
            FROM reconstructed_files
            ORDER BY created_at DESC
            LIMIT $limit;
            """;
        cmd.Parameters.AddWithValue("$limit", limit);
        using var reader = cmd.ExecuteReader();
        var items = new List<object>();
        while (reader.Read())
        {
            items.Add(new
            {
                clientId = reader.GetString(0),
                filename = reader.GetString(1),
                reconstructedPath = reader.GetString(2),
                completedAt = reader.GetString(3),
                status = "completed"
            });
        }
        return items;
    }

    public List<HealthComponent> GetHealthComponents()
    {
        var components = new List<HealthComponent>();
        if (!File.Exists(settings.ServerDatabasePath))
        {
            components.Add(new("serverDatabase", "degraded", "Banco do Server nao encontrado."));
        }
        else
        {
            try
            {
                using var conn = OpenReadOnly();
                using var cmd = conn.CreateCommand();
                cmd.CommandText = "SELECT 1;";
                cmd.ExecuteScalar();
                components.Add(new("serverDatabase", "healthy", "Banco do Server disponivel para leitura."));
            }
            catch
            {
                components.Add(new("serverDatabase", "degraded", "Banco do Server indisponivel para leitura."));
            }
        }
        components.Add(File.Exists(settings.ServerLogPath)
            ? new("serverLog", "healthy", "Log do Server disponivel.")
            : new("serverLog", "degraded", "Log do Server nao encontrado."));
        return components;
    }

    public ServerMetrics GetMetrics(int windowHours)
    {
        if (!File.Exists(settings.ServerDatabasePath))
            return new(0, 0, 0, 0, 0, 0, null);

        var modifier = $"-{windowHours} hours";
        return new(
            Scalar("SELECT COUNT(*) FROM reconstructed_files;"),
            Scalar("SELECT COUNT(*) FROM reconstructed_files WHERE created_at >= datetime('now', $window);", modifier),
            Scalar("SELECT COUNT(*) FROM error_logs WHERE created_at >= datetime('now', $window);", modifier),
            Scalar("SELECT COUNT(*) FROM chunks WHERE status = 'pending';"),
            Scalar("SELECT COUNT(*) FROM chunks WHERE status = 'failed';"),
            Scalar("SELECT COUNT(*) FROM chunks;"),
            ScalarText("SELECT MAX(created_at) FROM reconstructed_files;"));
    }

    public List<LogLine> GetServerLogs(string? level, string? query, int limit) =>
        TailLines(settings.ServerLogPath, limit)
            .Select(line => DashboardStore.ParseLogLine("server", null, line))
            .Where(l => DashboardStore.FilterLog(l, level, query))
            .OrderByDescending(l => l.Timestamp)
            .Take(limit)
            .ToList();

    private long Scalar(string sql)
    {
        try
        {
            using var conn = OpenReadOnly();
            using var cmd = conn.CreateCommand();
            cmd.CommandText = sql;
            return (long)(cmd.ExecuteScalar() ?? 0L);
        }
        catch
        {
            return 0L;
        }
    }

    private long Scalar(string sql, string window)
    {
        try
        {
            using var conn = OpenReadOnly();
            using var cmd = conn.CreateCommand();
            cmd.CommandText = sql;
            cmd.Parameters.AddWithValue("$window", window);
            return Convert.ToInt64(cmd.ExecuteScalar() ?? 0L);
        }
        catch
        {
            return 0L;
        }
    }

    private string? ScalarText(string sql)
    {
        try
        {
            using var conn = OpenReadOnly();
            using var cmd = conn.CreateCommand();
            cmd.CommandText = sql;
            var value = cmd.ExecuteScalar();
            return value is null or DBNull ? null : Convert.ToString(value);
        }
        catch
        {
            return null;
        }
    }

    private SqliteConnection OpenReadOnly()
    {
        var conn = new SqliteConnection($"Data Source={settings.ServerDatabasePath};Mode=ReadOnly;Cache=Shared");
        conn.Open();
        return conn;
    }

    private static IEnumerable<string> TailLines(string path, int limit)
    {
        if (!File.Exists(path)) yield break;

        string[] lines;
        try
        {
            using var stream = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
            using var reader = new StreamReader(stream, Encoding.UTF8, detectEncodingFromByteOrderMarks: true);
            var queue = new Queue<string>(limit);
            while (reader.ReadLine() is { } line)
            {
                if (queue.Count == limit)
                    queue.Dequeue();
                queue.Enqueue(line);
            }
            lines = queue.ToArray();
        }
        catch
        {
            yield break;
        }

        foreach (var line in lines)
            yield return line;
    }
}
