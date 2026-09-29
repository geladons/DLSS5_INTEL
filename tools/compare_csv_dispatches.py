import csv

def load_frame2(path):
    dispatches = {}
    with open(path, 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['frame'] == '2':
                idx = int(row['idx'])
                dispatches[idx] = {
                    'fam': row['fam'],
                    'note': row['note'],
                    'm': int(row['m']),
                    'n': int(row['n']),
                    'k': int(row['k']),
                    'busy_ms': float(row['busy_us']) / 1000.0,
                    'total_ms': float(row['total_us']) / 1000.0,
                    'bytes_mb': float(row['bytes']) / 1e6
                }
    return dispatches

def main():
    f16 = load_frame2('dlss5/m11d/build/Release/out/bench_fp16_1080p.csv')
    i8 = load_frame2('dlss5/m11d/build/Release/out/bench_int8_1080p.csv')
    
    print("=== Per-Dispatch Comparison: FP16 vs INT8 Pilot70 (Frame 2) ===")
    print(f"FP16 dispatches: {len(f16)}, INT8 dispatches: {len(i8)}")
    
    # In INT8, qact was added before each weight1, shifting subsequent indices
    # Let's match by comparing block0 weight1 and other weight1s
    print("\n--- Weight1 & QAct Dispatches in INT8 ---")
    print(f"{'Idx':<5} {'Kernel':<10} {'Shape':<22} {'INT8 Busy(ms)':<15} {'Traffic(MB)':<12}")
    print("-" * 65)
    i8_w1_total = 0.0
    for idx, d in sorted(i8.items()):
        if d['fam'] in ('qact', 'gemm_i8'):
            shape = f"{d['m']}x{d['n']}x{d['k']}"
            print(f"{idx:<5} {d['fam']:<10} {shape:<22} {d['busy_ms']:<15.2f} {d['bytes_mb']:<12.1f}")
            i8_w1_total += d['busy_ms']
            
    print("-" * 65)
    print(f"Total INT8 (qact + gemm_i8) busy time: {i8_w1_total:.2f} ms")
    
    print("\n--- Equivalent Weight1 Dispatches in FP16 ---")
    f16_w1_total = 0.0
    for idx, d in sorted(f16.items()):
        if d['fam'] == 'gemm' and d['k'] == 32 and d['n'] == 128:
            shape = f"{d['m']}x{d['n']}x{d['k']}"
            print(f"{idx:<5} {d['fam']:<10} {shape:<22} {d['busy_ms']:<15.2f} {d['bytes_mb']:<12.1f}")
            f16_w1_total += d['busy_ms']
            
    print("-" * 65)
    print(f"Total FP16 weight1 busy time: {f16_w1_total:.2f} ms")
    print(f"Difference: {i8_w1_total - f16_w1_total:+.2f} ms (INT8 is {i8_w1_total / f16_w1_total * 100.0:.1f}% of FP16)")

if __name__ == "__main__":
    main()
