SHELL := /bin/sh
TEX := pdflatex
MAIN := main
OUT := $(MAIN).pdf

.PHONY: all pdf watch clean cleanall clean-aux

all: $(OUT)

SECTIONS := sections/*.tex

$(OUT): $(MAIN).tex references.bib $(SECTIONS)
	@if command -v latexmk >/dev/null 2>&1; then \
		latexmk -pdf -interaction=nonstopmode $(MAIN); \
	else \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
		bibtex $(MAIN) >/dev/null 2>&1 || true; \
		makeindex $(MAIN) >/dev/null 2>&1 || true; \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
	fi

index:
	@if command -v latexmk >/dev/null 2>&1; then \
		latexmk -pdf -interaction=nonstopmode $(MAIN); \
	else \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
		bibtex $(MAIN) >/dev/null 2>&1 || true; \
		makeindex $(MAIN); \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
		$(TEX) -interaction=nonstopmode -halt-on-error $(MAIN); \
	fi

pdf: all

watch:
	@if command -v latexmk >/dev/null 2>&1; then \
		latexmk -pvc -pdf $(MAIN); \
	else \
		echo "latexmk not found; use 'make' for a one-shot build."; \
	fi

clean:
	rm -f $(MAIN).aux $(MAIN).bbl $(MAIN).blg $(MAIN).bcf \
	      $(MAIN).toc $(MAIN).lof $(MAIN).lot $(MAIN).out \
	      $(MAIN).fls $(MAIN).fdb_latexmk $(MAIN).synctex.gz \
	      $(MAIN).idx $(MAIN).ilg $(MAIN).ind $(MAIN).nav \
	      $(MAIN).snm $(MAIN).vrb $(MAIN).run.xml $(MAIN).xdv \
	      $(MAIN).glo $(MAIN).ist

cleanall: clean
	rm -f $(OUT)

clean-aux:
	@if command -v latexmk >/dev/null 2>&1; then \
		latexmk -c $(MAIN); \
	fi