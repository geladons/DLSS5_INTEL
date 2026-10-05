# SPDX-License-Identifier: Apache-2.0
# ============================================================================
# m13 - DLSS 5 Manager (M13 productization): stdlib-only tkinter manager for
# the validated injection paths (m11d daemon + m11 Vulkan layer, m12-dxgi
# DX12 proxy, m8b-live screen mode) on the Intel Arc Pro B50 box.
#
# Modules:
#   config.py     persistent user config (weights path, gain, games, hotkey)
#   daemonctl.py  NRCT control-channel client for m11d (runtime gain/status)
#   processes.py  detached start / tasklist check / taskkill stop
#   deploy.py     DLL deploy/undeploy (DX9 d3d9.dll-only rule!) + HKCU layer
#                 registration via winreg (reg.exe quoting gotcha avoided)
#   gamelaunch.py detached game launch with layer/proxy env, file-based
#                 pause/resume (never input injection into games)
#   screenmode.py m8b-live overlay (screen mode) start/stop
#   launcher.py   GameLauncher: guarded game launch as explicit timed steps
#   worker.py     ActionWorker action queue + WorkerSupervisor watchdog
#   cleaner.py    "Remove all traces": TraceScanner inventory + Cleaner
#                 removal (game dlls, HKCU layers, daemon, flags/logs, config)
#   logtail.py    threaded log file tails into the UI
#   overlay.py    frameless in-game control overlay (hotkey, pause, gain);
#                 in-layer mode composites the hidden panel through the
#                 Vulkan layer via shared memory, external mode is the
#                 classic on-screen toplevel
#   overlay_shm.py  Local\DLSS5_UI_<pid> shared-memory producer (ShmUI):
#                 seqlocked bitmap publish + heartbeat, input ring drain
#   overlay_win32.py  Win32 plumbing for the overlay (no-activate, cursor,
#                 foreground steal when overlay_focus is on; PrintWindow
#                 -> BGRA DIB capture for the in-layer publisher)
#   ui.py         main window (dark theme; zero I/O on the Tk thread -
#                 StateMonitor probes, ActionWorker mutates)
#   media.py      Media tab backend: EngineSession ctypes wrapper on
#                 d5c_chain.dll, Ffmpeg resolver/downloader, SavePolicy
#                 naming, MediaQueue batch
#   media_img.py  BMP/PNG codecs + preview downscale (no PIL on this host)
#   media_video.py VideoJob: ffmpeg/ffprobe pipes around EngineSession
#   ui_media.py   Media tab UI: queue listbox, before/after preview, the
#                 overlay knob set reprocessing photos live
# ============================================================================
