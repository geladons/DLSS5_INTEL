# SPDX-License-Identifier: Apache-2.0
import csv, statistics

for res, path in [('960x576', 'dlss5/m11d/bench_busy_idle_960.csv'), ('1080p', 'dlss5/m11d/bench_busy_idle_1080p.csv')]:
    rows = [r for r in csv.DictReader(open(path)) if r['frame'] == '4']
    chain = [r for r in rows if r['fam'] not in {'decode', 'features', 'featpack', 'compose', 'encode'}]
    idles = [float(r['idle_us']) for r in chain]
    busys = [float(r['busy_us']) for r in chain]
    print(f'=== {res} Chain Statistics (1246 dispatches) ===')
    print(f'Total Chain Busy: {sum(busys)/1000:.3f} ms')
    print(f'Total Chain Idle: {sum(idles)/1000:.3f} ms ({sum(idles)/(sum(busys)+sum(idles))*100:.2f}%)')
    print(f'Idle per dispatch: mean={statistics.mean(idles):.2f} us, median={statistics.median(idles):.2f} us, stdev={statistics.stdev(idles):.2f} us')
    print(f'Idle min={min(idles):.2f} us, max={max(idles):.2f} us, p90={statistics.quantiles(idles, n=10)[8]:.2f} us, p99={statistics.quantiles(idles, n=100)[98]:.2f} us')
    
    # Check count of dispatches with idle > 10 us
    large_idle = [r for r in chain if float(r['idle_us']) > 10.0]
    print(f'Dispatches with idle > 10 us: {len(large_idle)}')
    for r in large_idle:
        idx = r['idx']
        fam = r['fam']
        note = r['note']
        m = r['m']
        n = r['n']
        k = r['k']
        gx = r['gx']
        gy = r['gy']
        i_ms = float(r['idle_us']) / 1000.0
        b_ms = float(r['busy_us']) / 1000.0
        print(f'  idx={idx} fam={fam}[{note}] M={m} N={n} K={k} G=({gx},{gy}) idle={i_ms:.3f} ms busy={b_ms:.3f} ms')
