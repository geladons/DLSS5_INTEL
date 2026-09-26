# DLSS 5 Manager entry point - run with pythonw (no console window):
#   "%USERPROFILE%\AppData\Local\Programs\Python\Python312\pythonw.exe" m13.pyw
# or via "DLSS5 Manager.vbs" / M13.cmd next to this file.
import os
import sys

_PKG = os.path.dirname(os.path.abspath(__file__))
_DLSS5 = os.path.dirname(_PKG)
if _DLSS5 not in sys.path:
    sys.path.insert(0, _DLSS5)

# pythonw has NO console: sys.stderr is None or a dead handle, so an
# uncaught exception would vanish without a trace. Redirect stderr to a log
# file next to the bundle (logs\manager_err.log) BEFORE importing the UI.
try:
    _LOGDIR = os.path.join(_DLSS5, "logs")
    os.makedirs(_LOGDIR, exist_ok=True)
    _err = open(os.path.join(_LOGDIR, "manager_err.log"), "a",
                encoding="utf-8", buffering=1)
    sys.stderr = _err
    if sys.stdout is None:
        sys.stdout = _err
except OSError:
    pass

import traceback

from m13.ui import main

if __name__ == "__main__":
    try:
        main()
    except Exception:
        traceback.print_exc()
        raise
