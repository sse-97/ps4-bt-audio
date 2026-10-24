"""Generate the native app's artwork: smooth UI fonts, icons, app icon.

Outputs (under build/native, the native build's -I include dir):
  ui_font.h  - anti-aliased glyph bitmaps at several native pixel sizes
  ui_icons.h - 8-bit alpha masks, tinted at runtime
  icon0.png  - 512x512 PS4 home-screen icon

The same data is exposed through build_data() so the PC previewer renders
exactly what the console will.

Font note: glyph bitmaps are baked from DejaVu Sans (free license, see
LICENSE_DEJAVU.txt), taken from the copy bundled with matplotlib.
"""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

root = Path(__file__).resolve().parent
out = root.parent / 'build/native'

FONT_REG = 'DejaVuSans.ttf'
FONT_SEMI = 'DejaVuSans-Bold.ttf'

# name, file, pixel size. Sizes are native (nothing is ever pixel-doubled) and
# follow the PS4 Settings screens at 1080p: a large light title, list rows a
# little under 30 px, values and button labels a step smaller. The PS4's own
# typeface (SST) is Sony-proprietary, so DejaVu Sans stands in; it has no light
# weight, so the title uses the regular one. The bold face is only for the tiny
# OPTIONS button tag.
FACES = [
    ('TITLE', FONT_REG, 52),
    ('ROW', FONT_REG, 29),
    ('SMALL', FONT_REG, 25),
    ('TAG', FONT_SEMI, 14),
]
GAMMA = 0.85  # <1 thickens light-on-dark text slightly so it stays crisp on TVs

# The app icon follows the PS4's own app tiles (Internet Browser, TV & Video): a
# white line-art symbol on a deep blue, a little lighter at the top.
TILE_TOP = (8, 30, 156)
TILE_BOTTOM = (2, 18, 128)
WHITE = (255, 255, 255, 255)
SS = 8  # supersampling for icon masks


def _font(name, size):
    import importlib.util
    spec = importlib.util.find_spec('matplotlib')
    if spec is None or not spec.origin:
        raise SystemExit('matplotlib is required: it supplies the DejaVu Sans font files.')
    p = Path(spec.origin).parent / 'mpl-data/fonts/ttf' / name
    if not p.exists():
        raise SystemExit(f'DejaVu font not found: {p}')
    return ImageFont.truetype(str(p), size)


# --- Fonts -------------------------------------------------------------------
def build_face(path, size):
    font = _font(path, size)
    asc, desc = font.getmetrics()
    lut = [min(255, round(255 * ((i / 255.0) ** GAMMA))) for i in range(256)]
    glyphs, bits = [], bytearray()
    for code in range(32, 127):
        ch = chr(code)
        adv16 = round(font.getlength(ch) * 16)
        x0, y0, x1, y1 = font.getbbox(ch, anchor='ls')
        w, h = x1 - x0, y1 - y0
        if w <= 0 or h <= 0 or ch == ' ':
            glyphs.append((len(bits), adv16, 0, 0, 0, 0))
            continue
        im = Image.new('L', (w, h), 0)
        ImageDraw.Draw(im).text((-x0, -y0), ch, font=font, fill=255, anchor='ls')
        off = len(bits)
        bits.extend(lut[v] for v in im.tobytes())
        glyphs.append((off, adv16, w, h, x0, y0))
    return dict(glyphs=glyphs, bits=bytes(bits), asc=asc, line=asc + desc)


# --- Icons (alpha masks) ------------------------------------------------------
def mask_canvas(w, h):
    return Image.new('L', (w * SS, h * SS), 0)


def finish(im, w, h):
    return im.resize((w, h), Image.LANCZOS)


def draw_bt(d, cx, cy, hw, hh, color, width):
    """The Bluetooth rune as one round-jointed polyline."""
    unit = [(-1, -0.5), (1, 0.5), (0, 1), (0, -1), (1, -0.5), (-1, 0.5)]
    pts = [(cx + px * hw, cy + py * hh) for px, py in unit]
    d.line(pts, fill=color, width=width, joint='curve')
    r = width / 2
    for px, py in pts:
        d.ellipse((px - r, py - r, px + r, py + r), fill=color)


