"""Static recompiler: 68k CODE resources of Bonkheads Deluxe -> portable C.

Every original function (named from its MacsBug symbol) becomes a C function
that operates on the emulated register file and big-endian memory defined in
src/runtime/cpu.h. Toolbox traps call into the HLE layer (src/toolbox).

usage: python -I tools/recomp.py <app.rsrc> <outdir>
"""
import os, re, struct, sys
sys.path.insert(0, os.path.dirname(__file__))
import rsrc, m68k, traps, a5world, dumpcode

# ---------------------------------------------------------------- layout
LOWMEM_END = 0x00004000
SEG_BASE0 = 0x00010000
A5_WORLD = 0x00080000          # start of below-A5 globals
STACK_TOP = 0x00200000
HEAP_START = 0x00200000

X, N, Z, V, C = 16, 8, 4, 2, 1
ALL = X | N | Z | V | C
NZVC = N | Z | V | C

CCUSE = {'t': 0, 'f': 0, 'hi': C | Z, 'ls': C | Z, 'cc': C, 'cs': C, 'ne': Z, 'eq': Z, 'vc': V, 'vs': V,
         'pl': N, 'mi': N, 'ge': N | V, 'lt': N | V, 'gt': N | V | Z, 'le': N | V | Z}
CCEXPR = {'t': '1', 'f': '0', 'hi': '(!fC && !fZ)', 'ls': '(fC || fZ)', 'cc': '(!fC)', 'cs': '(fC)',
          'ne': '(!fZ)', 'eq': '(fZ)', 'vc': '(!fV)', 'vs': '(fV)', 'pl': '(!fN)', 'mi': '(fN)',
          'ge': '(fN == fV)', 'lt': '(fN != fV)', 'gt': '(!fZ && fN == fV)', 'le': '(fZ || fN != fV)'}

SZN = {'b': 1, 'w': 2, 'l': 4}
MASK = {'b': '0xFFu', 'w': '0xFFFFu', 'l': '0xFFFFFFFFu'}
CT = {'b': 'uint8_t', 'w': 'uint16_t', 'l': 'uint32_t'}
ST = {'b': 'int8_t', 'w': 'int16_t', 'l': 'int32_t'}


class Unsupported(Exception):
    pass


class Seg:
    pass


class Program:
    def __init__(self, path):
        self.r = rsrc.read_rsrc(open(path, 'rb').read())
        self.world = a5world.build(self.r)
        self.below = self.world['below']; self.above = self.world['above']
        self.A5 = A5_WORLD + self.below
        self.segs = {}
        base = SEG_BASE0
        for sid in sorted(self.r['CODE']):
            if sid == 0: continue
            body = self.r['CODE'][sid][2]
            s = Seg(); s.sid = sid; s.name = self.r['CODE'][sid][0]; s.body = bytearray(body)
            s.base = base
            if sid == 1:
                s.start, s.end = 4, len(body)
                s.relocs = self.world['code1_relocs']
            else:
                s.start = 12; s.end = struct.unpack_from('>I', body, 8)[0]
                s.relocs = a5world.segment_relocs(body)
            s.rkind = {}
            for kind, lst in s.relocs:
                for o in lst: s.rkind[o] = kind
            self.segs[sid] = s
            base = (base + len(body) + 0xFFF) & ~0xFFF
        assert base <= A5_WORLD
        self.jt = a5world.jumptable(self.world, self.r)
        # apply relocations to the segment images (as the startup code does)
        c1 = self.segs[1].base
        for s in self.segs.values():
            for o, kind in s.rkind.items():
                v = struct.unpack_from('>I', s.body, o)[0]
                add = {'a5': self.A5, 'code1': c1, 'self': s.base}[kind]
                struct.pack_into('>I', s.body, o, (v + add) & 0xFFFFFFFF)
        # A5 world image with relocations and loaded jump table
        img = bytearray(self.world['img'])
        for kind, lst in self.world['relocs']:
            add = {'a5': self.A5, 'code1': c1, 'self': self.A5}[kind]
            for o in lst:
                p = self.below + o
                v = struct.unpack_from('>I', img, p)[0]
                struct.pack_into('>I', img, p, (v + add) & 0xFFFFFFFF)
        for slot, (sid, off) in self.jt.items():
            if sid in self.segs:
                p = self.below + slot
                struct.pack_into('>HI', img, p, 0x4EF9, self.segs[sid].base + off)
        self.img = img
        # function discovery
        self.names = {}
        extra = {}
        for slot, (sid, off) in self.jt.items(): extra.setdefault(sid, set()).add(off)
        self.entries = set()
        for sid, s in self.segs.items():
            for (fn, st, ce, ns) in dumpcode.split_functions(bytes(self.r['CODE'][sid][2]), s.start, s.end,
                                                             extra.get(sid, ())):
                a = s.base + st
                self.entries.add(a)
                self.names[a] = fn
        for slot, (sid, off) in self.jt.items():
            if sid in self.segs: self.entries.add(self.segs[sid].base + off)

    def seg_of(self, addr):
        for s in self.segs.values():
            if s.base <= addr < s.base + len(s.body): return s
        return None

    def jt_target(self, addr):
        """If addr is a loaded jump table slot, return its target address."""
        o = addr - self.A5
        if o in self.jt:
            sid, off = self.jt[o]
            if sid in self.segs: return self.segs[sid].base + off
        return None

    def cname(self, addr):
        n = self.names.get(addr)
        s = self.seg_of(addr)
        if n and not n.startswith(('sub_', 'anon_')):
            return 'f_%s' % re.sub(r'[^A-Za-z0-9_]', '_', n)
        return 'f_s%d_%x' % (s.sid, addr - s.base)


