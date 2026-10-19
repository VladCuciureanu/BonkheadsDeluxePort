"""Minimal StuffIt 5 archive parser/extractor (methods 0, 13, 15).

usage: python -I tools/sit5.py --extract <archive.sit> <output dir>
       python -I tools/sit5.py <archive.sit>          (list the contents)
"""
import struct, sys, os

def be16(b, o): return struct.unpack_from('>H', b, o)[0]
def be32(b, o): return struct.unpack_from('>I', b, o)[0]

class Entry:
    pass

def parse(buf):
    assert buf[:7] == b'StuffIt'
    nroot = be16(buf, 0x5C)
    first = be32(buf, 0x5E)
    entries = []
    def walk(off, count, path):
        for _ in range(count):
            if off == 0:
                break
            assert be32(buf, off) == 0xA5A5A5A5, hex(off)
            ver = buf[off+4]
            hsize = be16(buf, off+6)
            flags = buf[off+9]
            nxt = be32(buf, off+22)
            namelen = be16(buf, off+30)
            dlen = be32(buf, off+34)
            dcomp = be32(buf, off+38)
            dcrc = be16(buf, off+42)
            # 46-byte fixed part then 2 bytes
            p = off + 44
            dmethod = buf[p]; dpass = buf[p+1]
            name = buf[off+48:off+48+namelen].decode('mac_roman')
            e = Entry()
            e.name = name; e.path = path + [name]; e.flags = flags
            if flags & 0x40:  # directory
                nfiles = be16(buf, off+46)
                e.isdir = True
                entries.append(e)
                walk(dlen, nfiles, e.path)
                off = nxt
                continue
            e.isdir = False
            q = off + hsize
            flags2 = be16(buf, q); q += 4
            e.type = buf[q:q+4]; e.creator = buf[q+4:q+8]; q += 8
            e.finderflags = be16(buf, q); q += 2
            q += 22 if ver == 1 else 18
            e.rlen = e.rcomp = 0; e.rmethod = None
            if flags2 & 1:
                e.rlen = be32(buf, q); e.rcomp = be32(buf, q+4); e.rcrc = be16(buf, q+8)
                e.rmethod = buf[q+12]; rpass = buf[q+13]
                q += 14 + rpass
            e.dlen = dlen; e.dcomp = dcomp; e.dcrc = dcrc; e.dmethod = dmethod
            e.rstart = q
            e.dstart = q + e.rcomp
            entries.append(e)
            off = nxt
    walk(first, nroot, [])
    return entries




# ---------------- Arsenic (method 15) ----------------
class BitIn:
    def __init__(self, data):
        self.data = data; self.pos = 0  # bit position
    def bit(self):
        p = self.pos; self.pos = p + 1
        i = p >> 3
        if i >= len(self.data): return 0
        return (self.data[i] >> (7 - (p & 7))) & 1
    def bits(self, n):
        v = 0
        for _ in range(n): v = (v << 1) | self.bit()
        return v

class Model:
    def __init__(self, first, last, inc, limit):
        self.first = first; self.n = last - first + 1; self.inc = inc; self.limit = limit
        self.reset()
    def reset(self):
        self.freq = [self.inc] * self.n; self.total = self.inc * self.n

NUMBITS = 26
ONE = 1 << (NUMBITS - 1)
HALF = 1 << (NUMBITS - 2)

class ArithDec:
    def __init__(self, bitin):
        self.inp = bitin; self.range = ONE; self.code = bitin.bits(NUMBITS)
    def sym(self, m):
        freq = m.freq
        total = m.total
        renorm = self.range // total
        f = self.code // renorm
        cum = 0; n = 0; last = m.n - 1
        while n < last:
            fn = freq[n]
            if cum + fn > f: break
            cum += fn; n += 1
        size = freq[n]
        lowincr = renorm * cum
        self.code -= lowincr
        if cum + size == total: self.range -= lowincr
        else: self.range = size * renorm
        inp = self.inp
        while self.range <= HALF:
            self.range <<= 1
            self.code = (self.code << 1) | inp.bit()
        freq[n] += m.inc
        m.total = total + m.inc
        if m.total > m.limit:
            t = 0
            for i in range(m.n):
                freq[i] = (freq[i] + 1) >> 1; t += freq[i]
            m.total = t
        return n + m.first
    def bitstring(self, m, nbits):
        r = 0
        for i in range(nbits):
            if self.sym(m): r |= 1 << i
        return r

