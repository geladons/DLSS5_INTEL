import csv
import sys
import os

default_paths = [
    'dlss5/m11d/bench_bw_960.csv',
    'dlss5/m11d/bench_bw_1080p.csv',
    'bench_bw_960.csv',
    'bench_bw_1080p.csv',
    'dlss5/m11d/bench_busy_idle_960.csv',
    'dlss5/m11d/bench_busy_idle_1080p.csv'
]

targets = []
if len(sys.argv) > 1:
    targets = sys.argv[1:]
else:
    seen = set()
    for p in default_paths:
        if os.path.isfile(p):
            norm = os.path.normpath(p)
            if norm not in seen:
                seen.add(norm)
                targets.append(p)

if not targets:
    print("No profiling CSV files found to analyze.")
    sys.exit(1)

for path in targets:
    if not os.path.isfile(path):
        print(f"Skipping non-existent file: {path}")
        continue

    print('=' * 90)
    print(f'ANALYSIS FOR: {path}')
    print('=' * 90)
    with open(path, 'r', newline='') as f:
        all_rows = list(csv.DictReader(f))

    if not all_rows:
        print("Empty CSV file.\n")
        continue

    rows = [r for r in all_rows if r.get('frame') == '2']
    if not rows:
        rows = [r for r in all_rows if r.get('frame') == '1']
    if not rows:
        rows = all_rows

    total_busy_us = sum(float(r.get('busy_us', 0)) for r in rows)
    total_idle_us = sum(float(r.get('idle_us', 0)) for r in rows)
    total_busy = total_busy_us / 1000.0
    total_idle = total_idle_us / 1000.0
    total = total_busy + total_idle
    total_bytes = sum(int(r.get('bytes', 0)) for r in rows)
    total_gb = total_bytes / 1e9
    total_mb = total_bytes / (1024.0 * 1024.0)

    overall_eff_bw = (total_gb / (total_busy_us * 1e-6)) if total_busy_us > 0 else 0.0

    print(f"Dispatches timed : {len(rows)}")
    print(f"Total Frame Time : {total:.2f} ms")
    print(f"GPU Busy Time    : {total_busy:.2f} ms ({total_busy/total*100:.2f}%)")
    print(f"Inter-Disp Idle  : {total_idle:.2f} ms ({total_idle/total*100:.2f}%)")
    print(f"Total DRAM Move  : {total_gb:.3f} GB ({total_mb:.1f} MB)")
    print(f"Avg Effective BW : {overall_eff_bw:.2f} GB/s")
    print()

    by_fam = {}
    for r in rows:
        fam = r.get('fam', 'unknown')
        if fam not in by_fam:
            by_fam[fam] = {'busy_us': 0.0, 'idle_us': 0.0, 'bytes': 0, 'count': 0}
        by_fam[fam]['busy_us'] += float(r.get('busy_us', 0))
        by_fam[fam]['idle_us'] += float(r.get('idle_us', 0))
        by_fam[fam]['bytes'] += int(r.get('bytes', 0))
        by_fam[fam]['count'] += 1

    print("FAMILY BREAKDOWN (Sorted by DRAM Traffic):")
    hdr = f"{'Family':<10} | {'Count':<5} | {'Busy(ms)':<8} | {'Idle(ms)':<8} | {'%Frame':<6} | {'Traffic(GB)':<11} | {'%DRAM':<6} | {'Avg GB/s':<8} | {'Classification':<15}"
    print(hdr)
    print('-' * len(hdr))

    for fam, d in sorted(by_fam.items(), key=lambda x: x[1]['bytes'], reverse=True):
        f_busy_ms = d['busy_us'] / 1000.0
        f_idle_ms = d['idle_us'] / 1000.0
        f_tot = f_busy_ms + f_idle_ms
        pct_frame = (f_tot / total * 100.0) if total > 0 else 0.0
        f_gb = d['bytes'] / 1e9
        pct_dram = (d['bytes'] / total_bytes * 100.0) if total_bytes > 0 else 0.0
        eff_bw = (f_gb / (d['busy_us'] * 1e-6)) if d['busy_us'] > 0 else 0.0
        classification = "DRAM-Bound" if eff_bw < 150.0 else "Compute-Bound"
        print(f"{fam:<10} | {d['count']:>5} | {f_busy_ms:>8.2f} | {f_idle_ms:>8.2f} | {pct_frame:>5.1f}% | {f_gb:>11.3f} | {pct_dram:>5.1f}% | {eff_bw:>8.1f} | {classification:<15}")
    print()

    print("TOP 10 HEAVIEST DRAM-TRAFFIC KERNELS:")
    sorted_traffic = sorted(rows, key=lambda r: int(r.get('bytes', 0)), reverse=True)[:10]
    hdr_tr = f"{'Idx':<5} | {'Family':<8} | {'Note':<6} | {'Shape (M,N,K)':<18} | {'Grid (Gx,Gy,Gz)':<18} | {'Traffic(MB)':<11} | {'Busy(us)':<9} | {'Eff GB/s':<8} | {'Class':<12}"
    print(hdr_tr)
    print('-' * len(hdr_tr))
    for r in sorted_traffic:
        m, n, k = r.get('m', '0'), r.get('n', '0'), r.get('k', '0')
        gx, gy, gz = r.get('gx', '0'), r.get('gy', '0'), r.get('gz', '0')
        shape_str = f"({m},{n},{k})"
        grid_str = f"({gx},{gy},{gz})"
        k_bytes = int(r.get('bytes', 0))
        k_mb = k_bytes / (1024.0 * 1024.0)
        k_busy_us = float(r.get('busy_us', 0))
        k_eff_bw = (k_bytes / 1e9) / (k_busy_us * 1e-6) if k_busy_us > 0 else 0.0
        k_cls = "DRAM-Bound" if k_eff_bw < 150.0 else "Compute-Bound"
        print(f"{r.get('idx','0'):<5} | {r.get('fam',''):<8} | {r.get('note',''):<6} | {shape_str:<18} | {grid_str:<18} | {k_mb:>11.2f} | {k_busy_us:>9.2f} | {k_eff_bw:>8.1f} | {k_cls:<12}")
    print()

    print("TOP 10 LARGEST IDLE GAPS (Inter-Dispatch Bubbles):")
    sorted_idle = sorted(rows, key=lambda r: float(r.get('idle_us', 0)), reverse=True)[:10]
    hdr_id = f"{'Idx':<5} | {'Family':<8} | {'Idle (us)':<9} | {'Busy (us)':<9} | {'Shape (M,N,K,Gx,Gy,Gz)':<32}"
    print(hdr_id)
    print('-' * len(hdr_id))
    for r in sorted_idle:
        m, n, k = r.get('m', '0'), r.get('n', '0'), r.get('k', '0')
        gx, gy, gz = r.get('gx', '0'), r.get('gy', '0'), r.get('gz', '0')
        shape_str = f"({m},{n},{k},{gx},{gy},{gz})"
        print(f"{r.get('idx','0'):<5} | {r.get('fam',''):<8} | {float(r.get('idle_us', 0)):>9.2f} | {float(r.get('busy_us', 0)):>9.2f} | {shape_str:<32}")
    print()
