# Security Notes

## Implemented

- AES-256-GCM envelope protects chunk packet confidentiality and integrity.
- Random 12-byte nonce per encrypted packet.
- 16-byte GCM authentication tag.
- PSK challenge-response handshake using HMAC-SHA256.
- HKDF-SHA256 derives the packet encryption key from the configured PSK.
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
- Rotate PSKs after suspected compromise.
- Restrict Server network exposure with firewall rules.

## Limitations

- `security.client_id` is backed by a per-client PSK when configured, but still
  does not provide certificate-chain identity.
- There is no certificate chain validation yet.
- mTLS is not implemented.
- Replay resistance is currently limited to per-connection nonce reuse and
  application-level chunk idempotency. Cross-session replay should still be
  strengthened with authenticated transfer/session IDs persisted by the Server.

## Recommended Next Step

Add a certificate-backed mode, either by introducing a TLS-compatible transport
layer or by designing a signed client identity mechanism over the existing UDT
channel. For internet-facing deployments, treat mTLS or equivalent certificate
identity as a prerequisite.
