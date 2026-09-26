# Changelog

All notable changes to this project. The day-by-day engineering log with
measured numbers lives in [DEV_STATE.md](DEV_STATE.md); this file tracks
user-visible milestones.

## v0.1-demo — 2026-09-25 (M13 v6)

First public demo build.

### Added
- Vulkan implicit layer (x64 + x86) intercepting `vkQueuePresentKHR`,
  gated by `ENABLE_NR_LAYER=1`, with hotkeys `CTRL+ALT+X` (pause/resume)
  and `CTRL+ALT+Q` (unload).
- DX9/10/11 support via per-game DXVK deployment; DX12 support via a custom
  `dxgi.dll` proxy (vtable hook on `Present`, `M12_LIVE=N` frame pacing).
- Frame daemon `m11d` (TCP 127.0.0.1:47990): 16-byte header + BGRA
  protocol, masked-UI passthrough, alpha preserved, NRCT control channel
  (`STATUS` / `SETGAIN` / `SETBLEND`), `--selftest` golden validation.
- Screen/window mode (`m8blive`): DDA capture → 71-block chain → echo-cancel
  compose → click-through Vulkan overlay; live knob file, no restart needed.
- DLSS5 Manager (M13): auto-setup (weights find, layer registration, daemon
  start), game scanner (Steam/Epic/GOG + all-drive PE-based API detection),
  per-game deploy/launch with backup/restore and one-time UAC for
  Program Files, two live knobs (gain 0–16, blend 0–1) in the header and in
  the in-game overlay, photo mode (frozen raw frame + live reprocess),
  EN/RU UI, console-free VBS launcher.
- Interactive before/after comparison page (`docs/compare.html`) with 7
  captured GTA IV pairs (menu, 4 loading screens, 2 in-game).

### Known limitations
- Performance: GEMM-bound chain (~135 ms @ 500×500, ~0.9–1.1 s/frame at
  1080p-class) — a slideshow; the #1 open problem.
- Screen/window mode is slow (tens of seconds for frame 0 at 1440p) and
  rough around the edges.
- In-game overlay works partially: mouse capture/release conflicts with some
  games; exclusive fullscreen may minimize the game.
- Daemon and screen mode are mutually exclusive at 16 GB VRAM (enforced by
  the manager).
