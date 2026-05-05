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

`AgentUDTC++_v7.2` watches configured directories, chunks files, tracks send state in SQLite, and sends chunks to one or more Server endpoints.

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
