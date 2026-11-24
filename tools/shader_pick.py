# tools/shader_pick.py: takes from the shader bundles (the ones of nxnative's dm/gfx/tools/shader_split.py) the records of
# the keys that fail in a sweep (the "[fail] <bundle> <key>: ..." lines of tools/shader_sweep.c) and writes them to another
# folder in the same format, to repeat only those (with LARECOMP_LOG_BINDLESS=1, for example).
# Usage: python tools/shader_pick.py <bundle folder> <sweep errors> <output folder> [reason text]
import os
import re
import struct
import sys


def main():
    pkg, err, out = sys.argv[1:4]
    want_why = sys.argv[4] if len(sys.argv) > 4 else ''
    want = {}
    for line in open(err, encoding='utf-8', errors='replace'):
        m = re.match(r'\[fail\] (\S+) ([0-9a-f]{16}): (.*)', line)
        if m and want_why in m.group(3):
            want.setdefault(m.group(1), set()).add(m.group(2))
    os.makedirs(out, exist_ok=True)
    total = 0
    for name, keys in sorted(want.items()):
        d = open(os.path.join(pkg, name), 'rb').read()
        o, recs = 0, []
        while o + 0x38 <= len(d):
            if d[o:o + 8] != b'BTCDBTCD':
                o += 1
                continue
            ctl, cl = struct.unpack_from('<2I', d, o + 0x28)
            end = o + 0x38 + cl + ctl
            if d[o + 8:o + 16].hex() in keys:
                recs.append(d[o:end])
            o = end
        with open(os.path.join(out, name), 'wb') as f:
            f.write(b''.join(recs))
        total += len(recs)
        print(f'{name}: {len(recs)} of {len(keys)}')
    print(f'{total} shaders in {out}')


main()
