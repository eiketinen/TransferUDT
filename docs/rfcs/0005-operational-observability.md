---
id: 0005
title: "Operational observability and health model"
status: approved
authors: ["eiketinen", "codex"]
created: 2026-07-10
updated: 2026-07-11
supersedes: []
superseded_by: null
related_adrs: []
---

# RFC-0005: Operational observability and health model

## Problem

The Dashboard exposes current Agent heartbeats, Server counters, transfers, and
logs, but operators cannot distinguish liveness from readiness, inspect recent
trends, or quantify backlog and failures over an operational time window. The
heartbeat history also grows without a retention policy.

## Decision

Add three complementary observability surfaces:

- `GET /health/live`, public and minimal, proves only that the Dashboard process
  can serve HTTP. It exposes no paths, identities, or infrastructure details.
- `GET /api/health`, authenticated, evaluates the Dashboard database, Server
  database and log access, and Agent heartbeat freshness. It returns `healthy`,
  `degraded`, or `unhealthy` with component-level reasons.
- `GET /api/metrics`, authenticated, returns current Agent totals, Server
  transfer/error counters for a bounded window, alerts, and bucketed heartbeat
  trends.

The Dashboard UI consumes these APIs, refreshes automatically at a configurable
interval, and provides log source, level, and text filters. Historical heartbeat
rows are removed after a configurable retention period while the latest sample
for each Agent remains available.

## Configuration

```json
{
  "Dashboard": {
    "AgentOfflineAfterSeconds": 60,
    "HeartbeatRetentionDays": 30,
    "MetricsWindowHours": 24,
    "MetricsBucketMinutes": 60,
    "LowDiskWarningBytes": 5368709120,
    "AutoRefreshSeconds": 30
  }
}
```

Values are normalized to bounded operational ranges. Metrics and detailed
health remain behind operator authentication. Only liveness is public.

## Metric semantics

- Agent counters are the latest signed heartbeat values.
- Agent pending counts use distinct file paths instead of chunk totals;
  processed and failed counts come from persisted file states in the same
  database snapshot.
- `lastTransferAt` is the latest persisted completion time in UTC, or `null`
  before the first successful completion.
- Trend points use the latest heartbeat per Agent in each time bucket.
- Server completion and error counters use persisted SQLite timestamps.
- Pending and failed chunk counts are current gauges, not event totals.
- Missing Server data produces `degraded`; an unavailable Dashboard database
  produces `unhealthy`.

## Security and privacy

- Public liveness contains only status, UTC timestamp, and application name.
- Detailed component messages are authenticated.
- Metrics do not expose private keys, certificate contents, PSKs, watched file
  names, reconstructed paths, or heartbeat signatures.
- Existing signed heartbeat replay and timestamp validation remains mandatory.

## Non-goals

- OpenTelemetry exporters, Prometheus scraping, or vendor-specific agents.
- Distributed tracing across UDT packets.
- Long-term analytics or billing-grade counters.
- Alert delivery to email, chat, or incident-management systems.

## Validation

- Verify public liveness before login.
- Verify detailed health remains authenticated.
- Submit signed heartbeat samples for pending and completed lifecycle states,
  then validate counters, completion time, and trends.
- Verify bounded window and bucket parameters.
- Exercise log filters and automatic UI refresh behavior.
- Build the Dashboard and regenerate x64 and x86 production packages.

## Approval

- Approver: eiketinen (repository owner)
- Date: 2026-07-10
- Notes: Approved by the request to improve section 4, Observability.
