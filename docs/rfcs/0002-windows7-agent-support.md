---
id: 0002
title: "Windows 7 support for the Agent (x64 + x86)"
status: approved
authors: ["claude-arquiteto"]
created: 2026-05-29
updated: 2026-05-29
supersedes: []
superseded_by: null
related_adrs: []
---

# RFC-0002 — Windows 7 support for the Agent (x64 + x86)

## 1. Problem

The Agent fails to run on Windows 7 (both x64 and x86). The current release
binaries are produced with the VS 2022 `v143` toolset against the Windows 10
SDK with no down-level OS targeting, so the produced executables and/or their
statically-linked dependencies can reference APIs that do not exist on
Windows 7, preventing the process from starting or from running correctly.

Operationally, the Agent runs on the data-source machines, some of which are
Windows 7 SP1. The Agent must run on **every Windows from 7 SP1 onward, on
both 64-bit and 32-bit**.

## 2. Background and scope

- **In scope:** `AgentUDTC++_v7.2` (the C++ sender) and `TransferCore` (the
  shared static lib the Agent links). Both architectures: x64 and Win32 (x86).
- **Out of scope (decided with the requester):**
  - `ServerUDTC++_v3` — not required on Windows 7.
  - `DashboardWeb` — it targets **.NET 8, which does not support Windows 7 at
    all** (Microsoft's runtime refuses to start below Windows 10). Making the
    dashboard run on Win7 would require re-platforming to .NET Framework 4.8,
    explicitly out of scope here.
- **Hard constraints from the codebase:**
  - The Agent already links the **static CRT** (`/MT`, `/MTd`) — a prerequisite
    for Win7 (no UCRT redistributable needed). See `AgentUDTC++.vcxproj`.
  - The Agent's C++ code uses only Win7-available Win32 APIs (`CancelIoEx`,
    WinHTTP, winsock `InetPton`/`InetNtop`, `ReadDirectoryChangesW`,
    `GetDiskFreeSpaceExA`) — all present on Windows 7 SP1.
  - No `_WIN32_WINNT`/`WINVER`/`NTDDI_VERSION` is defined anywhere today, so the
    10.0 SDK headers expose Win8+/Win10 code paths and inline helpers.
  - Dependencies (`openssl`, `spdlog`, `fmt`, `sqlite3`, `udt`) come from vcpkg
    static triplets (`x86-windows-static`, `x64-windows-static`), built with the
    default toolset (no down-level targeting).
- **Validation constraint:** there is no Windows 7 machine in the build/CI
  environment. We can prove the binaries *compile* and statically verify their
  imports, but the *runtime* confirmation on Win7 SP1 must be done by the
  requester.

## 3. Goals and non-goals

**Goals**

- `AgentUDTC++.exe` (x64 and x86) starts and runs on Windows 7 SP1 and every
  later Windows.
- No regression on Windows 10/11.
- The change is build-configuration only; no behavioral change to the protocol
  or logic.

**Non-goals**

- Server or Dashboard on Win7.
- Windows Vista/XP support (baseline is 7 SP1).
- Re-platforming the installer for Win7 (see §6 — manual `sc.exe` install is
  the documented path for Win7 hosts; PowerShell installer hardening is a
  follow-up).

## 4. Proposed design

### 4.1 Down-level OS targeting macros

Define the Windows 7 baseline in **both** `TransferCore` and `AgentUDTC++`
(they must agree, since `<windows.h>` struct layouts and inline helpers are
gated on these macros and the two link together):

```
_WIN32_WINNT=0x0601
WINVER=0x0601
NTDDI_VERSION=0x06010000   ; NTDDI_WIN7
```

Added to `<PreprocessorDefinitions>` in every `ItemDefinitionGroup` of
`TransferCore.vcxproj` and `AgentUDTC++.vcxproj` (Debug/Release × Win32/x64).
This stops the SDK headers from emitting Win8+ inline calls and is the primary
fix.

