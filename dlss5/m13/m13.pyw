# DLSS 5 Manager entry point - run with pythonw (no console window):
#   "C:\Users\AI\AppData\Local\Programs\Python\Python312\pythonw.exe" m13.pyw
# or via M13.cmd next to this file.
import os
import sys

_DLSS5 = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if _DLSS5 not in sys.path:
    sys.path.insert(0, _DLSS5)

from m13.ui import main

if __name__ == "__main__":
    main()
