---
id: 0001
title: "Adopt the multi-agent review workflow"
status: accepted
date: 2026-05-29
deciders: ["human-reviewer"]
rfc: 0001
supersedes: []
superseded_by: null
---

# ADR-0001 — Adopt the multi-agent review workflow

## Context

Changes were landing without uniform design review, producing latent
operational issues that an adversarial reviewer would have caught at design
time. The team had access to two distinct LLM toolchains (Claude, Codex)
and a sister project pattern that exploited that asymmetry.

See [RFC-0001](../rfcs/0001-multi-agent-workflow.md) for the full analysis.

## Decision

Adopt the pipeline (Issue → RFC → Claude-Arquiteto proposes →
Codex-Crítico critiques → Claude-Arquiteto consolidates → Human approves
design → Claude-Implementador codes → **produce x86+x64 production build** →
Codex-Revisor reviews PR → CI → Human approves merge → ADR), with the
following commitments:

- Every non-trivial change produces an RFC in `docs/rfcs/`.
- Every approved RFC produces an ADR in `docs/adrs/` after implementation.
- Approval is enforced by CODEOWNERS + branch protection, not by editing
  the frontmatter field alone.
- CI (`.github/workflows/rfc-check.yml`) blocks PRs that do not link an
  approved RFC, with a `no-rfc` escape label for the documented exception
  categories.
- Functional changes regenerate the production build for **both x86 and x64**
  (the dual-arch release that ships under `dist/`). CI
  (`.github/workflows/release-build.yml`) builds both architectures and
  validates the release artifacts, with a `no-build` escape label for
  build-irrelevant changes.
- Security hotfixes may merge ahead of a full RFC but must produce a
  retro-RFC within one week.

## Consequences

**Easier**

- Onboarding a new contributor: read `docs/dev-workflow.md` + recent
  RFCs/ADRs to absorb intent and history in one pass.
- Post-mortems: every decision has a single document with its alternatives
  and trade-offs recorded.
- Adversarial review: split between Claude (synthesis) and Codex (critique)
  by default; no role conflicts of interest.

**Harder**

- Trivial changes have slightly more ceremony if they happen to fall just
  above the "no RFC" threshold. The `no-rfc` label is the pressure-release
  valve.
- Initial RFC for cross-cutting refactors is non-trivial to write; this is
  intentional friction.

**Commits us to**

- Maintaining the agent personas and slash commands as tooling evolves
  (review every 6 months).
- Keeping the RFC template short. A template that grows past 2 pages will
  be rejected as scope creep on the template itself.
- Producing a retro-RFC for any hotfix, even when the urgency has passed.

## Status transitions

- 2026-05-29 — proposed (alongside RFC-0001 draft)
- 2026-05-29 — accepted (RFC-0001 approved + implementation merged)
