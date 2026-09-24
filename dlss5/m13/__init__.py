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
#   logtail.py    threaded log file tails into the UI
#   overlay.py    frameless in-game control overlay (hotkey, pause, gain)
#   ui.py         main window (dark theme; zero I/O on the Tk thread -
#                 StateMonitor probes, ActionWorker mutates)
# ============================================================================
