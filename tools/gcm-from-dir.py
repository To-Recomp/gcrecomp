#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""Rebuild a GameCube disc image (.iso, sparse) from an extracted disc directory (sys/ and files/), so a reference
emulator that takes only images can boot it (tools/dolphin-run.sh --iso).

    tools/gcm-from-dir.py DISC_DIR OUT.iso

DISC_DIR holds sys/{boot.bin,bi2.bin,apploader.img,fst.bin,main.dol} and files/ as the extractor writes them
(torecomp-disc extract). Every file goes to the offset the original file system table gives it, the system files
to the offsets boot.bin gives (main.dol at 0x420, fst.bin at 0x424), so the image is the disc up to padding.
The output is a sparse file of the original size (1459978240 bytes for a GameCube disc)."""
import os
import struct
import sys

DISC_SIZE = 1459978240


def main():
    if len(sys.argv) != 3 or sys.argv[1] in ("-h", "--help"):
        print(__doc__)
        return 0 if len(sys.argv) == 2 else 2
    root, out = sys.argv[1], sys.argv[2]
    sysd, files = os.path.join(root, "sys"), os.path.join(root, "files")
    rd = lambda n: open(os.path.join(sysd, n), "rb").read()
    boot, bi2, app, fst, dol = (rd(n) for n in ("boot.bin", "bi2.bin", "apploader.img", "fst.bin", "main.dol"))
    dol_off, fst_off = struct.unpack(">II", boot[0x420:0x428])
    with open(out, "wb") as f:
        f.truncate(DISC_SIZE)

        def put(off, data):
            f.seek(off)
            f.write(data)

        put(0, boot)
        put(0x440, bi2)
        put(0x2440, app)
        put(dol_off, dol)
        put(fst_off, fst)
        n = struct.unpack(">I", fst[8:12])[0]
        names = fst[n * 12:]

        def name(i):
            o = struct.unpack(">I", b"\0" + fst[i * 12 + 1:i * 12 + 4])[0]
            return names[o:names.index(b"\0", o)].decode("ascii")

        def walk(i, end, path):
            while i < end:
                is_dir = fst[i * 12] != 0
                off, size = struct.unpack(">II", fst[i * 12 + 4:i * 12 + 12])
                p = os.path.join(path, name(i))
                if is_dir:
                    walk(i + 1, size, p)
                    i = size
                else:
                    put(off, open(os.path.join(files, p), "rb").read())
                    i += 1

        walk(1, n, "")
    return 0


if __name__ == "__main__":
    sys.exit(main())
