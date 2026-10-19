"""Self-contained checks for the build tools; they need no game data.

Run with:  python -m unittest discover -s tools/tests
"""
import os, struct, sys, unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))

import m68k, rsrc


class Disassembler(unittest.TestCase):
    def dis(self, hexcode):
        d = m68k.Dis(bytes.fromhex(hexcode))
        out, off = [], 0
        while off < len(d.c):
            ins = d.dis(off)
            out.append(ins)
            off += ins.size
        return out

    def test_basic(self):
        ins = self.dis('4e71 4e75 303c1234'.replace(' ', ''))
        self.assertEqual([i.text for i in ins], ['nop', 'rts', 'move.w #$1234,d0'])
        self.assertEqual([i.size for i in ins], [2, 2, 4])
        self.assertEqual(ins[1].kind, 'ret')

    def test_unknown_opcode_is_data(self):
        ins = self.dis('ffff')
        self.assertEqual(ins[0].mn, 'dc.w')
        self.assertEqual(ins[0].size, 2)


def make_fork(resources):
    """Build a resource fork from {type: {id: (name, body)}}."""
    data = b''.join(struct.pack('>I', len(body)) + body
                    for t in resources.values() for _, body in t.values())
    types = list(resources.items())
    typelist = struct.pack('>H', len(types) - 1)
    reflist, names, doff = b'', b'', 0
    ref_base = 2 + 8 * len(types)
    for t, entries in types:
        typelist += struct.pack('>4sHH', t.encode('mac_roman'), len(entries) - 1,
                                ref_base + len(reflist))
        for rid, (name, body) in entries.items():
            noff = 0xFFFF
            if name is not None:
                noff = len(names)
                names += bytes([len(name)]) + name.encode('mac_roman')
            reflist += struct.pack('>hHII', rid, noff, doff, 0)
            doff += 4 + len(body)
    tl_off = 28
    nl_off = tl_off + len(typelist) + len(reflist)
    rmap = bytes(24) + struct.pack('>HH', tl_off, nl_off) + typelist + reflist + names
    hdr = struct.pack('>IIII', 256, 256 + len(data), len(data), len(rmap))
    return hdr + bytes(256 - len(hdr)) + data + rmap


class ResourceFork(unittest.TestCase):
    def test_round_trip(self):
        fork = make_fork({'STR ': {128: ('Hello', b'\x05Hello'), 129: (None, b'')},
                          'CODE': {1: (None, b'\x4e\x75')}})
        r = rsrc.read_rsrc(fork)
        self.assertEqual(list(r), ['STR ', 'CODE'])
        self.assertEqual(r['STR '][128], ('Hello', 0, b'\x05Hello'))
        self.assertEqual(r['STR '][129], (None, 0, b''))
        self.assertEqual(r['CODE'][1][2], b'\x4e\x75')


class Compiles(unittest.TestCase):
    def test_all_tools_compile(self):
        # some tools are scripts that run on import, so only byte-compile them
        tools = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..')
        for f in sorted(os.listdir(tools)):
            if f.endswith('.py'):
                with self.subTest(script=f), open(os.path.join(tools, f), encoding='utf-8') as fp:
                    compile(fp.read(), f, 'exec')


if __name__ == '__main__':
    unittest.main()
