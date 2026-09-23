# Detached launcher for m11d (bench/selftest). Foreground GPU hangs wedge the
# tool, so the child runs in its own process group with stdout to a log file
# and we poll the log. Decisive signal is the expected tail line in the log
# (a TDR kill still exits 0, so the exit code lies).
# Usage: python _run_detach.py <logfile> <timeout_s> <expected_substr> -- <args...>
import subprocess, sys, time, os

log = sys.argv[1]
timeout = float(sys.argv[2])
expected = sys.argv[3]
sep = sys.argv.index("--")
args = sys.argv[sep + 1:]

lf = open(log, "w")
p = subprocess.Popen(
    args,
    stdout=lf,
    stderr=subprocess.STDOUT,
    cwd=os.path.dirname(os.path.abspath(args[0])),
    creationflags=subprocess.DETACHED_PROCESS | subprocess.CREATE_NEW_PROCESS_GROUP,
    close_fds=True,
)

deadline = time.time() + timeout
ok = False
tail = []
while time.time() < deadline:
    if os.path.exists(log):
        with open(log, "r", errors="replace") as f:
            data = f.read()
        if expected in data:
            ok = True
            break
    if p.poll() is not None:
        # child gone; give the log a moment to flush
        time.sleep(1.0)
        with open(log, "r", errors="replace") as f:
            data = f.read()
        ok = expected in data
        break
    time.sleep(1.0)

with open(log, "r", errors="replace") as f:
    lines = f.read().splitlines()
print("\n".join(lines[-25:]))
print("=== RESULT:", "OK" if ok else "TIMEOUT/FAIL", "===")
sys.exit(0 if ok else 1)
