"""Find `lis rD, imm` (addis rD, 0, imm) instructions for a given 16-bit immediate and report
the containing .pdata function. Usage: find_lis.py image.bin 0x102A"""
import bisect
import struct
import sys

img = open(sys.argv[1], "rb").read()
imm = int(sys.argv[2], 16) & 0xFFFF
base = 0x82000000
text_start, text_size = 0x82140000, 0x744C8C
pd = img[0x82105600 - base:0x82105600 - base + 0x30F50]
ents = sorted((b, ((d >> 8) & 0x3FFFFF) * 4) for b, d in (struct.unpack_from(">II", pd, o) for o in range(0, len(pd), 8)) if b)
starts = [e[0] for e in ents]
words = struct.unpack_from(">%dI" % (text_size // 4), img, text_start - base)
for i, w in enumerate(words):
    if (w >> 26) == 15 and ((w >> 16) & 31) == 0 and (w & 0xFFFF) == imm:
        a = text_start + i * 4
        j = bisect.bisect_right(starts, a) - 1
        fn = starts[j] if j >= 0 and a < ents[j][0] + ents[j][1] else None
        print("lis r%d,0x%X at 0x%08X in function %s" % ((w >> 21) & 31, imm, a, hex(fn) if fn else "?"))
