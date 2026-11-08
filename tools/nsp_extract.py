"""Extract a game from its NSPs with the user's own keys (PFS0 > NCA3 > PFS0/RomFS, an update's RomFS as a Bktr patch).

Usage: nsp_extract.py <game.nsp> <prod.keys> <outdir> [--update <update.nsp>] [--dlc <dlc.nsp>] [--list]
                      [--romfs source|extract|none] [--files <text>]

Writes <outdir>/exefs/* from the program NCA (the update's when given), <outdir>/control/** from the control NCA and
<outdir>/romfs.src: where the RomFS is in the NSPs, for the runtime to read it in place (or --romfs extract: every
file to <outdir>/romfs, ~18 GB for MUA3 with its update).
"""
import argparse
import bisect
import os
import struct
import sys

from Crypto.Cipher import AES

MEDIA = 0x200
CONTENT_TYPES = {0: 'program', 1: 'meta', 2: 'control', 3: 'manual', 4: 'data', 5: 'publicdata'}


def load_keys(path):
    keys = {}
    for line in open(path, encoding='utf-8'):
        if '=' in line:
            k, v = line.split('=', 1)
            keys[k.strip()] = bytes.fromhex(v.strip())
    return keys


def pfs0_entries(read, base):
    """[(name, abs_offset, size)] of a PFS0 at `base`, read via read(off, n)."""
    magic, n, strsz, _ = struct.unpack('<4sIII', read(base, 0x10))
    assert magic == b'PFS0', magic
    ents = read(base + 0x10, n * 0x18)
    strs = read(base + 0x10 + n * 0x18, strsz)
    data = base + 0x10 + n * 0x18 + strsz
    out = []
    for i in range(n):
        off, size, so, _ = struct.unpack_from('<QQII', ents, i * 0x18)
        name = strs[so:strs.index(b'\0', so)].decode()
        out.append((name, data + off, size))
    return out


