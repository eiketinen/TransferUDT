# TransferUDT

TransferUDT is a Windows C++ file transfer system built around UDT sockets. It has an Agent process that watches local directories, chunks files, and sends them to a Server process that validates, stores, and reconstructs the files.

## Official Distribution

For a packaged Windows release, use the release workflow:

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-TransferUDTRelease.ps1 -Version 1.0.0 -Configuration Release -Build -RunTests
```

The official package is generated under:

```text
dist\releases\TransferUDT-1.0.0-windows-x64
```

Product documentation:

- `docs/html/TransferUDT-1.0.0-user-guide.html`
- `docs/security.md`
- `docs/release-package.md`

## Current Security Features

- AES-256-GCM encrypted packet envelope for chunk transfer.
- PSK challenge-response handshake using HMAC-SHA256.
- Optional per-client PSK binding for authenticated Agent identities.
- Rejection of plaintext packets when secure transfer is enabled.
- Server-side limits for queued work, logical file size, and per-client storage.
- Duplicate successful chunks are ignored without decrementing completion counters.

This project is not a substitute for a full audited secure transport. For public or production deployments, review `docs/security.md` before enabling external access.

## Repository Layout

```text
TransferCore/          Shared packet, crypto, logging, threading, and metadata code
AgentUDTC++_v7.2/      Agent process, watcher, chunk sender, local DB, tests
ServerUDTC++_v3/       Server process, receiver, reconstruction, DB, tests
docs/                  Architecture, configuration, and security notes
.github/               GitHub workflows and contribution templates
```

## Prerequisites

- Windows
- Visual Studio 2022 with Desktop development with C++
- vcpkg integration
- Dependencies declared in each `vcpkg.json`: `fmt`, `openssl`, `sqlite3`, `udt`, `spdlog`

## Quick Start

Copy the example configs before running locally:

```powershell
Copy-Item AgentUDTC++_v7.2\config.example.properties AgentUDTC++_v7.2\config.properties
Copy-Item ServerUDTC++_v3\config.example.properties ServerUDTC++_v3\config.properties
```

The copied examples are intentionally secure-by-default. Replace the placeholder
PSK before starting either process, or set `TRANSFERUDT_SECURITY_PSK`,
`SERVER_SECURITY_PSK`, or `AGENT_SECURITY_PSK` in the runtime environment.

Build tests:

```powershell
msbuild ServerUDTC++_v3\ServerUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
msbuild AgentUDTC++_v7.2\AgentUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

Run tests:

```powershell
.\ServerUDTC++_v3\tests\bin\Debug\ServerUDTC++Tests.exe
& "$env:LOCALAPPDATA\AgentUDTCppTests\bin\Debug\AgentUDTC++Tests.exe"
```

Build applications:

```powershell
msbuild ServerUDTC++_v3\ServerUDTC++.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
msbuild AgentUDTC++_v7.2\AgentUDTC++.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
```

## Windows Service Installation

Use `installer\build-installer.ps1` to generate a graphical Windows installer
for Agent-only, Server-only, or combined deployments. The underlying service
installer places binaries under `C:\Program Files\TransferUDT`, stores
configuration under `C:\ProgramData\TransferUDT`, and sets
`AGENT_CONFIG_PATH`/`SERVER_CONFIG_PATH` as service-specific environment
variables so config path updates only require a service restart, not a machine
reboot.

See `docs/windows-service-installation.md` for install, update, verification,
and uninstall commands.

## Secure Mode

Recommended local secure configuration:

```properties
security.enabled = true
security.handshake.enabled = true
security.allow_insecure = false
security.client_id = agent-default
security.psk = replace-with-a-strong-shared-secret-of-32-plus-chars
security.allowed_client_ids = agent-default
security.client_psk.agent-default = replace-with-a-strong-shared-secret-of-32-plus-chars
```

For multiple Agents, set each Agent's `security.psk` to the matching Server
`security.client_psk.<client_id>` value. Never commit a real PSK, private key,
certificate bundle, database, or log file. Plaintext/insecure operation requires
`security.allow_insecure=true` and should be limited to isolated development
environments.

## License

MIT. See `LICENSE`.
