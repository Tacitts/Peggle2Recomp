"""Find `stw rS, lo(rA)` instructions whose rA was set by `lis rA, hi` shortly before,
i.e. stores to a given absolute global address. Usage: find_store.py image.bin 0x82D73FE8"""
import struct
import sys

img = open(sys.argv[1], "rb").read()
target = int(sys.argv[2], 16)
base = 0x82000000
text_start, text_size = 0x82140000, 0x744C8C
hi = (target + 0x8000) >> 16 & 0xFFFF
lo = target & 0xFFFF
words = struct.unpack_from(">%dI" % (text_size // 4), img, text_start - base)
for i, w in enumerate(words):
    op = w >> 26
    if op in (36, 32) and (w & 0xFFFF) == lo:  # stw / lwz
        ra = (w >> 16) & 31
        for k in range(1, 24):
            if i - k < 0:
                break
            p = words[i - k]
            if (p >> 26) == 15 and ((p >> 21) & 31) == ra and ((p >> 16) & 31) == 0 and (p & 0xFFFF) == hi:
                kind = "STORE" if op == 36 else "load "
                if kind == "STORE":
                    print("%s at 0x%08X" % (kind, text_start + i * 4))
                break