def arsenic(data, outlen):
    dec = ArithDec(BitIn(data))
    initial = Model(0, 1, 1, 256)
    selector = Model(0, 10, 8, 1024)
    mtfm = [Model(2, 3, 8, 1024), Model(4, 7, 4, 1024), Model(8, 15, 4, 1024), Model(16, 31, 4, 1024),
            Model(32, 63, 2, 1024), Model(64, 127, 2, 1024), Model(128, 255, 1, 1024)]
    if dec.bitstring(initial, 8) != ord('A') or dec.bitstring(initial, 8) != ord('s'):
        raise ValueError('bad arsenic signature')
    blockbits = dec.bitstring(initial, 4) + 9
    blocksize = 1 << blockbits
    out = bytearray()
    end = dec.sym(initial)
    rle_last = -1; rle_count = 0
    while not end:
        selector.reset()
        for m in mtfm: m.reset()
        randomized = dec.sym(initial)
        tindex = dec.bitstring(initial, blockbits)
        block = bytearray()
        mtf = list(range(256))
        while True:
            sel = dec.sym(selector)
            if sel < 2:
                zstate = 1; zcount = 0
                while sel < 2:
                    zcount += zstate if sel == 0 else 2 * zstate
                    zstate <<= 1
                    sel = dec.sym(selector)
                block += bytes([mtf[0]]) * zcount
            if sel == 10: break
            if sel == 2: s = 1
            else: s = dec.sym(mtfm[sel - 3])
            b = mtf.pop(s); mtf.insert(0, b)
            block.append(b)
        nb = len(block)
        # inverse BWT
        counts = [0] * 256
        for b in block: counts[b] += 1
        cum = [0] * 256; t = 0
        for i in range(256): cum[i] = t; t += counts[i]
        transform = [0] * nb
        for i, b in enumerate(block):
            transform[cum[b]] = i; cum[b] += 1
        idx = tindex
        bwt = bytearray(nb)
        for i in range(nb):
            idx = transform[idx]; bwt[i] = block[idx]
        if randomized:
            ri = 0; rc = RNUMS[0] - 1
            for i in range(nb):
                if rc == 0:
                    bwt[i] ^= 1; ri = (ri + 1) & 511; rc = RNUMS[ri]
                rc -= 1
        # RLE
        o = out
        for b in bwt:
            if rle_count == 4:
                o += bytes([rle_last]) * b
                rle_count = 0
                continue
            if b == rle_last: rle_count += 1
            else: rle_last = b; rle_count = 1
            o.append(b)
        end = dec.sym(initial)
    return bytes(out)

RNUMS = [619,720,127,481,931,816,813,233,566,247,985,724,205,454,863,491,741,242,949,214,733,859,335,708,621,574,73,654,730,472,419,436,278,496,867,210,399,680,480,51,878,465,811,169,869,675,611,697,867,561,862,687,507,283,482,129,807,591,733,623,150,238,59,379,684,877,625,169,643,105,170,607,520,932,727,476,693,425,174,647,73,122,335,530,442,853,695,249,445,515,909,545,703,919,874,474,882,500,594,612,641,801,220,162,819,984,589,513,495,799,161,604,958,533,221,400,386,867,600,782,382,596,414,171,516,375,682,485,911,276,98,553,163,354,666,933,424,341,533,870,227,730,475,186,263,647,537,686,600,224,469,68,770,919,190,373,294,822,808,206,184,943,795,384,383,461,404,758,839,887,715,67,618,276,204,918,873,777,604,560,951,160,578,722,79,804,96,409,713,940,652,934,970,447,318,353,859,672,112,785,645,863,803,350,139,93,354,99,820,908,609,772,154,274,580,184,79,626,630,742,653,282,762,623,680,81,927,626,789,125,411,521,938,300,821,78,343,175,128,250,170,774,972,275,999,639,495,78,352,126,857,956,358,619,580,124,737,594,701,612,669,112,134,694,363,992,809,743,168,974,944,375,748,52,600,747,642,182,862,81,344,805,988,739,511,655,814,334,249,515,897,955,664,981,649,113,974,459,893,228,433,837,553,268,926,240,102,654,459,51,686,754,806,760,493,403,415,394,687,700,946,670,656,610,738,392,760,799,887,653,978,321,576,617,626,502,894,679,243,440,680,879,194,572,640,724,926,56,204,700,707,151,457,449,797,195,791,558,945,679,297,59,87,824,713,663,412,693,342,606,134,108,571,364,631,212,174,643,304,329,343,97,430,751,497,314,983,374,822,928,140,206,73,263,980,736,876,478,430,305,170,514,364,692,829,82,855,953,676,246,369,970,294,750,807,827,150,790,288,923,804,378,215,828,592,281,565,555,710,82,896,831,547,261,524,462,293,465,502,56,661,821,976,991,658,869,905,758,745,193,768,550,608,933,378,286,215,979,792,961,61,688,793,644,986,403,106,366,905,644,372,567,466,434,645,210,389,550,919,135,780,773,635,389,707,100,626,958,165,504,920,176,193,713,857,265,203,50,668,108,645,990,626,197,510,357,358,850,858,364,936,638]

def extract(archive, outdir):
    buf = open(archive, 'rb').read()
    for e in parse(buf):
        p = os.path.join(outdir, *[n.replace('/', '_').replace('\r', '').strip() for n in e.path])
        if e.isdir:
            os.makedirs(p, exist_ok=True); continue
        def get(start, comp, ln, method):
            raw = buf[start:start + comp]
            if method == 0 and comp != ln: method = 15
            if method == 0: return raw
            if method == 15: return arsenic(raw, ln)
            raise ValueError('method %d' % method)
        d = get(e.dstart, e.dcomp, e.dlen, e.dmethod) if e.dlen else b''
        open(p, 'wb').write(d)
        if e.rlen:
            r = get(e.rstart, e.rcomp, e.rlen, e.rmethod)
            if len(r) != e.rlen: print('WARN rsrc len', e.name, len(r), e.rlen)
            open(p + '.rsrc', 'wb').write(r)
        open(p + '.finf', 'wb').write(e.type + e.creator)
        print('extracted', '/'.join(e.path))


if __name__ == '__main__' and len(sys.argv) == 4 and sys.argv[1] == '--extract':
    extract(sys.argv[2], sys.argv[3])
elif __name__ == '__main__':
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip())
    buf = open(sys.argv[1], 'rb').read()
    for e in parse(buf):
        if e.isdir:
            print('DIR ', '/'.join(e.path))
        else:
            print('FILE', '/'.join(e.path), e.type, e.creator, 'data', e.dlen, e.dcomp, e.dmethod, 'rsrc', e.rlen, e.rcomp, e.rmethod)
