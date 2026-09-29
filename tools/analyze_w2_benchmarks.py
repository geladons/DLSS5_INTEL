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
        return
    total_busy = sum(d['busy_ms'] for d in dispatches)
    total_bytes = sum(d['bytes_mb'] for d in dispatches)
    
    # Weight1: n=128, k=32
    # Weight2: n=32, k=128
    # QAct: fam='qact'
    w1_busy = 0.0
    w1_bytes = 0.0
    w2_busy = 0.0
    w2_bytes = 0.0
    qact_w1_busy = 0.0
    qact_w1_bytes = 0.0
    qact_w2_busy = 0.0
    qact_w2_bytes = 0.0
    
    for d in dispatches:
        if d['fam'] in ('gemm', 'gemm_i8') and d['n'] == 128 and d['k'] == 32:
            w1_busy += d['busy_ms']
            w1_bytes += d['bytes_mb']
        elif d['fam'] in ('gemm', 'gemm_i8') and d['n'] == 32 and d['k'] == 128:
            w2_busy += d['busy_ms']
            w2_bytes += d['bytes_mb']
        elif d['fam'] == 'qact':
            if d['n'] == 32: # k stored in n column for qact in recorder.cpp
                qact_w1_busy += d['busy_ms']
                qact_w1_bytes += d['bytes_mb']
            elif d['n'] == 128:
                qact_w2_busy += d['busy_ms']
                qact_w2_bytes += d['bytes_mb']

    print(f"\n=== Profile: {name} ===")
    print(f"Total Dispatches: {len(dispatches)}")
    print(f"Total GPU Busy Time: {total_busy:.2f} ms")
    print(f"Total DRAM Traffic : {total_bytes:.1f} MB ({total_bytes/1024.0:.2f} GB)")
    print(f"Weight1 GEMMs: {w1_busy:.2f} ms, {w1_bytes:.1f} MB")
    if qact_w1_busy > 0:
        print(f"Weight1 QActs: {qact_w1_busy:.2f} ms, {qact_w1_bytes:.1f} MB")
    print(f"Weight2 GEMMs: {w2_busy:.2f} ms, {w2_bytes:.1f} MB")
    if qact_w2_busy > 0:
        print(f"Weight2 QActs: {qact_w2_busy:.2f} ms, {qact_w2_bytes:.1f} MB")
    return {
        'total_busy': total_busy,
        'total_bytes': total_bytes,
        'w1_busy': w1_busy,
        'w1_bytes': w1_bytes,
        'qact_w1_busy': qact_w1_busy,
        'qact_w1_bytes': qact_w1_bytes,
        'w2_busy': w2_busy,
        'w2_bytes': w2_bytes,
        'qact_w2_busy': qact_w2_busy,
        'qact_w2_bytes': qact_w2_bytes,
    }

def main():
    out_dir = 'dlss5/m11d/build/Release/out'
    print("================================================================================")
    print("                     DLSS 5 INT8 BENCHMARK MATRIX (1080p)")
    print("================================================================================")
    fp16_1080 = analyze_profile("1080p Pure FP16 Baseline", load_frame(f'{out_dir}/bench_fp16_1080p.csv'))
    w2_1080 = analyze_profile("1080p INT8 Weight2 Only", load_frame(f'{out_dir}/bench_w2_1080p.csv'))
    w1w2_1080 = analyze_profile("1080p INT8 W1 + W2", load_frame(f'{out_dir}/bench_w1w2_1080p.csv'))

    if fp16_1080 and w2_1080:
        print("\n--------------------------------------------------------------------------------")
        print("               WEIGHT2 COMPARISON (1080p FP16 vs INT8 Weight2)")
        print("--------------------------------------------------------------------------------")
        fp16_w2_time = fp16_1080['w2_busy']
        int8_w2_time = w2_1080['w2_busy'] + w2_1080['qact_w2_busy']
        print(f"FP16 Weight2 Busy Time       : {fp16_w2_time:.2f} ms ({fp16_1080['w2_bytes']:.1f} MB)")
        print(f"INT8 Weight2 Busy Time (GEMM): {w2_1080['w2_busy']:.2f} ms ({w2_1080['w2_bytes']:.1f} MB)")
        print(f"INT8 Weight2 QAct Time       : {w2_1080['qact_w2_busy']:.2f} ms ({w2_1080['qact_w2_bytes']:.1f} MB)")
        print(f"INT8 Weight2 Total Busy Time : {int8_w2_time:.2f} ms")
        print(f"Delta on Weight2 GEMM alone  : {w2_1080['w2_busy'] - fp16_w2_time:+.2f} ms (speedup: {fp16_w2_time / w2_1080['w2_busy']:.2f}x)")
        print(f"Total Frame Busy Delta       : {w2_1080['total_busy'] - fp16_1080['total_busy']:+.2f} ms")

    print("\n================================================================================")
    print("                     DLSS 5 INT8 BENCHMARK MATRIX (960x576)")
    print("================================================================================")
    fp16_960 = analyze_profile("960x576 Pure FP16 Baseline", load_frame(f'{out_dir}/bench_fp16_960.csv'))
    w2_960 = analyze_profile("960x576 INT8 Weight2 Only", load_frame(f'{out_dir}/bench_w2_960.csv'))
    w1w2_960 = analyze_profile("960x576 INT8 W1 + W2", load_frame(f'{out_dir}/bench_w1w2_960.csv'))

    if fp16_960 and w2_960:
        print("\n--------------------------------------------------------------------------------")
        print("             WEIGHT2 COMPARISON (960x576 FP16 vs INT8 Weight2)")
        print("--------------------------------------------------------------------------------")
        fp16_w2_time = fp16_960['w2_busy']
        int8_w2_time = w2_960['w2_busy'] + w2_960['qact_w2_busy']
        print(f"FP16 Weight2 Busy Time       : {fp16_w2_time:.2f} ms ({fp16_960['w2_bytes']:.1f} MB)")
        print(f"INT8 Weight2 Busy Time (GEMM): {w2_960['w2_busy']:.2f} ms ({w2_960['w2_bytes']:.1f} MB)")
        print(f"INT8 Weight2 QAct Time       : {w2_960['qact_w2_busy']:.2f} ms ({w2_960['qact_w2_bytes']:.1f} MB)")
        print(f"INT8 Weight2 Total Busy Time : {int8_w2_time:.2f} ms")
        print(f"Delta on Weight2 GEMM alone  : {w2_960['w2_busy'] - fp16_w2_time:+.2f} ms (speedup: {fp16_w2_time / w2_960['w2_busy']:.2f}x)")
        print(f"Total Frame Busy Delta       : {w2_960['total_busy'] - fp16_960['total_busy']:+.2f} ms")

if __name__ == '__main__':
    main()
