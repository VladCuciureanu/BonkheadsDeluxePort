"""Classic Mac OS resource fork reader.

usage: python -I tools/rsrc.py <file.rsrc> [-v]   (summarise; -v lists every resource)
"""
import struct, sys
from collections import OrderedDict

def read_rsrc(data):
    doff, moff, dlen, mlen = struct.unpack_from('>IIII', data, 0)
    m = moff
    tl_off = struct.unpack_from('>H', data, m + 24)[0]
    nl_off = struct.unpack_from('>H', data, m + 26)[0]
    tl = m + tl_off
    ntypes = struct.unpack_from('>H', data, tl)[0] + 1
    res = OrderedDict()
    for i in range(ntypes):
        t, n, ref = struct.unpack_from('>4sHH', data, tl + 2 + i * 8)
        t = t.decode('mac_roman')
        for j in range(n + 1):
            rid, noff, attr_off = struct.unpack_from('>hHI', data, tl + ref + j * 12)
            attrs = attr_off >> 24; off = attr_off & 0xFFFFFF
            name = None
            if noff != 0xFFFF:
                p = m + nl_off + noff
                name = data[p + 1:p + 1 + data[p]].decode('mac_roman')
            ln = struct.unpack_from('>I', data, doff + off)[0]
            body = data[doff + off + 4: doff + off + 4 + ln]
            res.setdefault(t, OrderedDict())[rid] = (name, attrs, body)
    return res

if __name__ == '__main__':
    if len(sys.argv) < 2:
        sys.exit(__doc__.strip())
    r = read_rsrc(open(sys.argv[1], 'rb').read())
    for t, d in r.items():
        tot = sum(len(b) for _, _, b in d.values())
        ids = list(d.keys())
        print('%-4s n=%4d bytes=%8d ids=%s' % (t, len(d), tot, ids[:12] if len(ids) > 12 else ids))
        if len(sys.argv) > 2:
            for rid, (nm, a, b) in d.items(): print('    ', rid, repr(nm), len(b), hex(a))
