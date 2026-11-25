import re
import struct
import sys

img = open(sys.argv[1], "rb").read()
base = 0x82000000
pd = img[0x82105600 - base:0x82105600 - base + 0x30F50]
ents = []
for o in range(0, len(pd), 8):
    b, d = struct.unpack_from(">II", pd, o)
    if b:
        ents.append((b, ((d >> 8) & 0x3FFFFF) * 4))
ents.sort()

# Which function did the recompiler actually emit around each failing bctr?
ppc_funcs = []
import glob
for path in glob.glob(sys.argv[2] + "/ppc_recomp.*.cpp"):
    for m in re.finditer(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]+)\)", open(path, encoding="utf-8", errors="ignore").read()):
        ppc_funcs.append(int(m.group(1), 16))
ppc_funcs.sort()

for t in [int(x, 16) for x in sys.argv[3:]]:
    near = [(hex(a), hex(s)) for a, s in ents if a <= t + 0x100 and a + s >= t - 0x100]
    import bisect
    i = bisect.bisect_right(ppc_funcs, t) - 1
    emitted = [hex(x) for x in ppc_funcs[max(0, i - 1):i + 2]]
    print(hex(t), "pdata:", near, "emitted fns:", emitted)
