import csv
import collections

def roofline_audit(csv_path, label):
    reader = csv.DictReader(open(csv_path))
    fam_bytes = collections.defaultdict(float)
    fam_busy = collections.defaultdict(float)
    for r in reader:
        if r['frame'] == '2':
            fam = r['fam']
            b = float(r['busy_us']) / 1e6
            by = float(r['bytes'])
            fam_busy[fam] += b
            fam_bytes[fam] += by
    
    print(f"\n=== Per-Kernel Roofline / Bandwidth Audit: {label} ===")
    print(f"{'Family':<12} | {'Busy (ms)':>10} | {'DRAM (MB)':>10} | {'BW (GB/s)':>10} | {'% Peak (224)':>15}")
    print("-" * 65)
    tot_busy = sum(fam_busy.values())
    tot_bytes = sum(fam_bytes.values())
    for fam in sorted(fam_busy.keys(), key=lambda k: fam_busy[k], reverse=True):
        busy_ms = fam_busy[fam] * 1e3
        mb = fam_bytes[fam] / 1e6
        bw = (fam_bytes[fam] / fam_busy[fam]) / 1e9 if fam_busy[fam] > 0 else 0
        pct = (bw / 224.0) * 100.0
        print(f"{fam:<12} | {busy_ms:10.2f} | {mb:10.1f} | {bw:10.1f} | {pct:14.1f}%")
    print("-" * 65)
    tot_bw = (tot_bytes / tot_busy) / 1e9 if tot_busy > 0 else 0
    print(f"{'TOTAL':<12} | {tot_busy*1e3:10.2f} | {tot_bytes/1e6:10.1f} | {tot_bw:10.1f} | {(tot_bw/224.0)*100.0:14.1f}%")

if __name__ == '__main__':
    roofline_audit('dlss5/m11d/build/Release/out/bench_fused_960.csv', '960x576 Fused')
    roofline_audit('dlss5/m11d/build/Release/out/bench_fused_1080p.csv', '1080p Fused')
