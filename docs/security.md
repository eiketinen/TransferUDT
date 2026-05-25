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
- Per-connection secure packet nonce replay detection.
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
- Replay resistance is currently limited to per-connection nonce reuse and
  application-level chunk idempotency. Cross-session replay should still be
  strengthened with authenticated transfer/session IDs persisted by the Server.

## Recommended Next Step

For controlled private networks, add a signed handshake over the existing UDT
channel: each Agent has a private signing key, the Server stores the matching
public key per `client_id`, the Server signs its challenge/identity, and both
sides derive per-session keys from fresh nonces plus the authenticated
transcript.

For internet-facing deployments or environments with untrusted networks, treat
mTLS or an equivalent certificate-backed identity layer as the production
baseline. A signed handshake can provide mutual identity, but it still leaves
certificate lifecycle, revocation, trust-chain validation, and transport
hardening to custom code unless those pieces are explicitly implemented.

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
