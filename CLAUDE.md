# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project shape

Windows file-transfer system over UDT with a packaged, signed-installer
release pipeline. Five top-level components:

- `TransferCore/` — shared C++17 static lib: wire packet parser, AES-256-GCM
  envelope, PSK handshake, logger, thread pool, circuit breaker, chunk
  metadata. Linked into both C++ endpoints. (vcxproj: Win32 + x64.)
- `AgentUDTC++_v7.2/` — C++ sender. Watches directories, chunks files,
  persists send-state in SQLite, retries via `NetworkManager`. Service
  `RadarAgentUDTService` or `/debug`. (vcxproj: Win32 + x64.)
- `ServerUDTC++_v3/` — C++ receiver. Accepts UDT connections, authenticates,
  decrypts, writes chunks into pre-allocated reconstructed files via
  `seekp(chunkOffset)`, finalizes once all chunks arrive. Service
  `ServerUDTService` or `/debug`. (vcxproj: Win32 + x64.)
- `DashboardWeb/` — **.NET 8** web dashboard (`DashboardWeb.csproj`),
  published self-contained per runtime (`win-x64` / `win-x86`).
- `installer/` + `scripts/` — Inno Setup (`TransferUDT.iss`) + PowerShell
  release pipeline that produces the dual-architecture distributable.

Each C++ process ships a sibling tests project (`*Tests.vcxproj`) producing
one `*.exe` that runs every suite in process. The C++ test projects are
x64-only; the application projects are dual-platform.

## Production build — REQUIRED after every functional change

This project ships a packaged release for **both x86 and x64**. Any change
that touches buildable code (C++, Dashboard, installer, configs that affect
packaging) must end by regenerating the production build for both
architectures, matching what already exists under `dist/`.

Canonical command (run once per architecture):

```powershell
# x64
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-TransferUDTRelease.ps1 `
  -Version 1.0.0 -Configuration Release -Architecture x64 -Build -RunTests
# x86
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-TransferUDTRelease.ps1 `
  -Version 1.0.0 -Configuration Release -Architecture x86 -Build -RunTests
```

Or use the helper: `/prod-build` (see `.claude/commands/prod-build.md`).

What the release script does per architecture (via `installer/build-installer.ps1`):

1. MSBuild the Agent and Server **solutions** at `Release|<VcPlatform>`
   (`x64` → `x64`, `x86` → `Win32`).
2. `dotnet publish DashboardWeb -c Release -r win-x64|win-x86 --self-contained`.
3. Resolve the built exes and assert their PE machine matches the target
   architecture (`Assert-TransferUDTPEMachine` in `scripts/TransferUDT.Build.psm1`).
4. Run Inno Setup (`iscc`) to produce `TransferUDT-Setup-Release[-x86]-Complete.exe`.
5. Package `dist\releases\TransferUDT-<ver>-windows-{x64,x86}.zip` + `-SHA256SUMS.txt`.

Outputs (the committed release artifacts) live in `dist/releases/`:
`TransferUDT-1.0.0-windows-x64.zip`, `...-x86.zip`, and their SHA256SUMS.

Architecture mapping is centralized in `scripts/TransferUDT.Build.psm1`
(`Resolve-TransferUDTArchitecture`): x86 → VcPlatform `Win32`, RID `win-x86`,
PE machine `0x014c`; x64 → `x64`, `win-x64`, `0x8664`.

## Build & test (development loop)

vcpkg manifests in each C++ project pull `fmt`, `openssl`, `sqlite3`, `udt`,
`spdlog` (triplet `x64-windows-static`). Requires VS 2022 + vcpkg integration;
Dashboard requires the .NET 8 SDK; the installer requires Inno Setup 6 (`iscc`).

```powershell
# Build + run server tests
msbuild ServerUDTC++_v3\ServerUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
.\ServerUDTC++_v3\tests\bin\Debug\ServerUDTC++Tests.exe

# Build + run agent tests (output lives in LOCALAPPDATA, not the project dir)
msbuild AgentUDTC++_v7.2\AgentUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
& "$env:LOCALAPPDATA\AgentUDTCppTests\bin\Debug\AgentUDTC++Tests.exe"

# Dashboard
dotnet test DashboardWeb       # if a test project is present
dotnet run --project DashboardWeb
```

