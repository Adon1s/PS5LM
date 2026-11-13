# PS5LM Voice artwork from brand/build.py's own functions: the icon is the stacked logo with VOICE under it;
# the home-screen background has VOICE (accent) after the horizontal logo's cursor line, tagline under.
# Usage: uv run --with fonttools --with brotli --with skia-pathops python brand/voice_art.py brand/build.py <out dir>
# then copy icon.svg and background.svg to ps5/app/sce_sys/source and run scripts/render-artwork.sh.
import os, sys
build, out = sys.argv[1], sys.argv[2]
head = open(build).read().split("tile('ps5lm-icon.svg'")[0]
sys.argv = [build, out]
g = {'__file__': build, '__name__': 'build'}
exec(compile(head, build, 'exec'), g)
union, glyphs, stacked, horizontal, bounds, write, background = (g[k] for k in
    ('union', 'glyphs', 'stacked', 'horizontal', 'bounds', 'write', 'background'))
CAP, LEAD, GROUND, INK, ACCENT = g['CAP'], g['LEAD'], g['GROUND'], g['INK_DARK'], g['ACCENT']
from fontTools.pens.transformPen import TransformPen
from pathops import Path

def scaled(p, s, dx, dy):
    q = Path(); p.draw(TransformPen(q.getPen(), (s, 0, 0, s, dx, dy))); return q

def icon(path, weight=760, size=512, fill=0.80):
    a, b, c = stacked(weight)
    x0, y0, x1, y1 = bounds([a, b, c])
    # VOICE: five mono cells set to the logo's width, a gap of one lead under LM.
    v = union(glyphs('VOICE', weight, 0, CAP))
    vx0, vy0, vx1, vy1 = v.bounds
    vs = (x1 - x0) / (vx1 - vx0)
    v = scaled(v, vs, x0 - vx0 * vs, y1 + LEAD * CAP)
    X0, Y0, X1, Y1 = bounds([a, b, c, v])
    s = size * fill / max(X1 - X0, Y1 - Y0)
    ox = (size - (X1 - X0) * s) / 2 - X0 * s
    oy = (size - (Y1 - Y0) * s) / 2 - Y0 * s
    write(path, [(INK, a), (ACCENT, b), (INK, c), (ACCENT, v)], size, size, s, ox, oy, bg=GROUND, title='PS5LM Voice')

def banner(path, w, h, tagline='llama.cpp on PlayStation 5'):
    cap = 0.136 * h
    x0, base = 0.5625 * w, 0.62 * h
    a, b, c = horizontal(760)
    s = cap / CAP
    lx, _, rx, _ = bounds([a, b, c])
    parts = [(col, '', p, s, x0 - lx * s, base - CAP * s) for col, p in ((INK, a), (ACCENT, b), (INK, c))]
    # VOICE under the logo, the same cap height, in the accent, left-aligned with it.
    v = union(glyphs('VOICE', 760, 0, CAP))
    parts.append((ACCENT, '', v, s, x0 - v.bounds[0] * s, base + 0.3 * cap + cap - CAP * s))
    t = union(glyphs(tagline, 400, 0, CAP, track=0))
    ts = 0.035 * h / 1000
    parts.append(('#ebebf5', ' fill-opacity="0.55"', t, ts, x0 - t.bounds[0] * ts, base + 1.3 * cap + 0.074 * h - CAP * ts))
    background(path, w, h, parts)

os.makedirs(out, exist_ok=True)
icon('icon.svg')
banner('background.svg', 3840, 2160)
for f in ('icon.svg', 'background.svg'):
    s = open(os.path.join(out, f)).read().replace('<title>PS5LM</title>', '<title>PS5LM Voice</title>')
    open(os.path.join(out, f), 'w').write(s)
print('voice_art: icon.svg and background.svg in', out)