def icon_bt(size):
    """Device rows: the service can't tell headphones from speakers, so every
    device wears the Bluetooth symbol."""
    im = mask_canvas(size, size)
    S = size * SS
    draw_bt(ImageDraw.Draw(im), S * 0.5, S * 0.5, 0.21 * S, 0.42 * S, 255, int(0.075 * S))
    return finish(im, size, size)


def icon_check(size, wf=0.075):
    im = mask_canvas(size, size)
    d = ImageDraw.Draw(im)
    w = int(size * SS * wf)
    pts = [(0.19, 0.52), (0.38, 0.71), (0.81, 0.27)]
    pts = [(x * size * SS, y * size * SS) for x, y in pts]
    d.line(pts, fill=255, width=w, joint='curve')
    for x, y in (pts[0], pts[2]):
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=255)
    return finish(im, size, size)


def btn_cross(size):
    im = mask_canvas(size, size)
    d = ImageDraw.Draw(im)
    S = size * SS
    w = int(0.13 * S)
    for a, b in (((0.24, 0.24), (0.76, 0.76)), ((0.76, 0.24), (0.24, 0.76))):
        d.line([(a[0] * S, a[1] * S), (b[0] * S, b[1] * S)], fill=255, width=w)
        for x, y in (a, b):
            d.ellipse((x * S - w / 2, y * S - w / 2, x * S + w / 2, y * S + w / 2), fill=255)
    return finish(im, size, size)


def btn_circle(size):
    im = mask_canvas(size, size)
    S = size * SS
    ImageDraw.Draw(im).ellipse((0.20 * S, 0.20 * S, 0.80 * S, 0.80 * S), outline=255, width=int(0.12 * S))
    return finish(im, size, size)


def btn_triangle(size):
    im = mask_canvas(size, size)
    d = ImageDraw.Draw(im)
    S = size * SS
    pts = [(0.50 * S, 0.17 * S), (0.84 * S, 0.77 * S), (0.16 * S, 0.77 * S)]
    w = int(0.12 * S)
    d.line(pts + [pts[0], pts[1]], fill=255, width=w, joint='curve')
    for x, y in pts:
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=255)
    return finish(im, size, size)


def icon_plus(size, wf=0.10):
    im = mask_canvas(size, size)
    d = ImageDraw.Draw(im)
    S = size * SS
    w = int(wf * S)
    d.rounded_rectangle((0.5 * S - w / 2, 0.24 * S, 0.5 * S + w / 2, 0.76 * S), radius=int(w / 2), fill=255)
    d.rounded_rectangle((0.24 * S, 0.5 * S - w / 2, 0.76 * S, 0.5 * S + w / 2), radius=int(w / 2), fill=255)
    return finish(im, size, size)


def icon_wrench(size):
    """Open-ended spanner, head at the top right like the PS4 Settings glyph."""
    im = mask_canvas(size, size)
    d = ImageDraw.Draw(im)
    S = size * SS
    w = int(0.17 * S)
    d.line([(0.30 * S, 0.30 * S), (0.72 * S, 0.72 * S)], fill=255, width=w)
    d.ellipse((0.72 * S - w / 2, 0.72 * S - w / 2, 0.72 * S + w / 2, 0.72 * S + w / 2), fill=255)
    hx, hy, hr = 0.30 * S, 0.30 * S, 0.20 * S
    d.ellipse((hx - hr, hy - hr, hx + hr, hy + hr), fill=255)       # head
    d.ellipse((hx - hr * 0.5, hy - hr * 0.5, hx + hr * 0.5, hy + hr * 0.5), fill=0)  # bolt hole
    d.polygon([(hx, hy), (hx - hr * 1.3, hy - hr * 0.35), (hx - hr * 0.35, hy - hr * 1.3)], fill=0)  # open jaw
    return finish(im.transpose(Image.Transpose.FLIP_LEFT_RIGHT), size, size)


def icon_chevron(size, up):
    """The arrows above and below the focused PIN digit."""
    im = mask_canvas(size, size)
    d = ImageDraw.Draw(im)
    S = size * SS
    w = int(0.09 * S)
    ys = (0.66, 0.34) if up else (0.34, 0.66)
    pts = [(0.18 * S, ys[0] * S), (0.50 * S, ys[1] * S), (0.82 * S, ys[0] * S)]
    d.line(pts, fill=255, width=w, joint='curve')
    for x, y in (pts[0], pts[2]):
        d.ellipse((x - w / 2, y - w / 2, x + w / 2, y + w / 2), fill=255)
    return finish(im, size, size)


