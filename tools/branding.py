"""ARMS Rearmed's own boot splash and logo tags ("Rearmed logo & splash art", Video tab), made at setup from the
player's own game files - nothing of the game ships with the port:

- the boot splash (Boot_Strap_00's Boot_ControllerStrap, 1600x900 BC7): the game's layout, yellow and curve, its logo
  with a REARMED tag set as the game sets "ARMS LIVE" (Common_Live_00), the ways to play on PC (Kenney's Input Prompts
  devices, CC0: third_party/kenney_input_prompts) and what this port is, in the game's own fonts (Font/BmpFont_*.szs);
- the intro's logo (Boot_Strap_00's Logo_Title_04, a 640x160 BC4 mask): the logo with the tag under it;
- the loading circle (Common_FadeShort_00's Fade_Logo_00, 720x720 BC7): the tag under its logo.

Writes <outdir>/branding/: for each texture <name>.orig (its level-0 bytes as the game holds them in memory: the
runtime recognises the texture by them, runtime/nvn/texpatch.c), <name>.rgba (the new image, RGBA8, w x h) and
manifest.txt ("<name> <w> <h> <bc4|bc7> <orig> <rgba>" per line). The runtime encodes the new image into the
texture's own format when the game uploads it, while the setting is on.

Usage: branding.py <game.xci|nsp> <prod.keys> <outdir> [--update <update.nsp>] [--port-version 0.1.0]
       (nsp_extract.py --branding runs it after extracting, with the same files)
"""
import argparse
import io
import math
import os
import struct
import sys

from PIL import Image, ImageChops, ImageDraw, ImageFilter, ImageOps

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # (python -I leaves the script's folder out)
import nsp_extract as nx  # noqa: E402


def _data_dir():
    """third_party/kenney_input_prompts: next to tools/ in the source tree, in the bundle when frozen (armsgen)"""
    base = getattr(sys, '_MEIPASS', None)
    if base:
        return os.path.join(base, 'kenney_input_prompts')
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'third_party', 'kenney_input_prompts')


# ---------------------------------------------------------------- the game's files
class Game:
    def __init__(self, romfs, files):
        self.romfs = romfs
        self.files = {p: (o, s) for p, o, s in files}

    def raw(self, path):
        off, size = self.files[path]
        d = self.romfs.read(self.romfs.data_off + off, size)
        return nx.yaz0_decompress(d) if d[:4] == b'Yaz0' else d

    def archive(self, path):
        """{member: bytes} of a (Yaz0) SARC, nested SARCs flattened ("a.arc/timg/__Combined.bntx")"""
        out, todo = {}, [('', self.raw(path))]
        while todo:
            prefix, d = todo.pop()
            for name, body in nx.sarc_files(d):
                body = nx.yaz0_decompress(body) if body[:4] == b'Yaz0' else body
                if body[:4] == b'SARC':
                    todo.append((prefix + name + '/', body))
                else:
                    out[prefix + name] = body
        return out

    def texture(self, layout, name):
        """(fmt, w, h, block height log2, level-0 bytes as stored) of texture `name` in Layout/<layout>"""
        for body in self.archive(f'/Layout/{layout}.Nin_NX_NVN.szs').values():
            if body[:4] == b'BNTX':
                for t in bntx_textures(body):
                    if t[0] == name:
                        return t[1:]
        raise KeyError(f'{layout}: no texture {name}')


def bntx_textures(d):
    count = struct.unpack_from('<I', d, 0x24)[0]
    info = struct.unpack_from('<Q', d, 0x28)[0]
    for i in range(count):
        brti = struct.unpack_from('<Q', d, info + 8 * i)[0]
        levels = struct.unpack_from('<H', d, brti + 0x16)[0]
        fmt, _, w, h, _, _, bhl = struct.unpack_from('<IIiiiiI', d, brti + 0x1C)
        size = struct.unpack_from('<I', d, brti + 0x50)[0]
        name_at = struct.unpack_from('<Q', d, brti + 0x60)[0]
        n = struct.unpack_from('<H', d, name_at)[0]
        ptrs = struct.unpack_from('<Q', d, brti + 0x70)[0]
        lv0 = struct.unpack_from('<Q', d, ptrs)[0]
        end = struct.unpack_from('<Q', d, ptrs + 8)[0] if levels > 1 else lv0 + size
        yield d[name_at + 2:name_at + 2 + n].decode(), fmt, w, h, bhl, d[lv0:end]


