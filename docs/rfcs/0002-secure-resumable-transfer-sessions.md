---
id: 0002
title: "Secure resumable transfer sessions"
status: approved
authors: ["eiketinen", "codex"]
created: 2026-07-09
updated: 2026-07-09
supersedes: []
superseded_by: null
related_adrs: []
---

# RFC-0002: Secure resumable transfer sessions

## Problem

TransferUDT authenticated peers and encrypted payloads, but a live connection did
not have a cryptographically bound session identity or a monotonic message
sequence. A valid encrypted packet could therefore be replayed or delivered out
of order without a protocol-level rejection. Chunk persistence also lacked a
stable transfer identity, making retries after disconnects or process crashes
susceptible to duplicate chunks, ambiguous partial state, or a full restart.

## Background

The protocol uses a custom TCP transport, AES-GCM packets, and either a
pre-shared-key handshake or signed ephemeral X25519 key agreement. Agent and
Server persist chunk state in SQLite and reconstruct the destination file after
all chunks are accepted. This RFC records the coordinated protocol and storage
changes introduced for authenticated sessions and crash-safe resume.

## Goals

- Bind every encrypted packet to a unique authenticated connection session.
- Reject replayed, cross-session, and out-of-order packets before processing.
- Give every file transfer a deterministic identity shared by Agent and Server.
- Make chunk acceptance idempotent and resume from the first missing chunk.
- Recover partial transfers after network failures and process restarts.
- Preserve enough state and logging to diagnose recovery behavior.

## Non-goals

- Managing a public-key infrastructure or certificate lifecycle.
- Replacing the existing PSK and signed-handshake authentication modes with mTLS.
- Supporting mixed protocol versions during a rolling upgrade.
- Resuming from arbitrary byte offsets inside a chunk.

## Design

### Authenticated session and packet sequence

After peer authentication, both sides derive the session secret using the
selected handshake mode. The handshake also establishes a fresh 16-byte random
session identifier. The identifier is authenticated as part of the handshake and
encoded as 32 hexadecimal characters for diagnostics.

Each traffic direction owns an independent unsigned 64-bit sequence beginning at
zero. The session identifier, direction, sequence, packet type, and framing
metadata are included in AES-GCM additional authenticated data. A receiver accepts
only the exact next sequence for its direction. Authentication failure, a foreign
session identifier, replay, or a sequence gap terminates the connection.

The PSK mode derives independent session material from the authenticated
challenge-response exchange. The signed mode authenticates ephemeral X25519 keys
and derives session material with HKDF. The session identifier is covered by the
same authentication transcript in both modes.

### Stable transfer identity

Every file is assigned a `transfer_id`, represented on the wire as a 64-character
lowercase SHA-256 hexadecimal value. It is calculated from stable file and chunk
metadata so a retry of the same transfer resolves to the same identity, while a
different payload cannot reuse prior progress accidentally.

Every chunk packet carries the `transfer_id`. Parsers reject malformed identifiers
before database access. Agent chunks, Server chunks, and reconstructed-file rows
persist the identifier and use it when querying progress or determining whether a
completed transfer is already present.

### Idempotent resume

The Server records accepted chunks transactionally and treats an already stored
chunk for the same transfer and index as an idempotent retry. It reports the first
missing chunk so the Agent can continue without retransmitting confirmed data.
When the completed transfer already exists, the Server acknowledges completion
without reconstructing or publishing the file a second time.

The Agent persists per-transfer progress and retains a partial transfer across
connection failures. Its connection pool reconnects and resumes from the Server's
authoritative first-missing-chunk response. A new file or mismatched transfer
identity starts at chunk zero.

### Crash recovery and persistence

SQLite migrations add `transfer_id` to Agent chunks, Server chunks, and
reconstructed files. Existing rows receive compatible defaults or are backfilled
before the new constraints are used. On startup, work left in `processing` is
changed to a recoverable failed/pending state instead of being considered active
forever.

Chunk files remain on disk until reconstruction commits. Database state and file
publication are ordered so a restart can detect the last durable point and retry
without silently marking an incomplete file as complete.

