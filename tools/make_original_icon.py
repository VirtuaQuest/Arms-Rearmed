"""ARMS Rearmed's own icon, drawn here (no game art): the launcher's look - yellow with darker diagonal stripes, a black
band, and a pink and a cyan coil spring meeting in the middle. The release's exes and windows use it (Makefile
RELEASE=1); a source build with your own dump can still use the game's icon (tools/make_icon.py).

Usage: make_original_icon.py <out dir>   ->  <out>/icon.png (512x512) and <out>/icon.ico (16..256)
"""
import math
import os
import sys

from PIL import Image, ImageDraw

YELLOW, YELLOW2 = (255, 225, 10, 255), (255, 205, 0, 255)
BLACK = (18, 18, 22, 255)
PINK, PINK_D = (255, 45, 120, 255), (170, 20, 80, 255)
CYAN, CYAN_D = (25, 211, 255, 255), (10, 120, 160, 255)
WHITE = (255, 255, 255, 255)
S = 1024  # drawn large, scaled down


def spring(d, p, q, radius, loops, col, shade, width):
    dx, dy = q[0] - p[0], q[1] - p[1]
    length = math.hypot(dx, dy)
    ux, uy = dx / length, dy / length
    nx, ny = -uy, ux
    seg = loops * 32
    prev = None
    for i in range(seg + 1):
        t = i / seg
        a = t * loops * 2 * math.pi
        along = t * length + math.sin(a) * radius * 0.55
        side = math.cos(a) * radius
        pt = (p[0] + ux * along + nx * side, p[1] + uy * along + ny * side)
        if prev:
            d.line([prev, pt], fill=col if math.sin(a) > 0 else shade, width=width)
            r = width / 2
            d.ellipse([pt[0] - r, pt[1] - r, pt[0] + r, pt[1] + r], fill=col if math.sin(a) > 0 else shade)
        prev = pt


def glove(d, c, r, col, shade):
    d.ellipse([c[0] - r - 18, c[1] - r - 18, c[0] + r + 18, c[1] + r + 18], fill=BLACK)
    d.ellipse([c[0] - r, c[1] - r, c[0] + r, c[1] + r], fill=col)
    d.ellipse([c[0] - r * 0.62, c[1] - r * 0.62, c[0] + r * 0.98, c[1] + r * 0.98], fill=shade)  # (shading)
    d.ellipse([c[0] - r * 0.98, c[1] - r * 0.98, c[0] + r * 0.62, c[1] + r * 0.62], fill=col)
    d.ellipse([c[0] - r * 0.55, c[1] - r * 0.62, c[0] - r * 0.05, c[1] - r * 0.22], fill=WHITE)  # (shine)


def main():
    out = sys.argv[1]
    im = Image.new('RGBA', (S, S), YELLOW)
    d = ImageDraw.Draw(im)
    w = 120
    for x in range(-S - 2 * w, S + w, 2 * w):  # diagonal stripes
        d.polygon([(x, 0), (x + w, 0), (x + w + S, S), (x + S, S)], fill=YELLOW2)
    d.polygon([(0, S * 0.80), (S, S * 0.66), (S, S), (0, S)], fill=BLACK)  # the black band
    spring(d, (-60, S * 0.86), (S * 0.40, S * 0.56), 92, 6, PINK, PINK_D, 34)
    spring(d, (S + 60, S * 0.16), (S * 0.62, S * 0.42), 80, 5, CYAN, CYAN_D, 30)
    glove(d, (S * 0.43, S * 0.54), 150, PINK, PINK_D)
    glove(d, (S * 0.63, S * 0.40), 128, CYAN, CYAN_D)
    mask = Image.new('L', (S, S), 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, S - 1, S - 1), radius=150, fill=255)
    im.putalpha(mask)
    os.makedirs(out, exist_ok=True)
    im.resize((512, 512), Image.LANCZOS).save(os.path.join(out, 'icon.png'))
    im.resize((256, 256), Image.LANCZOS).save(
        os.path.join(out, 'icon.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print('icon:', os.path.join(out, 'icon.png'))


if __name__ == '__main__':
    main()
