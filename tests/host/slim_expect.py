"""Expected values for tests/host/suitconv_slim_host_test.cpp, from the Python tool itself (extras-copies plan A2).

Usage: py -3 -I tests/host/slim_expect.py TOOL_DIR DUMP_DIR
  TOOL_DIR  the Python tool's scripts folder (<doax2_work>/physics_spike/scripts); imported, never written
  DUMP_DIR  the folder the test exe's `--dump DUMP_DIR` mode wrote (synthetic bytes only, no game data)

For every case in DUMP_DIR: a .tpr beside a .cat is read with slim_addon.read_suit and rebuilt with slim_addon.rebuild;
a .map beside a .cat is applied with slim_addon.cat_with_map. Prints the C++ table rows
    {"<case>", "OK fnv=<FNV-1a 64> len=<bytes>[ stripped=<...> ids=[...]]"},
or  {"<case>", "REFUSED <SlimError text>"},
in case-name order, then one buf_set line. Any other exception is printed as ERROR and the exit code is 1.

Grid mode (Task 4): py -3 -I tests/host/slim_expect.py --grid TOOL_DIR DUMP_DIR/grid
  runs slim.merge (or slim.choose_h) on every <case>.grid the dump wrote and prints the kGridExpected rows
    {"<case>", "n=<count> fnv=<FNV-1a 64 of the canonical text>"}            (merge)
    {"<case>", "h=<bits> lo=<bits> n=<count> fnv=<...>"}                     (choose_h)
    {"<case>", "REFUSED <SlimError text>"} or "REFUSED <ExceptionType>: <text>" for math.floor's and the division's
  errors. A .grid file: 'case NAME', then 'merge none|<h bits>' or 'choose <target>', then one line per triangle
  't KEY vid x y z payload vid x y z payload vid x y z payload' (doubles as their IEEE bits, 16 hex digits).
"""
import os
import struct
import sys


def fnv(b):
    h = 0xcbf29ce484222325
    for v in b:
        h ^= v
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def ok_line(b):
    return f'OK fnv={fnv(b):016x} len={len(b)}'


