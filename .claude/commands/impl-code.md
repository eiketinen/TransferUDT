---
description: Invoke Claude-Implementador to code an approved RFC. Creates a branch, commits, and runs tests. Pass the 4-digit RFC id.
argument-hint: <NNNN>
---

User passed: `$ARGUMENTS`

Invoke `implementador` to code RFC `$ARGUMENTS`.

Steps:

1. Validate `$ARGUMENTS` is a 4-digit number.

2. Locate the RFC. Verify `status: approved`. If not, refuse with the exact message:
   "RFC-NNNN is not approved (status: <X>). Implementation is gated on human approval per ADR-0001."

3. Verify git state is clean (`git status --short` returns empty). If not, ask the user to commit or stash first.

4. Create branch: `impl/rfc-NNNN-<slug>` from `main`. Slug is derived from the RFC filename.

5. Spawn the `implementador` subagent with this brief:
   - Target RFC: the full file path.
   - You may modify code, tests, config examples, and docs.
   - You may NOT modify the RFC itself (it is the contract).
   - For each file you touch, the change must trace back to a specific RFC section.
   - Stop and surface ambiguity if any RFC instruction has more than one valid interpretation.
   - After coding, run the full test suite for whichever side(s) you touched:
     - Server tests: `msbuild ServerUDTC++_v3\ServerUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m` then `.\ServerUDTC++_v3\tests\bin\Debug\ServerUDTC++Tests.exe`
     - Agent tests: `msbuild AgentUDTC++_v7.2\AgentUDTC++Tests.vcxproj /p:Configuration=Debug /p:Platform=x64 /m` then `& "$env:LOCALAPPDATA\AgentUDTCppTests\bin\Debug\AgentUDTC++Tests.exe"`

6. After the subagent returns, draft a PR body containing:
   - Reference to RFC-NNNN.
   - List of files touched with one-line rationale each.
   - Test results: X/Y passed for each suite.
   - Any ambiguity-related questions raised mid-implementation.

7. Do NOT open the PR. Surface the exact `gh pr create` command for the human to run after one final read.

8. Report to the user:
   - Branch name.
   - Test results.
   - The `gh pr create` command.
   - Next action: "Run `gh pr create ...`, then `/pr-review` on the new PR number."
