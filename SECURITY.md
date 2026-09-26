# Security policy

## Anti-cheat — read this first

This project injects DLLs into games (Vulkan layer registration, DXVK files,
a `dxgi.dll` proxy). That is indistinguishable from cheat injection from an
anti-cheat point of view.

**Policy:**

- Never enable this in online games or games with kernel-level anti-cheat
  (PUBG, CS2, GTA Online, Fortnite, EA/BattleEye/EAC titles, ...).
  Single-player / offline use only.
- We will not help bypass, hide from, or troubleshoot anti-cheat systems.
  Issues and PRs about anti-cheat evasion are closed on sight.
- Deployed DLLs are plain files next to the game executable with `.bak`
  backups; the manager restores them on disable.

## Vulnerability reporting

The manager runs unsandboxed with user privileges and does DLL deployment,
process suspension (photo mode fallback) and game launching — treat it with
the trust you would give any such tool.

If you found a genuine security vulnerability (remote code execution via the
TCP daemon, path traversal in deploy, credential handling, etc.):

1. **Do not open a public issue.**
2. Open a private vulnerability report via GitHub
   (*Security → Report a vulnerability*) if enabled, or contact the
   repository owner via their profile contacts.
3. Include reproduction steps and affected component; we will respond within
   a week.

## Data & privacy

- The daemon listens on `127.0.0.1:47990` only (loopback). If you find it
  bound to anything else, that is a bug — report it.
- No telemetry, no network calls beyond the local loopback, no data
  collection.

## Legal

Model weights are NVIDIA proprietary data and are not distributed
(see [docs/weights-format.md](docs/weights-format.md)). Do not attach weight
files, dumps of them, or links to them in issues, PRs or discussions — they
will be removed.
