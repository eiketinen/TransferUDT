# Architecture

## Components

`TransferCore` contains shared code:

- packet parsing and serialization support;
- secure packet envelope;
- challenge-response handshake helper;
- chunk metadata;
- logging;
- circuit breaker;
- thread pool.

`AgentUDTC++_v7.2` watches configured directories, computes changed-content
fingerprints, chunks files, tracks send state in SQLite, and sends chunks to one
or more Server endpoints.

`ServerUDTC++_v3` accepts UDT connections on a configured bind address, filters
source IPs, authenticates clients, decrypts packets, validates metadata and
hashes, stores chunk state, and reconstructs files.

## Secure Transfer Flow

```text
Agent                         Server
  | -------- connect --------> |
  | <--- AUTH_CHALLENGE_V1 --- |
  | ---- AUTH_RESPONSE_V1 client_id hmac ---> |
  | <--------- READY --------- |
  | ---- encrypted packet ---> |
  | <-------- SUCCESS -------- |
```

For single-chunk files the Server returns `SUCCESS_FILE_COMPLETE`, then the Agent sends `CLOSE_NOW`.

## Idempotency

The Server records successful chunks in SQLite. If the same successful chunk is received again, it is acknowledged without decrementing pending counters or completing the file early.

The Agent can reprocess the same watched path when
`resend.changed_files.enabled=true` and the file fingerprint changes. The
fingerprint currently uses size, last write time, and SHA-256 content hash. The
Server then applies `resend.changed_files.server_policy`:

- `overwrite`: replace a TransferUDT-managed reconstructed output.
- `reject`: keep the existing output and report the file as already present.
- `versioned`: keep the existing output and write the new content with a
  version suffix.

The Server does not overwrite files that merely exist on disk without matching
reconstructed metadata in its SQLite database.
