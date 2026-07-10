# Security Policy

## Reporting Vulnerabilities

Do not open a public issue for exploitable vulnerabilities. Report privately to the project maintainer or repository owner.

Include:

- affected commit or release;
- reproduction steps;
- expected and actual behavior;
- impact assessment;
- any logs or packet captures with secrets removed.

## Secret Handling

Never commit:

- real `security.psk` values;
- real `security.client_psk.<client_id>` values;
- private keys, certificate bundles, or production CRLs;
- production `config.properties`;
- runtime databases;
- logs containing hostnames, paths, user names, or transfer metadata.

Use `config.example.properties` for public examples.

## Current Threat Model

TransferUDT currently supports:

- AES-256-GCM packet confidentiality and integrity;
- PSK challenge-response authentication with HMAC-SHA256;
- optional per-client PSKs for authenticated client identities;
- signed raw-key and CA-backed X.509 mutual identity profiles;
- authenticated session IDs and strict replay/order protection;
- rejection of plaintext packet transfer when secure mode is enabled.

Known limitations:

- PSK distribution and rotation are manual.
- Certificate enrollment and renewal distribution remain manual. Local PEM CRL
  enforcement and certificate/CA hot rotation are available when explicitly
  configured.
- mTLS is not implemented.

Recommended production hardening:

- rotate PSKs periodically;
- store PSKs outside repository files;
- isolate Server access with firewall rules;
- monitor failed handshakes;
- use `certificate_handshake` with an internal CA for higher-assurance private
  deployments; add a standardized mTLS gateway if required before internet
  exposure.
- enable `security.revocation.mode=crl`, monitor CRL freshness and certificate
  expiry warnings, and stage overlapping CA bundles during issuer rollover.
