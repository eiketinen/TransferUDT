---
description: Invoke Claude-Arquiteto to fill the design sections (§4–§8) of an existing RFC. Pass the 4-digit RFC id.
argument-hint: <NNNN>
---

User passed: `$ARGUMENTS`

Invoke the `arquiteto` subagent to propose the design for RFC `$ARGUMENTS`.

Steps:

1. Validate `$ARGUMENTS` is a 4-digit number. Reject otherwise.

2. Locate the RFC file: `docs/rfcs/NNNN-*.md`. If not found, instruct the user to run `/rfc-new` first.

3. Read the file. Verify:
   - Frontmatter `status: draft`. If not, refuse: "RFC-NNNN is already at status `<X>`. Use `/rfc-consolidate` instead for an under-review RFC."
   - §1 Problem and §2 Background are non-empty (not still the template placeholder). If empty, refuse: "Fill §1 and §2 by hand before invoking Claude-Arquiteto. The pipeline does not invent context."

4. Spawn the `arquiteto` subagent with this brief:
   - Mode A — Propose.
   - Target file: the resolved RFC path.
   - Goal: fill §4 (Proposed design), §5 (Alternatives considered, ≥ 2), §6 (Migration / rollback), §7 (Risks with mitigations), §8 (Tests required, each as file:assertion).
   - Constraint: ground every claim in the codebase. Cite with file:line via Grep/Read.

5. After the subagent returns, update the RFC frontmatter:
   - `status: proposed`
   - `updated: <today>`

6. Report to the user:
   - Summary of what the arquiteto wrote (5 bullets max — the subagent already returned this).
   - Next action: "Run `/rfc-critique NNNN` to invoke Codex-Crítico."
