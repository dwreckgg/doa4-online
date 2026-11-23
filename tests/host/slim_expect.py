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

Scan mode (Task 5): py -3 -I tests/host/slim_expect.py --scan TOOL_DIR DUMP_DIR/scan
  runs read_suit, load_ref, bind_guard, Layout and _scan (kinds 2 and 4) on every case and lod_chain._frame /
  _to_local on tolocal.txt; prints the kScanExpected and kToLocalExpected rows (see scan_main).
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


def scan_layout(lay):
    """Layout's canonical text (the C++ test's LayoutCanon()): per kind rows, ids, new ids, nodes, node_of_code (sorted
    by code); the kinds; the allnodes ids; every frame (sorted by id) as R, t, dev bits."""
    out = []
    for kind in (2, 4):
        out.append(f'G{kind}:r=' + ','.join(f'{c:x}:{i}' for c, i in lay.rows[kind]))
        out.append(';i=' + ','.join(str(i) for i in lay.ids[kind]))
        out.append(';n=' + ','.join(f'{o}>{n}' for o, n in lay.new_id[kind].items()))
        out.append(';h=' + ','.join(f'{nd.id}/{nd.parent}/' + '.'.join(str(c) for c in nd.children)
                                    for nd in lay.nodes[kind]))
        out.append(';c=' + ','.join(f'{c:x}>{n}' for c, n in sorted(lay.node_of_code[kind].items())) + '|')
    out.append('k=' + ','.join(f's{k[1]:x}' if isinstance(k, tuple) else f'{k:x}' for k in lay.kinds) + '|')
    out.append('a=' + ','.join(str(nd.id) for nd in lay.allnodes) + '|')
    for nid in sorted(lay.frame):
        R, t, dev = lay.frame[nid]
        out.append(f'f{nid}:' + ','.join(dbits(R[r][c]) for r in range(3) for c in range(3)) + ',' +
                   ','.join(dbits(x) for x in t) + ',' + dbits(dev) + ';')
    return ''.join(out)


def scan_text(kind, tris, matsrc):
    """_scan's result as the C++ test's ScanCanon(): keys (dense, first-traversal order), vids (dense, first appearance),
    then per triangle its key id and per corner vid, position bits and payload (vid id, triangle, corner[, normal bits,
    uv bits])."""
    keys, vids = {}, {}
    ktext, vtext, ttext = [], [], []
    for key, cs in tris:
        if key not in keys:
            keys[key] = len(keys)
            if kind == 2:
                node, (tex, lp, flag, w4, m70, m80) = key
                ktext.append(f'{node},{tex:x},{lp},{flag},{w4:x},{m70.hex()},{m80.hex()},{fnv(matsrc[key]):016x};')
            else:
                ktext.append(f'{key[0]};')
        parts = [f'{keys[key]}:']
        for vid, pos, pl in cs:
            if vid not in vids:
                vids[vid] = len(vids)
                vtext.append(f'{vid[0]}.{vid[1]}.{vid[2]};')
            p = f'{vids[pl[0]]}.{pl[1]}.{pl[2]}'
            if kind == 2:
                p += '.' + '.'.join(dbits(x) for x in pl[3]) + '/' + '.'.join(dbits(x) for x in pl[4])
            parts.append(f'{vids[vid]},{dbits(pos[0])},{dbits(pos[1])},{dbits(pos[2])},{p};')
        ttext.append(''.join(parts))
    text = 'K' + ''.join(ktext) + 'V' + ''.join(vtext) + 'T' + '|'.join(ttext)
    nodes = ','.join(str(key[0]) for key, cs in tris)
    return f'S{kind}=n{len(tris)} k{len(keys)} v{len(vids)} fnv={fnv(text.encode("ascii")):016x} nodes{kind}=[{nodes}]'


def scan_main(tool, sdir):
    """--scan mode (Task 5): every <case>.tpr / <case>.cat (the suit) with <case>.ref.tpr / <case>.ref.cat (the
    reference) the test exe's `--dump DIR` wrote under DIR/scan, through read_suit, load_ref, bind_guard, Layout and
    _scan for kinds 2 and 4; prints the kScanExpected rows
        {"<case>", "L=<fnv of LayoutCanon> noc2=[...] noc4=[...] S2=... S4=..."}
    where a part is "REFUSED <SlimError text>" or "RAISED" (any other exception, its type in a comment line); then the
    to_local rows of DIR/scan/tolocal.txt (one case per line: 'NAME w <16 bits> v <3 bits> dir 0|1')."""
    sys.dont_write_bytecode = True
    sys.path.insert(0, tool)
    import slim_addon as sa                 # noqa: E402
    import lod_chain                        # noqa: E402
    print(f'# tool {os.path.abspath(sa.__file__)}')
    names = sorted(f[:-4] for f in os.listdir(sdir) if f.endswith('.tpr') and not f.endswith('.ref.tpr'))
    bad = 0

    def guard(case, what, fn):
        try:
            return fn(), None
        except sa.SlimError as e:
            return None, f'REFUSED {e}'
        except Exception as e:  # noqa: BLE001
            print(f'# {case} {what}: RAISED {type(e).__name__}: {e}')
            return None, 'RAISED'
    for name in names:
        base = os.path.join(sdir, name)
        try:
            s = sa.read_suit(base + '.tpr', base + '.cat')
            ref = sa.load_ref(base + '.ref.tpr', base + '.ref.cat')
        except Exception as e:  # noqa: BLE001
            print(f'    {{{c_str(name)}, {c_str("ERROR " + type(e).__name__ + ": " + str(e))}}},')
            bad += 1
            continue

        def layout():
            sa.bind_guard(s, ref)
            return sa.Layout(s, ref)
        lay, err = guard(name, 'layout', layout)
        if err:
            line = err
        else:
            parts = [f'L={fnv(scan_layout(lay).encode("ascii")):016x}']
            for kind in (2, 4):
                parts.append(f'noc{kind}=[' + ','.join(f'{c:x}>{n}' for c, n in sorted(lay.node_of_code[kind].items()))
                             + ']')
            for kind in (2, 4):
                res, err = guard(name, f'scan {kind}', lambda: sa._scan(s, ref, kind, lay))
                parts.append(f'S{kind}={err}' if err else scan_text(kind, *res))
            line = ' '.join(parts)
        print(f'    {{{c_str(name)}, {c_str(line)}}},')
    print('# to_local (kToLocalExpected)')
    for ln in open(os.path.join(sdir, 'tolocal.txt'), encoding='ascii').read().split('\n'):
        if not ln:
            continue
        w = ln.split(' ')
        assert w[1] == 'w' and w[18] == 'v' and w[22] == 'dir', ln
        m = [bits_d(x) for x in w[2:18]]
        v = [bits_d(x) for x in w[19:22]]
        R, t, dev = lod_chain._frame(m)
        q = lod_chain._to_local(v, R, None if w[23] == '1' else t, dev)
        print(f'    {{{c_str(w[0])}, "dev={dbits(dev)} q={",".join(dbits(x) for x in q)}"}},')
    return 1 if bad else 0


def main():
    if len(sys.argv) == 4 and sys.argv[1] == '--grid':
        return grid_main(sys.argv[2], sys.argv[3])
    if len(sys.argv) == 4 and sys.argv[1] == '--scan':
        return scan_main(sys.argv[2], sys.argv[3])
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
