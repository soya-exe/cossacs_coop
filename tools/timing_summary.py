#!/usr/bin/env python3
"""Summarise btwsteam `timing:` lines (BTWSTEAM_TIMING=1) and the mock's `delivery:` lines.

usage: tools/timing_summary.py [--since HH:MM:SS] [--until HH:MM:SS] btwsteam.log [steam_mock_N.log]

For each metric over the 10 s windows logged from --since to --until: total samples, the median of the
windows' p50, the median of the windows' p95, and the largest max (microseconds). The mock
log counts seconds from its InitFlat; it is aligned with the hook log's "steam: loaded" line.
"""
import re
import statistics
import sys


def hms(s):
    h, m, sec = s.split(':')
    return int(h) * 3600 + int(m) * 60 + float(sec)


def main(argv):
    since = until = None
    while len(argv) > 2 and argv[1] in ('--since', '--until'):
        if argv[1] == '--since':
            since = hms(argv[2])
        else:
            until = hms(argv[2])
        argv = argv[:1] + argv[3:]
    hook = argv[1]
    mock = argv[2] if len(argv) > 2 else None
    metrics, other = {}, {'polls/s': [], 'gt calls': [], 'gt ms': [], 'worker %': [], 'process %': []}
    loaded = None
    for line in open(hook, errors='replace'):
        m = re.match(r'(\d\d:\d\d:\d\d\.\d+) \[ *\d+\] (.*)', line)
        if not m:
            continue
        t, msg = hms(m.group(1)), m.group(2)
        if msg.startswith('steam: loaded') and loaded is None:
            loaded = t
        if not msg.startswith('timing: ') or (since is not None and t < since) or \
                (until is not None and t > until):
            continue
        for name, n, p50, p95, mx in re.findall(r'(\w+) n=(\d+) p50=(-?\d+) p95=(-?\d+) max=(\d+)', msg):
            d = metrics.setdefault(name, {'n': 0, 'p50': [], 'p95': [], 'max': 0})
            d['n'] += int(n)
            if int(n) >= 5:
                d['p50'].append(int(p50))
                d['p95'].append(int(p95))
            d['max'] = max(d['max'], int(mx))
        for key, rx in (('polls/s', r'game polls (\d+)/s'), ('gt calls', r'steam calls (\d+)'),
                        ('gt ms', r'with data\) ([\d.]+) ms'), ('worker %', r'cpu worker ([\d.]+)%'),
                        ('process %', r'process ([\d.]+)%')):
            mm = re.search(rx, msg)
            if mm:
                other[key].append(float(mm.group(1)))
    med = lambda v: int(statistics.median(v)) if v else -1
    for name, d in metrics.items():
        print(f"  {name:8s} n={d['n']:6d}  p50={med(d['p50']):6d}  p95={med(d['p95']):6d}  max={d['max']:7d} us")
    print('  ' + '  '.join(f"{k}={statistics.median(v):.2f}" for k, v in other.items() if v))
    if mock:
        dl = []
        for line in open(mock, errors='replace'):
            m = re.match(r'\[ *([\d.]+)\] delivery: n=(\d+) p50=(\d+) p95=(\d+) max=(\d+)', line)
            if not m:
                continue
            if loaded is not None and ((since is not None and loaded + float(m.group(1)) < since) or
                                       (until is not None and loaded + float(m.group(1)) > until)):
                continue
            dl.append(tuple(int(x) for x in m.groups()[1:]))
        if dl:
            print(f"  mock delivery (send -> ready, incl. lag): n={sum(x[0] for x in dl)}  "
                  f"p50={med([x[1] for x in dl])}  p95={med([x[2] for x in dl])}  max={max(x[3] for x in dl)} us")


if __name__ == '__main__':
    main(sys.argv)
