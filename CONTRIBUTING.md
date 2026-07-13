# Contributing to TransferUDT

## Active Branch

`main` is the only long-lived branch and represents the current integrated
code. There is no permanent `develop` or `release` branch.

Every change starts from the latest `main`, uses a short-lived descriptive
branch, and returns to `main` through a pull request after the required checks
pass.

## Development Flow

Non-trivial changes follow the multi-agent review pipeline in
[`docs/dev-workflow.md`](docs/dev-workflow.md):

```text
Issue -> RFC -> architecture proposal -> Codex critique -> consolidation ->
human design approval -> implementation -> x86/x64 production build ->
Codex PR review -> CI/CD -> human merge approval -> ADR
```

1. Open an issue describing the problem.
2. For non-trivial changes, scaffold and approve an RFC under `docs/rfcs/`.
3. Implement the approved RFC in a descriptive branch.
4. Regenerate the production build for both x86 and x64.
5. Run the PR review before requesting merge.
6. Record architectural decisions in an ADR after merge.

Trivial changes can use the existing `no-rfc` and `no-build` PR labels when
their documented criteria apply. Security hotfixes owe a retro-RFC within one
week.

## Build and Test

The Agent, Server, and TransferCore manifests must keep the same vcpkg
`builtin-baseline` and dependency list. Do not remove or advance the baseline
as a side effect of another change. Dependency updates must be explicit and
validated for both `x64-windows-static` and `x86-windows-static`.

```powershell
msbuild ServerUDTC++_v3\ServerUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
.\ServerUDTC++_v3\tests\bin\Debug\ServerUDTC++Tests.exe

msbuild AgentUDTC++_v7.2\AgentUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m
& "$env:LOCALAPPDATA\AgentUDTCppTests\bin\Debug\AgentUDTC++Tests.exe"
```

## Production Build

Functional changes regenerate and validate both architectures before review:

```powershell
.\scripts\New-TransferUDTRelease.ps1 -Version 1.0.0 -Configuration Release -Architecture x64 -Build -RunTests
.\scripts\New-TransferUDTRelease.ps1 -Version 1.0.0 -Configuration Release -Architecture x86 -Build -RunTests
```

## Branch Names

Codex-created branches use one of these forms:

- `codex/feature-<short-slug>` for new behavior.
- `codex/fix-<short-slug>` for corrections.
- `codex/security-<short-slug>` for security hardening.
- `codex/chore-<short-slug>` for CI, packaging, or repository maintenance.
- `codex/docs-<short-slug>` for documentation-only work.

Use lowercase ASCII words separated by hyphens. A branch must represent one
objective. Delete it after its pull request is merged.

## Pull Requests

1. Update the local `main` before creating the branch.
2. Keep commits scoped to the branch objective.
3. Target every normal pull request at `main`.
4. Run the applicable tests, E2E suites, documentation gate, and x64/x86
   package gate before review.
5. Merge only after required GitHub checks pass.
6. Prefer squash merge for focused changes. Preserve a multi-commit history
   only when the individual commits are operationally useful.
7. Delete the merged branch locally and remotely.

Pull request checklist:

- Reference the implemented RFC or apply the documented `no-rfc` exception.
- Pass both C++ suites and every applicable E2E suite.
- Regenerate the dual-architecture production packages when required.
- Address PR review findings.
- Synchronize public documentation and configuration examples.
- Include negative tests for security-sensitive changes.
- Commit no secrets, logs, databases, runtime configuration, or local paths.

Direct pushes to `main` are reserved for repository recovery when the normal PR
path is unavailable. The versioned `main-protection` ruleset normally blocks
them and requires an up-to-date PR with Windows CI, dual-architecture release,
security, dependency review, and RFC checks. It currently requires zero review
approvals so a single-maintainer repository is not deadlocked, while unresolved
review threads still block merge. Increase the approval count when a second
maintainer with write access is available.

Validate the desired ruleset locally with:

```powershell
.\scripts\Set-GitHubMainRuleset.ps1 -ValidateOnly
```

Applying it requires `GITHUB_TOKEN` with repository Administration write
permission. The script creates or updates the named ruleset idempotently.

## Version Tags

Tags are never created automatically by a push or merge. The repository owner
must explicitly request each version.

- Format: annotated `vMAJOR.MINOR.PATCH`, with an optional SemVer pre-release.
- Target: the current commit on `main`.
- Prerequisites: matching release notes, x64/x86 ZIPs, and checksum files are
  already committed on `main` and all required checks are green.
- Creation: run the `Create Version Tag` workflow manually and provide the
  version without the leading `v`, for example `1.1.0`.

The workflow validates package hashes and refuses duplicate or incomplete
versions before pushing the tag.
