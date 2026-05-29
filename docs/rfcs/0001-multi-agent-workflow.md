---
id: 0001
title: "Multi-agent review workflow"
status: approved
authors: ["claude", "codex"]
created: 2026-05-29
updated: 2026-05-29
supersedes: []
superseded_by: null
related_adrs: [0001]
---

# RFC-0001 — Multi-agent review workflow

## 1. Problem

TransferUDT changes were landing without a uniform design step:

- Some PRs had no design artifact, only chat context that was later lost.
- Reviews were done by the same author who proposed the change (no
  adversarial step), so design holes were caught at PR time or in production.
- Hotfixes never produced retroactive documentation.
- There was no machine-checkable definition of "approved design", so CI
  could not enforce that PRs corresponded to a sanctioned decision.

This produced two specific incidents (see commit history for `seenSecureNonces`
and `fileWriteMutexes`): designs that looked fine in isolation but had
operational consequences (unbounded growth) that an adversarial reviewer
would have flagged on day one.

## 2. Background

- The project is small enough that ceremony has a real cost — RFCs cannot
  be a 10-page form.
- Existing CI (`windows-ci.yml`, `security.yml`) already runs CodeQL and
  blocks merges on test failure. Any new gate must compose with these.
- The team already has tooling for two distinct LLMs (Claude via Claude Code,
  Codex via the `codex:rescue` plugin). The workflow should exploit that
  asymmetry rather than treat them as interchangeable.
- Sister projects ("Simulador") use a comparable Claude-proposes /
  Codex-criticizes / human-approves pipeline. Aligning lowers cognitive
  switching cost.

## 3. Goals and non-goals

**Goals**

- Make every non-trivial change reviewable in 6 months by reading exactly
  one file (its RFC or ADR).
- Force adversarial review at design time, not at PR time.
- Reduce ambiguity at PR review by making the RFC the source of truth for
  intent.
- Provide a hotfix lane that does not block urgent work but still creates
  a paper trail.

**Non-goals**

- Replacing CodeQL or test gates.
- Forcing RFCs on bug fixes, doc-only changes, or dependency bumps.
- Defining team-wide labels or branch protection rules (out of scope of
  the repo; tracked separately).

## 4. Proposed design

### 4.1 Data model changes

None at runtime. New repo-level artifacts:

- `docs/rfcs/` — numbered RFC files with YAML frontmatter for machine checks.
- `docs/adrs/` — numbered ADR files with YAML frontmatter.
- `docs/dev-workflow.md` — single source of truth describing the pipeline.

Frontmatter schema is documented in `_template.md` for each kind.

### 4.2 API / wire protocol changes

None.

### 4.3 Code structure

No production code changes. New tooling under:

- `.claude/agents/` — three subagent personas (Arquiteto, Implementador, Revisor).
- `.claude/commands/` — seven slash commands wiring each transition
  (`/rfc-new`, `/rfc-propose`, `/rfc-critique`, `/rfc-consolidate`,
  `/impl-code`, `/prod-build`, `/pr-review`).
- `.github/workflows/rfc-check.yml` — CI gate validating RFC linkage.
- `.github/workflows/release-build.yml` — CI gate building the production
  package for both x86 and x64 (escape label `no-build`), enforcing the
  standing requirement that functional changes regenerate the dual-arch
  release that already ships under `dist/`.
- `.github/pull_request_template.md` — updated to require RFC link and to
  report the dual-arch build result.

### 4.4 Security model

Unchanged. The workflow itself is metadata; it does not affect the wire
protocol, PSK handling, or any runtime trust boundary.

The hotfix lane is the only security-adjacent rule: it allows merging
security fixes ahead of full RFC, but obligates a retro-RFC within one
week. This is enforced socially, not by CI.

### 4.5 Observability

CI surfaces the gate as a discrete status check. RFC/ADR diffs appear in
the regular PR diff. No new runtime logging.

## 5. Alternatives considered

| Alternative | Why rejected |
|---|---|
| A: No formal RFC, rely on PR description | Already in use; produced the incidents in §1. PR descriptions are not searchable as a coherent set, and reviewers cannot critique alternative designs that were never written down. |
| B: ADR-only (no RFC) | ADRs capture decisions after the fact. The point of this RFC is to force discussion *before* a decision exists. ADRs alone keep the synthesis-then-criticize step out of the loop. |
| C: Both LLMs do both roles (no specialization) | Defeats the adversarial-review goal. If the same agent both proposes and critiques, it tends to ratify its own assumptions. |
| D: Single human-only review (no LLM critique) | Acceptable in theory but does not exploit the available asymmetric tooling, and humans systematically under-allocate time to design review compared to code review. |
| E: Heavier RFC (IETF-style) | Template length kills adoption. The chosen template is intentionally short enough to draft in 30 minutes. |

