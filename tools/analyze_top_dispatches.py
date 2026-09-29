import csv

def main():
    dispatches = []
    with open('dlss5/m11d/build/Release/out/bench_fp16_1080p.csv', 'r') as f:
        reader = csv.DictReader(f)
        for row in reader:
            if row['frame'] == '2':
                dispatches.append({
                    'idx': int(row['idx']),
                    'fam': row['fam'],
                    'note': row['note'],
                    'm': int(row['m']),
                    'n': int(row['n']),
                    'k': int(row['k']),
                    'busy_ms': float(row['busy_us']) / 1000.0,
                    'idle_ms': float(row['idle_us']) / 1000.0,
                    'total_ms': float(row['total_us']) / 1000.0,
                    'bytes_mb': float(row['bytes']) / 1e6
                })

    dispatches.sort(key=lambda x: x['busy_ms'], reverse=True)
    print("Top 25 most expensive dispatches in FP16 (Frame 2):")
    print(f"{'Idx':<5} {'Fam':<8} {'Shape (MxNxK)':<22} {'Busy(ms)':<10} {'Idle(ms)':<10} {'Total(ms)':<10} {'Traffic(MB)':<12}")
    print("-" * 80)
    total_busy = sum(d['busy_ms'] for d in dispatches)
    for d in dispatches[:25]:
        shape = f"{d['m']}x{d['n']}x{d['k']}" if d['m'] > 0 else f"{d['note']}"
        print(f"{d['idx']:<5} {d['fam']:<8} {shape:<22} {d['busy_ms']:<10.2f} {d['idle_ms']:<10.2f} {d['total_ms']:<10.2f} {d['bytes_mb']:<12.1f}")

    print(f"\nTotal busy time across all dispatches: {total_busy:.2f} ms")

if __name__ == "__main__":
    main()
