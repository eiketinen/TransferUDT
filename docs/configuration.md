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
security.identity.mode = psk
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

For private controlled networks, `security.identity.mode=signed_handshake`
switches the authentication layer from PSK identity to signed identity plus
ephemeral X25519 key agreement. The Agent signs the handshake transcript with
its private key, the Server validates the matching public key for the
`client_id`, the Server signs its proof, and both sides derive a fresh AES-GCM
session key from the X25519 shared secret and authenticated transcript.

Agent:

```properties
security.identity.mode = signed_handshake
security.client_id = agent-default
security.client_private_key_path = C:/ProgramData/TransferUDT/Agent/keys/agent-default.key
security.server_public_key_path = C:/ProgramData/TransferUDT/Agent/keys/server.pub
```

Server:

```properties
security.identity.mode = signed_handshake
security.allowed_client_ids = agent-default
security.server_private_key_path = C:/ProgramData/TransferUDT/Server/keys/server.key
security.client_public_key.agent-default = C:/ProgramData/TransferUDT/Server/clients/agent-default.pub
```

For environments with an internal PKI, `certificate_handshake` adds mutual
X.509 chain, purpose, and SAN/CN identity validation while retaining ephemeral
X25519 session keys. It is an application-layer handshake over UDT, not TLS or
DTLS encapsulation.

Agent:

```properties
security.identity.mode = certificate_handshake
security.client_id = agent-default
security.client_private_key_path = C:/ProgramData/TransferUDT/Agent/pki/agent-default.key
security.client_certificate_path = C:/ProgramData/TransferUDT/Agent/pki/agent-default-chain.pem
security.ca_bundle_path = C:/ProgramData/TransferUDT/Agent/pki/ca-rollover.pem
security.revocation.mode = crl
security.crl_path = C:/ProgramData/TransferUDT/Agent/pki/issuers.crl.pem
security.certificate_expiry_warning_days = 30
security.server_identity = transfer-server.example.internal
```

Server:

```properties
security.identity.mode = certificate_handshake
security.allowed_client_ids = agent-default
security.server_private_key_path = C:/ProgramData/TransferUDT/Server/pki/server.key
security.server_certificate_path = C:/ProgramData/TransferUDT/Server/pki/server-chain.pem
security.ca_bundle_path = C:/ProgramData/TransferUDT/Server/pki/ca-rollover.pem
security.revocation.mode = crl
security.crl_path = C:/ProgramData/TransferUDT/Server/pki/issuers.crl.pem
security.certificate_expiry_warning_days = 30
```

The first PEM certificate in each transmitted bundle is the peer leaf; later
certificates are intermediates. The bundle is limited to 24 KiB. Leaf EKU must
be `serverAuth` or `clientAuth` for its role. `security.revocation.mode` accepts
`off` (default) or `crl`; CRL mode is fail-closed and requires a local PEM file
up to 4 MiB. CA bundles may contain old and new roots during rollover. The
certificate, key, CA, and CRL are reloaded for each new handshake, while active
sessions continue with their existing session key. Keep Windows time
synchronized and stage files with service-account read ACLs. OCSP download and
automatic enrollment remain outside the application.

## Adaptive Chunk Sizing

```properties
chunk.adaptive.enabled = false
chunk.adaptive.min_kb = 32
chunk.adaptive.max_kb = 4096
chunk.adaptive.initial_kb = 256
chunk.adaptive.target_ack_ms = 700
```

When enabled, the Agent measures each chunk send plus authenticated ACK time,
keeps an EWMA of ACK latency and throughput per Server endpoint, and chooses
the next chunk size at send time. `chunk.adaptive.initial_kb` is used before
enough telemetry exists; `chunk.adaptive.min_kb` and
`chunk.adaptive.max_kb` bound the recommendation.

Adaptive chunks are variable-sized. Non-final chunks carry `total_chunk=0`
because the final count is not known yet. The final chunk declares the actual
total count, and retry metadata stores the byte `chunk_offset` so failed chunks
can be resent correctly even when chunk sizes differ.

`chunk.adaptive.max_kb` is capped to the protocol maximum of 4 MB.

## Transfer Recovery and Retry

Each file transfer carries a stable `transfer_id` derived from its content.
The Agent persists pending and failed chunks, including their byte offsets, so
an interrupted process can resume from the local database. On Server startup,
transfers left in `processing` state are recovered through the normal retry
path. A repeated chunk for the same transfer is idempotent; a new transfer for
the same logical file must restart with chunk zero so an incomplete previous
transfer cannot be mixed with it.

The retry worker uses the configured pending interval and bounded retry count.
Jitter is added to retry delays to avoid synchronized retries from multiple
Agents. A transfer is abandoned after `max.retries.abandon` attempts and can
then be investigated or requeued through the operational workflow.

## Changed-Content Resend

```properties
# Agent
resend.changed_files.enabled = true
resend.changed_files.identity = sha256

# Server
resend.changed_files.server_policy = overwrite
```

When enabled, the Agent does not resend every repeated path blindly. After the
file is stable, it computes a content fingerprint using path, size, last write
time, and whole-file SHA-256. The same path is skipped when the fingerprint is
unchanged and sent again when the content changes.

`resend.changed_files.server_policy` controls what the Server does when a new
version starts for a logical path already reconstructed by TransferUDT:

- `overwrite`: replace the managed reconstructed output.
- `reject`: keep the existing output and tell the Agent that the file already
  exists.
- `versioned`: keep the existing output and write the new content to a suffixed
  file name.

Files that exist only on disk but are not registered in the Server database are
not overwritten by `overwrite`; this protects external files in the
reconstructed directory.

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

## Network Simulation

```properties
test.ack_delay_pattern_ms =
```

This test-only Server setting delays authenticated ACKs by chunk number. For
example, `0,0,0,800,800,0` lets e2e runs simulate latency spikes and recovery so
adaptive chunk sizing can be validated without changing OS network settings.
Leave it empty in production.

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