def xts_decrypt_nintendo(key, data, sector0=0, sector_size=0x200):
    """AES-128-XTS with Nintendo's big-endian sector tweak."""
    k1, k2 = AES.new(key[:16], AES.MODE_ECB), AES.new(key[16:32], AES.MODE_ECB)
    out = bytearray()
    for s in range(len(data) // sector_size):
        t = int.from_bytes(k2.encrypt((sector0 + s).to_bytes(16, 'big')), 'little')
        sec = data[s * sector_size:(s + 1) * sector_size]
        for j in range(0, sector_size, 16):
            tb = t.to_bytes(16, 'little')
            x = bytes(a ^ b for a, b in zip(sec[j:j + 16], tb))
            out += bytes(a ^ b for a, b in zip(k1.decrypt(x), tb))
            t = ((t << 1) & ((1 << 128) - 1)) ^ (0x87 if t >> 127 else 0)
    return bytes(out)


class Nca:
    def __init__(self, f, off, size, keys, tickets):
        self.f, self.off, self.size = f, off, size
        f.seek(off)
        hdr = xts_decrypt_nintendo(keys['header_key'], f.read(0xC00))
        assert hdr[0x200:0x204] == b'NCA3', hdr[0x200:0x204]
        self.hdr = hdr
        self.content_type = hdr[0x205]
        self.program_id = struct.unpack_from('<Q', hdr, 0x210)[0]
        rev = max(hdr[0x206], hdr[0x220])
        rev = rev - 1 if rev else 0
        self.keygen = rev
        rights = hdr[0x230:0x240]
        if any(rights):
            enc = tickets[rights]
            self.key = AES.new(keys[f'titlekek_{rev:02x}'], AES.MODE_ECB).decrypt(enc)
        else:
            kak = keys[f'key_area_key_application_{rev:02x}']
            self.key = AES.new(kak, AES.MODE_ECB).decrypt(hdr[0x300 + 2 * 0x10:0x300 + 3 * 0x10])
        self.sections = []
        for i in range(4):
            start, end = struct.unpack_from('<II', hdr, 0x240 + i * 0x10)
            if not end:
                continue
            fsh = hdr[0x400 + i * 0x200:0x600 + i * 0x200]
            self.sections.append(Section(self, i, start * MEDIA, (end - start) * MEDIA, fsh))

    def raw(self, off, n):
        self.f.seek(self.off + off)
        return self.f.read(n)


class Section:
    def __init__(self, nca, idx, start, size, fsh):
        self.nca, self.idx, self.start, self.size, self.fsh = nca, idx, start, size, fsh
        self.fs_type, self.hash_type, self.enc = fsh[2], fsh[3], fsh[4]
        self.ctr_hi = fsh[0x140:0x148][::-1]
        if self.fs_type == 1:  # PFS0 under hierarchical sha256: data region is layer 1
            self.data_off = struct.unpack_from('<Q', fsh, 0x40)[0]
        else:  # RomFS under IVFC: data is the last (6th) level
            assert fsh[0x08:0x0C] == b'IVFC', fsh[0x08:0x0C]
            self.data_off = struct.unpack_from('<Q', fsh, 0x18 + 5 * 0x18)[0]
        assert self.enc in (1, 3, 4), f'section {idx}: unsupported encryption {self.enc}'
        # an update's RomFS (encryption 4, AesCtrEx): a patch over the base game's RomFS (Bktr)
        self.indirect = struct.unpack_from('<QQ4sII', fsh, 0x100)
        self.aesctrex = struct.unpack_from('<QQ4sII', fsh, 0x120)

    def read(self, off, n, generation=None):
        """Read n plaintext bytes at section-relative offset off (with generation: that AesCtrEx counter)."""
        a = self.start + off
        lo, hi = a & ~0xF, (a + n + 0xF) & ~0xF
        data = self.nca.raw(lo, hi - lo)
        if self.enc in (3, 4):
            ctr = self.ctr_hi if generation is None else self.ctr_hi[:4] + generation.to_bytes(4, 'big')
            iv = ctr + (lo >> 4).to_bytes(8, 'big')
            data = AES.new(self.nca.key, AES.MODE_CTR, nonce=b'', initial_value=iv).decrypt(data)
        return data[a - lo:a - lo + n]


def bucket_tree(sec, info, entry_size, fmt):
    """The entries of a Bktr bucket tree [(virtual offset, fields...)] and the tree's end offset.

    A 0x4000-byte header node (count, end offset, then each bucket's start), then the buckets, 0x4000 bytes each:
    (index, count, end offset) and the entries."""
    off, size, magic, _, count = info
    assert magic == b'BKTR', magic
    tree = sec.read(off, size)
    _, nb, end = struct.unpack_from('<IIQ', tree, 0)
    ents = []
    for b in range(nb):
        base = 0x4000 * (b + 1)
        _, ne, _ = struct.unpack_from('<IIQ', tree, base)
        ents += [struct.unpack_from(fmt, tree, base + 0x10 + i * entry_size) for i in range(ne)]
    assert len(ents) == count, (len(ents), count)
    return ents, end


class PatchedRomfs:
    """An update's RomFS: its virtual image is pieces of the base RomFS section (indirect storage 0) and of the
    update's own section (storage 1), and the update's pieces are AES-CTR with a counter generation per range."""

    def __init__(self, base_sec, patch_sec):
        self.base, self.patch = base_sec, patch_sec
        self.ind, self.size = bucket_tree(patch_sec, patch_sec.indirect, 0x14, '<QQi')
        self.ctr, _ = bucket_tree(patch_sec, patch_sec.aesctrex, 0x10, '<QII')
        self.ind_off = [e[0] for e in self.ind]
        self.ctr_off = [e[0] for e in self.ctr]
        self.data_off = patch_sec.data_off

    def _patch_read(self, off, n):
        out = bytearray()
        while n:
            i = bisect.bisect_right(self.ctr_off, off) - 1
            end = self.ctr_off[i + 1] if i + 1 < len(self.ctr) else self.patch.indirect[0]
            k = min(n, end - off)
            out += self.patch.read(off, k, self.ctr[i][2])
            off, n = off + k, n - k
        return bytes(out)

    def read(self, off, n):
        out = bytearray()
        while n:
            i = bisect.bisect_right(self.ind_off, off) - 1
            virt, phys, storage = self.ind[i]
            end = self.ind_off[i + 1] if i + 1 < len(self.ind) else self.size
            k = min(n, end - off)
            p = phys + off - virt
            out += self.base.read(p, k) if storage == 0 else self._patch_read(p, k)
            off, n = off + k, n - k
        return bytes(out)


def copy_out(read, off, size, path, chunk=16 << 20):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, 'wb') as o:
        done = 0
        while done < size:
            n = min(chunk, size - done)
            o.write(read(off + done, n))
            done += n


def extract_pfs0(sec, outdir):
    for name, off, size in pfs0_entries(sec.read, sec.data_off):
        copy_out(sec.read, off, size, os.path.join(outdir, name))
        print(f'  {name:24} {size:>12,}')


