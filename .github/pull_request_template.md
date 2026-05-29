## RFC

<!-- Reference the RFC this PR implements, e.g. "Implements RFC-0002".
     Doc-only / dependency / hotfix PRs: add the `no-rfc` label instead. -->

Implements RFC-

## Summary

- 

## Validation

- [ ] Server tests pass
- [ ] Agent tests pass
- [ ] Dual-arch production build regenerated (x64 + x86) — or `no-build` label with reason
- [ ] Tests required by RFC §8 are present in this diff
- [ ] `/pr-review` (Claude-Revisor + Codex-Revisor) ran and findings are addressed
- [ ] Security-sensitive changes include negative tests
- [ ] CodeQL/security workflow is clean or findings are triaged
- [ ] Secure defaults remain enabled unless this PR explicitly documents a dev-only exception
- [ ] Docs/config examples updated when behavior changes

## Production build (functional changes)

<!-- Paste the x64 and x86 build/test results, or state why the build was skipped. -->

- x64:
- x86:

## Security Checklist

- [ ] No secrets, PSKs, certificates, private keys, logs, databases, or binaries are committed
- [ ] `config.properties` was not committed
- [ ] If a wire-protocol field changed, `kVersion` was bumped and migration is documented
