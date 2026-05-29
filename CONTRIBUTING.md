# Contributing

## Development Flow

Non-trivial changes follow the multi-agent review pipeline in
[`docs/dev-workflow.md`](docs/dev-workflow.md):

```
Issue → RFC → Claude-Arquiteto proposes → Codex-Crítico critiques →
  Claude-Arquiteto consolidates → Human approves design →
  Claude-Implementador codes → produce x86+x64 prod build →
  Codex-Revisor reviews PR → CI/CD validates → Human approves merge → ADR
```

1. Open an issue describing the problem.
2. For non-trivial changes, scaffold an RFC: `/rfc-new <slug>` (see
   [`docs/rfcs/`](docs/rfcs/)). Walk it through `/rfc-propose`,
   `/rfc-critique`, `/rfc-consolidate`.
3. Get human design approval (RFC frontmatter `status: approved`).
4. `/impl-code <NNNN>` to branch and code.
5. **Regenerate the production build for both x86 and x64** (`/prod-build`).
6. `/pr-review <PR>` before requesting merge.
7. After merge, record the decision as an ADR in [`docs/adrs/`](docs/adrs/).

Trivial changes (doc-only, dependency bump, isolated bug fix) skip the RFC via
the `no-rfc` PR label. Build-irrelevant changes skip the release gate via the
`no-build` label. Security hotfixes may merge ahead of an RFC but owe a
retro-RFC within one week.

Keep changes scoped, branch from `main`, and run both C++ test suites before
opening a PR. Do not commit build outputs, databases, logs, local configs, or
secrets.

## Build and Test

```powershell
msbuild ServerUDTC++_v3\ServerUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
.\ServerUDTC++_v3\tests\bin\Debug\ServerUDTC++Tests.exe

msbuild AgentUDTC++_v7.2\AgentUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
& "$env:LOCALAPPDATA\AgentUDTCppTests\bin\Debug\AgentUDTC++Tests.exe"
```

## Production Build (required for functional changes)

This project ships a packaged release for both architectures. Regenerate both
before review (see `docs/dev-workflow.md` → "Production build"):

```powershell
.\scripts\New-TransferUDTRelease.ps1 -Version 1.0.0 -Configuration Release -Architecture x64 -Build -RunTests
.\scripts\New-TransferUDTRelease.ps1 -Version 1.0.0 -Configuration Release -Architecture x86 -Build
```

## Pull Request Checklist

- PR body references the RFC it implements (`RFC-NNNN`), or carries `no-rfc`.
- Both C++ test suites pass locally.
- Dual-arch production build regenerated for functional changes (or `no-build`).
- `/pr-review` ran and findings are addressed.
- Public docs are updated when behavior or configuration changes.
- Security-sensitive changes include negative tests.
- No secrets, logs, databases, binaries, or local paths are added.