class FuncGen:
    def __init__(self, prog, entry):
        self.p = prog; self.entry = entry
        self.seg = prog.seg_of(entry)
        self.d = m68k.Dis(bytes(self.seg.body), self.seg.base, traps.TRAPS)
        self.insns = {}
        self.fallprev = {}
        self.succ = {}
        self.calls = set()
        self.notes = []

    # ---------------------------------------------------------- decoding
    def dec(self, addr):
        off = addr - self.seg.base
        ins = self.d.dis(off)
        return ins

    def abs_value(self, ins, info):
        return info['v']

    def discover(self):
        work = [self.entry]
        while work:
            a = work.pop()
            if a in self.insns: continue
            if not (self.seg.base + self.seg.start <= a < self.seg.base + self.seg.end):
                raise Unsupported('flow leaves segment at %x' % a)
            ins = self.dec(a)
            self.insns[a] = ins
            nxt = a + ins.size
            self.fallprev.setdefault(nxt, a)
            succ = []
            mn = ins.mn
            kind = self.classify(ins)
            ins.kind2 = kind
            if kind in ('ret', 'tail', 'ijump', 'unimpl'):
                pass
            elif kind == 'goto':
                succ.append(ins.tgt)
            elif kind == 'cbranch':
                succ += [ins.tgt, nxt]
            elif kind == 'switch':
                succ += list(ins.cases.values())
            else:
                succ.append(nxt)
            self.succ[a] = succ
            work += succ
        return self

    def classify(self, ins):
        mn = ins.mn
        if mn == 'dc.w': return 'unimpl'
        if mn in ('rts', 'rtd', 'rte', 'rtr'): return 'ret'
        if mn == 'bra': ins.tgt = ins.target + self.seg.base; return 'goto'
        if mn.startswith('b') and ins.kind == 'cbranch': ins.tgt = ins.target + self.seg.base; return 'cbranch'
        if mn.startswith('db'): ins.tgt = ins.target + self.seg.base; return 'cbranch'
        if mn == 'bsr':
            ins.callee = ins.target + self.seg.base; return 'call'
        if mn in ('jsr', 'jmp'):
            info = ins.infos[0]
            t = self.static_target(info)
            if mn == 'jsr':
                ins.callee = t; return 'call'
            if t is not None:
                if t in self.p.entries:
                    ins.callee = t; return 'tail'
                ins.tgt = t; return 'goto'
            if info['m'] == 'pcidx':
                cases = self.switch_cases(ins, info)
                if cases is not None:
                    ins.cases = cases; return 'switch'
            return 'ijump'
        return 'op'

    def static_target(self, info):
        m = info['m']
        if m == 'pcrel': return info['t'] + self.seg.base
        if m == 'absl':
            v = info['v']
            jt = self.p.jt_target(v)
            return jt if jt is not None else v
        if m == 'disp' and info['r'] == 5:
            return self.p.jt_target(self.p.A5 + info['d'])
        return None

    def switch_cases(self, ins, info):
        tbase = info['t']
        # 68020 detection trick: moveq #k,dn ; jmp T(pc,dn.w*s)
        prev = self.prev_insn(ins.addr)
        if prev is not None and prev.mn == 'moveq' and int(prev.ops[1][1]) == info['xr'] and not info['xa']:
            k = int(prev.ops[0][1:]) * info['sc']
            return {None: tbase + k + self.seg.base}
        # CodeWarrior: cmpi.w #N,d0; bhi; add.w d0,d0; move.w T(pc,d0.w),d0; jmp T(pc,d0.w)
        chain = []
        a = ins.addr
        for _ in range(4):
            pi = self.prev_insn(a)
            if pi is None: break
            chain.append(pi); a = pi.addr
        if len(chain) == 4 and chain[0].mn == 'move.w' and chain[2].mn == 'bhi' and chain[3].mn.startswith('cmpi'):
            n = chain[3].infos[0]['v'] + 1
            cases = {}
            body = self.seg.body
            for i in range(n):
                o = struct.unpack_from('>h', body, tbase + 2 * i)[0]
                cases[o] = tbase + o + self.seg.base
            return cases
        return None

    def prev_insn(self, addr):
        a = self.fallprev.get(addr)
        if a is not None and a in self.insns:
            return self.insns[a]
        # linear back-scan fallback: try instruction lengths 2..10
        for ln in (2, 4, 6, 8, 10):
            a = addr - ln
            try:
                pi = self.dec(a)
            except Exception:
                continue
            if pi.size == ln and pi.mn != 'dc.w':
                return pi
        return None

    # ---------------------------------------------------------- flags liveness
    def flag_use_def(self, ins):
        mn = ins.mn; k = ins.kind2
        base = mn.split('.')[0]
        if k in ('ret', 'tail', 'ijump', 'unimpl'): return ALL, 0
        if k == 'call': return 0, ALL
        if base.startswith('_'): return 0, ALL
        if base == 'bra': return 0, 0
        if base.startswith('db'): return CCUSE[base[2:]], 0
        if k == 'cbranch': return CCUSE[base[1:]], 0
        if k == 'switch': return 0, 0
        if base.startswith('s') and base[1:] in CCUSE and base not in ('sub', 'subq', 'subi', 'suba', 'subx', 'swap'):
            return CCUSE[base[1:]], 0
        if base in ('movea', 'lea', 'pea', 'link', 'unlk', 'exg', 'adda', 'suba', 'movem', 'nop', 'cmpa'):
            if base == 'cmpa': return 0, NZVC
            return 0, 0
        if base in ('addq', 'subq') and ins.infos and ins.infos[0]['m'] == 'areg': return 0, 0
        if base in ('addx', 'subx', 'negx'): return X | Z, ALL
        if base in ('roxl', 'roxr'): return X, ALL
        if base in ('add', 'addq', 'addi', 'sub', 'subq', 'subi', 'neg', 'asl', 'asr', 'lsl', 'lsr'): return 0, ALL
        if base in ('btst', 'bset', 'bclr', 'bchg'): return 0, Z
        if base in ('andi', 'ori', 'eori') and ins.ops[-1] in ('ccr', 'sr'): return ALL, ALL
        return 0, NZVC

    def liveness(self):
        addrs = sorted(self.insns)
        ud = {a: self.flag_use_def(self.insns[a]) for a in addrs}
        live_in = {a: 0 for a in addrs}
        changed = True
        while changed:
            changed = False
            for a in reversed(addrs):
                out = 0
                for s in self.succ[a]: out |= live_in.get(s, ALL)
                u, d = ud[a]
                li = u | (out & ~d)
                if li != live_in[a]: live_in[a] = li; changed = True
        self.live_out = {}
        for a in addrs:
            out = 0
            for s in self.succ[a]: out |= live_in.get(s, ALL)
            self.live_out[a] = out

    # ---------------------------------------------------------- operand codegen
    def addr_expr(self, info, ins):
        """C expression computing the effective address (no side effects)."""
        m = info['m']
        if m == 'ind': return 'A[%d]' % info['r']
        if m == 'disp': return '(A[%d] + %d)' % (info['r'], info['d'])
        if m in ('idx', 'pcidx', 'fullidx'):
            xr = ('A' if info['xa'] else 'D') + '[%d]' % info['xr']
            x = xr if info['xl'] else '(uint32_t)(int32_t)(int16_t)%s' % xr
            if info['sc'] != 1: x = '(%s * %d)' % (x, info['sc'])
            if m == 'idx': return '(A[%d] + %d + %s)' % (info['r'], info['d'], x)
            if m == 'pcidx': return '(0x%x + %s)' % (info['t'] + self.seg.base, x)
            base = '0' if info['bs'] else ('A[%d]' % info['r'] if info['r'] is not None else '0x%x' % (info['pcb'] + self.seg.base))
            parts = [base, str(info['bd'])]
            if not info['is_']: parts.append(x)
            return '(%s)' % ' + '.join(parts)
        if m == 'absw': return '0x%x' % (info['v'] & 0xFFFFFFFF)
        if m == 'absl': return '0x%x' % self.abs_value(ins, info)
        if m == 'pcrel': return '0x%x' % (info['t'] + self.seg.base)
        raise Unsupported('addr mode %s' % m)

    def prep(self, info, ins, out, var):
        """Emit side effects for an EA; return addr expr (memory) or None for registers/imm."""
        m = info['m']; sz = info['size']
        n = SZN.get(sz, 4)
        if m == 'postinc':
            r = info['r']; inc = 2 if (r == 7 and n == 1) else n
            out.append('%s = A[%d]; A[%d] += %d;' % (var, r, r, inc)); return var
        if m == 'predec':
            r = info['r']; inc = 2 if (r == 7 and n == 1) else n
            out.append('A[%d] -= %d; %s = A[%d];' % (r, inc, var, r)); return var
        if m in ('dreg', 'areg', 'imm'): return None
        return self.addr_expr(info, ins)

    def read(self, info, ins, sz, out, var='ea'):
        m = info['m']
        if m == 'dreg':
            return {'b': '(D[%d] & 0xFF)', 'w': '(D[%d] & 0xFFFF)', 'l': 'D[%d]'}[sz] % info['r']
        if m == 'areg':
            return {'b': '(A[%d] & 0xFF)', 'w': '(A[%d] & 0xFFFF)', 'l': 'A[%d]'}[sz] % info['r']
        if m == 'imm':
            v = info['v'] & {'b': 0xFF, 'w': 0xFFFF, 'l': 0xFFFFFFFF}[sz]
            return '0x%xu' % v
        a = self.prep(info, ins, out, var)
        return 'rd%s(%s)' % (sz, a)

    def write(self, info, ins, sz, val, out, addr=None, var='ea2'):
        m = info['m']
        if m == 'dreg':
            r = info['r']
            if sz == 'l': out.append('D[%d] = %s;' % (r, val))
            elif sz == 'w': out.append('D[%d] = (D[%d] & 0xFFFF0000u) | ((%s) & 0xFFFFu);' % (r, r, val))
            else: out.append('D[%d] = (D[%d] & 0xFFFFFF00u) | ((%s) & 0xFFu);' % (r, r, val))
            return
        if m == 'areg':
            r = info['r']
            if sz == 'w': out.append('A[%d] = (uint32_t)(int32_t)(int16_t)(%s);' % (r, val))
            else: out.append('A[%d] = %s;' % (r, val))
            return
        if addr is None: addr = self.prep(info, ins, out, var)
        out.append('wr%s(%s, %s);' % (sz, addr, val))

    # ---------------------------------------------------------- instruction codegen
    def gen(self, ins, live):
        o = []
        mn = ins.mn; k = ins.kind2
        base, _, sz = mn.partition('.')
        nxt = ins.addr + ins.size
        fl = live  # flags needed after this insn
        if k == 'unimpl':
            o.append('UNIMPL(0x%x);' % ins.addr); return o
        if k == 'ret':
            if mn == 'rts': o.append('A[7] += 4; return;')
            elif mn == 'rtd': o.append('A[7] += 4 + %s; return;' % ins.ops[0][1:])
            else: o.append('UNIMPL(0x%x); return;' % ins.addr)
            return o
        if k == 'goto':
            o.append('goto L_%x;' % ins.tgt); return o
        if k == 'cbranch':
            if base.startswith('db'):
                cc = base[2:]; r = int(ins.ops[0][1])
                o.append('if (!%s) { uint16_t c_ = (uint16_t)(D[%d] - 1); D[%d] = (D[%d] & 0xFFFF0000u) | c_; '
                         'if (c_ != 0xFFFF) goto L_%x; }' % (CCEXPR[cc], r, r, r, ins.tgt))
            else:
                o.append('if %s goto L_%x;' % (CCEXPR[base[1:]], ins.tgt))
            return o
        if k == 'switch':
            if None in ins.cases:
                o.append('goto L_%x;' % ins.cases[None]); return o
            r = 'D[%d]' % ins.infos[0]['xr']
            o.append('switch ((int16_t)%s) {' % r)
            for off, t in sorted(ins.cases.items()):
                o.append('  case %d: goto L_%x;' % (off, t))
            o.append('  default: UNIMPL(0x%x); return;' % ins.addr)
            o.append('}')
            return o
        if k == 'call':
            if ins.callee is not None and ins.callee in self.p.entries:
                self.calls.add(ins.callee)
                o.append('PUSH32(0x%x); %s(); g_cur_fn = 0x%x;' % (nxt, self.p.cname(ins.callee), self.entry))
            elif ins.callee is not None:
                o.append('PUSH32(0x%x); call_addr(0x%x); g_cur_fn = 0x%x;' % (nxt, ins.callee, self.entry))
            else:
                a = self.addr_expr(ins.infos[0], ins)
                o.append('{ uint32_t t_ = %s; PUSH32(0x%x); call_addr(t_); g_cur_fn = 0x%x; }' % (a, nxt, self.entry))
            return o
        if k == 'tail':
            self.calls.add(ins.callee)
            o.append('%s(); return;' % self.p.cname(ins.callee)); return o
        if k == 'ijump':
            a = self.addr_expr(ins.infos[0], ins)
            o.append('jump_addr(%s); return;' % a); return o
        if base.startswith('_'):
            o.append('TRAP(0x%04X); /* %s */' % (ins.op, base[1:])); return o
        fn = getattr(self, 'g_' + base, None)
        if fn is None:
            o.append('UNIMPL(0x%x); /* %s */' % (ins.addr, ins.text)); return o
        r = fn(ins, sz, o, fl)
        return o

    # helpers
    def flags_logic(self, sz, v, o, fl):
        if fl & NZVC:
            o.append('FLG_LOGIC_%s(%s);' % (sz.upper(), v))

    def g_move(self, ins, sz, o, fl):
        if len(ins.infos) == 1 and ('sr' in ins.ops or 'ccr' in ins.ops):
            # status register moves: interrupts do not exist here, only the CCR matters
            if ins.ops[0] in ('sr', 'ccr'):
                self.write(ins.infos[0], ins, 'w',
                           '(0x2000u | ((uint32_t)fX << 4) | ((uint32_t)fN << 3) | ((uint32_t)fZ << 2) | ((uint32_t)fV << 1) | fC)', o)
            else:
                v = self.read(ins.infos[0], ins, 'w', o)
                o.append('t = %s; fX = (t >> 4) & 1; fN = (t >> 3) & 1; fZ = (t >> 2) & 1; fV = (t >> 1) & 1; fC = t & 1;' % v)
            return
        s, d = ins.infos
        v = self.read(s, ins, sz, o, 'ea')
        if fl & NZVC or d['m'] not in ('dreg',):
            o.append('t = %s;' % v); v = 't'
        self.write(d, ins, sz, v, o)
        self.flags_logic(sz, v, o, fl)

    def g_movea(self, ins, sz, o, fl):
        s = ins.infos[0]; r = int(ins.ops[1][1])
        v = self.read(s, ins, sz, o)
        if sz == 'w': o.append('A[%d] = (uint32_t)(int32_t)(int16_t)%s;' % (r, v))
        else: o.append('A[%d] = %s;' % (r, v))

    def g_moveq(self, ins, sz, o, fl):
        v = int(ins.ops[0][1:]) & 0xFFFFFFFF; r = int(ins.ops[1][1])
        o.append('D[%d] = 0x%xu;' % (r, v))
        if fl & NZVC: o.append('FLG_LOGIC_L(D[%d]);' % r)

    def g_lea(self, ins, sz, o, fl):
        r = int(ins.ops[1][1])
        o.append('A[%d] = %s;' % (r, self.addr_expr(ins.infos[0], ins)))

    def g_pea(self, ins, sz, o, fl):
        o.append('{ uint32_t t_ = %s; PUSH32(t_); }' % self.addr_expr(ins.infos[0], ins))

    def g_clr(self, ins, sz, o, fl):
        self.write(ins.infos[0], ins, sz, '0', o)
        if fl & NZVC: o.append('fN = 0; fZ = 1; fV = 0; fC = 0;')

    def g_tst(self, ins, sz, o, fl):
        v = self.read(ins.infos[0], ins, sz, o)
        if fl & NZVC: o.append('FLG_LOGIC_%s(%s);' % (sz.upper(), v))
        elif ins.infos[0]['m'] in ('postinc', 'predec'): pass

    def g_link(self, ins, sz, o, fl):
        r = int(ins.ops[0][1]); d = int(ins.ops[1][1:])
        o.append('PUSH32(A[%d]); A[%d] = A[7]; A[7] += %d;' % (r, r, d))

    def g_unlk(self, ins, sz, o, fl):
        r = int(ins.ops[0][1])
        o.append('A[7] = A[%d]; A[%d] = rdl(A[7]); A[7] += 4;' % (r, r))

    def g_movem(self, ins, sz, o, fl):
        info = ins.infos[0]
        mask = struct.unpack_from('>H', ins.raw, 2)[0]
        tomem = not (ins.op & 0x400)
        n = SZN[sz]
        regs = [('D', i) for i in range(8)] + [('A', i) for i in range(8)]
        if tomem and info['m'] == 'predec':
            # mask is reversed: bit0 = a7
            sel = [regs[15 - i] for i in range(16) if mask & (1 << i)]
            # store highest register first at highest address
            r = info['r']
            o.append('{ uint32_t p_ = A[%d];' % r)
            for (rk, ri) in sel:
                o.append('  p_ -= %d; wr%s(p_, %s[%d]);' % (n, sz, rk, ri))
            o.append('  A[%d] = p_; }' % r)
            return
        sel = [regs[i] for i in range(16) if mask & (1 << i)]
        if tomem:
            a = self.addr_expr(info, ins)
            o.append('{ uint32_t p_ = %s;' % a)
            for (rk, ri) in sel:
                o.append('  wr%s(p_, %s[%d]); p_ += %d;' % (sz, rk, ri, n))
            o.append('}')
            return
        if info['m'] == 'postinc':
            a = 'A[%d]' % info['r']
        else:
            a = self.addr_expr(info, ins)
        o.append('{ uint32_t p_ = %s;' % a)
        for (rk, ri) in sel:
            if sz == 'w': o.append('  %s[%d] = (uint32_t)(int32_t)(int16_t)rdw(p_); p_ += 2;' % (rk, ri))
            else: o.append('  %s[%d] = rdl(p_); p_ += 4;' % (rk, ri))
        if info['m'] == 'postinc': o.append('  A[%d] = p_;' % info['r'])
        o.append('}')

    def g_exg(self, ins, sz, o, fl):
        a, b = ins.ops
        ra = '%s[%s]' % (a[0].upper(), a[1]); rb = '%s[%s]' % (b[0].upper(), b[1])
        o.append('{ uint32_t t_ = %s; %s = %s; %s = t_; }' % (ra, ra, rb, rb))

    def g_swap(self, ins, sz, o, fl):
        r = int(ins.ops[0][1])
        o.append('D[%d] = (D[%d] >> 16) | (D[%d] << 16);' % (r, r, r))
        if fl & NZVC: o.append('FLG_LOGIC_L(D[%d]);' % r)

    def g_ext(self, ins, sz, o, fl):
        r = int(ins.ops[0][1])
        if sz == 'w':
            o.append('D[%d] = (D[%d] & 0xFFFF0000u) | (uint16_t)(int16_t)(int8_t)D[%d];' % (r, r, r))
            if fl & NZVC: o.append('FLG_LOGIC_W(D[%d]);' % r)
        else:
            o.append('D[%d] = (uint32_t)(int32_t)(int16_t)D[%d];' % (r, r))
            if fl & NZVC: o.append('FLG_LOGIC_L(D[%d]);' % r)

    def g_extb(self, ins, sz, o, fl):
        r = int(ins.ops[0][1])
        o.append('D[%d] = (uint32_t)(int32_t)(int8_t)D[%d];' % (r, r))
        if fl & NZVC: o.append('FLG_LOGIC_L(D[%d]);' % r)

    def _binop(self, ins, sz, o, fl, op, flagfn):
        """generic two-operand op: <op> src,dst"""
        s, d = self._two(ins)
        sv = self.read(s, ins, sz, o, 'ea')
        o.append('s = %s;' % sv)
        if d['m'] in ('dreg', 'areg'):
            dv = self.read(d, ins, sz, o); addr = None
        else:
            addr = self.prep(d, ins, o, 'ea2')
            dv = 'rd%s(%s)' % (sz, addr)
        o.append('d = %s;' % dv)
        if flagfn and fl:
            o.append('r = %s_%s(s, d);' % (flagfn, sz.upper()))
        else:
            o.append('r = %s;' % op)
            if flagfn is None and fl & NZVC: o.append('FLG_LOGIC_%s(r);' % sz.upper())
        self.write(d, ins, sz, 'r', o, addr)

    def _two(self, ins):
        """Return (src_info, dst_info) synthesising register operands."""
        ops = ins.ops; infos = list(ins.infos)
        res = []
        for t in ops:
            if re.fullmatch(r'[da][0-7]', t) and (not infos or infos[0].get('m') not in ('dreg', 'areg') or
                                                 not self._is_reg_info(infos[0], t)):
                res.append({'m': 'dreg' if t[0] == 'd' else 'areg', 'r': int(t[1]), 'size': 'l'})
            elif t.startswith('#') and (not infos or infos[0].get('m') != 'imm'):
                res.append({'m': 'imm', 'v': int(t[1:]) & 0xFFFFFFFF, 'size': 'l'})
            else:
                res.append(infos.pop(0))
        return res[0], res[1]

    def _is_reg_info(self, info, t):
        return info['m'] == ('dreg' if t[0] == 'd' else 'areg') and info['r'] == int(t[1])

    def g_add(self, ins, sz, o, fl): self._binop(ins, sz, o, fl, '(d + s)', 'ADD')
    def g_addi(self, ins, sz, o, fl): self._binop(ins, sz, o, fl, '(d + s)', 'ADD')
    def g_sub(self, ins, sz, o, fl): self._binop(ins, sz, o, fl, '(d - s)', 'SUB')
    def g_subi(self, ins, sz, o, fl): self._binop(ins, sz, o, fl, '(d - s)', 'SUB')
    def g_and(self, ins, sz, o, fl): self._logic(ins, sz, o, fl, '&')
    def g_andi(self, ins, sz, o, fl): self._logic(ins, sz, o, fl, '&')
    def g_or(self, ins, sz, o, fl): self._logic(ins, sz, o, fl, '|')
    def g_ori(self, ins, sz, o, fl): self._logic(ins, sz, o, fl, '|')
    def g_eor(self, ins, sz, o, fl): self._logic(ins, sz, o, fl, '^')
    def g_eori(self, ins, sz, o, fl): self._logic(ins, sz, o, fl, '^')

    def _logic(self, ins, sz, o, fl, op):
        if ins.ops[-1] in ('ccr', 'sr'):
            v = ins.infos[0]['v']
            o.append('ccr_op(%d, 0x%x);' % ({'&': 0, '|': 1, '^': 2}[op], v)); return
        s, d = self._two(ins)
        sv = self.read(s, ins, sz, o, 'ea')
        o.append('s = %s;' % sv)
        if d['m'] in ('dreg', 'areg'):
            dv = self.read(d, ins, sz, o); addr = None
        else:
            addr = self.prep(d, ins, o, 'ea2'); dv = 'rd%s(%s)' % (sz, addr)
        o.append('r = (%s %s s) & %s;' % (dv, op, MASK[sz]))
        if fl & NZVC: o.append('FLG_LOGIC_%s(r);' % sz.upper())
        self.write(d, ins, sz, 'r', o, addr)

    def g_addq(self, ins, sz, o, fl): self._quick(ins, sz, o, fl, 'ADD', '+')
    def g_subq(self, ins, sz, o, fl): self._quick(ins, sz, o, fl, 'SUB', '-')

    def _quick(self, ins, sz, o, fl, fn, op):
        q = int(ins.ops[0][1:]); d = ins.infos[0]
        if d['m'] == 'areg':
            o.append('A[%d] %s= %d;' % (d['r'], op, q)); return
        if d['m'] == 'dreg':
            dv = self.read(d, ins, sz, o); addr = None
        else:
            addr = self.prep(d, ins, o, 'ea2'); dv = 'rd%s(%s)' % (sz, addr)
        if fl:
            o.append('r = %s_%s(%d, %s);' % (fn, sz.upper(), q, dv))
        else:
            o.append('r = %s %s %d;' % (dv, op, q))
        self.write(d, ins, sz, 'r', o, addr)

    def g_adda(self, ins, sz, o, fl):
        s = ins.infos[0]; r = int(ins.ops[1][1])
        v = self.read(s, ins, sz, o)
        if sz == 'w': v = '(uint32_t)(int32_t)(int16_t)%s' % v
        o.append('A[%d] += %s;' % (r, v))

    def g_suba(self, ins, sz, o, fl):
        s = ins.infos[0]; r = int(ins.ops[1][1])
        v = self.read(s, ins, sz, o)
        if sz == 'w': v = '(uint32_t)(int32_t)(int16_t)%s' % v
        o.append('A[%d] -= %s;' % (r, v))

    def g_cmpa(self, ins, sz, o, fl):
        s = ins.infos[0]; r = int(ins.ops[1][1])
        v = self.read(s, ins, sz, o)
        if sz == 'w': v = '(uint32_t)(int32_t)(int16_t)%s' % v
        if fl: o.append('CMP_L(%s, A[%d]);' % (v, r))

    def g_cmp(self, ins, sz, o, fl):
        s, d = self._two(ins)
        sv = self.read(s, ins, sz, o, 'ea')
        o.append('s = %s;' % sv)
        dv = self.read(d, ins, sz, o, 'ea2')
        if fl: o.append('CMP_%s(s, %s);' % (sz.upper(), dv))

    g_cmpi = g_cmp

    def g_cmpm(self, ins, sz, o, fl):
        s, d = ins.ops
        rs = int(s[2]); rd = int(d[2]); n = SZN[sz]
        o.append('s = rd%s(A[%d]); A[%d] += %d; d = rd%s(A[%d]); A[%d] += %d;' % (sz, rs, rs, n, sz, rd, rd, n))
        if fl: o.append('CMP_%s(s, d);' % sz.upper())

    def g_neg(self, ins, sz, o, fl):
        d = ins.infos[0]
        if d['m'] == 'dreg': dv = self.read(d, ins, sz, o); addr = None
        else: addr = self.prep(d, ins, o, 'ea2'); dv = 'rd%s(%s)' % (sz, addr)
        if fl: o.append('r = SUB_%s(%s, 0);' % (sz.upper(), dv))
        else: o.append('r = 0u - %s;' % dv)
        self.write(d, ins, sz, 'r', o, addr)

    def g_negx(self, ins, sz, o, fl):
        d = ins.infos[0]
        if d['m'] == 'dreg': dv = self.read(d, ins, sz, o); addr = None
        else: addr = self.prep(d, ins, o, 'ea2'); dv = 'rd%s(%s)' % (sz, addr)
        o.append('r = SUBX_%s(%s, 0);' % (sz.upper(), dv))
        self.write(d, ins, sz, 'r', o, addr)

    def g_addx(self, ins, sz, o, fl): self._xop(ins, sz, o, 'ADDX')
    def g_subx(self, ins, sz, o, fl): self._xop(ins, sz, o, 'SUBX')

    def _xop(self, ins, sz, o, fn):
        s, d = ins.ops
        if s[0] == 'd':
            o.append('r = %s_%s(D[%s] & %s, D[%s] & %s);' % (fn, sz.upper(), s[1], MASK[sz], d[1], MASK[sz]))
            self.write({'m': 'dreg', 'r': int(d[1])}, ins, sz, 'r', o)
        else:
            raise Unsupported('addx mem')

    def g_not(self, ins, sz, o, fl):
        d = ins.infos[0]
        if d['m'] == 'dreg': dv = self.read(d, ins, sz, o); addr = None
        else: addr = self.prep(d, ins, o, 'ea2'); dv = 'rd%s(%s)' % (sz, addr)
        o.append('r = (~%s) & %s;' % (dv, MASK[sz]))
        if fl & NZVC: o.append('FLG_LOGIC_%s(r);' % sz.upper())
        self.write(d, ins, sz, 'r', o, addr)

    def _mul(self, ins, sz, o, fl, signed):
        s = ins.infos[0]
        if sz == 'w':
            r = int(ins.ops[1][1])
            v = self.read(s, ins, 'w', o)
            if signed: o.append('D[%d] = (uint32_t)((int32_t)(int16_t)%s * (int32_t)(int16_t)D[%d]);' % (r, v, r))
            else: o.append('D[%d] = (uint32_t)((uint32_t)(uint16_t)%s * (uint32_t)(uint16_t)D[%d]);' % (r, v, r))
            if fl & NZVC: o.append('FLG_LOGIC_L(D[%d]);' % r)
            return
        v = self.read(s, ins, 'l', o)
        dst = ins.ops[1]
        if ':' in dst:
            dh, dl = int(dst[1]), int(dst[4])
            if signed: o.append('{ int64_t p_ = (int64_t)(int32_t)%s * (int32_t)D[%d]; D[%d] = (uint32_t)p_; D[%d] = (uint32_t)((uint64_t)p_ >> 32); }' % (v, dl, dl, dh))
            else: o.append('{ uint64_t p_ = (uint64_t)%s * D[%d]; D[%d] = (uint32_t)p_; D[%d] = (uint32_t)(p_ >> 32); }' % (v, dl, dl, dh))
            if fl & NZVC: o.append('fN = D[%d] >> 31; fZ = (D[%d] | D[%d]) == 0; fV = 0; fC = 0;' % (dh, dh, dl))
        else:
            r = int(dst[1])
            if signed: o.append('{ int64_t p_ = (int64_t)(int32_t)%s * (int32_t)D[%d]; D[%d] = (uint32_t)p_; fV = (p_ != (int32_t)p_); }' % (v, r, r))
            else: o.append('{ uint64_t p_ = (uint64_t)%s * D[%d]; D[%d] = (uint32_t)p_; fV = (p_ >> 32) != 0; }' % (v, r, r))
            if fl & NZVC: o.append('fN = D[%d] >> 31; fZ = D[%d] == 0; fC = 0;' % (r, r))

    def g_muls(self, ins, sz, o, fl): self._mul(ins, sz, o, fl, True)
    def g_mulu(self, ins, sz, o, fl): self._mul(ins, sz, o, fl, False)

    def _div(self, ins, sz, o, fl, signed, base):
        s = ins.infos[0]
        if sz == 'w':
            r = int(ins.ops[1][1])
            v = self.read(s, ins, 'w', o)
            o.append('DIV%s_W(%s, %d);' % ('S' if signed else 'U', v, r))
            return
        v = self.read(s, ins, 'l', o)
        dst = ins.ops[1]
        if ':' in dst:
            dr, dq = int(dst[1]), int(dst[4])
            if base.endswith('l'):  # divsl/divul: 32/32 -> rem:quot
                o.append('DIV%sL_L(%s, %d, %d);' % ('S' if signed else 'U', v, dr, dq))
            else:
                raise Unsupported('64-bit divide')
        else:
            r = int(dst[1])
            o.append('DIV%sL_L(%s, -1, %d);' % ('S' if signed else 'U', v, r))

    def g_divs(self, ins, sz, o, fl): self._div(ins, sz, o, fl, True, 'divs')
    def g_divu(self, ins, sz, o, fl): self._div(ins, sz, o, fl, False, 'divu')
    def g_divsl(self, ins, sz, o, fl): self._div(ins, sz, o, fl, True, 'divsl')
    def g_divul(self, ins, sz, o, fl): self._div(ins, sz, o, fl, False, 'divul')

    def _shift(self, ins, sz, o, fl, kind):
        if len(ins.ops) == 1:  # memory form, word, count 1
            d = ins.infos[0]
            addr = self.prep(d, ins, o, 'ea2')
            o.append('r = SH_%s_W(rdw(%s), 1);' % (kind, addr))
            o.append('wrw(%s, r);' % addr)
            return
        c, dst = ins.ops
        r = int(dst[1])
        cnt = c[1:] if c.startswith('#') else '(D[%s] & 63)' % c[1]
        val = {'b': '(D[%d] & 0xFF)', 'w': '(D[%d] & 0xFFFF)', 'l': 'D[%d]'}[sz] % r
        if not fl and kind in ('LSL', 'LSR', 'ASR', 'ASL') and c.startswith('#'):
            n = int(c[1:])
            if kind in ('LSL', 'ASL'): e = '(%s << %d)' % (val, n)
            elif kind == 'LSR': e = '(%s >> %d)' % (val, n)
            else: e = '((uint32_t)((%s)%s >> %d))' % (ST[sz], val, n)
            self.write({'m': 'dreg', 'r': r}, ins, sz, e, o)
            return
        o.append('r = SH_%s_%s(%s, %s);' % (kind, sz.upper(), val, cnt))
        self.write({'m': 'dreg', 'r': r}, ins, sz, 'r', o)

    def g_asl(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'ASL')
    def g_asr(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'ASR')
    def g_lsl(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'LSL')
    def g_lsr(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'LSR')
    def g_rol(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'ROL')
    def g_ror(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'ROR')
    def g_roxl(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'ROXL')
    def g_roxr(self, ins, sz, o, fl): self._shift(ins, sz, o, fl, 'ROXR')

    def _bit(self, ins, sz, o, fl, kind):
        b, _ = ins.ops[0], None
        d = ins.infos[-1]
        bitexpr = b[1:] if b.startswith('#') else 'D[%s]' % b[1]
        if d['m'] == 'dreg':
            r = d['r']
            o.append('{ uint32_t m_ = 1u << ((%s) & 31); fZ = (D[%d] & m_) == 0;' % (bitexpr, r))
            if kind == 'bset': o.append('  D[%d] |= m_;' % r)
            elif kind == 'bclr': o.append('  D[%d] &= ~m_;' % r)
            elif kind == 'bchg': o.append('  D[%d] ^= m_;' % r)
            o.append('}')
            return
        if d['m'] == 'imm':
            o.append('fZ = (0x%x & (1u << ((%s) & 7))) == 0;' % (d['v'], bitexpr)); return
        addr = self.prep(d, ins, o, 'ea2')
        o.append('{ uint32_t a_ = %s; uint8_t v_ = rdb(a_); uint8_t m_ = (uint8_t)(1u << ((%s) & 7)); fZ = (v_ & m_) == 0;' % (addr, bitexpr))
        if kind == 'bset': o.append('  wrb(a_, v_ | m_);')
        elif kind == 'bclr': o.append('  wrb(a_, v_ & ~m_);')
        elif kind == 'bchg': o.append('  wrb(a_, v_ ^ m_);')
        o.append('}')

    def g_btst(self, ins, sz, o, fl): self._bit(ins, sz, o, fl, 'btst')
    def g_bset(self, ins, sz, o, fl): self._bit(ins, sz, o, fl, 'bset')
    def g_bclr(self, ins, sz, o, fl): self._bit(ins, sz, o, fl, 'bclr')
    def g_bchg(self, ins, sz, o, fl): self._bit(ins, sz, o, fl, 'bchg')

    def g_nop(self, ins, sz, o, fl): pass

    def __getattr__(self, name):
        # Scc
        if name.startswith('g_s') and name[3:] in CCEXPR:
            cc = name[3:]
            def f(ins, sz, o, fl):
                self.write(ins.infos[0], ins, 'b', '(%s ? 0xFFu : 0u)' % CCEXPR[cc], o)
            return f
        raise AttributeError(name)

    # ---------------------------------------------------------- emit function
    def emit(self):
        self.discover()
        self.liveness()
        addrs = sorted(self.insns)
        targets = set()
        for a in addrs:
            ins = self.insns[a]
            if ins.kind2 in ('goto', 'cbranch'): targets.add(ins.tgt)
            if ins.kind2 == 'switch': targets.update(ins.cases.values())
        body = []
        for i, a in enumerate(addrs):
            ins = self.insns[a]
            if a in targets: body.append('L_%x:' % a)
            try:
                code = self.gen(ins, self.live_out[a])
            except Exception as e:
                code = ['UNIMPL(0x%x); /* %s: %s */' % (a, ins.text, e)]
            body.append('    /* %06x  %s */' % (a - self.seg.base, ins.text))
            for c in code: body.append('    ' + c)
            # fallthrough fixup
            if ins.kind2 in ('op', 'call', 'cbranch'):
                nxt = a + ins.size
                if i + 1 >= len(addrs) or addrs[i + 1] != nxt:
                    body.append('    goto L_%x;' % nxt)
                    targets.add(nxt)
        # labels discovered late (fallthrough fixups) must exist: rebuild with all targets
        if any(('L_%x:' % t) not in body for t in targets):
            out = []
            for line in body:
                out.append(line)
            # insert missing labels before their instruction comment
            final = []
            labeled = set(l[2:-1] for l in body if l.startswith('L_'))
            for line in body:
                m_ = re.match(r'    /\* ([0-9a-f]{6})  ', line)
                if m_:
                    a = int(m_.group(1), 16) + self.seg.base
                    if a in targets and ('%x' % a) not in labeled:
                        final.append('L_%x:' % a); labeled.add('%x' % a)
                final.append(line)
            body = final
        name = self.p.names.get(self.entry, '')
        head = ['/* %s  (CODE %d "%s" +0x%x) */' % (name or '?', self.seg.sid, self.seg.name, self.entry - self.seg.base),
                'void %s(void)' % self.p.cname(self.entry), '{',
                '    uint32_t t, s, d, r, ea, ea2; (void)t; (void)s; (void)d; (void)r; (void)ea; (void)ea2;',
                '    g_cur_fn = 0x%x;' % self.entry]
        return head + body + ['}', '']


