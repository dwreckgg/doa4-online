"""Expected values for the type-2 map tests (extras-copies Task 6b): convert.parse_t2 and convert.build_cat on synthetic
bytes, from the Python converter itself. No game byte: every map, code, object id and state number below is made up. The
first map has the shape of a reference with a 0xDX entry inside a state group: main entries, a state with one entry, a
second state holding a 0xDX entry whose code has a zero middle byte, then groups 0x04 and 0x02.
Usage: py -3 -I tests/host/suitconv_t2_expect.py TOOL_DIR
  TOOL_DIR  the Python converter's scripts folder (<doax2_work>/physics_spike/scripts); imported, never written
Prints one line per case; suitconv_tpr_host_test.cpp (parse_t2_*) and suitconv_convert_host_test.cpp
(build_cat_t2_grammar) carry these lines' values, each beside the case name printed here.
"""
import struct
import sys
import types

# The maps (hex). The same strings are in the C++ tests.
M_HEL = ('900102' '900203' '910304' '900405'   # main: (0x9001,2) (0x9002,3) (0x9103,4) (0x9004,5)
         '6031' '903101'                       # state 0x31: (0x9031,1)
         '6032' 'd2000308'                     # state 0x32: (0xd20003,8), a 4-byte entry with a 00 byte inside
         '04' '900106'                         # marker 0x04: (0x9001,6)
         '02' '900207'                         # marker 0x02: (0x9002,7)
         '00')
M_HEL2 = ('900102' '900203' '910304' '900405' '6031' '903101'
          '6032' 'd2000308' '903204'           # state 0x32: (0xd20003,8) then (0x9032,4)
          '04' '900106' '02' '900207' '00')
M_EMPTYSTATE = '900102' '6033' '6034' '903407' '00'   # main, an EMPTY state 0x33, state 0x34: (0x9034,7)
M_AX ='a00503' '920704' '00'                  # main: (0xa005,3) (0x9207,4)
M_EMPTY = '900102' '04' '02' '900207' '00'     # main, an empty marker group 0x04, marker 0x02: (0x9002,7)
M_DXMAIN = '900102' 'd2000303' '900204' '00'   # main: (0x9001,2) (0xd20003,3) (0x9002,4)
REFUSE = {'state00': '901001' '6000' '00', 'state80': '901001' '6080' '00', 'unknown_b0': '901001' 'b00102' '00',
          'unknown_80': '80' '00'}


def cat_of(mapb):
    """CAT header, the map at 0x18, a type-5 span of 16 bytes 0x50..0x5f, a type-4 terminator, the directory."""
    out = bytearray(b'CAT\0' + bytes(20)) + mapb
    out += bytes((-len(out)) % 8)
    t5 = len(out)
    out += bytes(range(0x50, 0x60))
    t4 = len(out)
    out += b'\xff\xff' + bytes(6)
    d = len(out)
    for t, v in ((2, 0x18), (5, t5), (4, t4)):
        out += struct.pack('>BBBBI', 1, 0, t, 0, v)
    out += bytes(8)
    struct.pack_into('>I', out, 4, d)
    return bytes(out)


def groups_repr(groups):
    parts = []
    for kind, recs in groups:
        k = 'main' if kind == 'main' else f's{kind[1]:02x}' if isinstance(kind, tuple) else f'{kind:02x}'
        parts.append(k + ':' + ','.join(f'{c:x}/{i}' for c, i in recs))
    return ' '.join(parts)


def main():
    sys.path.insert(0, sys.argv[1])
    sys.dont_write_bytecode = True
    import convert
    for name, m in (('hel', M_HEL), ('ax', M_AX), ('empty', M_EMPTY), ('dxmain', M_DXMAIN),
                    ('emptystate', M_EMPTYSTATE)):
        b = bytes(4) + bytes.fromhex(m) + b'\xaa'
        groups, end = convert.parse_t2(b, 4)
        print(f'parse_t2 {name} @4: end={end} groups={groups_repr(groups)}')
    for name, m in sorted(REFUSE.items()):
        try:
            convert.parse_t2(bytes(4) + bytes.fromhex(m), 4)
            print(f'parse_t2 refuse_{name}: NOT REFUSED')
        except ValueError as e:
            print(f'parse_t2 refuse_{name}: {e}')
    cases = (('hel_keep2345', M_HEL, [2], [2, 3, 4, 5]), ('hel_keep12345', M_HEL, [2], [1, 2, 3, 4, 5]),
             ('hel_keep123458', M_HEL, [2], [1, 2, 3, 4, 5, 8]), ('hel2_keep123458', M_HEL2, [2], [1, 2, 3, 4, 5, 8]),
             ('empty_keep2', M_EMPTY, [2], [2]), ('emptystate_keep2', M_EMPTYSTATE, [2], [2]),
             ('dxmain_keep234', M_DXMAIN, [2], [2, 3, 4]), ('dxmain_keep24', M_DXMAIN, [2], [2, 4]),
('hel_t5_keep2345', M_HEL, [2, 5], [2, 3, 4, 5]))
    for name, m, sections, kept in cases:
        ref = types.SimpleNamespace(cat=cat_of(bytes.fromhex(m)))
        report = []
        out = convert.build_cat(ref, None, sections, set(kept), report)
        print(f'build_cat {name} in: {ref.cat.hex()}')
        print(f'build_cat {name} out: {out.hex()}')
        print(f'build_cat {name} report: {report[0]}')
    for name in ('state00', 'unknown_b0'):
        ref = types.SimpleNamespace(cat=cat_of(bytes.fromhex(REFUSE[name])))
        try:
            convert.build_cat(ref, None, [2], {1}, [])
            print(f'build_cat refuse_{name}: NOT REFUSED')
        except Exception as e:
            print(f'build_cat refuse_{name} in: {ref.cat.hex()}')
            print(f'build_cat refuse_{name}: {type(e).__name__}: {e}')


if __name__ == '__main__':
    main()
