#!/usr/bin/env python3
"""Transform p-column tables: body cells middle-aligned (M{}), header
cells bottom-aligned via \\thb. Skips tables already using \\thb."""
import re, sys, glob

COLSPEC_RE = re.compile(r'\\begin\{tabular\}\{([^{}]*(?:\{[^{}]*\}[^{}]*)*)\}')
TOKEN_RE = re.compile(r'([lcr])|([pmb])\{([^{}]*)\}')

def parse_cols(spec):
    cols = []
    for m in TOKEN_RE.finditer(spec):
        if m.group(1):
            cols.append(('lcr', m.group(1)))
        else:
            cols.append((m.group(2), m.group(3)))
    return cols

def norm_width(w):
    # header width must be evaluated against full text width, not the
    # current cell's \linewidth
    return w.replace('\\linewidth', '\\textwidth').replace('\\columnwidth', '\\textwidth')

def transform(text, fname):
    out, pos, ntab, nmod = [], 0, 0, 0
    while True:
        m = COLSPEC_RE.search(text, pos)
        if not m:
            out.append(text[pos:])
            break
        ntab += 1
        spec = m.group(1)
        cols = parse_cols(spec)
        has_p = any(k in ('p', 'm', 'b') for k, _ in cols)
        # find the table body up to \end{tabular}
        end = text.index(r'\end{tabular}', m.end())
        body = text[m.end():end]
        already = '\\thb' in body or not has_p
        out.append(text[pos:m.end()])
        if already:
            out.append(body)
            pos = end
            continue
        new_spec = spec
        for k, w in cols:
            if k in ('p', 'm', 'b'):
                new_spec = new_spec.replace(f'{k}{{{w}}}', f'M{{{w}}}', 1)
        out.append(text[pos:m.start()])
        out.append('\\begin{tabular}{' + new_spec + '}')
        # header row: first line containing & after optional \hline lines
        lines = body.split('\n')
        hidx = None
        for i, l in enumerate(lines):
            if not l.strip() or l.strip() == r'\hline':
                continue
            hidx = i
            break
        if hidx is None or '&' not in lines[hidx] or not lines[hidx].rstrip().endswith('\\\\'):
            print(f'  WARN {fname}: table {ntab} header not found/simple; skipped', file=sys.stderr)
            out.append(body)
            pos = end
            continue
        cells = lines[hidx].rstrip()
        assert cells.endswith('\\\\')
        cells = cells[:-2]
        parts = cells.split('&')
        if len(parts) != len(cols):
            print(f'  WARN {fname}: table {ntab} has {len(parts)} header cells, '
                  f'{len(cols)} columns; skipped', file=sys.stderr)
            out.append(body)
            pos = end
            continue
        newparts = []
        for (k, w), c in zip(cols, parts):
            c = c.strip()
            if k in ('p', 'm', 'b'):
                newparts.append(f'\\thb{{{norm_width(w)}}}{{{c}}}')
            else:
                newparts.append(c)
        lines[hidx] = ' & '.join(newparts) + ' \\\\'
        out.append('\n'.join(lines))
        nmod += 1
        pos = end
    return ''.join(out), ntab, nmod

total_t = total_m = 0
for f in sorted(glob.glob('sections/*.tex')):
    text = open(f).read()
    new, ntab, nmod = transform(text, f)
    if nmod:
        open(f, 'w').write(new)
    total_t += ntab; total_m += nmod
    print(f'{f}: {ntab} tables, {nmod} transformed')
print(f'TOTAL: {total_t} tables, {total_m} transformed')
