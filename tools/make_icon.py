"""Build the executable's icon from the player's own game files (no art is stored in this repo).

Usage: python -I tools/make_icon.py <png> <out.ico>

The XBLA package carries the game's 64x64 dashboard logo (SmallLogo.png, extracted next to
Default.xex). An .ico may hold PNG images directly (Windows Vista and later), so the PNG is
wrapped as-is: ICONDIR + one ICONDIRENTRY + the PNG bytes.
"""
import struct
import sys


def main():
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    png = open(sys.argv[1], "rb").read()
    if png[:8] != b"\x89PNG\r\n\x1a\n":
        print("not a PNG: %s" % sys.argv[1])
        return 1
    width, height = struct.unpack(">II", png[16:24])
    # ICONDIR: reserved, type 1 (icon), count 1. ICONDIRENTRY: width/height (0 = 256),
    # colours, reserved, planes, bits per pixel, data size, data offset.
    entry = struct.pack("<BBBBHHII", width % 256, height % 256, 0, 0, 1, 32, len(png), 6 + 16)
    with open(sys.argv[2], "wb") as f:
        f.write(struct.pack("<HHH", 0, 1, 1) + entry + png)
    print("icon %dx%d -> %s" % (width, height, sys.argv[2]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
