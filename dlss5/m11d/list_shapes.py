# SPDX-License-Identifier: Apache-2.0
import csv, collections, sys

def list_shapes(path):
    print("=" * 60)
    print(path)
    print("=" * 60)
    shapes = collections.defaultdict(lambda: {'count': 0, 'us': 0.0, 'flops': 0.0})
    frames = set()
    with open(path) as f:
        for r in csv.DictReader(f):
            frames.add(r['frame'])
            if r['frame'] == '1' or r['fam'] != 'gemm':
                continue
            key = (int(r['m']), int(r['n']), int(r['k']), int(r['gz']))
            shapes[key]['count'] += 1
            shapes[key]['us'] += float(r['us'])
            shapes[key]['flops'] += 2.0 * key[0] * key[1] * key[2] * key[3]
    
    nframes = len(frames) - 1
    for k in sorted(shapes, key=lambda x: -shapes[x]['us'])[:15]:
        d = shapes[k]
        ms = d['us'] / nframes / 1000.0
        n_per_frame = d['count'] // nframes
        tfs = (d['flops'] / (d['us'] * 1e-6)) / 1e12 if d['us'] > 0 else 0
        print(f"Shape {k}: {ms:6.2f} ms/frame, N={n_per_frame:3d}, TF/s={tfs:5.2f}")

if __name__ == '__main__':
    for p in sys.argv[1:]:
        list_shapes(p)
