"""Compact 68000/68020 disassembler for classic Mac CODE resources.

Produces one instruction per call; designed for readability of compiler output
(CodeWarrior / THINK C), not for round-trip assembly.
"""
import struct

CC = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']
SZ = {0: 'b', 1: 'w', 2: 'l'}


class Bad(Exception):
    pass


class Insn:
    pass

    def __init__(self):
        self.target = None; self.kind = None; self.trap = None; self.jtcall = None


class Dis:
    def __init__(self, code, base=0, traps=None):
        self.c = code; self.base = base; self.traps = traps or {}

    def w(self, o):
        if o + 2 > len(self.c): raise Bad('eof')
        return struct.unpack_from('>H', self.c, o)[0]

    def sw(self, o):
        if o + 2 > len(self.c): raise Bad('eof')
        return struct.unpack_from('>h', self.c, o)[0]

    def l(self, o):
        if o + 4 > len(self.c): raise Bad('eof')
        return struct.unpack_from('>I', self.c, o)[0]

    def sl(self, o):
        if o + 4 > len(self.c): raise Bad('eof')
        return struct.unpack_from('>i', self.c, o)[0]

    # ---- effective addresses ----
    def ea(self, mode, reg, size, p):
        """returns (text, newp, info); info is also appended to self.infos"""
        t, np_, info = self._ea(mode, reg, size, p)
        info['size'] = size
        self.infos.append(info)
        return t, np_, info

    def _ea(self, mode, reg, size, p):
        if mode == 0: return 'd%d' % reg, p, {'m': 'dreg', 'r': reg}
        if mode == 1: return 'a%d' % reg, p, {'m': 'areg', 'r': reg}
        if mode == 2: return '(a%d)' % reg, p, {'m': 'ind', 'r': reg}
        if mode == 3: return '(a%d)+' % reg, p, {'m': 'postinc', 'r': reg}
        if mode == 4: return '-(a%d)' % reg, p, {'m': 'predec', 'r': reg}
        if mode == 5:
            d = self.sw(p)
            return '%d(a%d)' % (d, reg), p + 2, {'m': 'disp', 'r': reg, 'd': d}
        if mode == 6:
            return self.indexed('a%d' % reg, p, None, reg)
        if mode == 7:
            if reg == 0:
                v = self.sw(p); return '$%x.w' % (v & 0xFFFFFFFF), p + 2, {'m': 'absw', 'v': v & 0xFFFFFFFF}
            if reg == 1:
                v = self.l(p); return '$%x.l' % v, p + 4, {'m': 'absl', 'v': v, 'at': p}
            if reg == 2:
                d = self.sw(p); t = p + d
                return 'L%x(pc)' % (t + self.base), p + 2, {'m': 'pcrel', 't': t}
            if reg == 3:
                return self.indexed('pc', p, p, None)
            if reg == 4:
                if size == 'b': v = self.w(p) & 0xFF; return '#%s' % hx(v), p + 2, {'m': 'imm', 'v': v}
                if size == 'w': v = self.w(p); return '#%s' % hx(v), p + 2, {'m': 'imm', 'v': v}
                if size == 'l': v = self.l(p); return '#%s' % hx(v), p + 4, {'m': 'imm', 'v': v, 'at': p}
                if size == 's': v = self.l(p); return '#%s' % hx(v), p + 4, {'m': 'imm', 'v': v}
                if size == 'd': v = self.c[p:p + 8].hex(); return '#$%s' % v, p + 8, {'m': 'imm'}
                if size == 'x': v = self.c[p:p + 12].hex(); return '#$%s' % v, p + 12, {'m': 'imm'}
                if size == 'p': v = self.c[p:p + 12].hex(); return '#$%s' % v, p + 12, {'m': 'imm'}
                raise Bad('imm size')
        raise Bad('ea')

    def indexed(self, basename, p, pcbase, areg):
        ext = self.w(p); p += 2
        xi = {'xa': bool(ext & 0x8000), 'xr': (ext >> 12) & 7, 'xl': bool(ext & 0x800), 'sc': 1 << ((ext >> 9) & 3)}
        xr = ('a' if ext & 0x8000 else 'd') + str((ext >> 12) & 7)
        xs = '.l' if ext & 0x800 else '.w'
        scale = 1 << ((ext >> 9) & 3)
        idx = xr + xs + ('*%d' % scale if scale > 1 else '')
        if not ext & 0x100:
            d = ext & 0xFF
            if d & 0x80: d -= 0x100
            if pcbase is not None:
                return 'L%x(pc,%s)' % (pcbase + d + self.base, idx), p, dict(xi, m='pcidx', t=pcbase + d)
            return '%d(%s,%s)' % (d, basename, idx), p, dict(xi, m='idx', r=areg, d=d)
        # full extension word (68020)
        bs = ext & 0x80; is_ = ext & 0x40; bdsz = (ext >> 4) & 3; iis = ext & 7
        bd = 0
        if bdsz == 2: bd = self.sw(p); p += 2
        elif bdsz == 3: bd = self.sl(p); p += 4
        od = 0
        if iis & 3 == 2: od = self.sw(p); p += 2
        elif iis & 3 == 3: od = self.sl(p); p += 4
        b = '' if bs else basename
        i = '' if is_ else idx
        if iis == 0:
            parts = [x for x in (str(bd) if bd else '', b, i) if x]
            return '(%s)' % ','.join(parts), p, dict(xi, m='fullidx', r=areg, pcb=pcbase, bd=bd, bs=bool(bs), is_=bool(is_))
        inner = ','.join(x for x in (str(bd) if bd else '', b) if x)
        if iis & 4:
            s = '([%s],%s,%d)' % (inner, i, od)
        else:
            s = '([%s,%s],%d)' % (inner, i, od) if i else '([%s],%d)' % (inner, od)
        return s, p, {'m': 'memind'}  # not supported by recompiler

    # ---- main decode ----
    def dis(self, off):
        ins = Insn(); ins.addr = off + self.base
        self.infos = []
        op = self.w(off); p = off + 2
        ins.op = op
        try:
            mn, ops, p = self.decode(op, p, ins)
        except Bad:
            mn, ops, p = 'dc.w', [hx(op)], off + 2
        ins.size = p - off; ins.mn = mn; ins.ops = ops
        ins.raw = self.c[off:p]
        ins.infos = self.infos
        ins.text = mn + (' ' + ','.join(ops) if ops else '')
        return ins

    def decode(self, op, p, ins):
        hi = op >> 12
        mode = (op >> 3) & 7; reg = op & 7
        if hi == 0: return self.grp0(op, p, ins)
        if hi in (1, 2, 3):
            sz = {1: 'b', 3: 'w', 2: 'l'}[hi]
            s, p, _ = self.ea(mode, reg, sz, p)
            dmode = (op >> 6) & 7; dreg = (op >> 9) & 7
            if dmode == 1:
                return 'movea.' + sz, [s, 'a%d' % dreg], p
            d, p, _ = self.ea(dmode, dreg, sz, p)
            return 'move.' + sz, [s, d], p
        if hi == 4: return self.grp4(op, p, ins)
        if hi == 5:
            if (op >> 6) & 3 == 3:
                cc = CC[(op >> 8) & 15]
                if mode == 1:
                    d = self.sw(p); t = p + d
                    ins.target = t; ins.kind = 'cbranch'
                    return 'db' + cc, ['d%d' % reg, 'L%x' % (t + self.base)], p + 2
                if mode == 7 and reg in (2, 3, 4):
                    if reg == 2: p += 2
                    elif reg == 3: p += 4
                    return 'trap' + cc, [], p
                s, p, _ = self.ea(mode, reg, 'b', p)
                return 's' + cc, [s], p
            q = (op >> 9) & 7 or 8
            sz = SZ[(op >> 6) & 3]
            s, p, _ = self.ea(mode, reg, sz, p)
            return ('subq.' if op & 0x100 else 'addq.') + sz, ['#%d' % q, s], p
        if hi == 6:
            cond = (op >> 8) & 15
            d = op & 0xFF
            if d == 0: d = self.sw(p); p += 2
            elif d == 0xFF: d = self.sl(p); p += 4
            else:
                if d & 0x80: d -= 0x100
            t = (ins.addr - self.base) + 2 + d
            ins.target = t
            if cond == 0: ins.kind = 'branch'; return 'bra', ['L%x' % (t + self.base)], p
            if cond == 1: ins.kind = 'call'; return 'bsr', ['L%x' % (t + self.base)], p
            ins.kind = 'cbranch'
            return 'b' + CC[cond], ['L%x' % (t + self.base)], p
        if hi == 7:
            if op & 0x100: raise Bad('moveq')
            v = op & 0xFF
            if v & 0x80: v -= 0x100
            return 'moveq', ['#%d' % v, 'd%d' % ((op >> 9) & 7)], p
        if hi in (8, 9, 0xB, 0xC, 0xD): return self.arith(op, p, ins)
        if hi == 0xA:
            ins.trap = op
            name = self.traps.get(op & 0xFBFF if (op & 0x0800) == 0 else op & 0xFFFF, None)
            if name is None: name = self.traps.get(op, None)
            if name is None and op & 0x0800: name = self.traps.get(op & 0xF9FF)
            return '_' + (name or 'Trap%04X' % op), [], p
        if hi == 0xE: return self.shift(op, p, ins)
        if hi == 0xF: return self.fline(op, p, ins)
        raise Bad('op')

    def grp0(self, op, p, ins):
        mode = (op >> 3) & 7; reg = op & 7
        if op & 0x100:
            if mode == 1:
                d = self.sw(p); dr = (op >> 9) & 7
                opm = (op >> 6) & 3
                sz = 'l' if opm & 1 else 'w'
                if opm < 2: return 'movep.' + sz, ['%d(a%d)' % (d, reg), 'd%d' % dr], p + 2
                return 'movep.' + sz, ['d%d' % dr, '%d(a%d)' % (d, reg)], p + 2
            bop = ['btst', 'bchg', 'bclr', 'bset'][(op >> 6) & 3]
            s, p, _ = self.ea(mode, reg, 'b', p)
            return bop, ['d%d' % ((op >> 9) & 7), s], p
        top = (op >> 9) & 7
        if top == 4:
            bop = ['btst', 'bchg', 'bclr', 'bset'][(op >> 6) & 3]
            b = self.w(p) & 0xFF; p += 2
            s, p, _ = self.ea(mode, reg, 'b', p)
            return bop, ['#%d' % b, s], p
        szb = (op >> 6) & 3
        if szb == 3:
            # 68020: cas, chk2/cmp2, callm/rtm
            if top in (0, 1, 2):
                ext = self.w(p); p += 2
                sz = ['b', 'w', 'l'][top]
                s, p, _ = self.ea(mode, reg, sz, p)
                r = ('a' if ext & 0x8000 else 'd') + str((ext >> 12) & 7)
                return ('chk2.' if ext & 0x800 else 'cmp2.') + sz, [s, r], p
            if top in (5, 6, 7):
                ext = self.w(p); p += 2
                sz = {5: 'b', 6: 'w', 7: 'l'}[top]
                s, p, _ = self.ea(mode, reg, sz, p)
                return 'cas.' + sz, ['d%d' % (ext & 7), 'd%d' % ((ext >> 6) & 7), s], p
            raise Bad('grp0')
        sz = SZ[szb]
        names = {0: 'ori', 1: 'andi', 2: 'subi', 3: 'addi', 5: 'eori', 6: 'cmpi'}
        if top not in names: raise Bad('grp0')
        imm, p, _ = self.ea(7, 4, sz, p)
        if mode == 7 and reg == 4:
            return names[top] + '.' + sz, [imm, 'ccr' if sz == 'b' else 'sr'], p
        d, p, _ = self.ea(mode, reg, sz, p)
        return names[top] + '.' + sz, [imm, d], p

    def grp4(self, op, p, ins):
        mode = (op >> 3) & 7; reg = op & 7
        if op == 0x4E75: ins.kind = 'ret'; return 'rts', [], p
        if op == 0x4E71: return 'nop', [], p
        if op == 0x4E73: ins.kind = 'ret'; return 'rte', [], p
        if op == 0x4E77: ins.kind = 'ret'; return 'rtr', [], p
        if op == 0x4E74: ins.kind = 'ret'; return 'rtd', ['#%d' % self.sw(p)], p + 2
        if op == 0x4E76: return 'trapv', [], p
        if op == 0x4E70: return 'reset', [], p
        if op == 0x4E72: return 'stop', ['#%s' % hx(self.w(p))], p + 2
        if op & 0xFFF0 == 0x4E40: return 'trap', ['#%d' % (op & 15)], p
        if op & 0xFFF8 == 0x4E50: return 'link.w', ['a%d' % reg, '#%d' % self.sw(p)], p + 2
        if op & 0xFFF8 == 0x4808: return 'link.l', ['a%d' % reg, '#%d' % self.sl(p)], p + 4
        if op & 0xFFF8 == 0x4E58: return 'unlk', ['a%d' % reg], p
        if op & 0xFFF8 == 0x4E60: return 'move.l', ['a%d' % reg, 'usp'], p
        if op & 0xFFF8 == 0x4E68: return 'move.l', ['usp', 'a%d' % reg], p
        if op & 0xFFFE == 0x4E7A:
            ext = self.w(p); p += 2
            r = ('a' if ext & 0x8000 else 'd') + str((ext >> 12) & 7)
            cr = 'cr%03x' % (ext & 0xFFF)
            return 'movec', ([cr, r] if op & 1 == 0 else [r, cr]), p
        if op & 0xFFF8 == 0x4840: return 'swap', ['d%d' % reg], p
        if op & 0xFFF8 == 0x4848: return 'bkpt', ['#%d' % reg], p
        if op & 0xFFF8 == 0x4880: return 'ext.w', ['d%d' % reg], p
        if op & 0xFFF8 == 0x48C0: return 'ext.l', ['d%d' % reg], p
        if op & 0xFFF8 == 0x49C0: return 'extb.l', ['d%d' % reg], p
        if op & 0xFFC0 == 0x4E80:
            s, p, info = self.ea(mode, reg, 'l', p)
            ins.kind = 'call'; self._tgt(ins, info)
            return 'jsr', [s], p
        if op & 0xFFC0 == 0x4EC0:
            s, p, info = self.ea(mode, reg, 'l', p)
            ins.kind = 'jump'; self._tgt(ins, info)
            return 'jmp', [s], p
        if op & 0xFB80 == 0x4880:
            # movem
            mask = self.w(p); p += 2
            sz = 'l' if op & 0x40 else 'w'
            s, p, _ = self.ea(mode, reg, sz, p)
            regs = regmask(mask, mode == 4)
            if op & 0x400: return 'movem.' + sz, [s, regs], p
            return 'movem.' + sz, [regs, s], p
        if op & 0xF1C0 == 0x41C0:
            s, p, info = self.ea(mode, reg, 'l', p)
            return 'lea', [s, 'a%d' % ((op >> 9) & 7)], p
        if op & 0xFFC0 == 0x4840:
            s, p, info = self.ea(mode, reg, 'l', p)
            return 'pea', [s], p
        if op & 0xF140 == 0x4100:
            sz = 'l' if (op >> 7) & 3 == 2 else 'w'
            if (op >> 7) & 3 not in (2, 3): raise Bad('chk')
            s, p, _ = self.ea(mode, reg, sz, p)
            return 'chk.' + sz, [s, 'd%d' % ((op >> 9) & 7)], p
        if op & 0xFFC0 == 0x4C00 or op & 0xFFC0 == 0x4C40:
            ext = self.w(p); p += 2
            s, p, _ = self.ea(mode, reg, 'l', p)
            sgn = 's' if ext & 0x800 else 'u'
            dl = (ext >> 12) & 7; dh = ext & 7
            if op & 0xFFC0 == 0x4C00:
                if ext & 0x400: return 'mul%s.l' % sgn, [s, 'd%d:d%d' % (dh, dl)], p
                return 'mul%s.l' % sgn, [s, 'd%d' % dl], p
            if ext & 0x400: return 'div%s.l' % sgn, [s, 'd%d:d%d' % (dh, dl)], p
            if dh != dl: return 'div%sl.l' % sgn, [s, 'd%d:d%d' % (dh, dl)], p
            return 'div%s.l' % sgn, [s, 'd%d' % dl], p
        if op & 0xFFC0 == 0x40C0:
            s, p, _ = self.ea(mode, reg, 'w', p); return 'move', ['sr', s], p
        if op & 0xFFC0 == 0x42C0:
            s, p, _ = self.ea(mode, reg, 'w', p); return 'move', ['ccr', s], p
        if op & 0xFFC0 == 0x44C0:
            s, p, _ = self.ea(mode, reg, 'w', p); return 'move', [s, 'ccr'], p
        if op & 0xFFC0 == 0x46C0:
            s, p, _ = self.ea(mode, reg, 'w', p); return 'move', [s, 'sr'], p
        if op & 0xFFC0 == 0x4800:
            s, p, _ = self.ea(mode, reg, 'b', p); return 'nbcd', [s], p
        if op & 0xFFC0 == 0x4AC0:
            s, p, _ = self.ea(mode, reg, 'b', p); return 'tas', [s], p
        szb = (op >> 6) & 3
        if szb == 3: raise Bad('grp4')
        sz = SZ[szb]
        names = {0x40: 'negx', 0x42: 'clr', 0x44: 'neg', 0x46: 'not', 0x4A: 'tst'}
        k = op >> 8
        if k in names:
            s, p, _ = self.ea(mode, reg, sz, p)
            return names[k] + '.' + sz, [s], p
        raise Bad('grp4')

    def _tgt(self, ins, info):
        m = info.get('m')
        if m == 'pcrel': ins.target = info['t']
        elif m == 'absl': ins.target = ('abs', info['v'], info['at'])
        elif m == 'disp' and info['r'] == 5: ins.jtcall = info['d']

    def arith(self, op, p, ins):
        hi = op >> 12
        mode = (op >> 3) & 7; reg = op & 7
        dr = (op >> 9) & 7
        opm = (op >> 6) & 7
        base = {8: 'or', 9: 'sub', 0xB: 'cmp', 0xC: 'and', 0xD: 'add'}[hi]
        if hi in (8, 0xC) and opm in (3, 7):
            s, p, _ = self.ea(mode, reg, 'w', p)
            n = ('div' if hi == 8 else 'mul') + ('s' if opm == 7 else 'u') + '.w'
            return n, [s, 'd%d' % dr], p
        if hi in (9, 0xB, 0xD) and opm in (3, 7):
            sz = 'l' if opm == 7 else 'w'
            s, p, _ = self.ea(mode, reg, sz, p)
            return base + 'a.' + sz, [s, 'a%d' % dr], p
        if opm >= 4 and mode in (0, 1) and hi != 0xB:
            sz = SZ[opm - 4]
            if hi == 8 and opm == 4: return 'sbcd', self._xops(mode, reg, dr), p
            if hi == 0xC and opm == 4: return 'abcd', self._xops(mode, reg, dr), p
            if hi == 0xC and opm == 5:
                if mode == 0: return 'exg', ['d%d' % dr, 'd%d' % reg], p
                return 'exg', ['a%d' % dr, 'a%d' % reg], p
            if hi == 0xC and opm == 6 and mode == 1: return 'exg', ['d%d' % dr, 'a%d' % reg], p
            if hi == 8 and opm in (5, 6): return ('pack' if opm == 5 else 'unpk'), self._xops(mode, reg, dr) + ['#%d' % self.w(p)], p + 2
            if hi in (9, 0xD):
                return base + 'x.' + sz, self._xops(mode, reg, dr), p
            raise Bad('arith')
        if hi == 0xB and opm >= 4:
            sz = SZ[opm - 4]
            if mode == 1: return 'cmpm.' + sz, ['(a%d)+' % reg, '(a%d)+' % dr], p
            s, p, _ = self.ea(mode, reg, sz, p)
            return 'eor.' + sz, ['d%d' % dr, s], p
        sz = SZ[opm & 3]
        s, p, _ = self.ea(mode, reg, sz, p)
        if opm < 4: return base + '.' + sz, [s, 'd%d' % dr], p
        return base + '.' + sz, ['d%d' % dr, s], p

    def _xops(self, mode, reg, dr):
        if mode == 0: return ['d%d' % reg, 'd%d' % dr]
        return ['-(a%d)' % reg, '-(a%d)' % dr]

    def shift(self, op, p, ins):
        mode = (op >> 3) & 7; reg = op & 7
        if (op >> 6) & 3 == 3:
            if op & 0x800:
                # bitfield
                ext = self.w(p); p += 2
                names = ['bftst', 'bfextu', 'bfchg', 'bfexts', 'bfclr', 'bfffo', 'bfset', 'bfins']
                n = names[(op >> 8) & 7]
                s, p, _ = self.ea(mode, reg, 'l', p)
                o = ext >> 6 & 31; w = ext & 31
                os_ = 'd%d' % (o & 7) if ext & 0x800 else str(o)
                ws = 'd%d' % (w & 7) if ext & 0x20 else str(w or 32)
                fld = '%s{%s:%s}' % (s, os_, ws)
                r = 'd%d' % ((ext >> 12) & 7)
                if n in ('bfextu', 'bfexts', 'bfffo'): return n, [fld, r], p
                if n == 'bfins': return n, [r, fld], p
                return n, [fld], p
            t = ['as', 'ls', 'rox', 'ro'][(op >> 9) & 3]
            s, p, _ = self.ea(mode, reg, 'w', p)
            return t + ('l' if op & 0x100 else 'r') + '.w', [s], p
        sz = SZ[(op >> 6) & 3]
        t = ['as', 'ls', 'rox', 'ro'][(op >> 3) & 3]
        n = t + ('l' if op & 0x100 else 'r') + '.' + sz
        c = (op >> 9) & 7
        if op & 0x20: return n, ['d%d' % c, 'd%d' % reg], p
        return n, ['#%d' % (c or 8), 'd%d' % reg], p

    def fline(self, op, p, ins):
        # 68881 FPU (coprocessor id 1)
        cpid = (op >> 9) & 7
        if cpid != 1: raise Bad('fline')
        typ = (op >> 6) & 7
        mode = (op >> 3) & 7; reg = op & 7
        if typ == 0:
            ext = self.w(p); p += 2
            rm = ext & 0x4000
            opc = (ext >> 13) & 7
            fops = {0: 'fmove', 1: 'fint', 2: 'fsinh', 3: 'fintrz', 4: 'fsqrt', 6: 'flognp1', 8: 'fetoxm1', 9: 'ftanh',
                    10: 'fatan', 12: 'fasin', 13: 'fatanh', 14: 'fsin', 15: 'ftan', 16: 'fetox', 17: 'ftwotox',
                    18: 'ftentox', 20: 'flogn', 21: 'flog10', 22: 'flog2', 24: 'fabs', 25: 'fcosh', 26: 'fneg',
                    28: 'facos', 29: 'fcos', 30: 'fgetexp', 31: 'fgetman', 32: 'fdiv', 33: 'fmod', 34: 'fadd',
                    35: 'fmul', 36: 'fsgldiv', 37: 'frem', 38: 'fscale', 39: 'fsglmul', 40: 'fsub', 56: 'fcmp',
                    58: 'ftst'}
            fsz = ['l', 's', 'x', 'p', 'w', 'd', 'b', 'p'][(ext >> 10) & 7]
            if opc in (0, 2):
                n = fops.get(ext & 0x7F, 'fop%02x' % (ext & 0x7F))
                dst = 'fp%d' % ((ext >> 7) & 7)
                if rm:
                    s, p, _ = self.ea(mode, reg, fsz, p)
                    return n + '.' + fsz, [s, dst], p
                return n + '.x', ['fp%d' % ((ext >> 10) & 7), dst], p
            if opc == 3:
                s, p, _ = self.ea(mode, reg, fsz, p)
                return 'fmove.' + fsz, ['fp%d' % ((ext >> 7) & 7), s], p
            if opc in (4, 5):
                s, p, _ = self.ea(mode, reg, 'l', p)
                return 'fmove.l', ([s, 'fpcr'] if opc == 4 else ['fpcr', s]), p
            if opc in (6, 7):
                s, p, _ = self.ea(mode, reg, 'l', p)
                return 'fmovem.x', ([s, '#%x' % (ext & 0xFF)] if opc == 6 else ['#%x' % (ext & 0xFF), s]), p
        if typ in (2, 3):
            if typ == 2: d = self.sw(p); p += 2
            else: d = self.sl(p); p += 4
            t = (ins.addr - self.base) + 2 + d
            ins.target = t; ins.kind = 'cbranch'
            return 'fb%02x' % (op & 0x3F), ['L%x' % (t + self.base)], p
        if typ == 1:
            ext = self.w(p); p += 2
            if mode == 1:
                d = self.sw(p); t = p + d; ins.target = t; ins.kind = 'cbranch'
                return 'fdb%02x' % (ext & 0x3F), ['d%d' % reg, 'L%x' % (t + self.base)], p + 2
            s, p, _ = self.ea(mode, reg, 'b', p)
            return 'fs%02x' % (ext & 0x3F), [s], p
        if typ in (4, 5):
            s, p, _ = self.ea(mode, reg, 'l', p)
            return 'fsave' if typ == 4 else 'frestore', [s], p
        raise Bad('fline')


def regmask(mask, rev):
    if rev: mask = int('{:016b}'.format(mask)[::-1], 2)
    names = ['d%d' % i for i in range(8)] + ['a%d' % i for i in range(8)]
    return '/'.join(n for i, n in enumerate(names) if mask & (1 << i)) or '0'


def hx(v):
    return '$%x' % v if v > 9 else str(v)