There is no C++ test framework. Each suite is a free function
(`runChunkPacketParserTests`, etc.) registered in `tests/TestMain.cpp` that
calls `runTest(name, lambda, stats)`. To run a single suite, edit
`TestMain.cpp` to skip the others — there is no filter flag. Failures throw
`TestFailure` from `require(...)`.

CI: `.github/workflows/windows-ci.yml` (build + both C++ test exes),
`.github/workflows/security.yml` (CodeQL + dependency review + secret scan),
`.github/workflows/rfc-check.yml` (RFC linkage gate),
`.github/workflows/release-build.yml` (dual-arch production build gate).

## Development workflow (multi-agent review)

Non-trivial changes follow the pipeline in `docs/dev-workflow.md`:

```
Issue → RFC → Claude-Arquiteto proposes → Codex-Crítico critiques →
  Claude-Arquiteto consolidates → Human approves design →
  Claude-Implementador codes → produce x86+x64 prod build →
  Codex-Revisor reviews PR → CI/CD validates → Human approves merge → ADR
```

RFCs live in `docs/rfcs/`, ADRs in `docs/adrs/`. Slash commands under
`.claude/commands/` wire each transition (`/rfc-new`, `/rfc-propose`,
`/rfc-critique`, `/rfc-consolidate`, `/impl-code`, `/prod-build`, `/pr-review`).
Subagent personas under `.claude/agents/`. The RFC gate (`rfc-check.yml`)
blocks PRs that do not link an `approved` RFC unless labelled `no-rfc`.

## Config & secrets

Neither C++ process starts without a `config.properties` next to the exe (or
pointed to by env var). Examples in `*/config.example.properties` are
secure-by-default but contain placeholder PSKs — startup intentionally fails
until you replace them. The service installer stores config under
`C:\ProgramData\TransferUDT` and sets `AGENT_CONFIG_PATH`/`SERVER_CONFIG_PATH`
per service.

- `SERVER_CONFIG_PATH` overrides the server's config path; agent uses its own default.
- PSK override hierarchy: server reads `SERVER_SECURITY_PSK` then `TRANSFERUDT_SECURITY_PSK`; agent reads `AGENT_SECURITY_PSK` then `TRANSFERUDT_SECURITY_PSK`.
- `security.enabled` (AES-GCM packets) and `security.handshake.enabled` (PSK HMAC) must both be true unless `security.allow_insecure=true` is set explicitly — `ServerConfig` constructor throws otherwise.
- PSK must be ≥ 32 chars and not equal to the placeholder string; the check is in `ServerConfig.cpp::isPlaceholderPsk`.
- `security.client_psk.<client_id>` entries bind a PSK to an identity. When any are configured, the server *only* accepts identities present in that map (see `ServerConfig::isClientIdentityAllowed`).
- Never commit `config.properties`, `*.db`, `*.log`, PSKs, certificates
  (`installer/*.pfx`, `*.key`, `*.pub`), or built binaries other than the
  intended `dist/releases/*.zip` artifacts.

## Wire protocol (one connection)

```
Agent                                        Server
  | -------- UDT connect ----------------->  |
  | <--------- AUTH_CHALLENGE_V1 <hex32> --- |   (plaintext)
  | --- AUTH_RESPONSE_V1 [client_id] hmac -> |   (plaintext)
  | <--------- READY ----------------------- |   (plaintext today)
  | --- encrypted chunk packet ------------> |   (AES-256-GCM)
  | <--- encrypted control: SUCCESS | -----  |
  |       SUCCESS_FILE_COMPLETE |            |
  |       FILE_ALREADY_EXISTS               |
  | --- encrypted CLOSE_NOW (if final) ----> |
```