def main(path, outdir):
    os.makedirs(outdir, exist_ok=True)
    p = Program(path)
    # every statically known call/tail-call target becomes a function entry
    changed = True
    while changed:
        changed = False
        for e in sorted(p.entries):
            g = FuncGen(p, e)
            try:
                g.discover()
            except Unsupported:
                continue
            for ins in g.insns.values():
                t = getattr(ins, 'callee', None)
                if ins.kind2 in ('call', 'tail') and isinstance(t, int) and p.seg_of(t) and t not in p.entries:
                    p.entries.add(t)
                    changed = True
    funcs = {}
    pending = sorted(p.entries)
    done = set()
    while pending:
        e = pending.pop()
        if e in done: continue
        done.add(e)
        g = FuncGen(p, e)
        try:
            funcs[e] = g.emit()
        except Unsupported as ex:
            funcs[e] = ['void %s(void) { UNIMPL(0x%x); /* %s */ }' % (p.cname(e), e, ex), '']
            print('unsupported', p.cname(e), ex)
        for c in g.calls:
            if c not in done and c in p.entries: pending.append(c)
    # check name uniqueness
    seen = {}
    for e in sorted(funcs):
        n = p.cname(e)
        if n in seen:
            p.names[e] = 'sub_%x' % e  # force address-based name
        seen[p.cname(e)] = e
    # regenerate after renames (cheap enough)
    for e in sorted(funcs):
        funcs[e] = FuncGen(p, e).emit()
    # write per-segment C files
    hdr = ['/* generated by tools/recomp.py - do not edit */', '#pragma once', '#include "runtime/cpu.h"', '']
    for e in sorted(funcs): hdr.append('void %s(void);' % p.cname(e))
    open(os.path.join(outdir, 'recomp_funcs.h'), 'w').write('\n'.join(hdr) + '\n')
    for sid, s in p.segs.items():
        lines = ['/* generated by tools/recomp.py from CODE %d "%s" - do not edit */' % (sid, s.name),
                 '#include "recomp_funcs.h"', '#if defined(_MSC_VER)', '#pragma warning(disable: 4102 4146 4244)', '#endif', '']
        for e in sorted(funcs):
            if p.seg_of(e) is s: lines += funcs[e]
        open(os.path.join(outdir, 'recomp_seg%d.c' % sid), 'w').write('\n'.join(lines) + '\n')
    # function table + memory image
    tab = ['/* generated - do not edit */', '#include "recomp_funcs.h"', '#include "runtime/image.h"', '',
           'const RecompFunc g_recomp_funcs[] = {']
    for e in sorted(funcs):
        tab.append('    {0x%x, %s, "%s"},' % (e, p.cname(e), p.names.get(e, p.cname(e))))
    tab.append('};')
    tab.append('const unsigned g_recomp_nfuncs = %d;' % len(funcs))
    tab.append('')
    chunks = [(s.base, bytes(s.body)) for s in p.segs.values()] + [(A5_WORLD, bytes(p.img))]
    for i, (addr, data) in enumerate(chunks):
        tab.append('static const unsigned char chunk%d[%d] = {' % (i, len(data)))
        for j in range(0, len(data), 24):
            tab.append('  ' + ','.join(str(b) for b in data[j:j + 24]) + ',')
        tab.append('};')
    tab.append('const ImageChunk g_image_chunks[] = {')
    for i, (addr, data) in enumerate(chunks):
        tab.append('    {0x%x, %d, chunk%d},' % (addr, len(data), i))
    tab.append('};')
    tab.append('const unsigned g_image_nchunks = %d;' % len(chunks))
    tab.append('const uint32_t g_image_a5 = 0x%x;' % p.A5)
    tab.append('const uint32_t g_image_main = 0x%x;' % (p.segs[1].base + 0x7ac))
    seginfo = ', '.join('{%d, 0x%x, %d}' % (s.sid, s.base, len(s.body)) for s in p.segs.values())
    tab.append('const SegInfo g_image_segs[] = {%s};' % seginfo)
    tab.append('const unsigned g_image_nsegs = %d;' % len(p.segs))
    open(os.path.join(outdir, 'recomp_table.c'), 'w').write('\n'.join(tab) + '\n')
    n_unimpl = sum(1 for e in funcs for l in funcs[e] if 'UNIMPL(' in l and 'default:' not in l)
    print('functions:', len(funcs), 'unimpl sites:', n_unimpl, 'A5=%x' % p.A5)


if __name__ == '__main__':
    if len(sys.argv) != 3:
        sys.exit('usage: python -I tools/recomp.py <"Bonkheads Deluxe.rsrc"> <output dir>\n'
                 '(the build runs this for you; see README.md)')
    main(sys.argv[1], sys.argv[2])
