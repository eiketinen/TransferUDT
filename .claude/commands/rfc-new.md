---
description: Scaffold a new RFC from the template. Pass a short slug (kebab-case) describing the change.
argument-hint: <short-slug>
---

User passed: `$ARGUMENTS`

Create a new RFC under `docs/rfcs/` from the template.

Steps:

1. Validate the slug:
   - Lowercase letters, digits, hyphens only.
   - 3–50 characters.
   - Reject otherwise with a clear message.

2. Determine the next free RFC number:
   - List files in `docs/rfcs/` matching `[0-9][0-9][0-9][0-9]-*.md`.
   - Find the maximum ID, add 1. Zero-pad to 4 digits.
   - The very first user RFC will be 0002 (0001 is the bootstrap workflow RFC).

3. Read `docs/rfcs/_template.md` as the source.

4. Write `docs/rfcs/NNNN-<slug>.md` with:
   - Frontmatter `id` set to NNNN.
   - Frontmatter `title` set to a humanized version of the slug (replace hyphens with spaces, capitalize first letter).
   - Frontmatter `status: draft`.
   - Frontmatter `created` and `updated` set to today's date (YYYY-MM-DD).
   - Frontmatter `authors` left as the placeholder.
   - Body unchanged from template.

5. Update `docs/rfcs/README.md` index table by appending the new RFC row at the bottom.

6. Report to the user:
   - The path of the created file.
   - The next action: "Fill §1 (Problem) and §2 (Background) by hand, then run `/rfc-propose NNNN` so Claude-Arquiteto can fill §4–§8."

Do not invoke any subagent yet. This command only scaffolds.
