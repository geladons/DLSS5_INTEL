# SPDX-License-Identifier: Apache-2.0
# One-off: copy the edited m13 .py files into the production bundle and
# sha1-verify each. (Inline $vars get mangled by the exec wrapper -
# see AGENTS.md host quirks.)
import hashlib
import os
import shutil
import sys

SRC = os.path.dirname(os.path.abspath(__file__))
DST = r"C:\Users\<owner>\Desktop\production\m13"
FILES = ["gamelaunch.py", "controller.py", "config.py",
         "ui_games.py", "i18n.py", "test_env.py"]


def sha1(p):
    h = hashlib.sha1()
    with open(p, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


ok_all = True
for name in FILES:
    s = os.path.join(SRC, name)
    d = os.path.join(DST, name)
    shutil.copy2(s, d)
    a, b = sha1(s), sha1(d)
    same = a == b
    ok_all = ok_all and same
    print("%-16s %s %s" % (name, "OK" if same else "MISMATCH", a[:16]))
stray = os.path.join(os.path.dirname(DST), "m13$f")
if os.path.exists(stray):
    os.remove(stray)
    print("removed stray file: %s" % stray)
sys.exit(0 if ok_all else 1)
