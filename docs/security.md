# Security Notes

## Implemented

- AES-256-GCM envelope protects chunk packet confidentiality and integrity.
- Random 12-byte nonce per encrypted packet.
- 16-byte GCM authentication tag.
- PSK challenge-response handshake using HMAC-SHA256.
- HKDF-SHA256 derives the packet encryption key from the configured PSK.
- Signed identity mode authenticates Agent and Server handshake transcripts with
  configured PEM signing keys and derives per-session AES-GCM secrets with
  ephemeral X25519 plus HKDF-SHA256.
- Optional authenticated `security.client_id` allowlisting.
- Server-side `security.client_psk.<client_id>` entries bind each configured
  client identity to its own PSK.
- Random authenticated `sessionId` per secure session.
- Strict directional sequence numbers shared by chunks and control messages.
- Replayed, cross-session, or out-of-order secure packets are rejected before
  chunk parsing; a sequence failure closes the connection.
- Secure mode is enabled by default and insecure operation requires an explicit
  `security.allow_insecure=true` configuration.
- Server rejects plaintext chunk packets when secure mode is enabled.
- Server rejects invalid challenge responses before sending `READY`.
- Server bind address and source IP allowlist are configurable.
- Server bounds pending worker tasks, maximum logical file size, and
  reconstructed storage per client namespace.

## Operational Guidance

- Generate a high-entropy PSK with at least 32 characters per Agent.
- Keep each Agent's `security.psk` equal to its Server-side
  `security.client_psk.<client_id>`.
- Store the PSK outside Git.
- For `security.identity.mode=signed_handshake`, generate one signing key pair
  per Agent plus one Server signing key pair. Store private keys only on their
  owning machine and distribute public keys through the deployment process.
- Restrict configuration file ACLs to Administrators and the service identity.
- Rotate PSKs after suspected compromise.
- Restrict Server network exposure with firewall rules.
- Run Agent and Server with the least Windows privileges possible for their
  configured data, log, database, and watched directories.

## Limitations

- `security.client_id` is backed by either a per-client PSK or a configured
  public signing key, depending on `security.identity.mode`, but still does not
  provide certificate-chain identity.
- There is no certificate chain validation yet.
- mTLS is not implemented.
- Replay resistance is scoped to the lifetime of the authenticated session.
  Application-level chunk idempotency remains as a second defense against
  duplicate logical chunks. Sequence state is intentionally not persisted
  across connections because a new authenticated session receives a new key
  and session identifier.

## Certificate-Based Identity

The current `signed_handshake` mode provides key-based mutual identity and
ephemeral session keys. It is not mTLS: there is no certificate-chain
validation, revocation, or TLS transport. For internet-facing deployments or
untrusted networks, use mTLS or an equivalent certificate-backed identity layer
as the production baseline.

## Signed Handshake Key Setup

Generate RSA signing keys with OpenSSL or an equivalent internal PKI process:

```powershell
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out server.key
openssl pkey -in server.key -pubout -out server.pub
openssl genpkey -algorithm RSA -pkeyopt rsa_keygen_bits:3072 -out agent-001.key
openssl pkey -in agent-001.key -pubout -out agent-001.pub
```

Install private keys only on the owner:

- Server private key: `C:\ProgramData\TransferUDT\Server\keys\server.key`
- Agent private key: `C:\ProgramData\TransferUDT\Agent\keys\agent-001.key`

Install public keys where they are needed for verification:

- Agent receives Server public key:
  `C:\ProgramData\TransferUDT\Agent\keys\server.pub`
- Server receives Agent public key:
  `C:\ProgramData\TransferUDT\Server\clients\agent-001.pub`

Restrict key directories to Administrators and the service identity.
