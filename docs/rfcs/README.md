# RFCs

Living design documents for non-trivial changes. See `docs/dev-workflow.md`
for when an RFC is required and how it moves through the pipeline.

## How to start

```
/rfc-new <short-slug>
```

The command scaffolds a numbered file from `_template.md`.

## Index

| ID | Title | Status | Authors |
|---|---|---|---|
| [0001](0001-multi-agent-workflow.md) | Multi-agent review workflow | approved | claude, codex |

## Status definitions

- **draft** — author is still writing
- **proposed** — ready for `/rfc-critique`
- **under-review** — Codex critique landed; awaiting consolidation or human approval
- **approved** — human-approved; ready for `/impl-code`
- **rejected** — closed without implementation; retained for institutional memory
- **superseded** — replaced by a later RFC (`superseded_by` populated)
