import csv
import os
import sys

def load_frame(path, frame_idx='2'):
    if not os.path.exists(path):
        return None
    dispatches = []
    with open(path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['frame'] == frame_idx:
                dispatches.append({
                    'idx': int(row['idx']),
                    'fam': row['fam'],
                    'note': row['note'],
                    'm': int(row['m']),
                    'n': int(row['n']),
                    'k': int(row['k']),
                    'busy_ms': float(row['busy_us']) / 1000.0,
                    'total_ms': float(row['total_us']) / 1000.0,
                    'bytes_mb': float(row['bytes']) / 1e6
                })
    return dispatches

def analyze_profile(name, dispatches):
    if not dispatches:
        print(f"No dispatches found for {name}")
        return {}
    total_busy = sum(d['busy_ms'] for d in dispatches)
    total_bytes = sum(d['bytes_mb'] for d in dispatches)
    
    w1_busy = 0.0
    w1_bytes = 0.0
    w2_busy = 0.0
    w2_bytes = 0.0
    qact_w1_busy = 0.0
    qact_w1_bytes = 0.0
    qact_w2_busy = 0.0
    qact_w2_bytes = 0.0
    qact_total_busy = 0.0
    qact_total_bytes = 0.0
    
    for d in dispatches:
        if d['fam'] in ('gemm', 'gemm_i8') and d['n'] == 128 and d['k'] == 32:
            w1_busy += d['busy_ms']
            w1_bytes += d['bytes_mb']
        elif d['fam'] in ('gemm', 'gemm_i8') and d['n'] == 32 and d['k'] == 128:
            w2_busy += d['busy_ms']
            w2_bytes += d['bytes_mb']
        elif d['fam'] == 'qact':
            qact_total_busy += d['busy_ms']
            qact_total_bytes += d['bytes_mb']
            if d['n'] == 32:
                qact_w1_busy += d['busy_ms']
                qact_w1_bytes += d['bytes_mb']
            elif d['n'] == 128:
                qact_w2_busy += d['busy_ms']
                qact_w2_bytes += d['bytes_mb']

    print(f"\n=== Profile: {name} ===")
    print(f"Total Dispatches: {len(dispatches)}")
    print(f"Total Frame Busy Time: {total_busy:.2f} ms")
    print(f"Total Frame DRAM Traffic: {total_bytes:.1f} MB ({total_bytes/1024:.2f} GB)")
    print(f"  Weight1 GEMM:       {w1_busy:.2f} ms, {w1_bytes:.1f} MB")
    print(f"  Weight2 GEMM:       {w2_busy:.2f} ms, {w2_bytes:.1f} MB")
    print(f"  QAct for W1:        {qact_w1_busy:.2f} ms, {qact_w1_bytes:.1f} MB")
    print(f"  QAct for W2:        {qact_w2_busy:.2f} ms, {qact_w2_bytes:.1f} MB")
    print(f"  Total QActs:        {qact_total_busy:.2f} ms, {qact_total_bytes:.1f} MB")
    print(f"  W1+W2+QActs Sum:    {w1_busy + w2_busy + qact_total_busy:.2f} ms, {w1_bytes + w2_bytes + qact_total_bytes:.1f} MB")

    return {
        'total_busy': total_busy,
        'total_bytes': total_bytes,
        'w1_busy': w1_busy,
        'w1_bytes': w1_bytes,
        'w2_busy': w2_busy,
        'w2_bytes': w2_bytes,
        'qact_w1_busy': qact_w1_busy,
        'qact_w1_bytes': qact_w1_bytes,
        'qact_w2_busy': qact_w2_busy,
        'qact_w2_bytes': qact_w2_bytes,
        'qact_total_busy': qact_total_busy,
        'qact_total_bytes': qact_total_bytes,
    }

def main():
    base_dir = r"c:\Users\<owner>\Desktop\DLSS5_INTEL\dlss5\m11d\build\Release\out"
    print("================================================================================")
    print("                    DLSS5 FUSION (W1->W2) BENCHMARK ANALYSIS")
    print("================================================================================")
    
    # 1080p
    u1080 = load_frame(os.path.join(base_dir, "bench_unfused_1080p.csv"))
    f1080 = load_frame(os.path.join(base_dir, "bench_fused_1080p.csv"))
    
    su1080 = analyze_profile("1080p Unfused W1+W2 (D5C_INT8_FUSION=0)", u1080)
    sf1080 = analyze_profile("1080p Fused W1->W2 (D5C_INT8_FUSION=1)", f1080)
    
    if su1080 and sf1080:
        print("\n--------------------------------------------------------------------------------")
        print("1080p COMPARISON SUMMARY:")
        busy_saved = su1080['total_busy'] - sf1080['total_busy']
        bytes_saved = su1080['total_bytes'] - sf1080['total_bytes']
        qact_saved = su1080['qact_w2_busy'] - sf1080['qact_w2_busy']
        print(f"  Frame Busy Time Saved : {busy_saved:+.2f} ms ({su1080['total_busy']:.2f} ms -> {sf1080['total_busy']:.2f} ms, {(su1080['total_busy']/sf1080['total_busy'] if sf1080['total_busy']>0 else 1.0):.2f}x speedup)")
        print(f"  DRAM Traffic Saved    : {bytes_saved:+.1f} MB ({su1080['total_bytes']:.1f} MB -> {sf1080['total_bytes']:.1f} MB, {bytes_saved/1024:+.2f} GB)")
        print(f"  QAct W2 Eliminated    : {su1080['qact_w2_busy']:.2f} ms -> {sf1080['qact_w2_busy']:.2f} ms ({qact_saved:+.2f} ms)")

    # 960x576
    u960 = load_frame(os.path.join(base_dir, "bench_unfused_960.csv"))
    f960 = load_frame(os.path.join(base_dir, "bench_fused_960.csv"))
    
    su960 = analyze_profile("960x576 Unfused W1+W2 (D5C_INT8_FUSION=0)", u960)
    sf960 = analyze_profile("960x576 Fused W1->W2 (D5C_INT8_FUSION=1)", f960)

    if su960 and sf960:
        print("\n--------------------------------------------------------------------------------")
        print("960x576 COMPARISON SUMMARY:")
        busy_saved = su960['total_busy'] - sf960['total_busy']
        bytes_saved = su960['total_bytes'] - sf960['total_bytes']
        qact_saved = su960['qact_w2_busy'] - sf960['qact_w2_busy']
        print(f"  Frame Busy Time Saved : {busy_saved:+.2f} ms ({su960['total_busy']:.2f} ms -> {sf960['total_busy']:.2f} ms, {(su960['total_busy']/sf960['total_busy'] if sf960['total_busy']>0 else 1.0):.2f}x speedup)")
        print(f"  DRAM Traffic Saved    : {bytes_saved:+.1f} MB ({su960['total_bytes']:.1f} MB -> {sf960['total_bytes']:.1f} MB, {bytes_saved/1024:+.2f} GB)")
        print(f"  QAct W2 Eliminated    : {su960['qact_w2_busy']:.2f} ms -> {sf960['qact_w2_busy']:.2f} ms ({qact_saved:+.2f} ms)")

if __name__ == '__main__':
    main()
