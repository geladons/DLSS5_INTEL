# Control-channel validation for m11d (M13 part 1).
# Starts nothing itself - expects a fresh m11d on 127.0.0.1:47990.
# Checks: STATUS/SETGAIN roundtrip, and that a gain change takes effect on
# the NEXT frame WITHOUT a daemon restart (same frame in at gain 2.0 vs 1.0
# must produce different outputs).
import socket
import struct
import sys
import time

sys.path.insert(0, r"C:\Users\AI\Desktop\DLSS5_INTEL\dlss5\m13")
from daemonctl import M11dClient, MAGIC_CTRL

HOST, PORT = "127.0.0.1", 47990
W = H = 320
MAGIC_PLAIN = 0x304E524E


def send_frame(payload_gain_check=False):
    # deterministic 320x320 gradient frame
    px = bytearray(W * H * 4)
    for i in range(W * H):
        px[i * 4 + 0] = i % 256
        px[i * 4 + 1] = (i // 7) % 256
        px[i * 4 + 2] = (i // 13) % 256
        px[i * 4 + 3] = 255
    with socket.create_connection((HOST, PORT), timeout=30) as s:
        s.sendall(struct.pack("<4I", MAGIC_PLAIN, W, H, 44))
        s.sendall(bytes(px))
        out = b""
        need = W * H * 4
        while len(out) < need:
            chunk = s.recv(need - len(out))
            if not chunk:
                break
            out += chunk
    if len(out) != need:
        raise SystemExit("frame reply short: %d bytes" % len(out))
    return out


def mean_abs_diff(a, b):
    n = min(len(a), len(b))
    return sum(abs(a[i] - b[i]) for i in range(0, n, 997)) / (n // 997 + 1)


c = M11dClient(HOST, PORT, timeout=10)

g, f = c.status()
print("STATUS: gain=%.3f frames=%d" % (g, f))
assert g == 1.0 and f == 0, "fresh daemon must report gain 1.0, 0 frames"

g, f = c.set_gain(2.0)
print("SETGAIN 2.0 -> active gain=%.3f frames=%d" % (g, f))
assert abs(g - 2.0) < 1e-6

t0 = time.time()
out_hi = send_frame()
print("frame at gain 2.0: %d bytes in %.1f s" % (len(out_hi), time.time() - t0))

g, f = c.set_gain(1.0)
assert abs(g - 1.0) < 1e-6
out_lo = send_frame()

g, f = c.status()
print("STATUS: gain=%.3f frames=%d (expect 2)" % (g, f))
assert f == 2, "daemon must have processed exactly 2 frames"

d = mean_abs_diff(out_hi, out_lo)
print("mean|out(gain2)-out(gain1)| = %.3f/255" % d)
assert d > 0.5, "gain change had NO effect on the output frame!"

# invalid gain must be rejected and leave the active value alone
try:
    c.set_gain(99.0)
    raise SystemExit("out-of-range gain was accepted!")
except Exception as e:
    print("range check OK:", e)
g, _ = c.status()
assert abs(g - 1.0) < 1e-6, "rejected gain must not stick"

print("CONTROL CHANNEL VALIDATION: ALL PASS")
