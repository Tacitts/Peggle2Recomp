"""Extract files from an Xbox 360 STFS package (CON/LIVE/PIRS).

Usage: python -I stfs_extract.py <package> <out_dir> [--list] [--verify]

Implements the STFS block layout (interleaved level 0/1/2 hash tables,
male/female packages with one or two copies of each hash table).
"""
import hashlib
import os
import struct
import sys

BLOCK = 0x1000
L0 = 0xAA
L1 = 0xAA * 0xAA  # 0x70E4


def u24le(b, o):
    return b[o] | (b[o + 1] << 8) | (b[o + 2] << 16)


class Stfs:
    def __init__(self, path):
        self.f = open(path, "rb")
        hdr = self.f.read(0x971A)
        self.magic = hdr[:4]
        if self.magic not in (b"CON ", b"LIVE", b"PIRS"):
            raise ValueError("not an STFS package: %r" % self.magic)
        self.header_size = struct.unpack_from(">I", hdr, 0x340)[0]
        self.content_type = struct.unpack_from(">I", hdr, 0x344)[0]
        self.title_id = struct.unpack_from(">I", hdr, 0x360)[0]
        vd = hdr[0x379:0x379 + 0x24]
        self.block_sep = vd[2]
        self.ft_block_count = vd[3] | (vd[4] << 8)
        self.ft_block = u24le(vd, 5)
        self.total_blocks = struct.unpack_from(">I", vd, 0x1C)[0]
        self.display_name = hdr[0x411:0x411 + 0x80].decode("utf-16-be").split("\0")[0]
        self.sex = (~self.block_sep) & 1  # 0 = female (1 table copy), 1 = male (2)
        self.step = (0xAB, 0x718F) if self.sex == 0 else (0xAC, 0x723A)
        self.first = (self.header_size + 0xFFF) & 0xFFFFF000
        if self.total_blocks <= L0:
            self.top = 0
        elif self.total_blocks <= L1:
            self.top = 1
        else:
            self.top = 2
        self._cache = {}

    # --- block address math -------------------------------------------------
    def data_backing(self, b):
        r = (((b + L0) // L0) << self.sex) + b
        if b < L0:
            return r
        if b < L1:
            return r + (((b + L1) // L1) << self.sex)
        return (1 << self.sex) + r + (((b + L1) // L1) << self.sex)

    def block_addr(self, b):
        return (self.data_backing(b) << 12) + self.first

    def hash_backing(self, b, level):
        if level == 0:
            n = (b // L0) * self.step[0]
            if b >= L0:
                n += ((b // L1) + 1) << self.sex
                if b >= L1:
                    n += 1 << self.sex
            return n
        if level == 1:
            if b < L1:
                return self.step[0]
            return (b // L1) * self.step[1] + (1 << self.sex)
        return self.step[1]

    def _read_at(self, addr, n):
        self.f.seek(addr)
        return self.f.read(n)

    def _table(self, b, level, active):
        addr = (self.hash_backing(b, level) << 12) + self.first
        if self.sex == 1 and active:
            addr += BLOCK
        key = addr
        t = self._cache.get(key)
        if t is None:
            t = self._read_at(addr, BLOCK)
            if len(self._cache) > 64:
                self._cache.clear()
            self._cache[key] = t
        return t

    def hash_entry(self, b):
        """Return (sha1, status, next_block) for data block b."""
        active = (self.block_sep >> 1) & 1
        for level in range(self.top, 0, -1):
            t = self._table(b, level, active)
            idx = (b // (L0 ** level)) % L0
            status = t[idx * 0x18 + 0x14]
            active = (status >> 6) & 1
        t = self._table(b, 0, active)
        e = t[(b % L0) * 0x18:(b % L0) * 0x18 + 0x18]
        nxt = (e[0x15] << 16) | (e[0x16] << 8) | e[0x17]
        return e[:0x14], e[0x14], nxt

    def read_block(self, b, verify=False):
        data = self._read_at(self.block_addr(b), BLOCK)
        if verify:
            h, _, _ = self.hash_entry(b)
            if hashlib.sha1(data).digest() != h:
                raise IOError("hash mismatch at block 0x%X" % b)
        return data

    def chain(self, start, count, consecutive):
        b = start
        for _ in range(count):
            yield b
            b = b + 1 if consecutive else self.hash_entry(b)[2]

    # --- file table -------------------------------------------------------
    def entries(self):
        raw = b"".join(self.read_block(b) for b in self.chain(self.ft_block, self.ft_block_count, False))
        out = []
        for i in range(0, len(raw), 0x40):
            e = raw[i:i + 0x40]
            flags = e[0x28]
            nlen = flags & 0x3F
            if nlen == 0:
                break
            out.append({
                "index": len(out),
                "name": e[:nlen].decode("latin-1"),
                "dir": bool(flags & 0x80),
                "consecutive": bool(flags & 0x40),
                "blocks": u24le(e, 0x29),
                "start": u24le(e, 0x2F),
                "parent": struct.unpack_from(">h", e, 0x32)[0],
                "size": struct.unpack_from(">I", e, 0x34)[0],
            })
        return out

    def path_of(self, ents, e):
        parts = [e["name"]]
        p = e["parent"]
        while p != -1:
            parts.append(ents[p]["name"])
            p = ents[p]["parent"]
        return "/".join(reversed(parts))


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = {a for a in sys.argv[1:] if a.startswith("--")}
    if len(args) < 1:
        print(__doc__)
        return 2
    s = Stfs(args[0])
    print("%s package, title %08X, '%s', header 0x%X, blocks %d, sex %d, top level %d"
          % (s.magic.decode(), s.title_id, s.display_name, s.header_size, s.total_blocks, s.sex, s.top))
    ents = s.entries()
    verify = "--verify" in flags
    for e in ents:
        path = s.path_of(ents, e)
        if "--list" in flags or len(args) < 2:
            print("%s %10d  blk %6d  %s" % ("D" if e["dir"] else "F", e["size"], e["start"], path))
            continue
        dst = os.path.join(args[1], *path.split("/"))
        if e["dir"]:
            os.makedirs(dst, exist_ok=True)
            continue
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        remaining = e["size"]
        with open(dst, "wb") as o:
            for b in s.chain(e["start"], e["blocks"], e["consecutive"]):
                d = s.read_block(b, verify)
                o.write(d[:min(remaining, BLOCK)])
                remaining -= BLOCK
                if remaining <= 0:
                    break
        print("extracted %s (%d bytes)" % (path, e["size"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