### Code ownership

- `TransferUDT.TransferCore`: secure packet framing, handshake transcript,
  sequence validation, chunk parsing, and transfer metadata.
- `TransferUDT.Agent`: local database migration, file processing state, resume
  requests, and reconnect behavior.
- `TransferUDT.Server`: server migration, chunk acceptance, reconstruction, and
  client-session enforcement.

### Observability

Logs identify the connection session, transfer identifier, accepted/rejected
sequence, first missing chunk, retry decision, and recovery transition. Secrets,
raw keys, authentication tags, and plaintext file contents are never logged.

## Compatibility and rollout

The wire format is intentionally version-coupled: Agent and Server must be
upgraded together. Deployment stops old peers, backs up both SQLite databases,
installs the matching binaries, and starts the Server before Agents. Existing
partial transfers are either migrated with a valid identifier or restarted from
chunk zero.

Rollback requires stopping both peers, restoring matching old binaries and the
pre-migration database backups, and then restarting both sides. A mixed-version
rollback is unsupported.

## Security considerations

- The monotonic sequence is per session and per direction; it is never reused
  with the same session key.
- Sequence exhaustion closes the connection before wraparound.
- AES-GCM authenticates framing and routing metadata as additional data.
- Invalid transfer identifiers and sequence violations fail closed.
- Resume decisions are scoped to the authenticated peer and transfer identity.
- Database parameters are bound values; identifiers are not concatenated into
  SQL or filesystem paths.

## Alternatives considered

### Track only previously seen nonces

This would require an unbounded or evicting nonce set and would not reject
out-of-order delivery deterministically. A monotonic sequence is simpler and
provides a strict ordering contract.

### Restart every interrupted file

This avoids resume state but wastes bandwidth and time for large industrial
transfers, and it does not solve duplicate completion side effects.

### Require mTLS immediately

mTLS offers strong machine identity but introduces certificate issuance,
rotation, revocation, and trust-store operations. It remains a future transport
profile; the current design strengthens both supported authentication modes
without requiring a PKI.

## Risks and mitigations

- **Mixed versions:** coordinated deployment is mandatory and documented.
- **Legitimate retry rejected as replay:** retries reconnect and negotiate a new
  session, then resume by transfer identity rather than replaying packets.
- **SQLite migration failure:** migrations run before listeners/workers start and
  deployment retains a database backup.
- **Hashing cost:** hashing is linear and performed once per prepared transfer;
  persisted identity is reused on retry.
- **Partial disk state:** reconstruction is repeatable and completion is committed
  only after the final output is durable.

## Validation requirements

- `ChunkPacketParserTests` rejects malformed transfer identifiers.
- `FileReceiverTests` proves completed-transfer idempotency and restart from chunk
  zero when transfer identity changes.
- `SecureTransferIntegrationTests` proves replay and out-of-order rejection.
- `SecurityHandshakeTests` proves both peers authenticate the same fresh session.
- The production-readiness E2E scenario interrupts the Agent, restarts it, resumes
  the transfer, validates the output hash, and proves a single completion.
- Agent and Server Release suites pass for x64 and x86 packaging.

## Codex critique

The strict sequence contract increases security but makes application-level
packet retry on the same connection invalid; reconnect and transfer-level resume
must remain the only retry path. Crash consistency spans SQLite and the filesystem,
so tests must interrupt the process at realistic boundaries rather than only mock
errors. Full-file SHA-256 can affect very large files and should be measured before
adding content-defined identifiers elsewhere. Finally, the coordinated wire change
needs an explicit deployment guard because protocol negotiation is not included.

## Consolidation

The accepted design addresses the critique by making reconnect-and-resume an
explicit invariant, adding process-restart E2E coverage, persisting the computed
identity, and documenting coordinated deployment and rollback. Protocol version
negotiation and mTLS remain separate RFC candidates so they do not weaken or delay
the current replay and recovery controls.

## Approval

- Approver: eiketinen (repository owner)
- Date: 2026-07-09
- Notes: Approved in the implementation conversation as the retrospective design
  record for PR #15.