def extract_romfs(sec, outdir, verbose=False):
    base = sec.data_off
    h = struct.unpack('<10Q', sec.read(base, 0x50))
    dir_meta = sec.read(base + h[3], h[4])
    file_meta = sec.read(base + h[7], h[8])
    data_base = base + h[9]
    count, total = 0, 0

    def name_at(meta, off, hdrlen):
        nlen = struct.unpack_from('<I', meta, off + hdrlen - 4)[0]
        return meta[off + hdrlen:off + hdrlen + nlen].decode('utf-8')

    def walk_files(foff, path):
        nonlocal count, total
        while foff != 0xFFFFFFFF:
            _, sib, doff, dsize, _, _ = struct.unpack_from('<IIQQII', file_meta, foff)
            fpath = os.path.join(path, name_at(file_meta, foff, 0x20))
            copy_out(sec.read, data_base + doff, dsize, fpath)
            count += 1
            total += dsize
            if verbose or count % 500 == 0:
                print(f'  {count:6} files  {total / 1e9:6.2f} GB  {fpath}', flush=True)
            foff = sib

    def walk_dir(doff, path):
        while doff != 0xFFFFFFFF:
            _, sib, child, files, _, _ = struct.unpack_from('<IIIIII', dir_meta, doff)
            here = os.path.join(path, name_at(dir_meta, doff, 0x18)) if doff else path
            os.makedirs(here, exist_ok=True)
            walk_files(files, here)
            walk_dir(child, here)
            doff = sib if doff else 0xFFFFFFFF

    walk_dir(0, outdir)
    print(f'  romfs: {count} files, {total:,} bytes')


def romfs_files(sec):
    """[(path, offset in the image, size)] of a RomFS image read via sec.read at sec.data_off."""
    base = sec.data_off
    h = struct.unpack('<10Q', sec.read(base, 0x50))
    dir_meta = sec.read(base + h[3], h[4])
    file_meta = sec.read(base + h[7], h[8])
    out = []

    def name_at(meta, off, hdrlen):
        nlen = struct.unpack_from('<I', meta, off + hdrlen - 4)[0]
        return meta[off + hdrlen:off + hdrlen + nlen].decode('utf-8')

    def walk(doff, path):
        while doff != 0xFFFFFFFF:
            _, sib, child, foff, _, _ = struct.unpack_from('<IIIIII', dir_meta, doff)
            here = path + '/' + name_at(dir_meta, doff, 0x18) if doff else path
            while foff != 0xFFFFFFFF:
                _, fsib, off, size, _, _ = struct.unpack_from('<IIQQII', file_meta, foff)
                out.append((here + '/' + name_at(file_meta, foff, 0x20), h[9] + off, size))
                foff = fsib
            walk(child, here)
            doff = sib if doff else 0xFFFFFFFF

    walk(0, '')
    return out


def write_romfs_source(path, romfs, sections):
    """The runtime's description of where the RomFS image is in the NSPs (runtime/romfs_nca.c):
        romfs <image offset in the virtual storage> <image size>
        section <n> <offset in the NSP> <offset in its NCA (the AES-CTR counter)> <size> <counter high> <key> <NSP>
        end <virtual storage size>, ind <virtual> <physical> <section> ... (the update's remapping, by offset)
        ctrend <end>, ctr <offset> <counter generation> ... (the update section's counter generations)"""
    fsh = (romfs.patch if isinstance(romfs, PatchedRomfs) else romfs).fsh
    image = struct.unpack_from('<Q', fsh, 0x18 + 5 * 0x18 + 8)[0]  # (IVFC level 6: the image)
    with open(path, 'w', encoding='utf-8') as o:
        o.write("# The game's RomFS, read in place from your NSPs (tools/nsp_extract.py). Holds your title key.\n")
        o.write(f'romfs {romfs.data_off:#x} {image:#x}\n')
        for i, (nsp, sec) in enumerate(sections):
            o.write(f'section {i} {sec.nca.off + sec.start:#x} {sec.start:#x} {sec.size:#x} {sec.ctr_hi.hex()} '
                    f'{sec.nca.key.hex()} '
                    f'{os.path.abspath(nsp)}\n')
        if isinstance(romfs, PatchedRomfs):
            o.write(f'end {romfs.size:#x}\n')
            for virt, phys, storage in romfs.ind:
                o.write(f'ind {virt:#x} {phys:#x} {storage}\n')
            o.write(f'ctrend {romfs.patch.indirect[0]:#x}\n')
            for off, _, gen in romfs.ctr:
                o.write(f'ctr {off:#x} {gen}\n')
        else:
            o.write(f'end {romfs.size:#x}\nind 0x0 0x0 0\n')


