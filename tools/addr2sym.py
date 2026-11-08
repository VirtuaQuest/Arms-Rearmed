"""Name guest addresses: the nearest function symbol of the module they fall in (main, sdk, subsdk1: glslc).

Usage: addr2sym.py <addr> [<addr> ...]   (hex, with or without 0x; f_<addr> as in crash logs works too)
"""
import bisect
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from nso import Nso  # noqa: E402

EXEFS = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'extracted', 'exefs')
MODULES = [('main', 0x7000000000), ('sdk', 0x7006000000), ('subsdk1', 0x7007000000)]


def tables():
    out = []
    for name, base in MODULES:
        n = Nso(os.path.join(EXEFS, name))
        syms = sorted((base + s[3], s[0]) for s in n.syms if s[2] and (s[1] & 0xF) == 2 and s[0])
        out.append((name, base, base + n.size, syms, [a for a, _ in syms]))
    return out


def main():
    t = tables()
    for arg in sys.argv[1:]:
        a = int(arg.removeprefix('f_'), 16)
        for name, lo, hi, syms, addrs in t:
            if not lo <= a < hi:
                continue
            i = bisect.bisect_right(addrs, a) - 1
            sym = f'{syms[i][1]} + {a - syms[i][0]:#x}' if i >= 0 else '?'
            print(f'{a:#x}  {name}+{a - lo:#x}  {sym}')
            break
        else:
            print(f'{a:#x}  (not in a module)')


if __name__ == '__main__':
    main()
