---
description: Invoke Claude-Arquiteto in consolidation mode to respond to each Codex critique and finalize the RFC for human approval. Pass the 4-digit RFC id.
argument-hint: <NNNN>
---

User passed: `$ARGUMENTS`

Invoke `arquiteto` in consolidation mode for RFC `$ARGUMENTS`.

Steps:

1. Validate `$ARGUMENTS` is a 4-digit number.

2. Locate the RFC file. Verify:
   - Frontmatter `status: under-review`.
   - §9 (Critique) contains at least one numbered concern. If empty, refuse: "No critique to consolidate. Run `/rfc-critique` first."

3. Spawn the `arquiteto` subagent with this brief:
   - Mode B — Consolidate.
   - Target file: the resolved RFC path.
   - For each numbered concern in §9, write a numbered response in §10.
   - Each response is **Accepted** (apply a concrete change to §4–§8, summarize the diff in §10) **or Rejected** (with explicit reasoning).
   - No "noted" / "will consider" hand-waves. Each item must close.

4. Do NOT change `status` to `approved` — that is the human's gate. Leave at `under-review`.

5. Update frontmatter `updated: <today>`.

6. Report to the user:
   - Count of accepted vs rejected critiques.
   - Summary of what changed in §4–§8 as a result.
   - Next action: "Human reviewer: read the consolidated RFC. If satisfied, set `status: approved` in the frontmatter and add yourself to §11 Approval. Then `/impl-code NNNN` becomes available."
