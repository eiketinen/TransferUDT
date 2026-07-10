# Development Workflow — Multi-Agent Review

This project uses a deterministic multi-agent review pipeline for non-trivial
changes. The goal is to make each step auditable, push back early on
under-specified work, and split synthesis (Claude) from adversarial review
(Codex) so neither role is judging its own output.

## Pipeline

```
Issue / Problem
    ↓
RFC (Request for Comments)
    ↓
Claude-Arquiteto      proposes architecture           → /rfc-propose
    ↓
Codex-Crítico         questions, finds gaps           → /rfc-critique
    ↓
Claude-Arquiteto      consolidates decision           → /rfc-consolidate
    ↓
Human                 approves design                 (RFC status: approved)
    ↓
Claude-Implementador  codifies                        → /impl-code
    ↓
Production build      x86 + x64 release + tests       → /prod-build
    ↓
Codex-Revisor         reviews the PR                  → /pr-review
    ↓
CI/CD                 validates (incl. dual-arch gate) (.github/workflows/)
    ↓
Human                 approves merge                  (PR review + merge)
    ↓
ADR (Architecture Decision Record)
```

Each arrow produces a durable artifact. The **production build** stage is not
optional for functional changes: this project ships a packaged release for
both x86 and x64 (see `dist/`), so any change to buildable code must
regenerate both architectures before review. See "Production build" below.

Each arrow is a transition that must produce a durable artifact. Verbal
agreement does not count.

## Roles

| Role | Tooling | System prompt |
|---|---|---|
| Claude-Arquiteto | `.claude/agents/arquiteto.md` | Proposes, consolidates. Synthesizes constraints into a concrete design. |
| Codex-Crítico | invoked via `codex:rescue` skill | Adversarial reviewer at design time. Looks for missing assumptions, scope creep, premature optimization, design contradictions. |
| Claude-Implementador | `.claude/agents/implementador.md` | Writes code, tests, and docs to match the approved RFC. Mechanical, not creative. |
| Codex-Revisor | invoked via `codex:rescue` skill | Adversarial reviewer at PR time. Focuses on what the implementation deviates from the RFC, latent bugs, untested paths. |
| Claude-Revisor (internal) | `.claude/agents/revisor.md` | Local pre-flight review before opening PR. Catches mechanical issues. Not a substitute for Codex-Revisor. |
| Human | CODEOWNERS + branch protection | Two distinct approvals: (1) RFC design approval, (2) PR merge approval. |

## When the pipeline applies

| Change type | RFC required? | ADR required? |
|---|---|---|
| New feature with cross-module impact | Yes | Yes (after merge) |
| Wire protocol change | Yes | Yes |
| Security model change | Yes | Yes |
| Persistence schema change | Yes | Yes |
| Refactor touching > 3 files | Recommended | Optional |
| Bug fix isolated to one module | No | No |
| Doc-only change | No | No |
| Security hotfix | Retro-RFC within 1 week | Yes |

If unsure: open the RFC. Cost of an RFC that gets quickly approved is one
template fill-in. Cost of skipping an RFC and discovering scope mid-PR is
a wasted review cycle.

## RFCs vs ADRs

- **RFC** = "we are deciding about X". Lives during the decision. Has a
  status (`draft`, `proposed`, `under-review`, `approved`, `rejected`,
  `superseded`). Captures alternatives considered and trade-offs explicitly.
- **ADR** = "we decided X". Written *after* the RFC is approved and the
  implementation lands. Short. Immutable except for status changes
  (`accepted` → `deprecated` / `superseded`). The single source of truth
  for "why did we do this".

Both live under `docs/rfcs/` and `docs/adrs/`, numbered sequentially with
zero-padded four-digit IDs.

## Slash commands

The pipeline transitions are wired as slash commands so each step is a single
explicit action with a known persona and artifact contract:

| Command | Step | Output |
|---|---|---|
| `/rfc-new <slug>` | Scaffold an RFC from the template | New file in `docs/rfcs/NNNN-<slug>.md`, status=`draft` |
| `/rfc-propose <NNNN>` | Claude-Arquiteto fills the design sections | Status → `proposed` |
| `/rfc-critique <NNNN>` | Codex-Crítico questions the proposal | Critique appended; status → `under-review` |
| `/rfc-consolidate <NNNN>` | Claude-Arquiteto incorporates critique, presents final design | Status remains `under-review` until human flips to `approved` |
| `/impl-code <NNNN>` | Claude-Implementador codes against the approved RFC | Branch + commits + draft PR |
| `/prod-build [ver]` | Generate the x86 + x64 Release package | Dual-arch installers + `dist/releases` zips + SHA256SUMS |
| `/pr-review <PR>` | Codex-Revisor reviews the PR against the RFC | Review comment with deviations and risks |

## Production build (x86 + x64)

TransferUDT ships a packaged Windows release for **both architectures**. The
committed reference artifacts live in `dist/releases/`
(`TransferUDT-<ver>-windows-x64.zip`, `...-x86.zip`, `*-SHA256SUMS.txt`).

Any change that touches buildable code (C++ in `TransferCore/`,
`AgentUDTC++_v7.2/`, `ServerUDTC++_v3/`; the `DashboardWeb/` .NET project; the
installer; or packaging configs) must regenerate **both** architectures before
the PR is reviewed:

```powershell
.\scripts\New-TransferUDTRelease.ps1 -Version <ver> -Configuration Release -Architecture x64 -Build -RunTests
.\scripts\New-TransferUDTRelease.ps1 -Version <ver> -Configuration Release -Architecture x86 -Build -RunTests
```

`/prod-build` wraps both invocations and verifies the artifacts. Architecture
mapping (x86 → `Win32`/`win-x86`, x64 → `x64`/`win-x64`) and PE-machine
assertions live in `scripts/TransferUDT.Build.psm1`. Per-architecture build
detail is in `installer/build-installer.ps1` and `docs/build-installer.md`.

Doc-only and test-only changes that cannot affect either binary may skip this
stage, but must state so explicitly in the PR.

## CI gates

`.github/workflows/rfc-check.yml` verifies on every PR:

- PR body links to at least one RFC (`RFC-NNNN`) unless the PR carries the
  `no-rfc` label (used for doc-only, dependency bumps, hotfixes).
- The referenced RFC file exists at `docs/rfcs/NNNN-*.md`.
- The RFC's frontmatter `status` field is `approved`.

`.github/workflows/release-build.yml` verifies on every PR that touches
buildable code (skipped when the PR carries `no-build`):

- The Release package builds for **both** x64 and x86.
- Each produced Agent/Server executable's PE machine matches its target
  architecture.
- Both `dist/releases/TransferUDT-*-windows-{x64,x86}.zip` artifacts and their
  SHA256SUMS are produced.
- No job x64, o cenario E2E `agent-restart-resume` encerra o Agent durante a
  transferencia e exige retomada com o mesmo SHA-256.

Existing CI workflows (`windows-ci.yml`, `security.yml`) continue to run
independently. Merge is blocked if any of them fail.

## Hotfix exception

Security hotfixes can skip the RFC up front but must:

1. Carry the `hotfix` label on the PR.
2. Link to a tracking issue.
3. Land a retro-RFC + ADR within one week, documenting what was changed
   and why the normal flow was bypassed.

The retro-RFC is a forcing function: if it cannot be written, the hotfix
was not understood and likely needs a follow-up.

## Anti-patterns this prevents

- Claude critiquing Claude's own design (single-perspective tunnel).
- Codex implementing without a Claude consolidation step (loses trade-off context).
- PRs with no design artifact (institutional memory loss).
- "Verbal approval in chat" decisions (not auditable).
- Scope creep mid-implementation (caught at `/rfc-critique`).
- Hotfixes that never get documented (caught by retro-RFC requirement).

## Bootstrap reference

The first artifact produced by this pipeline is the pipeline itself:

- `docs/rfcs/0001-multi-agent-workflow.md` — the RFC that adopted this flow.
- `docs/adrs/0001-adopt-multi-agent-workflow.md` — the ADR recording the decision.

Use them as worked examples when writing new RFCs/ADRs.
