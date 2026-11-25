"""Breadth-first static call graph from a guest function (direct bl only, pdata bounds),
listing every import thunk reached within N levels.
Usage: callgraph.py image.bin imports.txt 0x8230A968 [depth]"""
import bisect
import struct
import sys

img = open(sys.argv[1], "rb").read()
base = 0x82000000
thunks = {}
for line in open(sys.argv[2], encoding="utf-8"):
    p = line.split()
    if p and p[0] == "FUNC":
        thunks[int(p[2], 16)] = p[3].replace("__imp__", "")
pd = img[0x82105600 - base:0x82105600 - base + 0x30F50]
ents = sorted((b, ((d >> 8) & 0x3FFFFF) * 4) for b, d in (struct.unpack_from(">II", pd, o) for o in range(0, len(pd), 8)) if b)
starts = [e[0] for e in ents]
sizes = dict(ents)


def size_of(fn):
    if fn in sizes:
        return sizes[fn]
    # leaf without pdata: scan to first blr
    a = fn
    while a < fn + 0x2000:
        if struct.unpack_from(">I", img, a - base)[0] == 0x4E800020:
            return a + 4 - fn
        a += 4
    return 0x200


def callees(fn):
    out = []
    for a in range(fn, fn + size_of(fn), 4):
        w = struct.unpack_from(">I", img, a - base)[0]
        if (w >> 26) == 18:   # b / bl
            d = w & 0x3FFFFFC
            if d & 0x2000000:
                d -= 0x4000000
            t = a + d
            if (w & 1) or t in thunks:   # bl, or tail-branch to a thunk
                out.append(t)
    return out


root = int(sys.argv[3], 16)
depth = int(sys.argv[4]) if len(sys.argv) > 4 else 3
seen = {root}
frontier = [root]
for level in range(depth):
    nxt = []
    for fn in frontier:
        for t in callees(fn):
            if t in thunks:
                print("  level %d: 0x%08X -> %s" % (level, fn, thunks[t]))
            elif t not in seen and 0x825F0A00 > t or t > 0x825F2400:   # skip save/restore helpers
                if t not in seen:
                    seen.add(t)
                    nxt.append(t)
    frontier = nxt
print("functions visited:", len(seen))
