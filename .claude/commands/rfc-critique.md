---
description: Invoke Codex-Crítico to adversarially review an RFC's design and append findings to §9. Pass the 4-digit RFC id.
argument-hint: <NNNN>
---

User passed: `$ARGUMENTS`

Invoke Codex as the adversarial critic for RFC `$ARGUMENTS`.

Steps:

1. Validate `$ARGUMENTS` is a 4-digit number. Reject otherwise.

2. Locate the RFC file: `docs/rfcs/NNNN-*.md`. Verify frontmatter `status: proposed`. If `draft`, instruct the user to run `/rfc-propose` first. If `under-review`, ask whether to re-critique (and if yes, append a new "Critique round 2" subsection).

3. Read the full RFC and `docs/dev-workflow.md`. Identify the key claims, alternatives rejected, and risks asserted.

4. Invoke Codex via the `codex:rescue` skill with this brief:
   - You are Codex-Crítico.
   - The full RFC text is provided.
   - Your job: find missing assumptions, unjustified rejections of alternatives, hidden risks, contradictions with existing ADRs or `CLAUDE.md`, scope creep, premature optimization, and any "we'll figure it out later" hand-waves.
   - Output: numbered list of concerns. Each concern is one paragraph. Cite RFC sections by number.
   - Refuse the critique if the RFC's §1, §2, or §4 are too thin to evaluate. Reply with which sections need more detail.

5. Append the Codex output verbatim into §9 (Critique) of the RFC.

6. Update frontmatter:
   - `status: under-review`
   - `updated: <today>`

7. Report to the user:
   - The number of concerns Codex raised.
   - Whether Codex refused to critique due to under-specification.
   - Next action: "Run `/rfc-consolidate NNNN` to have Claude-Arquiteto address each concern."
