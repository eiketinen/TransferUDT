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
- private keys or certificate bundles;
- production `config.properties`;
- runtime databases;
- logs containing hostnames, paths, user names, or transfer metadata.

Use `config.example.properties` for public examples.

## Current Threat Model

TransferUDT currently supports:

- AES-256-GCM packet confidentiality and integrity;
- PSK challenge-response authentication with HMAC-SHA256;
- optional per-client PSKs for authenticated client identities;
- rejection of plaintext packet transfer when secure mode is enabled.

Known limitations:

- PSK distribution and rotation are manual.
- There is no certificate identity model yet.
- Replay protection is limited to application-level chunk/idempotency behavior.
- mTLS is not implemented.

Recommended production hardening:

- rotate PSKs periodically;
- store PSKs outside repository files;
- isolate Server access with firewall rules;
- monitor failed handshakes;
- add mTLS or a certificate-backed identity layer before internet exposure.
