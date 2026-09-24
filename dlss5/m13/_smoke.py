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

# controller: monitor snapshot + async worker (no daemon mutation)
from m13.controller import M13Controller
ctl = M13Controller(config.Config(os.path.join(tmp, "c2.json")))
import threading, time
got = []
ctl.start_worker(lambda ok, msg: got.append((ok, msg)))
time.sleep(1.5)
snap = ctl.snapshot()
check("monitor snapshot", isinstance(snap, dict) and "active_mode" in snap)
check("monitor sees daemon", snap.get("daemon_pid") is not None,
      "pid=%s" % snap.get("daemon_pid"))
check("monitor gain probe", snap.get("daemon_gain") is not None,
      "gain=%s frames=%s" % (snap.get("daemon_gain"),
                             snap.get("daemon_frames")))
check("active_mode none", snap.get("active_mode") is None)
done = threading.Event()
ctl.submit(lambda: (done.set(), (True, "worker ok"))[1])
check("worker runs actions", done.wait(5.0) and got and got[-1][1]
      == "worker ok")
ctl.shutdown()

# config: new overlay keys survive a roundtrip
cfg.set("overlay_autopause", True)
cfg3 = config.Config(os.path.join(tmp, "c.json"))
check("overlay_autopause persist", cfg3.get("overlay_autopause") is True)

# gamescan: PE import parsing on real exes
from m13 import gamescan
gta4 = r"D:\downdloads\Grand Theft Auto IV\GTAIV.exe"
gta5 = r"D:\Grand Theft Auto V Enhanced\GTA5_Enhanced.exe"
if os.path.exists(gta4):
    arch, apis = gamescan.pe_info(gta4)
    check("pe GTAIV x86+dx9", arch == "x86" and "dx9" in apis,
          "%s %s" % (arch, apis))
if os.path.exists(gta5):
    arch, apis = gamescan.pe_info(gta5)
    check("pe GTA5E x64+dx12", arch == "x64" and "dx12" in apis,
          "%s %s" % (arch, apis))
rdr2 = r"D:\Red Dead Redemption 2\RDR2.exe"
if os.path.exists(rdr2):
    arch, apis = gamescan.pe_info(rdr2)
    check("pe RDR2 vulkan", apis and apis[0] == "vulkan",
          "%s %s" % (arch, apis))
t0 = time.time()
found = gamescan.scan()
check("drive scan returns", isinstance(found, list) and len(found) > 0,
      "%d candidates in %.1fs" % (len(found), time.time() - t0))

# add_game auto-detect (no deploy, temp config)
ctl2 = M13Controller(config.Config(os.path.join(tmp, "c3.json")))
if os.path.exists(gta4):
    ok, msg = ctl2.add_game(gta4)
    meta = (ctl2.cfg.get("games") or {}).get(gta4) or {}
    check("add_game auto dx9/x86", ok and meta.get("mode") == "dx9"
          and meta.get("arch") == "x86", msg)

# deploy_mode roundtrip in a temp dir (dx9 x86, dx11 x64)
g2 = os.path.join(tmp, "game2")
os.makedirs(g2)
d2 = Deployer(g2)
out = d2.deploy_mode("dx9", "x86")
check("deploy_mode dx9", out.get("d3d9.dll") == "deployed", str(out))
check("dx9 deploys ONLY d3d9", not os.path.exists(os.path.join(g2,
      "dxgi.dll")))
out = d2.deploy_mode("dx11", "x64")
check("deploy_mode dx11 x3", all(s == "deployed" for s in out.values())
      and len(out) == 3, str(out))
check("mode_status deployed", d2.mode_status("dx11", "x64") == "deployed")
d2.undeploy_mode("dx11", "x64")
check("undeploy_mode dx11", d2.mode_status("dx11", "x64") == "clean")
ctl2.shutdown()

# suspend/resume on a sacrificial child process (busy loop = measurable CPU)
import subprocess
sleeper = subprocess.Popen([sys.executable, "-c", "while True: pass"],
                           creationflags=processes.CREATE_NO_WINDOW)
time.sleep(0.4)
def cputime(pid):
    import ctypes
    h = ctypes.windll.kernel32.OpenProcess(0x0400, False, pid)  # QUERY_LIMITED

    class FT(ctypes.Structure):
        _fields_ = [("lo", ctypes.c_ulong), ("hi", ctypes.c_ulong)]
    creation, exit_, kernel, user = FT(), FT(), FT(), FT()
    ctypes.windll.kernel32.GetProcessTimes(
        ctypes.c_void_p(h), ctypes.byref(creation), ctypes.byref(exit_),
        ctypes.byref(kernel), ctypes.byref(user))
    ctypes.windll.kernel32.CloseHandle(h)
    return (kernel.hi << 32 | kernel.lo) + (user.hi << 32 | user.lo)
ok, _ = processes.suspend_pid(sleeper.pid)
t1 = cputime(sleeper.pid); time.sleep(0.4); t2 = cputime(sleeper.pid)
check("suspend freezes cpu", ok and t2 - t1 < 100000, "delta=%d" % (t2 - t1))
ok, _ = processes.resume_pid(sleeper.pid)
t3 = cputime(sleeper.pid); time.sleep(0.4); t4 = cputime(sleeper.pid)
check("resume unfreezes", ok and t4 - t3 > 100000, "delta=%d" % (t4 - t3))
sleeper.terminate()

print("SMOKE:", "ALL PASS" if not fails else "FAILURES: %s" % fails)
sys.exit(1 if fails else 0)
