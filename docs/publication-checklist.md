# Publication Checklist

Before creating the public GitHub repository:

- Confirm the repository name and owner.
- Review `LICENSE`.
- Review all files from `git add -n .`.
- Confirm no real `config.properties` files are staged.
- Confirm no databases, logs, binaries, `vcpkg_installed`, `.vs`, or `.user` files are staged.
- Confirm no real `security.psk`, `security.client_psk.<client_id>`, private key,
  certificate bundle, database, or production path is staged.
- Confirm CodeQL, dependency review, secret pattern scan, and Windows CI pass.
- Confirm public examples keep secure mode enabled and use only placeholder
  secrets.
- Replace placeholders in `README.md` if the public project name changes.
- Decide whether to keep the historical analysis files outside Git.

Suggested first commit:

```powershell
git add .
git commit -m "Initial public project structure"
```

Suggested remote setup:

```powershell
git remote add origin https://github.com/<owner>/<repo>.git
git push -u origin main
```
