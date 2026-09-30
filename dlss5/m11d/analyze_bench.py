# SPDX-License-Identifier: Apache-2.0
"""Analyze per-kernel bench CSV: compute per-family TF/s and time breakdown."""
import csv, sys, collections

def analyze(path, skip_warmup=True):
    rows = []
    with open(path) as f:
        for r in csv.DictReader(f):
            rows.append(r)
    
    # Group by family
    fam_times = collections.defaultdict(list)   # fam -> [us]
    fam_flops = collections.defaultdict(float)  # fam -> total FLOPs (accumulated)
    fam_count = collections.defaultdict(int)
    
    # Skip frame 1 (warmup, weights still settling) if requested
    frames = set(int(r['frame']) for r in rows)
    skip_frame = min(frames) if skip_warmup and len(frames) > 1 else None
    
    for r in rows:
        frame = int(r['frame'])
        if frame == skip_frame:
            continue
        fam = r['fam']
        m, n, k = int(r['m']), int(r['n']), int(r['k'])
        us = float(r['us'])
        fam_times[fam].append(us)
        fam_count[fam] += 1
        if m > 0 and n > 0 and k > 0:
            # GEMM FLOPs: 2*M*N*K per dispatch (multiply-add)
            batch = int(r['gz']) if int(r['gz']) > 0 else 1
            fam_flops[fam] += 2.0 * m * n * k * batch
    
    # Per-frame stats (skip warmup frame)
    nframes = len(frames) - (1 if skip_frame is not None else 0)
    
    print(f"\n{'='*70}")
    print(f"Bench CSV: {path}")
    print(f"Frames analyzed: {nframes} (skipped warmup: frame {skip_frame})")
    print(f"{'='*70}")
    
    # Time per family (sum across all dispatches per frame, then median across frames)
    # But simpler: total us per family / nframes = avg frame contribution
    print(f"\n{'Family':<25} {'Total ms':>10} {'Per-frame ms':>14} {'Dispatches':>11} {'TF/s':>8}")
    print('-'*70)
    
    total_us = 0
    family_data = []
    for fam in sorted(fam_times.keys(), key=lambda f: -sum(fam_times[f])):
        t_us = sum(fam_times[fam])
        per_frame_us = t_us / nframes
        n_disp = fam_count[fam] // nframes
        tfs = ""
        if fam_flops[fam] > 0:
            # FLOPs are PER FRAME (accumulated across dispatches of this family in analyzed frames)
            flops_per_frame = fam_flops[fam] / nframes
            tf_per_s = (flops_per_frame / (per_frame_us * 1e-6)) / 1e12
            tfs = f"{tf_per_s:.2f}"
        print(f"{fam:<25} {t_us/1e3:>10.2f} {per_frame_us/1e3:>14.2f} {n_disp:>11} {tfs:>8}")
        total_us += t_us
        family_data.append((fam, per_frame_us, fam_flops.get(fam,0)/nframes if nframes else 0))
    
    chain_per_frame_ms = total_us / nframes / 1e3
    print(f"{'TOTAL':<25} {total_us/1e3:>10.2f} {chain_per_frame_ms:>14.2f}")
    
    # GEMM-specific breakdown
    print(f"\n--- GEMM shape analysis ---")
    gemm_shapes = collections.defaultdict(lambda: {'us':0,'count':0,'flops':0})
    for r in rows:
        frame = int(r['frame'])
        if frame == skip_frame:
            continue
        if r['fam'] != 'gemm':
            continue
        m, n, k = int(r['m']), int(r['n']), int(r['k'])
        batch = int(r['gz']) if int(r['gz']) > 0 else 1
        key = (m, n, k, batch)
        gemm_shapes[key]['us'] += float(r['us'])
        gemm_shapes[key]['count'] += 1
        gemm_shapes[key]['flops'] += 2.0 * m * n * k * batch
    
    print(f"{'Shape (M,N,K,B)':<30} {'ms/frame':>10} {'N':>5} {'TF/s':>8}")
    print('-'*55)
    for key in sorted(gemm_shapes.keys(), key=lambda k: -gemm_shapes[k]['us']):
        d = gemm_shapes[key]
        ms_per_frame = d['us'] / nframes / 1e3
        n_per_frame = d['count'] // nframes
        flops_per_frame = d['flops'] / nframes
        us_per_frame = d['us'] / nframes
        tfs = (flops_per_frame / (us_per_frame * 1e-6)) / 1e12 if us_per_frame > 0 else 0
        m, n, k, b = key
        print(f"({m:>6},{n:>5},{k:>4},{b:>3}) {ms_per_frame:>10.2f} {n_per_frame:>5} {tfs:>8.2f}")

if __name__ == '__main__':
    path = sys.argv[1] if len(sys.argv) > 1 else 'bench_fuse_res.csv'
    analyze(path)
