"""Rebuild the initial A5 world (globals + jump table) of a CodeWarrior 68k app
from its CODE 0 and DATA 0 resources, mirroring the startup code in CODE 1.

The image is returned as a bytearray covering [A5 - below, A5 + above).
"""
import struct


def decompress_data0(data, below, above):
    img = bytearray(below + above)
    a5 = below  # index of A5 inside img
    p = 4  # first long is skipped by the startup code
    for _ in range(3):
        off = struct.unpack_from('>i', data, p)[0]; p += 4
        q = a5 + off
        while True:
            b = data[p]; p += 1
            if b & 0x80:
                n = (b & 0x7F) + 1
                img[q:q + n] = data[p:p + n]; p += n; q += n
            elif b & 0x40:
                q += (b & 0x3F) + 1
            elif b & 0x20:
                n = (b & 0x1F) + 2
                img[q:q + n] = bytes([data[p]]) * n; p += 1; q += n
            elif b & 0x10:
                n = (b & 0x0F) + 1
                img[q:q + n] = b'\xff' * n; q += n
            elif b == 0:
                break
            elif b == 1:
                q += 4; img[q:q + 2] = b'\xff\xff'; q += 2
                img[q:q + 2] = data[p:p + 2]; p += 2; q += 2
            elif b == 2:
                q += 4; img[q] = 0xFF; q += 1
                img[q:q + 3] = data[p:p + 3]; p += 3; q += 3
            elif b == 3:
                img[q:q + 2] = b'\xa9\xf0'; q += 2; q += 2
                img[q:q + 2] = data[p:p + 2]; p += 2; q += 2; q += 1
                img[q] = data[p]; p += 1; q += 1
            elif b == 4:
                img[q:q + 2] = b'\xa9\xf0'; q += 2; q += 1
                img[q:q + 3] = data[p:p + 3]; p += 3; q += 3; q += 1
                img[q] = data[p]; p += 1; q += 1
            else:
                raise ValueError('bad DATA opcode %x' % b)
    return img, p


def read_relocs(data, p):
    """Parse one relocation list; returns (list of offsets, new p)."""
    cnt = struct.unpack_from('>I', data, p)[0]; p += 4
    d2 = 0; out = []
    for _ in range(cnt):
        b = data[p]; p += 1
        if b & 0x80:
            v = (b * 2) & 0xFF
            if v & 0x80: v -= 0x100
            d2 += v
        elif b & 0x40:
            w = (b << 8) | data[p]; p += 1
            v = (w << 2) & 0xFFFF
            if v & 0x8000: v -= 0x10000
            d2 += v >> 1
        else:
            l = struct.unpack_from('>I', bytes([b]) + data[p:p + 3])[0]; p += 3
            v = (l << 2) & 0xFFFFFFFF
            if v & 0x80000000: v -= 1 << 32
            d2 = v >> 1
        out.append(d2)
    return out, p


def build(r):
    code0 = r['CODE'][0][2]
    above, below, jtsize, jtoff = struct.unpack_from('>IIII', code0, 0)
    data0 = r['DATA'][0][2]
    img, p = decompress_data0(data0, below, above)
    relocs = []
    for kind in ('a5', 'code1', 'self'):
        lst, p = read_relocs(data0, p)
        relocs.append((kind, lst))
    # second set of three lists relocates CODE 1 itself
    c1 = []
    for kind in ('a5', 'code1', 'self'):
        lst, p = read_relocs(data0, p)
        c1.append((kind, lst))
    return {'img': img, 'below': below, 'above': above, 'relocs': relocs, 'code1_relocs': c1}


def segment_relocs(body):
    """Relocation lists stored at the end of a far-model segment (CODE >= 2)."""
    off = struct.unpack_from('>I', body, 8)[0]
    out = []; p = off
    for kind in ('a5', 'code1', 'self'):
        lst, p = read_relocs(body, p)
        out.append((kind, lst))
    return out


def jumptable(world, r):
    """Map A5 offset of each far jump-table slot -> (segment, offset)."""
    img = world['img']; a5 = world['below']
    jt = {}
    for sid, (_, _, body) in r['CODE'].items():
        if sid < 2: continue
        cnt, off = struct.unpack_from('>HI', body, 2)
        for i in range(cnt + 1):
            slot = off + i * 8
            o = struct.unpack_from('>I', img, a5 + slot + 2)[0]
            s = struct.unpack_from('>H', img, a5 + slot + 6)[0]
            jt[slot] = (s, o)
    return jt
