---
description: Run the two-stage PR review (Claude-Revisor pre-flight + Codex-Revisor adversarial). Pass the PR number or omit to use the current branch.
argument-hint: [<PR-number>]
---

User passed: `$ARGUMENTS`

Run the PR review pipeline.

Steps:

1. Resolve target:
   - If `$ARGUMENTS` is a number, fetch the PR diff: `gh pr diff $ARGUMENTS` and extract the linked RFC from the PR body.
   - If empty, run against the current branch's diff vs `main`: `git diff main...HEAD`. Try to infer the RFC from branch name (`impl/rfc-NNNN-*` convention) or commit messages.
   - If no RFC can be resolved, ask the user explicitly.

2. **Stage 1 — Claude-Revisor (local pre-flight).**
   - Spawn the `revisor` subagent.
   - Input: the diff + the RFC file.
   - Output: report with Blockers / Warnings / Notes sections.
   - If Blockers exist, surface them prominently and ask the user whether to continue to Stage 2 anyway.

3. **Stage 2 — Codex-Revisor (adversarial).**
   - Invoke Codex via the `codex:rescue` skill with this brief:
     - You are Codex-Revisor.
     - The PR diff is attached.
     - The RFC the PR claims to implement is attached.
     - Your job:
       1. Identify any deviation from the RFC (extra changes, missing changes, reinterpretation).
       2. Find latent bugs, race conditions, missing error paths, untested branches.
       3. Verify §8 RFC test requirements are all present in the diff.
       4. Check that the wire protocol version bumps and migration code match what the RFC §6 promised.
     - Output: numbered list. Each item: severity (blocker / important / nit), file:line, one-sentence concern, suggested change.

4. Aggregate the two reports.

5. If a PR number was supplied, post the aggregated review as a comment via `gh pr comment $ARGUMENTS --body-file ...`. Otherwise print to chat for the user to paste manually.

6. Report to the user:
   - Total blockers across both reviews.
   - Whether the PR is ready for human merge approval (no blockers + CI green).
   - Next action: "If blockers exist, fix them in the same branch and re-run `/pr-review`. If clean, request human review for merge."
