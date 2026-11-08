"""The window / executable icon from the game's own icon (extracted/control/icon_*.dat: the 256x256 JPEG the Switch
shows on its home menu, from your dump - never committed).

Usage: make_icon.py <extracted/control> <out dir>   ->  <out>/icon.png (window, launcher) and <out>/icon.ico (exe)
"""
import os
import sys

from PIL import Image, ImageDraw

LANGS = ['AmericanEnglish', 'BritishEnglish', 'Japanese', 'French', 'German', 'Italian', 'Spanish',
         'LatinAmericanSpanish', 'CanadianFrench', 'Dutch', 'Portuguese', 'Russian', 'Korean']


def main():
    control, out = sys.argv[1], sys.argv[2]
    src = next((os.path.join(control, 'icon_%s.dat' % l) for l in LANGS
                if os.path.exists(os.path.join(control, 'icon_%s.dat' % l))), None)
    if not src:
        sys.exit('no icon_*.dat in %s' % control)
    im = Image.open(src).convert('RGBA').resize((256, 256), Image.LANCZOS)
    # rounded corners, as the home menu shows it (the square JPEG reads as a hole in a taskbar otherwise)
    mask = Image.new('L', im.size, 0)
    ImageDraw.Draw(mask).rounded_rectangle((0, 0, 255, 255), radius=36, fill=255)
    im.putalpha(mask)
    os.makedirs(out, exist_ok=True)
    im.save(os.path.join(out, 'icon.png'))
    im.save(os.path.join(out, 'icon.ico'), sizes=[(16, 16), (24, 24), (32, 32), (48, 48), (64, 64), (128, 128), (256, 256)])
    print('icon from', os.path.basename(src))


if __name__ == '__main__':
    main()
