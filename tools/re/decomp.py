# Rough DXIL -> pseudo-HLSL: inlines extractvalue / single-use temporaries, names cbuffer loads.
# usage: python -I out/sh/decomp.py out/sh/TAG.full > out/sh/TAG.hl
import re, sys, struct

src = open(sys.argv[1], encoding='utf-8', errors='replace').read()
body = src[src.index('define void'):]
body = body[:body.index('\n}\n') + 3]
lines = [l.strip() if l.strip().startswith(';') else re.sub(r'\s+;.*$', '', l).strip() for l in body.splitlines()]

def fconst(t):
    t = t.strip()
    if t.startswith('0x'):
        return repr(round(struct.unpack('<d', struct.pack('<Q', int(t, 16)))[0], 7))
    return t

handles = {}   # %n -> name of resource
cbufs = {}     # %n (cbufferLoadLegacy result) -> 'cbX[r]'
expr = {}
uses = {}
for l in lines:
    for m in re.findall(r'%(\d+)', l.split('=', 1)[1] if '=' in l and l.startswith('%') else l):
        uses[m] = uses.get(m, 0) + 1

CLASS = {'0': 't', '1': 'u', '2': 'cb', '3': 's'}
out = []
def val(tok):
    tok = tok.strip()
    m = re.fullmatch(r'(?:float|i32|i1|half)?\s*(%\d+|[-0-9.e+x]+[0-9A-Fa-f]*|true|false|undef)', tok.replace('float ', '').replace('i32 ', '').strip())
    if not m: return tok
    v = m.group(1)
    if v.startswith('%'):
        return expr.get(v[1:], 'r' + v[1:])
    return fconst(v)
UN = {'6': 'abs', '7': 'saturate', '12': 'cos', '13': 'sin', '17': 'atan', '21': 'exp2', '22': 'frac', '23': 'log2', '24': 'sqrt', '25': 'rsqrt',
      '26': 'round_ne', '27': 'floor', '28': 'ceil', '29': 'trunc', '83': 'ddx', '84': 'ddy', '85': 'ddx_fine', '86': 'ddy_fine'}
