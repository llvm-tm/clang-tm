#!/usr/bin/env python3
"""Print a per-chapter structural index of docs/book/main.tex.

Usage (from the repository root):
    python3 docs/book/tools/extract_book_structure.py

Output per chapter: line count, section/subsection titles, and counts of
figures, tables, tabulars, listings, and tikzpictures. Used to keep the
chapter summaries in docs/book/summaries/ in sync with the source.
"""
import re, os, sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
src_path = os.path.join(root, 'docs', 'book', 'main.tex')
if not os.path.exists(src_path):
    sys.exit(f'main.tex not found at {src_path}; run from the repo root')
src = open(src_path).read()
lines = src.split('\n')

# find chapter boundaries
chapters = []  # (label, start_line, end_line)
ch_re = re.compile(r'\\chapter\{(.+?)\}')
cur = None
for i, l in enumerate(lines):
    m = ch_re.search(l)
    if m:
        if cur: cur[2] = i
        cur = [m.group(1), i, len(lines)]
        chapters.append(cur)
# fix ends
for k in range(len(chapters)-1):
    chapters[k][2] = chapters[k+1][1]

def clean(s):
    # strip latex commands and overhead
    s = s.replace('\\texttt{', '').replace('\\emph{', '').replace('\\textbf{','')
    s = re.sub(r'\\[A-Za-z]+\{', '', s)
    s = s.replace('}', '').replace('{', '')
    s = re.sub(r'\\(?:label|index|item|ldots|ref|cite|S|sec|section|subsection|emph|texttt)\b.*', '', s)
    return s.strip()

for title, start, end in chapters:
    body = lines[start:end]
    nlines = end - start
    # sections
    secs = []
    for l in body:
        m = re.search(r'\\section\{(.+?)\}', l)
        if m: secs.append(m.group(1))
    subs = []
    for l in body:
        m = re.search(r'\\subsection\{(.+?)\}', l)
        if m: subs.append(m.group(1))
    figs = sum(1 for l in body if '\\begin{figure}' in l)
    tabs = sum(1 for l in body if '\\begin{table}' in l)
    lsts = sum(1 for l in body if '\\begin{lstlisting}' in l)
    tbls_env = sum(1 for l in body if '\\begin{tabular}' in l)
    tiks = sum(1 for l in body if '\\begin{tikzpicture}' in l)
    nwords = len(re.sub(r'\\[A-Za-z]+\{[^}]*\}', ' ', ' '.join(body)))
    print(f"### {title}")
    print(f"  lines={nlines} sections={len(secs)} subsections={len(subs)} figures={figs} tables={tabs} tabulars={tbls_env} listings={lsts} tikz={tiks}")
    for s in secs: print(f"    SEC: {s}")
    for s in subs: print(f"      SUB: {s}")
