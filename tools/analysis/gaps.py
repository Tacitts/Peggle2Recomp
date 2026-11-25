import collections
import struct
import sys

img = open(sys.argv[1], "rb").read()
base = 0x82000000
pd = img[0x82105600 - base:0x82105600 - base + 0x30F50]
ents = []
for o in range(0, len(pd), 8):
    b, d = struct.unpack_from(">II", pd, o)
    if b:
        ents.append((b, ((d >> 8) & 0x3FFFFF) * 4, (d >> 31) & 1))
ents.sort()
# The 8 bytes immediately before a pdata function with the exception flag set.
exc_words = collections.Counter()
gap8 = collections.Counter()
for b, s, exc in ents:
    w0, w1 = struct.unpack_from(">II", img, b - 8 - base)
    if exc:
        exc_words[hex(w0)] += 1
for (b, s, e), (nb, ns, ne) in zip(ents, ents[1:]):
    gap = nb - (b + s)
    if gap == 8:
        gap8[hex(struct.unpack_from(">I", img, b + s - base)[0])] += 1
print("pdata entries:", len(ents), "with exception flag:", sum(e for _, _, e in ents))
print("first word before exception-flagged functions:", exc_words.most_common(8))
print("first word of 8-byte gaps between pdata functions:", gap8.most_common(8))