Chunk wire format is defined by `ChunkPacketParser::Parse`. All length-prefixed
fields are network-byte-order. Filename and directory pass through
`IsSafeFilename` / `IsSafeRelativeDirectory` (reject control chars, drive
letters, `..`, absolute paths). The hash field carries a SHA-256 digest.

Single-chunk files use the `SUCCESS_FILE_COMPLETE` path; multi-chunk files use
repeated `SUCCESS` and only the last triggers completion.

## Server data flow

`ServerUDT::acceptClients` → `ThreadPool` → one `ClientHandler` per connection
→ `FileReceiver::receiveChunk` → SQLite + direct write into the reconstructed
path with `seekp(chunkOffset)`. A separate `tryReconstructFile` task (also via
thread pool) only validates contiguity and marks the file done — the file
content is already on disk.

Key invariants:

- The reconstructed file is pre-allocated to `totalFileSize` on the first chunk via `fs::resize_file`. Existing on-disk file + no chunk rows in DB = "externally placed", returns `AlreadyCompleted`.
- Per-file write mutex lives in `fileWriteMutexes` (never purged today — known leak).
- Per-connection replay protection: `seenSecureNonces` is an unbounded `unordered_set` per `ClientHandler`.
- Restart-resilience: `FileReceiver::loadPendingCounters` rehydrates `remainingChunks` from SQLite on construction.

## Logging idioms

`TransferCore/Logger.h` exposes two overload families that collide subtly:

- **Non-template**: `info(const std::string& tag, const std::string& message)` — string concatenation.
- **Template**: `info(const std::string& tag, fmt::format_string<Args...>, Args&&...)` — fmt-style.

`Logger::info("X", "msg {}" + value)` resolves to the **non-template** overload,
so `{}` ends up literal in the log. Always pass the format string as its own
argument: `Logger::info("X", "msg {}", value)`. Same for `error`, `warning`,
`debug`, `critical`, and their `*C` (context) variants. `redact()` strips
Windows paths and SHA-256 hex from every formatted message before writing.

Logger must be `Initialize()`d before the first `getInstance()`; main() does
this. Don't call `Logger::getInstance()` from static initializers.

## Windows path quirks

Both endpoints route filesystem access through `normalizePathForFs` (defined
twice — `ServerUDTC++_v3/FileReceiver.cpp` and `ServerUDTC++_v3/Database.cpp`),
which prepends `\\?\` (or `\\?\UNC\`) so long paths don't hit `MAX_PATH`. New
filesystem code on the server side should use this helper.

UTF-8 ↔ wchar_t conversion lives in `TransferCore/ChunkMetadata.h`
(`wideToUtf8`). `FileReceiver.cpp` has a local `utf8_to_wstring` duplicating
the same logic. Path-safety helpers (`samePathElement`, `pathStartsWith`,
`hasReparsePointInPath`, `weaklyCanonicalPath`) are duplicated verbatim between
`AgentUDTC++_v7.2/FileWatcher.cpp` and `FileProcessor.cpp` — consolidation is a
known refactor target.

## Things that look weird but are intentional

- `READY` and `AUTH_FAILED` are plaintext even after secure mode is on; only chunk packets and post-`READY` control messages are encrypted. Don't "fix" this without bumping the wire protocol version.
- Chunk hash is sent on the wire even though AES-GCM already authenticates. Belt-and-suspenders, matching the SQLite schema.
- The server's reconstructed file is written *during* chunk reception, not assembled afterwards. `tryReconstructFile` is misnamed — it only finalizes metadata.
- Singletons (`ServerConfig`, `RadarConfig`, `Database`, `Logger`) throw from their constructors on bad inputs; caught in `main()` / `ServiceMain()`. Don't suppress those.

## Pull-request expectations

- PR body references the RFC it implements (`RFC-NNNN`), or carries `no-rfc`.
- Both C++ test exes run clean locally.
- The dual-arch production build was regenerated for functional changes.
- Security-sensitive changes ship a negative test (rejection path).
- Never commit `config.properties`, `*.db`, `*.log`, PSKs, certs, or
  unintended binaries.
- Docs in `docs/` are kept in sync when behavior changes.
