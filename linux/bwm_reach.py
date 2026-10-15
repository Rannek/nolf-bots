#!/usr/bin/env python3
"""Directed reachability on a bot map scan: bwm_reach.py file.bwm x y z  x y z
(player-center positions).  Prints whether / how far each reaches the other."""
import sys, heapq, bisect
sys.argv, args = sys.argv[:2], sys.argv[2:]
src = open(__file__.replace('bwm_reach.py', 'bwm_view.py')).read().replace('\nmain()\n', '\n')
exec(src)
m = load(sys.argv[1]); sp = m['spans']; cs = m['colstart']; cx = m['cx']; vmin = m['vmin']
DX = [1, 1, 0, -1, -1, -1, 0, 1]; DZ = [0, 1, 1, 1, 0, -1, -1, -1]
links = {}
for l in m['links']: links.setdefault(l[0], []).append(l)
def find(x, y, z):
    ix = int((x - vmin[0]) // 32); iz = int((z - vmin[2]) // 32); c = iz * cx + ix
    best = None
    for i in range(cs[c], cs[c + 1]):
        if sp[i][0] <= y - 53 + 40 and sp[i][12] & 32: return i
    return best
def pos(i):
    c = bisect.bisect_right(cs, i) - 1
    return (vmin[0] + (c % cx + .5) * 32, sp[i][0], vmin[2] + (c // cx + .5) * 32)
def path(a, b):
    dist = {a: 0}; prev = {}; pq = [(0, a)]
    while pq:
        d, n = heapq.heappop(pq)
        if n == b: break
        if d > dist[n]: continue
        s = sp[n]
        for k in range(8):
            mv = (s[11] >> (2 * k)) & 3; t = s[3 + k]
            if mv == 0 or t < 0: continue
            nd = d + (45 if k & 1 else 32) * (1.5 if sp[t][12] & 2 else 1)
            if nd < dist.get(t, 1e18): dist[t] = nd; prev[t] = (n, 'mv%d' % mv); heapq.heappush(pq, (nd, t))
        for l in links.get(n, []):
            t = l[1]; nd = d + l[6]
            if nd < dist.get(t, 1e18): dist[t] = nd; prev[t] = (n, ['', 'JUMP', 'ZIP', 'LADDER'][l[2]]); heapq.heappush(pq, (nd, t))
    if b not in dist: return None, len(dist)
    steps = []; n = b
    while n != a: p, how = prev[n]; steps.append((how, n)); n = p
    return dist[b], steps[::-1]
P = [tuple(map(float, args[i:i + 3])) for i in range(0, len(args), 3)]
for i in range(len(P)):
    for j in range(len(P)):
        if i == j: continue
        a, b = find(*P[i]), find(*P[j])
        d, st = path(a, b)
        if d is None: print(P[i], '->', P[j], 'UNREACHABLE (reached %d cells)' % st); continue
        special = [(h, tuple(round(v) for v in pos(n))) for h, n in st if h in ('JUMP', 'ZIP', 'LADDER', 'mv2', 'mv3')]
        swim = sum(1 for h, n in st if sp[n][12] & 2)
        print(P[i], '->', P[j], 'cost %.0f cells %d swim %d special %d:' % (d, len(st), swim, len(special)), special[:12])
