"""Count Xbox 360 shader containers (XenosRecomp signature) in files.
Usage: scan_shaders.py file [file...]"""
import struct
import sys

MAGIC_MASK = 0xFFFFFF00
MAGIC = 0x102A1100
CHUNK = 64 * 1024 * 1024

for path in sys.argv[1:]:
    found = 0
    vs = ps = 0
    first = []
    with open(path, "rb") as f:
        offset = 0
        tail = b""
        while True:
            block = f.read(CHUNK)
            if not block:
                break
            data = tail + block
            base = offset - len(tail)
            # candidate positions: 4-byte aligned (relative to file start)
            start = (-base) % 4
            idx = data.find(b"\x10\x2A\x11", start)
            while idx != -1 and idx + 36 <= len(data):
                if (base + idx) % 4 == 0:
                    flags, vsize, psize, _c, _ct, _dt, _so, f1c, f20 = struct.unpack_from(">9I", data, idx)
                    if (flags & MAGIC_MASK) == MAGIC and f1c == 0 and f20 == 0 and 0 < vsize < 0x100000 and psize < 0x100000:
                        found += 1
                        if (flags & 0xFF) == 1:
                            ps += 1
                        else:
                            vs += 1
                        if len(first) < 5:
                            first.append((base + idx, flags, vsize, psize))
                idx = data.find(b"\x10\x2A\x11", idx + 1)
            tail = data[-64:]
            offset += len(block)
    print("%s: %d shader containers (flags&0xFF==1: %d, other: %d)" % (path, found, ps, vs))
    for o, fl, v, p in first:
        print("   at 0x%X flags 0x%08X virtual 0x%X physical 0x%X" % (o, fl, v, p))