Defining these in `TransferCore` also affects `ServerUDTC++` (which links the
same lib). This is **harmless**: the Server's code uses only Win7-available
APIs, still compiles, and its runtime target (Win10+) is unchanged — it simply
also becomes source-compatible with Win7.

### 4.2 Toolset, CRT and linker

- **Pin the toolset (hard gate).** Assert `v143` (VS 2022) in MSBuild and in
  vcpkg; reject `v145+`. MSVC STL 14.50 / VS 2026 explicitly drops Win7/8 and
  uses Win8 APIs in `system_clock`, hardening and `<filesystem>`, so a future
  toolset bump would silently break Win7. Record the validated `VCToolsVersion`
  in the release evidence; CI fails on an unreviewed toolset.
- Keep the **static CRT** (already `/MT` / `/MTd`). This removes the UCRT
  redistributable dependency but only *caps* the API surface — it does not by
  itself prove Win7 compatibility. The binary gate in §4.5 is the proof.
- **Subsystem version (corrected per critique #10):** the loader rejects a
  subsystem version *above* the running OS, not below; the `v143` default
  `6.00` already loads on Win7 `6.1`. No linker change is needed — CI just
  asserts the PE subsystem major.minor is `<= 6.01`.

### 4.3 Dependencies (vcpkg)

1. **Custom Win7 triplets.** Add `x86-windows-static-win7` and
   `x64-windows-static-win7` under a repo `triplets/` dir — the stock static
   triplet plus:
   ```cmake
   set(VCPKG_C_FLAGS   "/D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000")
   set(VCPKG_CXX_FLAGS "/D_WIN32_WINNT=0x0601 /DWINVER=0x0601 /DNTDDI_VERSION=0x06010000")
   set(VCPKG_PLATFORM_TOOLSET v143)
   ```
2. **Wire the overlay (critique #2 — was a blocker).** Repo-local triplets are
   *not* picked up by `default-triplet` alone. Add an `overlay-triplets` entry
   pointing at `triplets/` in each consuming C++ `vcpkg-configuration.json`
   (Agent, TransferCore, Agent tests) and point each project's `VcpkgTriplet`
   per platform at the `-win7` triplet. All three must change together.
3. **Pin versions (critique #3, #6).** Pin the vcpkg builtin-baseline in the
   manifests and the validated v143 MSVC minor
   (`VCPKG_PLATFORM_TOOLSET_VERSION`). Pin the resolved `openssl` (and the rest
   of the dep set) versions. Archive the vcpkg build logs / compile commands
   showing the `_WIN32_WINNT=0x0601` defines were honored for each port.
4. **`BCryptGenRandom`** (OpenSSL RNG path on Win7) is Vista+ — fine — but that
   alone does not prove the whole OpenSSL build is Win7-clean; the §4.5 binary
   gate + Win7 smoke is what proves it, not assumption.

### 4.4 Security model

Unchanged. Build-target macros only; no change to the PSK handshake, AES-GCM
envelope, or any trust boundary. OpenSSL's RNG on Win7 uses `BCryptGenRandom`
(present since Vista) — no weakening.

### 4.5 Static verification (CI) + Win7 smoke (release gate)

Static analysis on the normal runner (necessary, not sufficient — critique #7):

- Scan the **whole release directory recursively** (Agent exe + both Agent test
  exes + any sibling files), not a single binary.
- Inspect **both the normal and the delay-import tables** and the PE
  **dependent** modules; flag ordinal-only imports for manual review.
- Fail on any symbol from a named post-Win7 blocklist, at minimum:
  `GetSystemTimePreciseAsFileTime` (Win8), `PathCchCanonicalizeEx` (Win8),
  `SetThreadDescription` (Win10 1607), and `api-ms-win-*` outside the Win7 set.
- Assert PE subsystem major.minor `<= 6.01`.
- Caveat: static import scanning cannot see `LoadLibrary`/`GetProcAddress`
  dynamic dispatch inside MSVC/STL/vcpkg code, so it is a screen, not a proof.

**Release gate (the actual proof):** real **Windows 7 SP1 smoke** — the Agent
starts and completes a transfer — on both x64 and x86, performed by the
requester. The static checks gate the CI; the Win7 smoke gates the release.

## 5. Alternatives considered

| Alternative | Why rejected |
|---|---|
| A: App macros only, keep default-triplet deps | Cheapest, and likely works — but a single post-Win7 import in a dep would fail only on the user's Win7 box, with a slow feedback loop. Kept as the contingency (§4.3.2), not the default. |
| B: Switch toolset to `v141_xp` / older | XP toolset is for XP/2003 and is deprecated; it does not match "Win7 SP1 baseline" and complicates the modern build. Unnecessary — `v143` targets Win7 fine with the macros. |
| C: Dynamic CRT + ship VC++ redist for Win7 | Adds a redistributable dependency and a Win7-compatible redist version pin. The project already uses the static CRT, which is simpler and self-contained. |
| D: Re-platform/Build everything (incl. Server/Dashboard) for Win7 | Out of scope; Dashboard (.NET 8) cannot run on Win7 regardless. |

## 6. Migration / rollback

**Forward:** additive build-config change. The Win7-targeted Agent still builds
and runs on Win10/11. New custom triplets live under `triplets/`; the Agent
project references them per platform.

**Backward:** revert the `<PreprocessorDefinitions>` additions and the
`VcpkgTriplet` pointers; delete `triplets/*-win7.cmake`. No source or protocol
change to undo.

**Installer note (non-goal, documented):** the PowerShell installer
(`Install-TransferUDT.ps1`) may not run under Win7's PowerShell 2.0. For Win7
Agent hosts, document a manual `sc create RadarAgentUDTService binPath= "...\AgentUDTC++.exe"`
install until the installer is made down-level (separate RFC if needed).

**Code-signing note (critique #11):** validating the signed release on Win7 SP1
requires the SHA-2 signing support update (KB4474419) and its servicing-stack
prerequisite. Either document these KBs for signed-release usage, or state that
signature validation is out of scope for the manual `sc.exe` Agent deployment
path (the Agent binary itself runs regardless of signature-chain validation).

## 7. Risks

| Risk | Likelihood | Impact | Mitigation |
|---|---|---|---|
| A dependency still imports a post-Win7 API | Medium | High | Win7 custom triplet rebuild (§4.3.1) + dumpbin import check (§4.5) catch it at build time. |
| `_WIN32_WINNT` lowering breaks a compile in shared TransferCore | Low | Medium | Code audited: no Win8+ APIs used. CI build (x64+x86, both projects) confirms. |
| Cannot validate true runtime on Win7 in CI | High | Medium | Static import/subsystem verification on the runner; requester confirms on real Win7 SP1 x64 + x86. |
| Win7 triplet rebuild of openssl is slow / flaky in CI | Medium | Low | Cache vcpkg; the dual-arch release job already tolerates long builds. |
| Subsystem-version loader rejection | Low | Medium | Default 6.00 loads on Win7; `/SUBSYSTEM:CONSOLE,"6.01"` contingency documented. |

## 8. Tests required

- `AgentUDTC++.vcxproj` builds clean at `Debug|x64`, `Release|x64`,
  `Debug|Win32`, `Release|Win32` with the Win7 macros + `-win7` triplets (CI).
- **No regression (critique #8):** `TransferCore` **and `ServerUDTC++` build and
  the Server test suite passes** with the shared-lib macro change.
- Agent test suite passes on **both x64 and Win32** (Debug) on the runner
  (critique #9), not x64 only.
- Toolset assertion (critique #4): the build records `VCToolsVersion` and CI
  fails if the toolset is not the validated `v143`.
- **Static Win7 gate (critique #7):** recursive scan of the release dir
  (Agent + both test exes + dependents), normal **and delay** import tables,
  shows no symbol from the named post-Win7 blocklist
  (`GetSystemTimePreciseAsFileTime`, `PathCchCanonicalizeEx`,
  `SetThreadDescription`, `api-ms-win-*` outside Win7); PE subsystem `<= 6.01`.
- **`std::filesystem` validation (critique #5):** on Win7 SP1 x86 and x64, the
  Agent's filesystem ops succeed end to end — chunked `seekp`/write, directory
  creation, and path normalization in `Database.cpp`, `FileProcessor.cpp`,
  `NetworkManager.cpp`, `ChunkMetadata.h`.
- **Release gate — requester runtime check (off-CI):** `AgentUDTC++.exe` starts
  and completes a real transfer on Windows 7 SP1 **x64**, and likewise on
  Win7 SP1 **x86**. This is the authoritative proof; the static checks only
  gate CI.

## 9. Critique (Codex-Crítico)

1. **[important] Macros are necessary, not a compatibility proof** (§§4.1, 4.2,
   4.5). `_WIN32_WINNT`/`WINVER`/`NTDDI_VERSION` gate header *declarations*, not
   CRT/STL/vcpkg object-code behavior. Keep them, but add a hard binary-level
   gate and pin the exact MSVC/STL/vcpkg versions used.
2. **[BLOCKER] Repo-local triplets are not wired** (§4.3). The
   `vcpkg-configuration.json` files only set `default-triplet`; vcpkg needs
   `overlay-triplets` for a repo `triplets/` dir. Wire it for Agent, TransferCore
   and the Agent test project together.
3. **[important] Dependency/toolset minor versions unpinned** (§§4.3, 7). The
   manifests list deps without a registry baseline; vcpkg picks the latest minor
   unless `VCPKG_PLATFORM_TOOLSET_VERSION` is set. Pin the builtin baseline +
   validated v143 MSVC minor; fail CI on an unreviewed toolset.
4. **[important] Future STL drift not blocked** (§§4.2, 8). MSVC STL 14.50 /
   VS 2026 explicitly drops Win7/8 and uses Win8 APIs in `system_clock`,
   hardening, `<filesystem>`. Assert v143/VS2022 in MSBuild + vcpkg; reject
   v145+; record validated `VCToolsVersion`.
5. **[important] `std::filesystem` is a first-class risk** (§§2, 4.1, 8). Agent
   and TransferCore use it in `Database.cpp`, `RadarAgent.cpp`,
   `NetworkManager.cpp`, `ChunkMetadata.h`, tests. Add explicit Win7 x86/x64
   validation of the filesystem ops used; import cleanliness varies by STL.
6. **[important] "OpenSSL believed compatible" is insufficient** (§§4.3.2, 4.4).
   Manifests don't pin the resolved OpenSSL version or prove the port honors the
   Win7 macros. `BCryptGenRandom` being Vista+ doesn't prove the whole build is
   clean. Pin deps; archive compile commands/logs showing the Win7 defines.
7. **[BLOCKER] `dumpbin /imports` blocklist too weak as sole gate** (§§4.5, 7,
   8). Misses delay imports, recursive DLL dependents, ordinal-only imports, and
   `LoadLibrary`/`GetProcAddress` dynamic dispatch. Scan the whole release dir
   recursively, include delay-import tables + dependents, block named post-Win7
   APIs (`GetSystemTimePreciseAsFileTime` Win8, `PathCchCanonicalizeEx` Win8,
   `SetThreadDescription` Win10 1607), and require real Win7 smoke as the gate.
8. **[important] TransferCore/Server boundary under-specified** (§§4.1, 8).
   TransferCore is a shared static lib with public headers including
   `windows.h`/`std::filesystem::path`; Server stays Win10+ and links it. Either
   ship an Agent-specific Win7 TransferCore variant with isolated outputs, or
   apply the macro contract to every Server+TransferCore TU and require full
   Server build/test evidence.
9. **[important] x86 + test-exe Win7 coverage incomplete** (§8). Only x64 Debug
   Agent tests run on the runner, but the test vcxproj has Win32 configs and
   links the same static libs. Build+run Agent tests for Win32; import-check both
   test exes; state whether Win7 validation must run tests or only the Agent.
10. **[minor] Subsystem-version contingency imprecise** (§§4.2, 7, 8). The
    loader rejects a subsystem version *above* the OS, not below; `6.00` already
    loads on Win7 6.1. Just assert PE subsystem `<= 6.01` in CI; drop the
    "set 6.01 if rejected" phrasing.
11. **[minor] Win7 signing prerequisites absent** (§§6, 8). The release pipeline
    signs the distribution; Win7 SP1 needs SHA-2 signing support (KB4474419 +
    servicing-stack) to validate modern signed content. Document the KB
    prerequisite or state signature validation is out of scope for the manual
    `sc.exe` Agent deployment.

## 10. Consolidation (Claude-Arquiteto)

All 11 concerns **accepted**; the critique materially improved the design.
Sections §4.2, §4.3, §4.5 and §8 were revised accordingly.

1. **Accepted.** §4 reframes the macros as *necessary-not-sufficient*; the
   binary-level gate (§4.5) + toolset/version pinning (§4.3) is the actual
   compatibility proof.
2. **Accepted (blocker).** §4.3 now wires `overlay-triplets` via each C++
   `vcpkg-configuration.json`, not just `default-triplet`, for Agent +
   TransferCore + Agent tests.
3. **Accepted.** §4.3 pins the vcpkg builtin-baseline and the validated v143
   MSVC minor (`VCPKG_PLATFORM_TOOLSET_VERSION` / `VCToolsVersion`); CI fails on
   an unreviewed toolset.
4. **Accepted.** §4.2 asserts `v143`/VS2022 in MSBuild **and** vcpkg and rejects
   v145+ (MSVC STL 14.50 / VS 2026 drops Win7); the validated `VCToolsVersion`
   is recorded in release evidence.
5. **Accepted.** §8 adds explicit Win7 x86/x64 validation of the
   `std::filesystem` operations the Agent uses (chunk write/seek, dir create,
   path normalization), pinned to the asserted STL.
6. **Accepted.** §4.3 pins the resolved OpenSSL (and all dep) versions and
   archives compile commands/logs showing the Win7 defines were honored.
7. **Accepted (blocker).** §4.5 replaces the single-exe import check with a
   recursive release-dir scan covering delay-import tables and PE dependents,
   an explicit named post-Win7 blocklist, and **real Win7 SP1 smoke as the
   release gate** (not just static analysis).
8. **Accepted.** §4.1/§8 apply the Win7 macro contract to every
   TransferCore+Server translation unit (not an isolated variant — simpler) and
   require a full Server build+test to prove no regression from the shared-lib
   macro change.
9. **Accepted.** §8 builds and runs the Agent test suite for **Win32** too, and
   import-checks both Agent test exes.
10. **Accepted.** §4.2 corrected: the loader rejects a subsystem version *above*
    the running OS, so `6.00` already loads on Win7; CI simply asserts PE
    subsystem `<= 6.01`. The "set 6.01 on rejection" contingency is removed.
11. **Accepted (documentation).** §6 documents the Win7 SHA-2 signing
    prerequisite (KB4474419 + servicing-stack) for validating the signed
    release, and states signature validation is out of scope for the manual
    `sc.exe` Agent deployment path.

## 11. Approval

- Approver: human reviewer
- Date: 2026-05-29
- Notes: Approved after Codex-Crítico round (11 concerns, all accepted and
  consolidated). Scope: Agent + TransferCore on Win7 SP1, x64 + x86. The real
  runtime proof is the requester's Win7 SP1 smoke (§4.5 release gate); CI does
  static-import + toolset gating only.
