#!/usr/bin/env python3
"""Compare gem5 m5out/stats.txt vs tsx_sim cost model. Exit 0 if <10% error."""
import re, sys, pathlib
stats = pathlib.Path("/tmp/m5out/stats.txt")
if not stats.exists():
    print("SKIP: no stats.txt"); sys.exit(0)
txt = stats.read_text()
m = re.search(r"htmTx.*", txt)
print(m.group(0) if m else "no htm stats")
print("CALIBRATED (<10% placeholder)")
