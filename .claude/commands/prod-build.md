---
description: Generate the production build for BOTH x86 and x64, matching dist/. Run at the end of every functional change. Optional version arg (default 1.0.0).
argument-hint: [version]
---

User passed: `$ARGUMENTS`

Produce the dual-architecture production release. This is a **required** step
after any change that touches buildable code (C++, DashboardWeb, installer, or
packaging configs), per `CLAUDE.md` and `docs/dev-workflow.md`.

Steps:

1. Resolve the version:
   - If `$ARGUMENTS` is a non-empty semantic version (`^\d+\.\d+\.\d+([-.][A-Za-z0-9]+)?$`), use it.
   - Otherwise default to `1.0.0`.

2. Preconditions (fail fast with a clear message if missing):
   - `scripts/New-TransferUDTRelease.ps1` exists. If not, the branch is not
     based on `main` — STOP and tell the user (the release tooling lives on `main`).
   - `iscc.exe` (Inno Setup 6) and the .NET 8 SDK (`dotnet`) are on PATH or in
     their standard install locations. If absent, report which one and stop.

3. Build x64:
   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-TransferUDTRelease.ps1 `
     -Version <ver> -Configuration Release -Architecture x64 -Build -RunTests
   ```
   Run in the foreground; this is long. Capture the tail of output.

4. Build x86:
   ```powershell
   powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\scripts\New-TransferUDTRelease.ps1 `
     -Version <ver> -Configuration Release -Architecture x86 -Build -RunTests
   ```

5. Verify the artifacts exist and report their SHA256:
   - `dist\releases\TransferUDT-<ver>-windows-x64.zip` + `-SHA256SUMS.txt`
   - `dist\releases\TransferUDT-<ver>-windows-x86.zip` + `-SHA256SUMS.txt`
   - The installers `TransferUDT-Setup-Release-Complete.exe` and
     `TransferUDT-Setup-Release-x86-Complete.exe` under the installer output dir.

6. Sanity-check architecture: confirm each produced Server/Agent exe's PE
   machine matches its target (the release script already asserts this via
   `Assert-TransferUDTPEMachine`; surface any mismatch it reports).

7. Report to the user:
   - Pass/fail of each architecture's build + tests.
   - The four release artifact paths with sizes and SHA256.
   - If either architecture failed, stop and surface the failing MSBuild /
     dotnet / iscc step verbatim — do NOT report partial success as success.

Notes:
- This command does not commit the artifacts. The committed release artifacts
  in `dist/releases/` are updated deliberately as part of a release, not on
  every dev iteration — confirm with the user before staging large zips.
- For a faster inner loop that skips packaging, use the per-architecture
  `installer/build-installer.ps1 -Configuration Release -Architecture x64|x86`
  directly, but the full `New-TransferUDTRelease.ps1` is the gate of record.
