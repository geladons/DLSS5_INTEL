# SPDX-License-Identifier: Apache-2.0
import sys
import os

bundle_dir = r"C:\Users\<owner>\Desktop\production"
sys.path.insert(0, bundle_dir)

print("=== Testing production m13 package imports ===")
try:
    import m13
    print("  [OK] import m13")
    import m13.config
    print("  [OK] import m13.config")
    import m13.paths
    print("  [OK] import m13.paths")
    import m13.daemonctl
    print("  [OK] import m13.daemonctl")
    import m13.deploy
    print("  [OK] import m13.deploy")
    import m13.gamelaunch
    print("  [OK] import m13.gamelaunch")
    import m13.gamescan
    print("  [OK] import m13.gamescan")
    import m13.i18n
    print("  [OK] import m13.i18n")
    import m13.icons
    print("  [OK] import m13.icons")
    import m13.logtail
    print("  [OK] import m13.logtail")
    import m13.overlay
    print("  [OK] import m13.overlay")
    import m13.processes
    print("  [OK] import m13.processes")
    import m13.screenmode
    print("  [OK] import m13.screenmode")
    import m13.splash
    print("  [OK] import m13.splash")
    import m13.controller
    print("  [OK] import m13.controller")
    import m13.ui_games
    print("  [OK] import m13.ui_games")
    import m13.ui
    print("  [OK] import m13.ui")
    print("All modules imported cleanly as package m13!")
except Exception as e:
    import traceback
    traceback.print_exc()
