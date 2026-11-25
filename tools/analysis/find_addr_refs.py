"""List functions (by .pdata) that build a given absolute address with lis+addi/ori.
Usage: find_addr_refs.py image.bin 0x828F6340"""
import bisect
import struct
import sys

img = open(sys.argv[1], "rb").read()
target = int(sys.argv[2], 16)
base = 0x82000000
text_start, text_size = 0x82140000, 0x744C8C
hi = ((target + 0x8000) >> 16) & 0xFFFF
lo = target & 0xFFFF
pd = img[0x82105600 - base:0x82105600 - base + 0x30F50]
ents = sorted((b, ((d >> 8) & 0x3FFFFF) * 4) for b, d in (struct.unpack_from(">II", pd, o) for o in range(0, len(pd), 8)) if b)
starts = [e[0] for e in ents]
words = struct.unpack_from(">%dI" % (text_size // 4), img, text_start - base)
hits = {}
for i, w in enumerate(words):
    if (w >> 26) == 14 and (w & 0xFFFF) == lo:   # addi rD, rA, lo
        ra = (w >> 16) & 31
        for k in range(1, 32):
            if i - k < 0:
                break
            p = words[i - k]
            if (p >> 26) == 15 and ((p >> 21) & 31) == ra and ((p >> 16) & 31) == 0 and (p & 0xFFFF) == hi:
                a = text_start + i * 4
                j = bisect.bisect_right(starts, a) - 1
                fn = starts[j] if j >= 0 and a < ents[j][0] + ents[j][1] else None
                hits.setdefault(fn, []).append(a)
                break
for fn, addrs in sorted(hits.items(), key=lambda x: x[0] or 0):
    print("fn %s: %s" % (hex(fn) if fn else "?", " ".join(hex(a) for a in addrs)))
