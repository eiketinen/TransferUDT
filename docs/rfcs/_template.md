---
id: NNNN
title: "<short noun phrase>"
status: draft         # draft | proposed | under-review | approved | rejected | superseded
authors: ["<github-handle>"]
created: YYYY-MM-DD
updated: YYYY-MM-DD
supersedes: []        # list of RFC IDs this replaces, if any
superseded_by: null   # RFC ID that replaces this, once superseded
related_adrs: []      # filled in when ADR is written
---

# RFC-NNNN — <title>

## 1. Problem

What concrete situation forces a decision now? Who is affected? What breaks
if nothing changes? Cite the issue or incident that triggered this RFC.

## 2. Background

Existing constraints from the codebase, protocol, security model, or
operational reality that any solution must respect. Link to files,
existing ADRs, docs. Do not re-explain things already in `docs/architecture.md`
— link to them.

## 3. Goals and non-goals

- Goal 1
- Goal 2

**Non-goals:**

- Non-goal 1 (explicitly out of scope)

## 4. Proposed design

Filled by Claude-Arquiteto via `/rfc-propose`. Must include:

### 4.1 Data model changes

Schema delta. New tables, columns, indexes. Migration shape.

### 4.2 API / wire protocol changes

External surface: config keys, command-line flags, wire packet format,
HTTP/gRPC endpoints. Backwards compatibility statement.

### 4.3 Code structure

Which modules/files change. New classes. New dependencies. Threading.

### 4.4 Security model

What new authentication, authorization, validation, secrets, or
trust boundary appears. How it composes with the existing PSK/HMAC model.

### 4.5 Observability

Logs, metrics, error paths. What an operator sees when this works and
when it fails.

## 5. Alternatives considered

At least two, with explicit trade-off. "We considered nothing else" is
not acceptable — write the rejected alternatives down.

| Alternative | Why rejected |
|---|---|
| A: ... | ... |
| B: ... | ... |

## 6. Migration / rollback

How does an existing deployment upgrade? How does it downgrade? What state
is irreversible?

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| ... | low/med/high | low/med/high | ... |

## 8. Tests required

Concrete test cases that must exist before approval. Each one names the
file and a one-line assertion.

- `tests/X.cpp` — assertion A
- `tests/X.cpp` — rejection of bad input B

## 9. Critique (Codex-Crítico)

Filled by `/rfc-critique`. Codex pushes back here. Each critique is a
numbered concern that the consolidation step must address.

1. ...
2. ...

## 10. Consolidation (Claude-Arquiteto)

Filled by `/rfc-consolidate`. For each numbered critique, either:

- **Accepted**: revised section ref + summary of the change made above.
- **Rejected**: explicit reasoning why the critique does not apply.

No "noted" / "will consider" hand-waves. Each item is closed.

## 11. Approval

Filled by the human reviewer. Sets `status: approved` in frontmatter and
optionally adds rationale here.

- Approver:
- Date:
- Notes:
