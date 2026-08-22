#!/usr/bin/env python3
"""Print a per-chapter structural index of docs/book/main.tex.

Usage (from the repository root):
    python3 docs/book/tools/extract_book_structure.py

Output per chapter: line count, section/subsection titles, and counts of
figures, tables, listings, and tikzpictures. Used to keep the
chapter summaries in docs/book/summaries/ in sync with the source.
"""
import re, os, sys

root = os.path.abspath(os.path.join(os.path.dirname(__file__), '..', '..', '..'))
src_path = os.path.join(root, 'docs', 'book', 'main.tex')
if not os.path.exists(src_path):
    sys.exit(f'main.tex not found at {src_path}; run from the repo root')

# Read main.tex
with open(src_path) as f:
    main_src = f.read()
main_lines = main_src.split('\n')

# Read all section files
section_contents = {}
sections_dir = os.path.join(root, 'docs', 'book', 'sections')
if os.path.isdir(sections_dir):
    for fname in os.listdir(sections_dir):
        fpath = os.path.join(sections_dir, fname)
        if os.path.isfile(fpath):
            with open(fpath) as sf:
                section_contents[fname] = sf.read().split('\n')

# Find chapter boundaries via \input{sections/...}
# Build a mapping of line numbers to included file content
all_lines_with_offset = []  # (line_idx, 'main'|fname, offset, line_text)
for i, l in enumerate(main_lines):
    all_lines_with_offset.append(('main', None, i, l))

for fname, content in section_contents.items():
    for j, l in enumerate(content):
        all_lines_with_offset.append((fname, fname, j, l))

# Now find \input directives and track chapter boundaries
input_pattern = re.compile(r'\\input\{sections/(ch\d+|app\d+)\}')
chapters = []  # (title, start_main_line, end_main_line, start_idx in combined, end_idx in combined)
current_chapter = None
current_start = None

# Process main.tex line by line to find \input directives
for i, l in enumerate(main_lines):
    m = input_pattern.search(l)
    if m:
        # If we were in a chapter, close it
        if current_chapter is not None and current_start is not None:
            # end_idx is the line before this \input
            chapters.append((current_chapter, current_start, i-1, current_start, i-1))
        rel = m.group(1)
        if rel.startswith('ch'):
            num = rel[2:]
            current_chapter = f'CHAPTER {num}'
        else:
            num = rel[3:]
            current_chapter = f'APPENDIX {num}'
        current_start = i  # line number in main.tex of the \input

# Close last chapter
if current_chapter is not None and current_start is not None:
    chapters.append((current_chapter, current_start, len(main_lines)-1, current_start, len(main_lines)-1))

def clean_sec(s):
    s = s.replace('\\texttt{', '').replace('\\emph{', '').replace('\\textbf{','')
    s = re.sub(r'\\[A-Za-z]+\{', '', s)
    s = s.replace('}', '').replace('{', '')
    s = re.sub(r'\\(?:label|index|item|ldots|ref|cite|S|sec|section|subsection|emph|texttt)\b.*', '', s)
    return s.strip()

for title, smain, emain, sidx, eidx in chapters:
    # Collect all lines from main.tex and included section files for this chapter range
    # A simple approach: count lines in main.tex within the range, plus content from input files
    body_main = main_lines[smain:emain+1]
    
    # Count how many \input directives are within this range (to understand structure)
    # But for structural metrics, we need to look at the included files
    
    # Figure out which section files are included in this chapter
    # by looking at all \input directives in main.tex and which chapter they belong to
    # For now, just use the main.tex body
    
    nlines = len(body_main)
    
    # Count sections within the body's \section directives
    secs = []
    for l in body_main:
        m = re.search(r'\\section\{(.+?)\}', l)
        if m: secs.append(m.group(1))
    subs = []
    for l in body_main:
        m = re.search(r'\\subsection\{(.+?)\}', l)
        if m: subs.append(m.group(1))
    figs = sum(1 for l in body_main if '\\begin{figure}' in l)
    tabs = sum(1 for l in body_main if '\\begin{table}' in l)
    lsts = sum(1 for l in body_main if '\\begin{lstlisting}' in l)
    tbls_env = sum(1 for l in body_main if '\\begin{tabular}' in l)
    tiks = sum(1 for l in body_main if '\\begin{tikzpicture}' in l)
    
    print(f"### {title}")
    print(f"  lines={nlines} sections={len(secs)} subsections={len(subs)} figures={figs} tables={tabs} listings={lsts} tikz={tiks}")
    for s in secs: print(f"    SEC: {clean_sec(s)}")
    for s in subs: print(f"      SUB: {clean_sec(s)}")