def _gob_off(xb, y, wg, bh):
    gx, gy = xb >> 6, y >> 3
    by, giy = gy >> bh, gy & ((1 << bh) - 1)
    off = (by * wg + gx) * (512 << bh) + giy * 512
    xi, yi = xb & 63, y & 7
    return off + (((xi >> 5) << 8) | ((yi >> 1) << 6) | (((xi >> 4) & 1) << 5) | ((yi & 1) << 4) | (xi & 15))


def _bh_for(hb, bh):
    while bh > 0 and (1 << (bh - 1)) * 8 >= hb:
        bh -= 1
    return bh


def deswizzle(data, wb, hb, bpb, bh):
    """block-linear -> rows of blocks (wb x hb blocks of bpb bytes)"""
    row = wb * bpb
    bh = _bh_for(hb, bh)
    out = bytearray(row * hb)
    for y in range(hb):
        for x in range(0, row, 16):
            o = _gob_off(x, y, (row + 63) // 64, bh)
            out[y * row + x:y * row + x + 16] = data[o:o + 16]
    return bytes(out)


def decode(fmt, w, h, bh, data):
    """RGBA image of a BC4 / BC7 level 0 (Pillow's BCn decoders, through a DDS header)"""
    kind = fmt >> 8
    bpb = 8 if kind == 0x1d else 16
    lin = deswizzle(data, (w + 3) // 4, (h + 3) // 4, bpb, bh)
    hdr = bytearray(128)
    struct.pack_into('<4sIIIIII', hdr, 0, b'DDS ', 124, 0x1007 | 0x80000, (h + 3) // 4 * 4, (w + 3) // 4 * 4, len(lin), 0)
    struct.pack_into('<II', hdr, 76, 32, 4)
    struct.pack_into('<I', hdr, 108, 0x1000)
    if kind == 0x1d:
        struct.pack_into('<4s', hdr, 84, b'ATI1')
        blob = bytes(hdr) + lin
    else:
        struct.pack_into('<4s', hdr, 84, b'DX10')
        blob = bytes(hdr) + struct.pack('<IIIII', 99 if (fmt & 0xff) == 6 else 98, 3, 0, 1, 0) + lin
    return Image.open(io.BytesIO(blob)).convert('RGBA').crop((0, 0, w, h))


# ---------------------------------------------------------------- the game's fonts (.bffnt: BC4 sheets)
class Font:
    def __init__(self, d):
        self.d = d
        t = d.index(b'TGLP')
        tsize = struct.unpack_from('<I', d, t + 4)[0]
        self.cw, self.ch, nsheets = d[t + 8] + 1, d[t + 9] + 1, d[t + 10]
        ss, self.base, _, self.cols, self.rows, self.sw, self.sh, off = struct.unpack_from('<IHHHHHHI', d, t + 12)
        self.bytes = (self.sw // 4) * (self.sh // 4) * 8
        gobs = (self.sh // 4 + 7) // 8
        self.bh = 0
        while (1 << self.bh) < gobs and self.bh < 4:
            self.bh += 1
        al = 512 << self.bh  # (each sheet's image starts at its slot rounded up to the block size)
        self.sheet_at = [(off + i * ss + al - 1) // al * al for i in range(nsheets)]
        self.cmap, self.widths, self._sheets = {}, {}, {}
        p = t + tsize
        while p + 8 <= len(d):
            m = d[p:p + 4]
            if m == b'CMAP':
                cb, ce, meth = struct.unpack_from('<IIH', d, p + 8)
                body = p + 0x18
                if meth == 0:
                    f = struct.unpack_from('<H', d, body)[0]
                    for c in range(cb, ce + 1):
                        self.cmap[c] = f + c - cb
                elif meth == 1:
                    for i, c in enumerate(range(cb, ce + 1)):
                        g = struct.unpack_from('<H', d, body + 2 * i)[0]
                        if g != 0xFFFF:
                            self.cmap[c] = g
                elif meth == 2:
                    for i in range(struct.unpack_from('<H', d, body)[0]):
                        c, g = struct.unpack_from('<IH', d, body + 4 + 8 * i)
                        self.cmap[c] = g
            elif m == b'CWDH':
                first, last = struct.unpack_from('<HH', d, p + 8)
                for i in range(last - first + 1):
                    self.widths[first + i] = struct.unpack_from('<bBB', d, p + 16 + 3 * i)
            p += 4

    def sheet(self, i):
        if i not in self._sheets:
            src = self.d[self.sheet_at[i]:self.sheet_at[i] + self.bytes]
            lin = deswizzle(src, self.sw // 4, self.sh // 4, 8, self.bh)
            hdr = bytearray(128)
            struct.pack_into('<4sIIIIII', hdr, 0, b'DDS ', 124, 0x1007 | 0x80000, self.sh, self.sw, len(lin), 0)
            struct.pack_into('<II4s', hdr, 76, 32, 4, b'ATI1')
            struct.pack_into('<I', hdr, 108, 0x1000)
            self._sheets[i] = ImageOps.flip(Image.open(io.BytesIO(bytes(hdr) + lin)).convert('L'))  # (rows bottom-up)
        return self._sheets[i]

    def render(self, text, height, color=(0, 0, 0, 255), tracking=0.0):
        """RGBA image of `text`, a glyph cell `height` px tall"""
        k = height / self.ch
        parts, x = [], 0.0
        for c in text:
            g = self.cmap.get(ord(c))
            left, _, adv = self.widths.get(g, (0, self.cw, self.cw)) if g is not None else (0, 0, self.cw // 3)
            if g is not None:
                sh_, n = divmod(g, self.cols * self.rows)
                cx, cy = (n % self.cols) * self.cw, (n // self.cols) * self.ch
                parts.append((x + left * k, self.sheet(sh_).crop((cx, cy, cx + self.cw - 1, cy + self.ch - 1))))
            x += (adv + tracking) * k
        out = Image.new('L', (max(1, int(x + self.cw * k)), max(1, round(self.ch * k))), 0)
        for px, img in parts:
            g = img.resize((max(1, round(img.width * k)), max(1, round(img.height * k))), Image.LANCZOS)
            out.paste(g, (round(px), 0), g)
        bbox = out.getbbox() or (0, 0, 1, 1)
        out = out.crop((0, 0, max(bbox[2], 1), out.height))
        im = Image.new('RGBA', out.size, tuple(color[:3]) + (0,))
        im.putalpha(out.point(lambda v: v * color[3] // 255))
        return im


def shear(im, k):
    """the game's lean: rows shifted right towards the top"""
    w, h = im.size
    return im.transform((w + int(h * k) + 2, h), Image.AFFINE, (1, k, -h * k, 0, 1, 0), Image.BICUBIC)


# ---------------------------------------------------------------- Kenney's device outlines (SVG: one path, M / L / Q)
def svg_paths(path):
    import re
    d = ' '.join(re.findall(r'\sd="([^"]+)"', open(path, encoding='utf-8').read()))
    toks = re.findall(r'[MLQZmlqz]|-?\d*\.?\d+(?:e-?\d+)?', d)
    subs, cur, i, cmd, pos = [], [], 0, None, (0.0, 0.0)
    while i < len(toks):
        t = toks[i]
        if t in 'MLQZmlqz':
            cmd = t
            i += 1
            if cmd in 'Zz' and cur:
                subs.append(cur)
                cur = []
            continue
        if cmd == 'M':
            if cur:
                subs.append(cur)
            pos = (float(toks[i]), float(toks[i + 1]))
            cur, cmd = [pos], 'L'
            i += 2
        elif cmd == 'L':
            pos = (float(toks[i]), float(toks[i + 1]))
            cur.append(pos)
            i += 2
        elif cmd == 'Q':
            c, e, p0 = (float(toks[i]), float(toks[i + 1])), (float(toks[i + 2]), float(toks[i + 3])), pos
            for k in range(1, 13):
                u = k / 12
                cur.append(((1 - u) ** 2 * p0[0] + 2 * (1 - u) * u * c[0] + u * u * e[0],
                            (1 - u) ** 2 * p0[1] + 2 * (1 - u) * u * c[1] + u * u * e[1]))
            pos = e
            i += 4
        else:
            i += 1
    if cur:
        subs.append(cur)
    return [p for p in subs if len(p) > 2]


def _area(p):
    return abs(sum(p[i][0] * p[i - 1][1] - p[i - 1][0] * p[i][1] for i in range(len(p)))) / 2


def _bounds(p):
    xs, ys = [q[0] for q in p], [q[1] for q in p]
    return min(xs), min(ys), max(xs), max(ys)


def svg_device(path, width, body, detail):
    """the outline in `body`, the shapes inside it (buttons, sticks: holes in the original) in `detail`"""
    subs = svg_paths(path)
    boxes = [_bounds(p) for p in subs]
    x0, y0 = min(b[0] for b in boxes), min(b[1] for b in boxes)
    x1, y1 = max(b[2] for b in boxes), max(b[3] for b in boxes)
    k = width / (x1 - x0)
    im = Image.new('RGBA', (int(width) + 2, int((y1 - y0) * k) + 2), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    outers = []
    for i in sorted(range(len(subs)), key=lambda i: -_area(subs[i])):
        inner = any(boxes[j][0] <= boxes[i][0] + 0.01 and boxes[j][1] <= boxes[i][1] + 0.01 and
                    boxes[i][2] <= boxes[j][2] + 0.01 and boxes[i][3] <= boxes[j][3] + 0.01 for j in outers)
        d.polygon([((x - x0) * k + 1, (y - y0) * k + 1) for x, y in subs[i]], fill=detail if inner else body)
        if not inner:
            outers.append(i)
    return im


# ---------------------------------------------------------------- the REARMED tag (as the game's "ARMS LIVE")
LEAN = 0.30


def word_mask(special, height):
    """REARMED as the game sets the word beside its logo: its display face, set wide, leaning; an L mask `height` tall"""
    m = special.render('REARMED', int(height * 1.85), (255, 255, 255, 255), tracking=-2).split()[3]
    m = m.crop(m.getbbox())
    m = m.resize((int(m.width * 1.14), m.height), Image.LANCZOS)
    m = m.resize((int(m.width * height / m.height), height), Image.LANCZOS)
    return shear(m, LEAN)


def _tag_shape(w, h):
    """a block with only its bottom right rounded (the game's title tag)"""
    m = Image.new('L', (w, h), 0)
    d = ImageDraw.Draw(m)
    r = max(2, h // 8)
    d.rounded_rectangle([0, 0, w, h], radius=r, fill=255)
    d.rectangle([0, 0, w - r, h - r], fill=255)
    d.rectangle([0, r, r * 2, h], fill=255)
    return m


def tag_mask(special, cap):
    """the one-colour tag, for logos the game tints: REARMED cut out of a leaning block whose left end is hatched with
    thin diagonal stripes (LIVE's stripes)"""
    word = word_mask(special, cap)
    h, pad, hatch = int(cap * 1.55), int(cap * 0.55), int(cap * 1.1)
    w = hatch + word.width + 2 * pad
    blk = _tag_shape(w, h)
    hm = Image.new('L', (hatch, h), 0)
    hp = hm.load()
    period, line = max(4, cap // 4), max(2, cap // 10)
    for y in range(h):
        for x in range(hatch):
            if (x + int(y * 0.9)) % period < line:
                hp[x, y] = 255
    blk.paste(0, (0, 0), hm)
    blk = shear(blk, LEAN)
    hole = Image.new('L', blk.size, 0)
    hole.paste(word, (int(h * LEAN / 2) + hatch + pad - int(cap * 0.05), (h - word.height) // 2))
    return ImageChops.subtract(blk, hole)


def colour_tag(special, cap):
    """the tag in colour: a charcoal block with light diagonal stripes, the word in white"""
    word = word_mask(special, cap)
    h, pad = int(cap * 1.55), int(cap * 0.6)
    w = word.width + 2 * pad
    blk = Image.new('RGBA', (w, h))
    bp = blk.load()
    period, line = max(5, cap // 3), max(2, cap // 9)
    for y in range(h):
        for x in range(w):
            bp[x, y] = (88, 88, 88, 255) if (x + int(y * 0.66)) % period < line else (28, 28, 28, 255)
    blk.putalpha(_tag_shape(w, h))
    blk = shear(blk, LEAN)
    wl = Image.new('RGBA', word.size, (255, 255, 255, 255))
    wl.putalpha(word)
    blk.alpha_composite(wl, (int(h * LEAN / 2) + pad - int(cap * 0.1), (h - word.height) // 2))
    return blk


# ---------------------------------------------------------------- the boot splash
S = 2  # (drawn at twice the size, then reduced: smooth edges)
W, H, PANEL = 1600, 900, 1066
YEL, YEL2, LOGO_Y = (255, 222, 0), (253, 245, 0), (239, 239, 0)
DARK, GREY = (35, 24, 21), (73, 67, 65)
WHITE, BLACK = (255, 255, 255), (0, 0, 0)


def s(v):
    return int(round(v * S))


def keyboard_mouse(kenney):
    """a full keyboard in the style of Kenney's caps (WASD in the game's yellow) and Kenney's mouse (its pieces dark,
    the wheel grey: the seams show white once the rim is added)"""
    kw, kh = 300, 122
    im = Image.new('RGBA', (s(kw + 128), s(kh + 46)), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    d.rounded_rectangle([0, s(18), s(kw), s(18 + kh)], radius=s(22), fill=DARK + (255,))
    key, gap, top = 18.5, 4.2, 18 + 12
    lit = {(1, 2), (2, 1), (2, 2), (2, 3)}
    for r, (indent, n) in enumerate([(0, 12), (6, 12), (11, 11), (17, 10)]):
        for i in range(n):
            x, y = 12 + indent + i * (key + gap), top + r * (key + gap)
            if x + key <= kw - 10:
                d.rounded_rectangle([s(x), s(y), s(x + key), s(y + key)], radius=s(5),
                                    fill=(LOGO_Y if (r, i) in lit else GREY) + (255,))
    y = top + 4 * (key + gap)
    d.rounded_rectangle([s(82), s(y), s(kw - 72), s(y + key - 2)], radius=s(5), fill=GREY + (255,))
    subs = svg_paths(os.path.join(kenney, 'mouse.svg'))
    boxes = [_bounds(p) for p in subs]
    x0, y0, x1 = min(b[0] for b in boxes), min(b[1] for b in boxes), max(b[2] for b in boxes)
    k = s(96) / (x1 - x0)
    small = min(range(len(subs)), key=lambda i: _area(subs[i]))
    for i, p in enumerate(subs):
        d.polygon([((x - x0) * k + s(kw + 22), (y - y0) * k + s(26)) for x, y in p],
                  fill=(GREY if i == small else DARK) + (255,))
    return im


def keycap(canvas, body, x, y, label, h=30, mouse=None):
    d = ImageDraw.Draw(canvas)
    if mouse:
        w = h * 0.68
        d.rounded_rectangle([s(x), s(y), s(x + w), s(y + h)], radius=s(w / 2), outline=WHITE, width=s(2.2))
        d.line([s(x + w / 2), s(y + 2), s(x + w / 2), s(y + h * 0.44)], fill=WHITE, width=s(2))
        d.line([s(x + 2), s(y + h * 0.44), s(x + w - 2), s(y + h * 0.44)], fill=WHITE, width=s(2))
        half = (x + 2, y + 2, x + w / 2, y + h * 0.44) if mouse == 'L' else (x + w / 2, y + 2, x + w - 2, y + h * 0.44)
        body_m = Image.new('L', canvas.size, 0)
        ImageDraw.Draw(body_m).rounded_rectangle([s(x), s(y), s(x + w), s(y + h)], radius=s(w / 2), fill=255)
        box = Image.new('L', canvas.size, 0)
        ImageDraw.Draw(box).rectangle([s(v) for v in half], fill=255)
        canvas.paste(WHITE, mask=ImageChops.multiply(body_m, box))
        return w
    im = body.render(label, s(h * 0.95), BLACK + (255,))
    w = max(h, im.width / S + h * 0.5)
    d.rounded_rectangle([s(x), s(y), s(x + w), s(y + h)], radius=s(h * 0.22), fill=WHITE)
    canvas.alpha_composite(im, (s(x + w / 2) - im.width // 2, s(y + h / 2) - im.height // 2 + s(h * 0.04)))
    return w


def splash(special, body, logo_l, curve_l, kenney, credits):
    cv = Image.new('RGBA', (s(W), s(H)), (12, 12, 12, 255))

    def text(font, t, x, y, height, color, slant=0.0, anchor='l'):
        im = font.render(t, s(height), color + (255,))
        if slant:
            im = shear(im, slant)
        bx = s(x) - (im.width if anchor == 'r' else im.width // 2 if anchor == 'c' else 0)
        cv.alpha_composite(im, (bx, s(y)))
        return im.width / S

    # the yellow side: the game's gradient (orange-yellow middle, lemon edges) and its light curve
    grad = Image.new('RGB', (s(PANEL) // 4, s(H) // 4))
    gp = grad.load()
    for yy in range(grad.height):
        for xx in range(grad.width):
            t = min(1.0, math.hypot((xx / grad.width - 0.48) * 1.1, (yy / grad.height - 0.36) * 1.4) * 1.55)
            gp[xx, yy] = tuple(int(YEL[i] + (YEL2[i] - YEL[i]) * t) for i in range(3))
    cv.paste(grad.resize((s(PANEL), s(H)), Image.BICUBIC), (0, 0))
    curve = curve_l.resize((s(1180), s(1180)), Image.LANCZOS).rotate(28, resample=Image.BICUBIC)
    tint = Image.new('RGBA', curve.size, (255, 236, 40, 255))
    tint.putalpha(curve.point(lambda v: v * 120 // 255))
    layer = Image.new('RGBA', cv.size, (0, 0, 0, 0))
    layer.alpha_composite(tint, (s(-210), s(-260)))
    side = cv.crop((0, 0, s(PANEL), s(H)))
    side.alpha_composite(layer.crop((0, 0, s(PANEL), s(H))))
    cv.paste(side, (0, 0))
    d = ImageDraw.Draw(cv)

    # the bar with the logo (its own mask, in its yellow), then REARMED as the game sets LIVE beside it
    bar_t, bar_b, slant = 30, 107, 22
    d.polygon([(0, s(bar_t)), (s(392), s(bar_t)), (s(392 - slant), s(bar_b)), (0, s(bar_b))], fill=BLACK)
    mask = logo_l.point(lambda v: 255 - v).resize((s(300), s(75)), Image.LANCZOS)
    ly = Image.new('RGBA', mask.size, LOGO_Y + (255,))
    ly.putalpha(mask)
    cv.alpha_composite(ly, (s(66), s(31)))
    word = special.render('REARMED', s(96), WHITE + (255,), tracking=-2)
    word = word.crop(word.getbbox())
    word = word.resize((int(word.width * 1.14), word.height), Image.LANCZOS)
    lean = slant / (bar_b - bar_t)
    word = shear(word, lean)
    bw, bh = word.width / S + 60, bar_b - bar_t
    blk = Image.new('RGBA', (s(bw + bh * lean + 4), s(bh)), (0, 0, 0, 0))
    bp = blk.load()
    for y in range(blk.height):
        for x in range(blk.width):
            bp[x, y] = (88, 88, 88, 255) if (x + int(y * lean * 2.2)) % s(9) < s(3) else (28, 28, 28, 255)
    shape = Image.new('L', blk.size, 0)
    sd = ImageDraw.Draw(shape)
    sd.rounded_rectangle([0, 0, s(bw), s(bh)], radius=s(9), fill=255)
    sd.rectangle([0, 0, s(bw) - s(12), s(bh) - s(12)], fill=255)
    sd.rectangle([0, s(12), s(20), s(bh)], fill=255)
    blk.putalpha(shape.transform(shape.size, Image.AFFINE, (1, lean, -shape.height * lean, 0, 1, 0), Image.BICUBIC))
    blk.alpha_composite(word, (s(bh * lean / 2 + bw / 2 - 6) - word.width // 2, s(bh / 2) - word.height // 2))
    cv.alpha_composite(blk, (s(400 - bh * lean), s(bar_t)))

    text(special, 'Play it your way!', 66, 116, 118, BLACK, slant=0.2)

    # the ways to play: dark devices with a white sticker rim (the white of the game's hands) and a shadow
    kb = keyboard_mouse(kenney)
    kb = kb.resize((int(kb.width * 0.8), int(kb.height * 0.8)), Image.LANCZOS)
    ps = svg_device(os.path.join(kenney, 'controller_playstation5.svg'), s(252), DARK + (255,), GREY + (255,))
    xb = svg_device(os.path.join(kenney, 'controller_xboxseries.svg'), s(252), DARK + (255,), GREY + (255,))
    for dev, cx, cy, ang in ((kb, 204, 460, 6), (ps, 540, 450, -5), (xb, 868, 460, 5)):
        lay = Image.new('RGBA', (dev.width + s(60), dev.height + s(60)), (0, 0, 0, 0))
        lay.alpha_composite(dev, (s(30), s(30)))
        lay = lay.rotate(ang, resample=Image.BICUBIC, expand=True)
        at = (s(cx) - lay.width // 2, s(cy) - lay.height // 2)
        rim = lay.split()[3].point(lambda v: 255 if v > 40 else 0)
        rim = rim.filter(ImageFilter.MaxFilter(s(9) | 1)).filter(ImageFilter.MaxFilter(s(9) | 1))
        rim = rim.filter(ImageFilter.GaussianBlur(s(1.6))).point(
            lambda v: 0 if v < 70 else 255 if v > 185 else (v - 70) * 255 // 115)
        sh = Image.new('RGBA', lay.size, (170, 110, 0, 0))
        sh.putalpha(rim.filter(ImageFilter.GaussianBlur(s(9))).point(lambda v: v * 80 // 255))
        cv.alpha_composite(sh, (at[0] + s(9), at[1] + s(12)))
        wr = Image.new('RGBA', lay.size, WHITE + (0,))
        wr.putalpha(rim)
        cv.alpha_composite(wr, at)
        cv.alpha_composite(lay, at)
    for label, cx in (('Keyboard & mouse', 200), ('PlayStation', 540), ('Xbox', 868)):
        text(special, label, cx, 668, 54, BLACK, slant=0.2, anchor='c')

    d.rectangle([0, s(838), s(PANEL), s(H)], fill=WHITE)
    text(body, 'The button icons follow what you play with: keys, PlayStation or Xbox buttons.', 62, 853, 32, (30, 30, 30))

    # the panel: what this is, who made it, the keys
    px = 1112
    w1 = text(special, 'ARMS', px, 36, 84, LOGO_Y, slant=0.2)
    text(special, 'Rearmed', px + w1 + 4, 36, 84, WHITE, slant=0.2)
    y = 118
    for line in ('A native PC version of ARMS, recompiled', 'from the Switch game you dumped yourself.'):
        text(body, line, px, y, 30, WHITE)
        y += 30
    y += 16
    for line, col in zip(credits, (LOGO_Y, (200, 200, 200), (150, 150, 150))):
        text(body, line, px, y, 28, col)
        y += 30
    y += 26
    d.line([s(px), s(y), s(1560), s(y)], fill=(70, 70, 70), width=s(1.5))
    y += 22
    text(special, 'Keyboard & mouse', px, y, 50, WHITE, slant=0.2)
    y += 54
    for keys, what in ((['mL', 'mR'], 'Punch (left / right)'), ([' Space '], 'Jump'), (['Shift'], 'Dash'),
                       (['Ctrl'], 'Guard'), (['Q', 'E'], 'Rush'), (['WASD'], 'Move, curve punches'),
                       (['Esc'], 'Menu: controls, video, audio')):
        x = px
        for k in keys:
            x += keycap(cv, body, x, y, k, 30, mouse=k[1] if k.startswith('m') else None) + 8
        text(body, what, max(x + 6, px + 150), y + 1, 30, (225, 225, 225))
        y += 42
    text(body, 'Every control can be rebound (Esc, then Controls).', px, y + 10, 26, (150, 150, 150))
    # (above the corner the game keeps for its red ARMS emblem, drawn over the splash: x 1325.., y 835..)
    text(body, 'ARMS is a trademark of Nintendo.', px, 778, 24, (120, 120, 120))
    text(body, 'Not affiliated with or endorsed by Nintendo.', px, 802, 24, (120, 120, 120))
    return cv.resize((W, H), Image.LANCZOS)


# ---------------------------------------------------------------- the intro logo and the loading circle
def intro_logo(special, orig):
    """Logo_Title_04 (its R: 0 = the logo, 255 = around it): the logo a little smaller, the tag under it, aligned with
    the S, the two centred in the texture"""
    lum = orig.split()[0]
    logo = lum.crop(ImageOps.invert(lum).getbbox())
    f = 0.71
    logo = logo.resize((int(logo.width * f), int(logo.height * f)), Image.LANCZOS)
    cap = int(logo.height * 0.27)
    tag = tag_mask(special, cap)
    gap = int(logo.height * 0.05)  # (a little air between the letters and the tag)
    gw, gh = logo.width, logo.height + gap + tag.height
    out = Image.new('L', lum.size, 255)
    x0, y0 = (lum.width - gw) // 2, (lum.height - gh) // 2
    out.paste(logo, (x0, y0))
    out.paste(0, (x0 + gw - int(logo.width * 0.05) - tag.width, y0 + logo.height + gap), tag)
    return Image.merge('RGBA', (out, out, out, Image.new('L', out.size, 255)))


def loading_circle(special, orig):
    """Fade_Logo_00 (a white disc: the emblem, the logo, a small line under it): the tag in place of that line"""
    im = orig.copy()
    ImageDraw.Draw(im).rectangle([150, 470, 570, 520], fill=(255, 255, 255, 255))
    tag = colour_tag(special, 30)
    im.alpha_composite(tag, (im.width // 2 - tag.width // 2 + 20, 468))
    return im


# ---------------------------------------------------------------- all of it
def build(romfs, files, outdir, port_version='0.1.0'):
    g = Game(romfs, files)
    region = next(r for r in ('US', 'EU', 'JP', 'KR') if f'/Font/BmpFont_{r}.szs' in g.files)
    fonts = g.archive(f'/Font/BmpFont_{region}.szs')
    special, body = Font(fonts['ARMSSpecialFont.bffnt']), Font(fonts['ARMSNTLGFont.bffnt'])
    game_version = ''
    try:
        with open(os.path.join(outdir, 'control', 'control.nacp'), 'rb') as f:
            f.seek(0x3060)
            game_version = f.read(0x10).split(b'\0')[0].decode()
    except OSError:
        pass
    credits = ('PC recompilation by VirtuaQuest', 'Built on LAS Recomp (Peiikai and contributors)',
               f'v{port_version}  ·  ARMS {game_version or "5.x"}  ·  offline modes')
    out = os.path.join(outdir, 'branding')
    os.makedirs(out, exist_ok=True)
    fmt, w, h, bh, strap = g.texture('Boot_Strap_00', 'Boot_ControllerStrap^u')
    fmt4, w4, h4, bh4, logo4 = g.texture('Boot_Strap_00', 'Logo_Title_04^r')
    cfmt, cw, ch, cbh, cdata = g.texture('Tutorial_Loading_00', 'Tutorial_BGCurve^s')
    fmt7, w7, h7, bh7, fade = g.texture('Common_FadeShort_00', 'Fade_Logo_00^u')
    logo_img = decode(fmt4, w4, h4, bh4, logo4)
    curve_l = decode(cfmt, cw, ch, cbh, cdata).split()[0]
    jobs = [('Boot_ControllerStrap', fmt, w, h, strap, splash(special, body, logo_img.split()[0], curve_l, _data_dir(), credits)),
            ('Logo_Title_04', fmt4, w4, h4, logo4, intro_logo(special, logo_img)),
            ('Fade_Logo_00', fmt7, w7, h7, fade, loading_circle(special, decode(fmt7, w7, h7, bh7, fade)))]
    lines = []
    for name, fmt_, w_, h_, orig, img in jobs:
        with open(os.path.join(out, name + '.orig'), 'wb') as f:
            f.write(orig)
        with open(os.path.join(out, name + '.rgba'), 'wb') as f:
            f.write(img.convert('RGBA').tobytes())
        img.convert('RGBA').save(os.path.join(out, name + '.png'))  # (to look at; the runtime reads the .rgba)
        lines.append(f'{name} {w_} {h_} {"bc4" if fmt_ >> 8 == 0x1d else "bc7"} {name}.orig {name}.rgba')
    with open(os.path.join(out, 'manifest.txt'), 'w', encoding='utf-8') as f:
        f.write('# ARMS Rearmed logo & splash art, made from your own game files (tools/branding.py)\n')
        f.write('\n'.join(lines) + '\n')
    print(f' branding: {len(jobs)} textures into {out}')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nsp')
    ap.add_argument('keys')
    ap.add_argument('outdir')
    ap.add_argument('--update')
    ap.add_argument('--port-version', default='0.1.0')
    a = ap.parse_args()
    keys = nx.load_keys(a.keys)
    bsec = {sc.idx: sc for sc in nx.open_nsp(a.nsp, keys)['program'].sections}
    if a.update:
        usec = {sc.idx: sc for sc in nx.open_nsp(a.update, keys)['program'].sections}
        romfs = nx.PatchedRomfs(bsec[1], usec[1])
    else:
        romfs = bsec[1]
    build(romfs, nx.romfs_files(romfs), a.outdir, a.port_version)


if __name__ == '__main__':
    main()
