---
id: 0003
title: "Certificate-backed mutual authentication"
status: approved
authors: ["eiketinen", "codex"]
created: 2026-07-10
updated: 2026-07-10
supersedes: []
superseded_by: null
related_adrs: []
---

# RFC-0003: Certificate-backed mutual authentication

## Problem

The existing `psk` and `signed_handshake` profiles encrypt transfers and
authenticate peers, but they do not validate a certificate chain. Raw public-key
distribution becomes operationally expensive as the Agent fleet grows and does
not provide issuer, validity, purpose, or identity constraints.

## Decision

Add an opt-in `certificate_handshake` identity profile to the existing UDT
application handshake. It is not TLS over UDT. Both peers send an X.509 PEM
certificate bundle, prove possession of the corresponding private key by signing
the ephemeral handshake transcript, and derive the packet key from X25519 plus
HKDF.

The Agent validates the Server bundle against `security.ca_bundle_path`, requires
server-auth purpose, and matches `security.server_identity` or the configured
endpoint host against SAN/CN. The Server validates the Agent bundle against the
same configured trust anchor, requires client-auth purpose, and matches the
claimed `security.client_id` against SAN/CN and `security.allowed_client_ids`.

Certificate bundles are size-limited before parsing. The complete transmitted
certificate representation is bound into both signatures and the session KDF.
Authentication failures close the connection before a secure session is issued.

## Configuration

Agent:

```properties
security.identity.mode = certificate_handshake
security.client_id = agent-001
security.client_private_key_path = C:/ProgramData/TransferUDT/Agent/pki/agent-001.key
security.client_certificate_path = C:/ProgramData/TransferUDT/Agent/pki/agent-001-chain.pem
security.ca_bundle_path = C:/ProgramData/TransferUDT/Agent/pki/ca.pem
security.server_identity = transfer-server.example.internal
```

Server:

```properties
security.identity.mode = certificate_handshake
security.allowed_client_ids = agent-001
security.server_private_key_path = C:/ProgramData/TransferUDT/Server/pki/server.key
security.server_certificate_path = C:/ProgramData/TransferUDT/Server/pki/server-chain.pem
security.ca_bundle_path = C:/ProgramData/TransferUDT/Server/pki/ca.pem
```

## Security properties

- CA chain, validity period, basic constraints, and OpenSSL verification purpose
  are checked.
- Peer identity is matched against IP/DNS SAN, with subject CN fallback provided
  by OpenSSL when SAN is absent.
- A valid certificate without its private key cannot complete the handshake.
- Certificate bytes, nonces, ephemeral keys, client identity, and both signatures
  are bound into the derived packet key.
- Packet replay and ordering remain enforced by RFC-0002 session sequencing.

## Non-goals

- TLS or DTLS record-layer encapsulation of the UDT socket.
- OCSP, CRL download, automatic enrollment, renewal, or key rotation.
- Supporting encrypted private-key PEM files that require interactive passwords.
- Mixed identity modes on one Server listener.

## Compatibility and rollout

The profile is opt-in. Existing `psk` and `signed_handshake` deployments retain
their wire behavior. Agent and Server must select the same identity mode. Operators
stage CA, certificate bundle, and private key files with restricted ACLs before
switching configuration, then restart Server before Agents.

Rollback changes both peers back to their previous matching identity mode and
restarts them. The previous credentials must remain available until validation of
the certificate deployment completes.

## Risks and mitigations

- **Expired or mismatched certificates:** fail closed with a non-secret diagnostic.
- **Oversized certificate chain:** reject before allocation beyond the handshake
  limit.
- **CA compromise:** trust anchors remain external files and must be rotated by
  operators; revocation automation is a follow-up RFC.
- **Clock drift:** certificate validity depends on correct Windows time; operations
  documentation requires time synchronization.
- **Mode mismatch:** distinct wire prefixes produce an immediate authentication
  failure instead of falling back to a weaker profile.

## Validation

- Generate a temporary CA plus server/client/unauthorized leaf certificates.
- Prove mutual authentication and equal derived session secrets.
- Reject a wrong CA, wrong server identity, wrong client identity, wrong EKU,
  tampered transcript, and mismatched private key.
- Run existing PSK, signed-handshake, secure packet, replay, recovery, x64, and x86
  test gates unchanged.

## Approval

- Approver: eiketinen (repository owner)
- Date: 2026-07-10
- Notes: Approved by the request to continue the security improvements after the
  secure resumable-transfer PR reached green status.