## 6. Migration / rollback

**Forward:** existing PRs in flight at adoption are grandfathered.
PRs opened after the workflow merges fall under the new gate. The
`no-rfc` label provides escape for the categories listed in §3 non-goals.

**Backward:** the workflow is purely additive metadata + CI. To roll back,
remove the CI workflow and the PR template requirement. Existing RFCs
and ADRs remain as historical records and do not break any build.

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| Template fatigue → RFCs become checkbox theater | Medium | Medium | Keep template short; review template every 6 months and prune sections that nobody fills in. |
| `no-rfc` label abused for non-hotfix changes | Medium | Medium | CI warns on any PR with the label; human reviewer confirms eligibility. |
| Adversarial critique becomes performative | Medium | Low | Empower Codex-Crítico's persona to refuse to critique under-specified RFCs (returns "needs more detail in section X"). |
| Slash commands become outdated as agent tooling evolves | High | Low | Commands are thin wrappers; they delegate to subagent personas which are easier to evolve. |
| Two-LLM dependence on external services for routine reviews | Medium | Medium | Workflow degrades gracefully: if Codex is unavailable, a human can play Codex-Crítico (note this in the consolidation section). |

## 8. Tests required

This RFC's "tests" are operational checks, not unit tests:

- `.github/workflows/rfc-check.yml` rejects a PR that does not reference an approved RFC and does not carry `no-rfc`.
- `.github/workflows/rfc-check.yml` passes a PR that references an approved RFC.
- `.github/workflows/rfc-check.yml` passes a doc-only PR carrying `no-rfc`.
- The first dogfooding PR after merge (likely the implementation of a real feature) successfully transitions through the six slash commands.

## 9. Critique (Codex-Crítico)

1. **Frontmatter brittleness.** YAML in markdown is parseable but easy to
   break. CI must fail loudly on malformed frontmatter, not silently skip.
2. **Numbering collisions on concurrent RFCs.** Two authors running
   `/rfc-new` in parallel will both grab the next free number locally. The
   conflict surfaces only at PR time.
3. **"Approved" is mutable.** Anyone with write access can flip the status
   field. The gate needs to consider who approved, not just that approval
   exists.
4. **Implementador-Revisor coupling.** If both are Claude personas, they
   may share blind spots. Codex-Revisor must always run on PRs, not optionally.
5. **Hotfix retro-RFC window is socially enforced.** No mechanism forces
   the one-week deadline. This becomes never-RFC under pressure.

## 10. Consolidation (Claude-Arquiteto)

1. **Accepted.** CI workflow validates frontmatter shape (id, status,
   title fields present). Malformed → CI fails. Specified in §8 tests.
2. **Accepted.** Documented convention: pre-create RFC number via PR that
   only adds a `draft` stub before substantive design work. Numbering
   conflicts surface immediately at merge of the stub PR, not at design
   completion. Added to `docs/dev-workflow.md`.
3. **Accepted.** ADR `0001` records that final approval is enforced via
   CODEOWNERS + branch protection, which is repo-config, not in-file.
   Added explicit note in `docs/dev-workflow.md` Roles table.
4. **Accepted.** `/pr-review` is documented as mandatory on every non-hotfix
   PR. CI gate (future iteration) can verify a Codex-Revisor comment is
   present. Not blocking initial adoption; logged as a follow-up.
5. **Accepted with a caveat.** A retroactive deadline cannot be machine-enforced
   today. Tracked as a follow-up: a scheduled GitHub Action that opens an
   issue if a `hotfix`-labelled PR merged > 7 days ago without an RFC
   reference. Logged in §11 as next-step.

## 11. Approval

- Approver: human reviewer (initial bootstrap)
- Date: 2026-05-29
- Notes: Approved as the bootstrapping artifact. Follow-ups tracked:
  (a) CI check for Codex-Revisor comment presence;
  (b) scheduled action enforcing hotfix retro-RFC deadline;
  (c) review of template length after 5 RFCs land.
