"""Disassemble all CODE segments of a CodeWarrior far-model 68k app into
per-segment listings split on MacsBug symbols.

usage: python -I tools/dumpcode.py <app.rsrc> <outdir>
"""
import os, re, struct, sys, json
sys.path.insert(0, os.path.dirname(__file__))
import rsrc, m68k, traps, a5world

NAMECH = re.compile(rb'[A-Za-z_][A-Za-z0-9_:.~]*$')


def macsbug_at(code, o):
    """If a MacsBug symbol starts at o, return (name, end_offset_after_constants)."""
    if o >= len(code): return None
    b = code[o]
    if 0x81 <= b <= 0x9F:
        n = b & 0x1F; s = o + 1
    elif b == 0x80 and o + 1 < len(code):
        n = code[o + 1]; s = o + 2
    else:
        return None
    name = code[s:s + n]
    if len(name) != n or not n or not NAMECH.match(name): return None
    e = s + n
    if e & 1: e += 1
    if e + 2 > len(code): return name.decode(), e
    lit = struct.unpack_from('>H', code, e)[0]
    return name.decode(), e + 2 + lit


def split_functions(code, start, end, extra=()):
    """Return list of (name, start, codeend, nextstart)."""
    extra = set(extra)
    d = m68k.Dis(code, 0, traps.TRAPS)
    funcs = []
    o = start; fstart = start
    while o < end:
        if o in extra and o != fstart:
            funcs.append(('sub_%x' % fstart, fstart, o, o)); fstart = o
        ins = d.dis(o)
        o += ins.size
        if ins.mn in ('rts', 'rtd', 'jmp', 'rte') or ins.text.startswith('jmp'):
            sym = macsbug_at(code, o)
            if sym:
                funcs.append((sym[0], fstart, o, sym[1]))
                o = sym[1]; fstart = o
    if fstart < end:
        funcs.append(('anon_%x' % fstart, fstart, end, end))
    return funcs


def main(path, outdir):
    os.makedirs(outdir, exist_ok=True)
    r = rsrc.read_rsrc(open(path, 'rb').read())
    segs = {}
    for sid, (name, attr, body) in r['CODE'].items():
        if sid == 0: continue
        if sid == 1:
            start = 4; end = len(body); jt = (0, 1)
        else:
            jtoff, jtcnt, _, reloc = struct.unpack_from('>HHII', body, 0)
            start = 12; end = reloc; jt = (jtoff, jtcnt)
        segs[sid] = (name, body, start, end, jt)
    # symbol table: (seg, offset) -> name
    world = a5world.build(r)
    jtab = a5world.jumptable(world, r)
    extra = {}
    for slot, (s_, o_) in jtab.items(): extra.setdefault(s_, set()).add(o_)
    allfuncs = {}
    for sid, (name, body, start, end, jt) in segs.items():
        allfuncs[sid] = split_functions(body, start, end, extra.get(sid, ()))
    index = {}
    for sid, fl in allfuncs.items():
        for (fn, s, ce, ns) in fl:
            index[(sid, s)] = fn
    jtnames = {}
    for slot, (s_, o_) in jtab.items():
        if (s_, o_) in index: jtnames[slot] = '%s' % index[(s_, o_)]
    json.dump({'jt': {'%x' % k: v for k, v in jtnames.items()}, 'jtfull': {'%x' % k: '%d:%x' % v for k, v in jtab.items()}}, open(os.path.join(outdir, 'jumptable.json'), 'w'), indent=0)
    open(os.path.join(outdir, 'a5world.bin'), 'wb').write(world['img'])
    json.dump({'%d:%x' % k: v for k, v in index.items()}, open(os.path.join(outdir, 'symbols.json'), 'w'), indent=0)
    for sid, (name, body, start, end, jt) in segs.items():
        d = m68k.Dis(body, 0, traps.TRAPS)
        out = []
        out.append('; CODE %d "%s"  size=%d jt=A5+$%x x%d' % (sid, name, len(body), jt[0], jt[1]))
        for (fn, s, ce, ns) in allfuncs[sid]:
            # collect branch targets for labels
            labels = set()
            o = s; insns = []
            while o < ce:
                ins = d.dis(o); insns.append(ins); o += ins.size
                if isinstance(ins.target, int): labels.add(ins.target)
            out.append('')
            out.append('; ==== %s  (seg %d +$%x .. $%x)' % (fn, sid, s, ce))
            for ins in insns:
                lab = 'L%x:' % ins.addr if ins.addr in labels else ''
                txt = ins.text
                if isinstance(ins.target, tuple):
                    v = ins.target[1]
                    if v in jtnames: txt += '    ; -> %s [jt]' % jtnames[v]
                    elif (sid, v) in index: txt += '    ; -> %s' % index[(sid, v)]
                    else: txt += '    ; -> A5+$%x' % v if v > 0x6000 else '    ; -> seg+$%x' % v
                elif ins.jtcall is not None and ins.jtcall in jtnames:
                    txt += '    ; -> %s [jt]' % jtnames[ins.jtcall]
                elif ins.kind == 'call' and isinstance(ins.target, int) and (sid, ins.target) in index:
                    txt += '    ; -> %s' % index[(sid, ins.target)]
                out.append('%-10s %06x  %-20s %s' % (lab, ins.addr, ins.raw.hex(), txt))
            if ns > ce:
                out.append('           ; literal pool / symbol: %d bytes' % (ns - ce))
        open(os.path.join(outdir, 'seg%d_%s.s' % (sid, name.replace(' ', '_'))), 'w').write('\n'.join(out) + '\n')
        print('seg', sid, name, len(allfuncs[sid]), 'functions')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