def open_nsp(path, keys):
    """{content kind: Nca} of an NSP."""
    f = open(path, 'rb')
    rd = lambda off, n: (f.seek(off), f.read(n))[1]
    ents = pfs0_entries(rd, 0)
    tickets = {}
    for name, off, size in ents:
        if name.endswith('.tik'):
            t = rd(off, size)
            assert struct.unpack_from('<I', t, 0)[0] == 0x10004, 'expected RSA-2048 ticket'
            tickets[t[0x2A0:0x2B0]] = t[0x180:0x190]
    ncas = {}
    for name, off, size in ents:
        if not name.endswith('.nca') or name.endswith('.cnmt.nca'):
            continue
        nca = Nca(f, off, size, keys, tickets)
        kind = CONTENT_TYPES.get(nca.content_type, '?')
        print(f'{os.path.basename(path)}: {name}  {kind:8} id {nca.program_id:016x}  keygen {nca.keygen}  '
              + '  '.join(f's{s.idx}:{"pfs0" if s.fs_type == 1 else "romfs"} {s.size:,}' for s in nca.sections))
        ncas[kind] = nca
    return ncas


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('nsp', help='the base game')
    ap.add_argument('keys')
    ap.add_argument('outdir')
    ap.add_argument('--update', help="the game's update NSP: its code, and its RomFS patched over the base game's")
    ap.add_argument('--dlc', action='append', default=[], help='an add-on content NSP (its data to <outdir>/aoc)')
    ap.add_argument('--list', action='store_true', help='describe the NCAs only')
    ap.add_argument('--romfs', choices=('source', 'extract', 'none'), default='source',
                    help='source (default): write <outdir>/romfs.src, the RomFS read in place from the NSPs; '
                         'extract: copy every file out to <outdir>/romfs')
    ap.add_argument('--files', help='also copy out the RomFS files whose path contains this text')
    a = ap.parse_args()
    keys = load_keys(a.keys)
    base = open_nsp(a.nsp, keys)
    upd = open_nsp(a.update, keys) if a.update else None
    dlcs = [(d, open_nsp(d, keys)) for d in a.dlc]
    if a.list:
        return
    prog = (upd or base)['program']
    sec = {s.idx: s for s in prog.sections}
    bsec = {s.idx: s for s in base['program'].sections}
    print(' exefs:')
    extract_pfs0(sec[0], os.path.join(a.outdir, 'exefs'))
    if upd:
        romfs = PatchedRomfs(bsec[1], sec[1])
        sources = [(a.nsp, bsec[1]), (a.update, sec[1])]
    else:
        romfs, sources = bsec[1], [(a.nsp, bsec[1])]
    files = romfs_files(romfs)
    print(f' romfs: {len(files)} files, {sum(f[2] for f in files):,} bytes')
    if a.romfs == 'source':
        write_romfs_source(os.path.join(a.outdir, 'romfs.src'), romfs, sources)
        print(f'  {os.path.join(a.outdir, "romfs.src")}: read in place from the NSPs')
    for path, off, size in files:
        if a.romfs == 'extract' or (a.files and a.files in path):
            copy_out(romfs.read, romfs.data_off + off, size, os.path.join(a.outdir, 'romfs', path.lstrip('/')))
            if a.romfs != 'extract':
                print(f'  {path} {size:,}')
    print(' control:')
    extract_romfs((upd or base)['control'].sections[0], os.path.join(a.outdir, 'control'), verbose=True)
    for path, ncas in dlcs:  # (as a RomFS image, served as it is: MUA3's pass holds one empty file, named by a
        for kind in ('publicdata', 'data'):  # path on the developer's drive)
            if kind in ncas:
                d = ncas[kind].sections[0]
                image = struct.unpack_from('<Q', d.fsh, 0x18 + 5 * 0x18 + 8)[0]
                out = os.path.join(a.outdir, 'aoc', f'{ncas[kind].program_id:016x}.romfs')
                copy_out(d.read, d.data_off, image, out)
                print(f' aoc: {out} {image:,} bytes, ' + ', '.join(p for p, _, _ in romfs_files(d)))


if __name__ == '__main__':
    sys.exit(main())
