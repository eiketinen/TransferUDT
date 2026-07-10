---
id: 0004
title: "Certificate lifecycle and CRL revocation"
status: approved
authors: ["eiketinen", "codex"]
created: 2026-07-10
updated: 2026-07-10
supersedes: []
superseded_by: null
related_adrs: []
---

# RFC-0004: Certificate lifecycle and CRL revocation

## Problem

RFC-0003 authenticates Agent and Server with CA-issued certificates, but a
compromised certificate remains usable until it expires. Certificate renewal,
CA rollover, and expiration monitoring also need an operationally safe path
that does not terminate active transfers.

## Decision

Add optional, fail-closed PEM CRL validation to `certificate_handshake`.
`security.revocation.mode=crl` requires `security.crl_path`; an absent,
malformed, oversized, expired, incorrectly signed, or applicable revoking CRL
causes peer certificate validation to fail. `off` remains the default for
backward compatibility and rejects a configured CRL path to avoid a false
expectation that revocation is active.

Certificate, private-key, CA, and CRL files are read for each new handshake.
Operators can atomically replace them without terminating established encrypted
sessions. New connections use the new material. A CA bundle may contain old and
new trust anchors during a controlled rollover. Both peers emit warnings when a
local or peer leaf certificate has no more than
`security.certificate_expiry_warning_days` remaining.

## Configuration

```properties
security.identity.mode = certificate_handshake
security.ca_bundle_path = C:/ProgramData/TransferUDT/pki/ca-rollover.pem
security.revocation.mode = crl
security.crl_path = C:/ProgramData/TransferUDT/pki/issuers.crl.pem
security.certificate_expiry_warning_days = 30
```

The CRL file may contain multiple PEM CRLs. Strict chain checking requires an
applicable CRL for every non-trust-anchor issuer in the selected certificate
chain. The file is limited to 4 MiB.

## Rotation procedure

1. Add the future CA to the existing CA bundle on Agent and Server.
2. Atomically replace the CRL bundle and verify that it covers each active
   issuing CA.
3. Atomically replace the leaf certificate and matching private key.
4. Confirm successful new handshakes and expiry/revocation telemetry.
5. Remove the retired CA only after all peers have moved to the new chain.

Existing sessions continue with their already authenticated session keys.
Revocation applies when a new connection performs a handshake.

## Security properties

- Revoked peer leaf certificates fail before a secure session is issued.
- Missing or unreadable configured CRLs fail closed.
- Old and new CAs can overlap without disabling certificate verification.
- Certificate replacement is observed on the next handshake.
- Expiration warnings contain identity and remaining days, but no key or
  certificate contents.

## Non-goals

- Downloading CRLs from certificate distribution-point URLs.
- Online OCSP validation.
- Automatic enrollment through AD CS, ACME, SCEP, or EST.
- Storing private keys in Windows CNG or hardware security modules.
- Terminating already established sessions immediately after a revocation.

## Risks and mitigations

- **Stale CRL:** operational monitoring must track CRL `nextUpdate`; malformed
  or expired CRLs are rejected by OpenSSL verification.
- **Incomplete rollover CRLs:** strict checking fails closed, so both CA and CRL
  bundles must be staged before issuing from the new CA.
- **Repeated expiry warnings:** log aggregation should deduplicate by identity
  and remaining-day window.
- **Atomicity across certificate and key files:** replace the pair within the
  same maintenance window; mismatched pairs fail the proof-of-possession step.

## Validation

- Accept an unrevoked certificate with a valid CRL.
- Reject a revoked certificate and a configured missing CRL.
- Trust certificates issued by old and new CAs during rollover.
- Replace a certificate file and prove that the next load sees the new leaf.
- Verify remaining-validity telemetry.
- Run the complete certificate, transfer, recovery, x64, and x86 gates.

## Approval

- Approver: eiketinen (repository owner)
- Date: 2026-07-10
- Notes: Approved by the request to proceed with the recommended certificate
  lifecycle improvement.
