# TransferUDT DashboardWeb

DashboardWeb is a read-only observability service for TransferUDT Server and
Agents. Production deployments must run it with HTTPS enabled.

## Required Production Settings

Set these values in `appsettings.json`, command-line configuration, or
environment variables:

```text
Dashboard:BindAddress=0.0.0.0
Dashboard:HttpsPort=8443
Dashboard:RequireHttps=true
Dashboard:CertificatePath=C:/ProgramData/TransferUDT/Dashboard/certs/dashboard.pfx
Dashboard:CertificatePasswordEnv=TRANSFERUDT_DASHBOARD_CERT_PASSWORD
Dashboard:OperatorPasswordEnv=TRANSFERUDT_DASHBOARD_OPERATOR_PASSWORD
Dashboard:DatabasePath=C:/TransferUDT/Dashboard/db/dashboard.db
Dashboard:ServerDatabasePath=C:/TransferUDT/Server/db/server.db
Dashboard:ServerLogPath=C:/TransferUDT/Server/log/server.log
Dashboard:AgentPublicKeys:<client_id>=C:/ProgramData/TransferUDT/Server/clients/<client_id>.pub
```

The HTTPS certificate should be issued by the internal CA and trusted by the
Windows trust store on operator workstations and Agent machines.

## Agent Heartbeat

Agents post to:

```text
POST /api/agents/heartbeat
```

Required headers:

```text
X-Client-Id: <client_id>
X-Timestamp: <unix_seconds>
X-Nonce: <random_hex>
X-Signature: <rsa_sha256_hex(timestamp + "\n" + nonce + "\n" + body)>
```

The Dashboard verifies the signature with the configured public key for that
`client_id`, rejects replayed nonces, and rejects timestamps outside the
configured skew window.

## Local Test

```powershell
dotnet build DashboardWeb\DashboardWeb.csproj
powershell.exe -ExecutionPolicy Bypass -File scripts\e2e-dashboard-https.ps1 -UseHttpForLocalTest
```

`-UseHttpForLocalTest` exists only to validate API/login/signature behavior in
local environments where temporary certificate handshakes are blocked by
Windows Schannel policy.