BI = {'35': 'max', '36': 'min', '37': 'umax', '38': 'umin', '39': 'imax', '40': 'imin'}
OPS = {'fadd': '+', 'fsub': '-', 'fmul': '*', 'fdiv': '/', 'add': '+', 'sub': '-', 'mul': '*', 'and': '&', 'or': '|', 'xor': '^', 'shl': '<<', 'lshr': '>>', 'ashr': '>>'}
for l in lines:
    l = re.sub(r', !.*$', '', l)
    m = re.match(r'%(\d+) = (.*)$', l)
    if not m:
        if l.startswith('call void @dx.op.storeOutput'):
            a = re.search(r'\(i32 5, i32 (\d+), i32 \d+, i8 (\d+), \w+ (.*)\)', l)
            out.append(f'  o{a.group(1)}.{"xyzw"[int(a.group(2))]} = {val(a.group(3))};')
        elif l.startswith('call void @dx.op.discard'):
            a = re.search(r'i1 (.*)\)', l); out.append(f'  discard_if({val(a.group(1))});')
        elif l.startswith('br ') or l.startswith('; <label>') or l.startswith('ret'):
            out.append('  ' + re.sub(r'%(\d+)', lambda mm: expr.get(mm.group(1), 'r' + mm.group(1)) if not l.startswith('; <label>') else 'L' + mm.group(1), l))
        elif l.startswith('store') or l.startswith('call void'):
            out.append('  ' + l)
        continue
    n, rhs = m.group(1), m.group(2)
    e = None
    if 'createHandleFromBinding' in rhs:
        a = re.search(r'ResBind (?:\{ i32 (\d+), i32 \d+, i32 (\d+), i8 (\d+) \}|zeroinitializer)', rhs)
        if a.group(1) is None: handles[n] = 't0'
        else: handles[n] = f'{CLASS[a.group(3)]}{a.group(1)}' + (f'_space{a.group(2)}' if a.group(2) != '0' else '')
        continue
    if 'annotateHandle' in rhs:
        a = re.search(r'Handle %(\d+)', rhs); handles[n] = handles.get(a.group(1), '?'); continue
    if 'cbufferLoadLegacy' in rhs:
        a = re.search(r'Handle %(\d+), i32 (.*)\)', rhs); cbufs[n] = f'{handles[a.group(1)]}[{val(a.group(2))}]'; continue
    a = re.match(r'extractvalue \S+ %(\d+), (\d+)', rhs)
    if a:
        base = a.group(1)
        if base in cbufs: e = f'{cbufs[base]}.{"xyzw"[int(a.group(2))]}'
        else: e = f'r{base}.{"xyzw"[int(a.group(2))]}'
        expr[n] = e; continue
    a = re.match(r'(fadd|fsub|fmul|fdiv|add|sub|mul|and|or|xor|shl|lshr|ashr)(?: fast| nuw| nsw| exact)* \w+ (.*), (.*)$', rhs)
    if a: e = f'({val(a.group(2))} {OPS[a.group(1)]} {val(a.group(3))})'
    a = re.match(r'(?:fcmp|icmp) (?:fast )?(\w+) \w+ (.*), (.*)$', rhs)
    if a: e = f'({val(a.group(2))} {a.group(1)} {val(a.group(3))})'
    a = re.match(r'select i1 (.*), \w+ (.*), \w+ (.*)$', rhs)
    if a: e = f'({val(a.group(1))} ? {val(a.group(2))} : {val(a.group(3))})'
    a = re.match(r'(?:sitofp|uitofp|fptosi|fptoui|bitcast|zext|sext|trunc|fpext|fptrunc) \w+ (.*) to \w+', rhs)
    if a: e = f'{rhs.split()[0]}({val(a.group(1))})'
    a = re.match(r'call \S+ @dx\.op\.unary\.\w+\(i32 (\d+), \w+ (.*)\)', rhs)
    if a: e = f'{UN.get(a.group(1), "un" + a.group(1))}({val(a.group(2))})'
    a = re.match(r'call \S+ @dx\.op\.binary\.\w+\(i32 (\d+), \w+ (.*), \w+ (.*)\)', rhs)
    if a: e = f'{BI.get(a.group(1), "bin" + a.group(1))}({val(a.group(2))}, {val(a.group(3))})'
    a = re.match(r'call \S+ @dx\.op\.tertiary\.\w+\(i32 46, \w+ (.*), \w+ (.*), \w+ (.*)\)', rhs)
    if a: e = f'({val(a.group(1))} * {val(a.group(2))} + {val(a.group(3))})'
    a = re.match(r'call \S+ @dx\.op\.dot([234])\.\w+\(i32 \d+, (.*)\)', rhs)
    if a:
        k = int(a.group(1)); args = [val(x) for x in a.group(2).split(', ')]
        e = f'dot({",".join(args[:k])} | {",".join(args[k:])})'
    a = re.match(r'call \S+ @dx\.op\.loadInput\.\w+\(i32 4, i32 (\d+), i32 (\S+), i8 (\d+)', rhs)
    if a: e = f'in{a.group(1)}{"" if a.group(2) == "0" else "[" + a.group(2) + "]"}.{"xyzw"[int(a.group(3))]}'
    a = re.match(r'call \S+ @dx\.op\.(sample\w*|textureLoad|textureGather\w*|bufferLoad|rawBufferLoad|textureGatherCmp)\.\w+\(i32 \d+, %dx\.types\.Handle %(\d+)(?:, %dx\.types\.Handle %(\d+))?, (.*)\)', rhs)
    if a:
        args = [val(x) for x in a.group(4).split(', ') if 'undef' not in x]
        e = f'{a.group(1)}({handles.get(a.group(2), "?")}{"," + handles.get(a.group(3), "?") if a.group(3) else ""}; {", ".join(args)})'
        expr[n] = 'r' + n
        out.append(f'  r{n} = {e};'); continue
    a = re.match(r'phi \w+ (.*)$', rhs)
    if a:
        e = 'phi(' + ', '.join(re.sub(r'%(\d+)', lambda mm: 'r' + mm.group(1) if mm.group(1) in expr or True else '', x) for x in re.findall(r'\[ ([^,]+), %\d+ \]', a.group(1))) + ')'
        expr[n] = 'r' + n; out.append(f'  r{n} = {e};'); continue
    if e is None:
        e = re.sub(r'%(\d+)', lambda mm: expr.get(mm.group(1), 'r' + mm.group(1)), rhs)
        expr[n] = 'r' + n; out.append(f'  r{n} = {e};'); continue
    if uses.get(n, 0) <= 1 and len(e) < 160:
        expr[n] = e
    else:
        expr[n] = 'r' + n; out.append(f'  r{n} = {e};')
print('\n'.join(out))