def c_str(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'


def dbits(x):
    """A double's IEEE bits as 16 hex digits (the C++ Bits()); -0.0 and 0.0 differ."""
    return struct.pack('>d', x).hex()


def bits_d(s):
    return struct.unpack('>d', bytes.fromhex(s))[0]


def canon(merged):
    """slim.merge's dict as the C++ test's Canon(): per key in dict order 'K<key>:' then 'v<x>,<y>,<z>,<payload>;' per
    vertex (positions as IEEE bits) and 'f<i>,<j>,<k>;' per face, then '|'."""
    out = []
    for key, (verts, faces) in merged.items():
        out.append(f'K{key}:')
        for pos, payload in verts:
            out.append(f'v{dbits(pos[0])},{dbits(pos[1])},{dbits(pos[2])},{payload};')
        for f in faces:
            out.append(f'f{f[0]},{f[1]},{f[2]};')
        out.append('|')
    return ''.join(out)


def grid_main(tool, gdir):
    """--grid mode: every <case>.grid file the test exe's `--dump DIR` wrote under DIR/grid, run through slim.py."""
    sys.dont_write_bytecode = True
    sys.path.insert(0, tool)
    import slim                             # noqa: E402
    print(f'# tool {os.path.abspath(slim.__file__)}')
    bad = 0
    for fn in sorted(f for f in os.listdir(gdir) if f.endswith('.grid')):
        lines = open(os.path.join(gdir, fn), encoding='ascii').read().split('\n')
        name = lines[0].split(' ', 1)[1]
        mode = lines[1].split(' ')
        tris = []
        for ln in lines[2:]:
            if not ln:
                continue
            w = ln.split(' ')
            assert w[0] == 't', ln
            corners = []
            for c in range(3):
                v = w[2 + 5 * c: 7 + 5 * c]
                corners.append((int(v[0]), (bits_d(v[1]), bits_d(v[2]), bits_d(v[3])), int(v[4])))
            tris.append((int(w[1]), tuple(corners)))
        try:
            if mode[0] == 'merge':
                h = None if mode[1] == 'none' else bits_d(mode[1])
                m = slim.merge(tris, h, slim.origin(tris))
                line = f'n={slim.count(m)} fnv={fnv(canon(m).encode("ascii")):016x}'
            else:
                h, m, lo = slim.choose_h(tris, int(mode[1]))
                line = f'h={dbits(h)} lo={dbits(lo)} n={slim.count(m)} fnv={fnv(canon(m).encode("ascii")):016x}'
        except slim.SlimError as e:
            line = f'REFUSED {e}'
        except (ValueError, OverflowError, ZeroDivisionError) as e:
            line = f'REFUSED {type(e).__name__}: {e}'
        except Exception as e:  # noqa: BLE001
            line = f'ERROR {type(e).__name__}: {e}'
            bad += 1
        print(f'    {{{c_str(name)}, {c_str(line)}}},')
    return 1 if bad else 0


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--grid':
        return grid_main(sys.argv[2], sys.argv[3])
    if len(sys.argv) != 3:
        print(__doc__)
        return 2
    tool, dump = sys.argv[1], sys.argv[2]
    sys.dont_write_bytecode = True          # never write a __pycache__ into the tool's folder
    sys.path.insert(0, tool)
    import slim_addon as sa                 # noqa: E402
    import tpr                              # noqa: E402
    print(f'# tool {os.path.abspath(sa.__file__)}')
    names = sorted({os.path.splitext(f)[0] for f in os.listdir(dump)})
    bad = 0
    for name in names:
        base = os.path.join(dump, name)
        try:
            if os.path.exists(base + '.map'):
                cat = open(base + '.cat', 'rb').read()
                new_map = open(base + '.map', 'rb').read()
                line = ok_line(sa.cat_with_map(cat, new_map))
            else:
                s = sa.read_suit(base + '.tpr', base + '.cat')
                ids = '[' + ', '.join(str(nd.id) for nd in s.nodes) + ']'
                line = f'{ok_line(sa.rebuild(s))} stripped={s.stripped} ids={ids} groups={desc(s.groups)}'
        except (sa.SlimError, ValueError) as e:   # parse_t2's ValueError stops the tool as a SlimError does
            line = f'REFUSED {e}'
        except Exception as e:  # noqa: BLE001
            line = f'ERROR {type(e).__name__}: {e}'
            bad += 1
        print(f'    {{{c_str(name)}, {c_str(line)}}},')
    d = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0x00, 0x01, 0x20, 0x03, 0x00, 0x00, 0x04, 0x02])
    print(f'# buf_set(d, 0x3000, 0x80) = {tpr.buf_set(d, 0x3000, 0x80).hex()}')
    import convert                          # noqa: E402
    print('# t2_bytes refusals (kT2Expected)')
    for label, kind, recs in T2_CALLS:
        try:
            convert.t2_bytes(kind, recs)
            text = 'no refusal'
        except AssertionError as e:
            text = str(e)
        print(f'    {{{c_str(label)}, {c_str(text)}}},')
    return 1 if bad else 0


# the same calls as T2Asserts() in suitconv_slim_host_test.cpp
T2_CALLS = [('marker 0x60', 0x60, []),
            ('marker 0', 0, []),
            ('state 0x80', ('state', 0x80), []),
            ('code 0x8fff', 'main', [(0x9001, 0), (0x8FFF, 5)]),
            ('code 0xe00000', 'main', [(0xE00000, 1)])]


def desc(groups):
    """The groups as the C++ test's Desc() prints them: main[9001:0,...] s10[...] 02[...]."""
    out = []
    for kind, recs in groups:
        k = 'main' if kind == 'main' else f's{kind[1]:02x}' if isinstance(kind, tuple) else f'{kind:02x}'
        out.append(k + '[' + ','.join(f'{c:x}:{i}' for c, i in recs) + ']')
    return ' '.join(out)


if __name__ == '__main__':
    sys.exit(main())
