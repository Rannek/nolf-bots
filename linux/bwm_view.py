#!/usr/bin/env python3
"""Look at a bot map scan (NOLF/Bots/<map>.bwm): stats + a top-down picture.

  bwm_view.py Bots/07HYDRO_AM.bwm [out.png]

Picture: each 32u column coloured by its highest reachable floor (dark = low,
bright = high), blue = swimming, cyan = flooded (no air), red = unreachable
floor, black = no floor.  Yellow lines = zipcords, magenta = ladders,
green = running jumps.
"""
import struct, sys

SPAN = struct.Struct('<3f8iHBBi')     # BotSpan
LINK = struct.Struct('<2iB3x3f f i')  # BotWorldLink
F_WATER, F_SWIM, F_NOAIR, F_LADDER, F_TIGHT, F_REACH, F_LEDGE = 1, 2, 4, 8, 16, 32, 64

def load(path):
    b = open(path, 'rb').read()
    o = 0
    magic, ver, key = struct.unpack_from('<3I', b, o); o += 12
    body = struct.unpack_from('<8f', b, o); o += 32
    vmin = struct.unpack_from('<3f', b, o); o += 12
    vmax = struct.unpack_from('<3f', b, o); o += 12
    cx, cz, islands, secs = struct.unpack_from('<3if', b, o); o += 16
    n, = struct.unpack_from('<I', b, o); o += 4
    colstart = struct.unpack_from('<%di' % n, b, o); o += 4 * n
    n, = struct.unpack_from('<I', b, o); o += 4
    spans = [SPAN.unpack_from(b, o + i * SPAN.size) for i in range(n)]; o += SPAN.size * n
    n, = struct.unpack_from('<I', b, o); o += 4
    links = [LINK.unpack_from(b, o + i * LINK.size) for i in range(n)]
    return dict(ver=ver, body=body, vmin=vmin, vmax=vmax, cx=cx, cz=cz, islands=islands, secs=secs,
                colstart=colstart, spans=spans, links=links)

def main():
    m = load(sys.argv[1])
    spans, cs, cx, cz = m['spans'], m['colstart'], m['cx'], m['cz']
    print('version', m['ver'], 'body radius %.0f half %.0f step %.0f climb %.0f jump %.0f grav %.0f run %.0f zip %.0f' % m['body'])
    print('box', ['%.0f' % v for v in m['vmin']], ['%.0f' % v for v in m['vmax']], 'cells %dx%d' % (cx, cz), 'scan %.0fs' % m['secs'])
    cnt = lambda f: sum(1 for s in spans if s[12] & f)
    print('spans %d reach %d swim %d noair %d ladder %d tight %d ledge %d islands %d' % (
        len(spans), cnt(F_REACH), cnt(F_SWIM), cnt(F_NOAIR), cnt(F_LADDER), cnt(F_TIGHT), cnt(F_LEDGE), m['islands']))
    kinds = {1: 'jump', 2: 'zip', 3: 'ladder'}
    for k in (1, 2, 3):
        print('%s links %d' % (kinds[k], sum(1 for l in m['links'] if l[2] == k)))
    moves = [0, 0, 0, 0]
    for s in spans:
        mv = s[11]
        for d in range(8):
            moves[(mv >> (2 * d)) & 3] += 1
    print('moves none %d walk %d drop %d climb %d' % tuple(moves))
    if len(sys.argv) < 3:
        return
    from PIL import Image, ImageDraw
    reach = [s for s in spans if s[12] & F_REACH]
    lo = min(s[0] for s in reach); hi = max(s[0] for s in reach)
    img = Image.new('RGB', (cx, cz))
    px = img.load()
    def cell_of(i):
        import bisect
        c = bisect.bisect_right(cs, i) - 1
        return c % cx, c // cx
    for c in range(cx * cz):
        x, z = c % cx, c // cx
        col = (0, 0, 0)
        for i in range(cs[c], cs[c + 1]):
            s = spans[i]
            f = s[12]
            if not f & F_REACH:
                if col == (0, 0, 0): col = (90, 0, 0)
                continue
            if f & F_NOAIR: col = (0, 200, 200)
            elif f & F_SWIM: col = (30, 60, 220)
            else:
                t = int(40 + 215 * (s[0] - lo) / max(1, hi - lo))
                col = (t, t, t)
            break
        px[x, cz - 1 - z] = col
    scale = 3
    img = img.resize((cx * scale, cz * scale), Image.NEAREST)
    dr = ImageDraw.Draw(img)
    colors = {1: (0, 255, 0), 2: (255, 255, 0), 3: (255, 0, 255)}
    for l in m['links']:
        a, b = cell_of(l[0]), cell_of(l[1])
        dr.line([(a[0] * scale, (cz - 1 - a[1]) * scale), (b[0] * scale, (cz - 1 - b[1]) * scale)], fill=colors[l[2]], width=2)
    img.save(sys.argv[2])
    print('wrote', sys.argv[2], 'floor heights %.0f..%.0f' % (lo, hi))

main()
