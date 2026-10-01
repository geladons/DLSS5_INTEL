import csv

with open('dlss5/m11d/bench_bw_960.csv', 'r') as f:
    reader = csv.DictReader(f)
    gemms = [row for row in reader if row['fam'] == 'gemm']

print(f'Total GEMM dispatches: {len(gemms)}')
total_busy = sum(float(r['busy_us']) for r in gemms)
total_bytes = sum(float(r['bytes']) for r in gemms)
print(f'Total GEMM busy_us: {total_busy:.1f} us ({total_busy/1000.0:.2f} ms)')
print(f'Total GEMM bytes: {total_bytes / 1e6:.2f} MB')

gemms.sort(key=lambda r: float(r['busy_us']), reverse=True)
print('\nTop 20 GEMMs by busy_us:')
for r in gemms[:20]:
    busy = float(r['busy_us'])
    b = float(r['bytes'])
    print(f"idx={r['idx']:<4}: M={r['m']:<7} N={r['n']:<5} K={r['k']:<5} "
          f"busy={busy:>8.1f} us ({busy/1000:>6.2f} ms) "
          f"bytes={b/1e6:>6.2f} MB note={r['note']}")

