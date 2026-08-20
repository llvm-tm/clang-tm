# Chapter summaries for the TM book

One short plain-text file per chapter lives in this directory:

- `ch01.txt` ... `ch19.txt` — the 19 main chapters
- `app01.txt` ... `app07.txt` — the 7 appendices
- `front.txt` — front matter (Preface, Map of Protocols and Acronyms)

## Purpose

These files are a **token-efficient map of the book**. When editing or
extending `docs/book/main.tex`, read the relevant `chNN.txt` first: it tells
you the chapter's purpose, every section/subsection, and what figures,
tables, listings, and citations it contains, without loading the full LaTeX
source into context.

## Format

Each file is plain text (no LaTeX, no markup) with:

- Chapter number/title and the Part it belongs to
- Rough size (lines, sections, figures, tables, listings, TikZ)
- Purpose: one-line summary of the chapter's argument
- Sections (with subsections), each followed by a one-line gloss
- Figures, tables, and listings by their caption/label
- Citations used (if the chapter cites distinctive references)

Newly added sections are marked `(NEW)` so a stale summary is obvious.

## How to keep them updated

Do this **whenever you change `main.tex`** (add/edit/remove a section,
figure, table, listing, or citation), and at minimum before committing book
changes:

1. Edit the matching `docs/book/summaries/chNN.txt` in the same change.
2. Regenerate the structural numbers with the extraction script:

   ```sh
   python3 docs/book/tools/extract_book_structure.py   # prints lines/sections/figures/tables/listings per chapter
   ```

   (or, from scratch: a small script that scans `main.tex` for
   `\chapter`, `\section`, `\subsection`, `\begin{figure}`,
   `\begin{table}`, `\begin{lstlisting}`, `\begin{tikzpicture}`).
3. Update only the numbers and the sections that actually changed; keep the
   glosses one line each.
4. Keep the summary **short**: it should fit comfortably in a chat context
   window alongside the chapter you are editing. If a chapter grows, prune
   old glosses rather than growing the file.

## Rules of thumb

- The single source of truth is `docs/book/main.tex`. The `sections/*.tex`
  split is a stale AIedu artifact — do not update it.
- `chapterNN_prompt.txt` / `chapterNN_response.txt` were removed; do not
  recreate them.
- A summary is a map, not a duplicate. Never paste prose from the chapter
  into these files.
- If a chapter lacks a Summary/Exercises section (e.g. ch15), say so rather
  than inventing one.