ICON_SPECS = [
    ('BT', lambda: icon_bt(40)),
    ('WRENCH', lambda: icon_wrench(26)),
    ('PLUS', lambda: icon_plus(32)),
    ('CHECK', lambda: icon_check(32)),
    ('GX', lambda: btn_cross(19)),
    ('GO', lambda: btn_circle(20)),
    ('GT', lambda: btn_triangle(19)),
    ('CHEV_UP', lambda: icon_chevron(28, True)),
    ('CHEV_DOWN', lambda: icon_chevron(28, False)),
]


# --- App icon -------------------------------------------------------------------
def build_icon512():
    """App tile in the style of the PS4's own apps: the Bluetooth symbol from the
    app's device rows, white and in the same proportions (its stroke about a
    tenth of its height), centred on the deep app-tile blue. No frame or glow:
    the system draws the tile's focus."""
    S = 512 * 3
    icon = Image.new('RGBA', (S, S))
    d = ImageDraw.Draw(icon)
    for y in range(S):
        t = y / (S - 1)
        d.line([(0, y), (S, y)], fill=tuple(round(TILE_TOP[i] * (1 - t) + TILE_BOTTOM[i] * t) for i in range(3)) + (255,))
    draw_bt(d, S * 0.5, S * 0.5, 0.125 * S, 0.25 * S, WHITE, int(0.045 * S))
    return icon.resize((512, 512), Image.LANCZOS).convert('RGB')


def build_data():
    faces = {name: build_face(path, size) for name, path, size in FACES}
    icons = {name: fn() for name, fn in ICON_SPECS}
    return dict(faces=faces, icons=icons, icon512=build_icon512())


# --- C header emission ---------------------------------------------------------
def write_headers(data):
    out.mkdir(parents=True, exist_ok=True)
    lines = ['/* Generated by native/make_assets.py - do not edit. */',
             'struct glyph{unsigned off;unsigned short adv16;unsigned char w,h;signed char bx,by;};',
             'struct face{const struct glyph*g;const unsigned char*bits;int asc,line;};']
    for name, _, _ in FACES:
        f = data['faces'][name]
        lines.append(f'static const unsigned char fb_{name}[]={{' + ','.join(map(str, f['bits'])) + '};')
        rows = ','.join('{%d,%d,%d,%d,%d,%d}' % g for g in f['glyphs'])
        lines.append(f'static const struct glyph fg_{name}[95]={{{rows}}};')
    lines.append('enum{' + ','.join(f'F_{n}' for n, _, _ in FACES) + ',F_COUNT};')
    lines.append('static const struct face FACES[F_COUNT]={' + ','.join(
        '{fg_%s,fb_%s,%d,%d}' % (n, n, data['faces'][n]['asc'], data['faces'][n]['line']) for n, _, _ in FACES) + '};')
    (out / 'ui_font.h').write_text('\n'.join(lines) + '\n')

    lines = ['/* Generated by native/make_assets.py - do not edit. */',
             'struct icon{const unsigned char*a;unsigned short w,h;};']
    for name, im in data['icons'].items():
        lines.append(f'static const unsigned char ia_{name}[]={{' + ','.join(map(str, im.tobytes())) + '};')
    lines.append('enum{' + ','.join(f'I_{n}' for n in data['icons']) + ',I_COUNT};')
    lines.append('static const struct icon ICONS[I_COUNT]={' + ','.join(
        '{ia_%s,%d,%d}' % (n, im.width, im.height) for n, im in data['icons'].items()) + '};')
    (out / 'ui_icons.h').write_text('\n'.join(lines) + '\n')
    data['icon512'].save(out / 'icon0.png')


if __name__ == '__main__':
    d = build_data()
    write_headers(d)
    print('Native assets generated:', out)
    for n, _, _ in FACES:
        print('  face', n, len(d['faces'][n]['bits']), 'bytes')
    print('  icons', len(d['icons']))
