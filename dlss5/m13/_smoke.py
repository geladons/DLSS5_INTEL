# Part 2 smoke test: config roundtrip, deployer to a temp dir, registry
# read-only checks, game-launch flag channels, process probes. No UI, no
# game starts. Run: python _smoke.py
import os
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from m13 import config, deploy, gamelaunch, processes
from m13.deploy import Deployer, LayerRegistry, DXVK_X32_D3D9, M12_PROXY

fails = []


def check(name, cond, detail=""):
    print("%-46s %s %s" % (name, "OK" if cond else "FAIL", detail))
    if not cond:
        fails.append(name)


# config roundtrip in a temp location (never touch the real user config)
tmp = tempfile.mkdtemp(prefix="m13smoke_")
cfg = config.Config(os.path.join(tmp, "c.json"))
check("config first_run", cfg.first_run)
cfg.set("weights_path", r"C:\w\dlssnr-logical.safetensors")
cfg.set("gain", 1.5)
cfg2 = config.Config(os.path.join(tmp, "c.json"))
check("config persist", cfg2.get("weights_path").endswith("safetensors")
      and abs(cfg2.get("gain") - 1.5) < 1e-9)

# deployer: park a fake game dll, deploy DXVK d3d9.dll only, undeploy restores
gdir = os.path.join(tmp, "game")
os.makedirs(gdir)
with open(os.path.join(gdir, "d3d9.dll"), "wb") as f:
    f.write(b"ORIGINAL-GAME-D3D9")
d = Deployer(gdir)
check("dx9 status clean", d.status("d3d9.dll", DXVK_X32_D3D9) == "foreign")
ok = d.deploy("d3d9.dll", DXVK_X32_D3D9)
check("dx9 deploy", ok and d.status("d3d9.dll", DXVK_X32_D3D9) == "deployed")
with open(os.path.join(gdir, "d3d9.dll"), "rb") as f:
    check("dx9 deployed is dxvk", f.read(4) != b"ORIG")
d.undeploy("d3d9.dll", DXVK_X32_D3D9)
with open(os.path.join(gdir, "d3d9.dll"), "rb") as f:
    check("dx9 undeploy restores orig", f.read() == b"ORIGINAL-GAME-D3D9")

# DX9 guard must flag a DXVK dxgi.dll, ignore absence
check("dx9 guard quiet", deploy.dx9_guard(gdir) == [])
with open(os.path.join(gdir, "dxgi.dll"), "wb") as f:
    f.write(b"MZ............DXVK............")
check("dx9 guard flags dxvk dxgi", len(deploy.dx9_guard(gdir)) == 1)
os.remove(os.path.join(gdir, "dxgi.dll"))

# registry: read-only probes (never write in a smoke test)
paths = LayerRegistry.registered_paths()
check("registry readable", isinstance(paths, list),
      "%d entries" % len(paths))
check("x64 manifest known", os.path.exists(deploy.MANIFEST_X64))
check("x86 manifest known", os.path.exists(deploy.MANIFEST_X86))
check("dxvk x32 present", os.path.exists(DXVK_X32_D3D9))
check("m12 proxy present", os.path.exists(M12_PROXY))

# flag channels (no game launched)
gamelaunch._flag_set(gamelaunch.DX9_TRIGGER, True)
check("dx9 trigger on", not gamelaunch.dx9_paused())
gamelaunch.dx9_pause()
check("dx9 pause via flag", gamelaunch.dx9_paused())
gamelaunch.dx9_resume()
check("dx9 resume via flag", not gamelaunch.dx9_paused())
gamelaunch.dx12_pause()
check("dx12 pause via flag", gamelaunch.dx12_paused())
gamelaunch.dx12_resume()
check("dx12 resume via flag", not gamelaunch.dx12_paused())

# process probes: m11d is running (started for part 1 validation)
pid = processes.find_pid("m11d.exe")
check("m11d detectable", pid is not None, "pid=%s" % pid)
check("nonexistent exe", processes.find_pid("definitely_not_running.exe")
      is None)

print("SMOKE:", "ALL PASS" if not fails else "FAILURES: %s" % fails)
sys.exit(1 if fails else 0)
