# Configuration

Runtime configuration is intentionally not committed. Copy the examples:

```powershell
Copy-Item AgentUDTC++_v7.2\config.example.properties AgentUDTC++_v7.2\config.properties
Copy-Item ServerUDTC++_v3\config.example.properties ServerUDTC++_v3\config.properties
```

## Security

For multiple agents, configure a distinct PSK per Agent identity on the Server.
The Agent still uses `security.psk`, but that value must match its Server-side
`security.client_psk.<client_id>` entry:

```properties
security.enabled = true
security.handshake.enabled = true
security.allow_insecure = false
security.client_id = agent-default
security.psk = replace-with-a-strong-shared-secret-of-32-plus-chars
security.allowed_client_ids = agent-default
security.client_psk.agent-default = replace-with-a-strong-shared-secret-of-32-plus-chars
```

`security.enabled` controls AES-256-GCM encrypted packet transfer.

`security.handshake.enabled` controls the HMAC-SHA256 challenge-response authentication before the Server sends `READY`.

`security.allow_insecure` must remain `false` for production. If either secure
packet encryption or handshake authentication is disabled while
`security.allow_insecure=false`, startup fails.

`security.psk` can be supplied through configuration or environment variables.
Server reads `SERVER_SECURITY_PSK` first and then `TRANSFERUDT_SECURITY_PSK`.
Agent reads `AGENT_SECURITY_PSK` first and then `TRANSFERUDT_SECURITY_PSK`.
The placeholder PSK is rejected when secure mode is enabled.

`security.client_id` is sent by the Agent as part of the authenticated
challenge-response. On the Server, `security.allowed_client_ids` is a
comma-separated allowlist. When any `security.client_psk.<client_id>` entry is
configured, the Server only accepts identities present in that map and uses the
matching PSK for handshake, packet decryption, and authenticated control
messages.

## Server Resource Limits

```properties
work.max_pending_tasks = 256
server.max_file_size_mb = 10240
server.max_client_storage_mb = 20480
```

`work.max_pending_tasks` bounds accepted connection work queued behind the
worker pool. When the queue is full, the Server closes newly accepted sockets.

`server.max_file_size_mb` rejects oversized logical files before preallocation.
`server.max_client_storage_mb` bounds reconstructed storage per authenticated
client namespace.

## Server Exposure

```properties
server.bind_address = 127.0.0.1
server.allowed_clients = 127.0.0.1
```

`server.bind_address` controls the local interface used by the Server. Use
`0.0.0.0` only behind firewall/VPN controls. `server.allowed_clients` is a
comma-separated IPv4 allowlist checked immediately after accept.

## Paths

Prefer environment-specific local paths in `config.properties`. Do not commit them. The example files use safe placeholder paths.
