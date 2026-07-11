sealed record HealthComponent(string Name, string Status, string Message);

sealed record HealthSnapshot(
    DateTimeOffset GeneratedAt,
    string Status,
    IReadOnlyList<HealthComponent> Components);

sealed record ObservabilityAlert(
    string Severity,
    string Source,
    string? ClientId,
    string Message,
    DateTimeOffset At);

sealed record AgentTrendPoint(
    DateTimeOffset Timestamp,
    int ReportingAgents,
    int PendingFiles,
    int FailedFiles,
    long MinimumDiskFreeBytes);

sealed record ServerMetrics(
    long CompletedTransfersTotal,
    long CompletedTransfersInWindow,
    long ErrorEventsInWindow,
    long PendingChunks,
    long FailedChunks,
    long StoredChunks,
    string? LastCompletedAt);

sealed record MetricsSnapshot(
    DateTimeOffset GeneratedAt,
    int WindowHours,
    int AutoRefreshSeconds,
    int AgentsOnline,
    int AgentsOffline,
    int PendingFiles,
    int ProcessedFiles,
    int FailedFiles,
    long MinimumDiskFreeBytes,
    long HeartbeatSamples,
    ServerMetrics Server,
    IReadOnlyList<AgentTrendPoint> Trend,
    IReadOnlyList<ObservabilityAlert> Alerts);

sealed class ObservabilityService(
    DashboardStore dashboardStore,
    ServerReadModel server,
    DashboardSettings settings)
{
    public HealthSnapshot GetHealth()
    {
        var components = new List<HealthComponent> { dashboardStore.GetHealthComponent() };
        components.AddRange(server.GetHealthComponents());

        var agents = dashboardStore.GetAgents(settings.AgentOfflineAfterSeconds);
        components.Add(agents.Count == 0
            ? new("agents", "degraded", "Nenhum heartbeat de Agent foi recebido.")
            : agents.Any(agent => !agent.IsOnline)
                ? new("agents", "degraded", $"{agents.Count(agent => !agent.IsOnline)} Agent(s) offline.")
                : new("agents", "healthy", "Todos os Agents conhecidos estao reportando."));

        var status = components.Any(component => component.Status == "unhealthy")
            ? "unhealthy"
            : components.Any(component => component.Status == "degraded")
                ? "degraded"
                : "healthy";
        return new(DateTimeOffset.UtcNow, status, components);
    }

    public MetricsSnapshot GetMetrics(int? requestedWindowHours)
    {
        var windowHours = Math.Clamp(requestedWindowHours ?? settings.MetricsWindowHours, 1, 168);
        var agents = dashboardStore.GetAgents(settings.AgentOfflineAfterSeconds);
        var diskValues = agents.Select(agent => agent.DiskFreeBytes).Where(bytes => bytes >= 0).ToList();
        return new(
            DateTimeOffset.UtcNow,
            windowHours,
            settings.AutoRefreshSeconds,
            agents.Count(agent => agent.IsOnline),
            agents.Count(agent => !agent.IsOnline),
            agents.Sum(agent => agent.PendingFiles),
            agents.Sum(agent => agent.ProcessedFiles),
            agents.Sum(agent => agent.FailedFiles),
            diskValues.Count == 0 ? 0 : diskValues.Min(),
            dashboardStore.CountHeartbeats(windowHours),
            server.GetMetrics(windowHours),
            dashboardStore.GetTrend(windowHours, settings.MetricsBucketMinutes),
            dashboardStore.GetAlerts(settings.AgentOfflineAfterSeconds, settings.LowDiskWarningBytes, 50));
    }
}
