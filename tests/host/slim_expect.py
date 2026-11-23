"""Expected values for tests/host/suitconv_slim_host_test.cpp, from the Python tool itself (extras-copies plan A2).

Usage: py -3 -I tests/host/slim_expect.py TOOL_DIR DUMP_DIR
  TOOL_DIR  the Python tool's scripts folder (<doax2_work>/physics_spike/scripts); imported, never written
  DUMP_DIR  the folder the test exe's `--dump DUMP_DIR` mode wrote (synthetic bytes only, no game data)

For every case in DUMP_DIR: a .tpr beside a .cat is read with slim_addon.read_suit and rebuilt with slim_addon.rebuild;
a .map beside a .cat is applied with slim_addon.cat_with_map. Prints the C++ table rows
    {"<case>", "OK fnv=<FNV-1a 64> len=<bytes>[ stripped=<...> ids=[...]]"},
or  {"<case>", "REFUSED <SlimError text>"},
in case-name order, then one buf_set line. Any other exception is printed as ERROR and the exit code is 1.
"""
import os
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


def main():
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
