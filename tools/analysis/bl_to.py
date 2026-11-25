import struct
import sys

img = open(sys.argv[1], "rb").read()
base = 0x82000000
text = (0x82140000, 0x744C8C)
targets = {int(x, 16) for x in sys.argv[2:]}
hits = {t: [] for t in targets}
start, size = text
for off in range(start - base, start - base + size, 4):
    w = struct.unpack_from(">I", img, off)[0]
    if (w >> 26) == 18 and (w & 1):
        disp = w & 0x3FFFFFC
        if disp & 0x2000000:
            disp -= 0x4000000
        a = off + base
        t = (disp if (w & 2) else a + disp) & 0xFFFFFFFF
        if t in hits:
            hits[t].append(hex(a))
for t, h in hits.items():
    print(hex(t), "bl from:", h[:5], "count", len(h))
