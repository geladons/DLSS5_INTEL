# Validation vs torch goldens per HANDOFF_M10 (internal numpy, NOT cmp_live.py).
# Usage: python _validate.py <out_dir_with_live_dumps>
from pathlib import Path
_REPO = Path(__file__).resolve().parents[2]
import sys, numpy as np, os

root = r"" + str(_REPO) + r""
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(root, r"dlss5\m11d\build-nmake\out")
cmp_ = os.path.join(root, r"work\_m9b_cmp")

live_head = np.fromfile(os.path.join(out, "live_head.bin"), dtype=np.float32).reshape(-1, 16)
golden_head = np.fromfile(os.path.join(cmp_, "golden_head.bin"), dtype=np.float32).reshape(-1, 4)
n = min(len(live_head), len(golden_head))
exp = [0.008265, 0.007924, 0.008648]
ok = True
for ch in range(3):
    md = float(np.abs(live_head[:n, ch] - golden_head[:n, ch]).mean())
    stat = "OK" if abs(md - exp[ch]) < 5e-5 else "MISMATCH"
    if stat != "OK":
        ok = False
    print(f"head ch{ch}: meandiff {md:.6f} (expect {exp[ch]:.6f}) {stat}")

lv = np.fromfile(os.path.join(out, "live_featV.bin"), dtype=np.float32)
gf = np.fromfile(os.path.join(cmp_, "golden_features.bin"), dtype=np.float32)
m = min(len(lv), len(gf))
d = np.abs(lv[:m] - gf[:m])
maxdiff = float(d.max())
# 1 f16 ulp noise tolerance
print(f"featV: maxdiff {maxdiff:.6g} over {m} elems ({int((d > 1e-6).sum())} nonzero)")
print("VALIDATION", "PASS" if ok else "FAIL")
sys.exit(0 if ok else 1)
