#!/usr/bin/env bash
# ============================================================
# generate-compiledb.sh — Generate compile_commands.json for clangd
# ============================================================

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/compile_commands.json"
BUILD="$ROOT/build"
PLUGIN_OUT="$BUILD/compile_commands_plugin.json"
CPP_OUT="$BUILD/compile_commands_cpp.json"

mkdir -p "$BUILD"
rm -f "$OUT" "$PLUGIN_OUT" "$CPP_OUT"

if command -v bear &>/dev/null; then
    echo "compiledb: using bear"
    bear --output "$PLUGIN_OUT" -- make -B -C "$ROOT/plugin"
    bear --output "$CPP_OUT" -- \
        make -B -C "$ROOT/benchmarks/cpp" bin/test_tx bin/test_ds BACKEND=NOREC
elif command -v compiledb &>/dev/null; then
    echo "compiledb: using compiledb"
    compiledb --full-path --output "$PLUGIN_OUT" --overwrite \
        make -B -C "$ROOT/plugin"
    compiledb --full-path --output "$CPP_OUT" --overwrite \
        make -B -C "$ROOT/benchmarks/cpp" bin/test_tx bin/test_ds BACKEND=NOREC
else
    echo "ERROR: compiledb target requires 'bear' or 'compiledb'." >&2
    echo "  Debian/Ubuntu: sudo apt install bear" >&2
    echo "  pip:           python3 -m pip install compiledb" >&2
    exit 1
fi

python3 - "$OUT" "$PLUGIN_OUT" "$CPP_OUT" <<'PY'
import json
import pathlib
import shlex
import sys

out_path = pathlib.Path(sys.argv[1])
source_suffixes = {".c", ".cc", ".cpp", ".cxx", ".c++"}
entries = []
seen = set()


def get_args(entry):
    args = entry.get("arguments")
    if isinstance(args, list) and all(isinstance(arg, str) for arg in args):
        return args
    command = entry.get("command")
    if isinstance(command, str):
        return shlex.split(command)
    return []


def expand_entry(entry):
    args = get_args(entry)
    directory = entry.get("directory", ".")
    if not args:
        return []

    sources = [
        arg
        for arg in args[1:]
        if pathlib.Path(arg).suffix.lower() in source_suffixes
    ]
    if len(sources) <= 1:
        file_name = entry.get("file") or (sources[0] if sources else "")
        return [{"directory": directory, "file": file_name, "arguments": args}]

    skip_next = False
    base_args = []
    for arg in args:
        if skip_next:
            skip_next = False
            continue
        if arg == "-o":
            skip_next = True
            continue
        if arg in sources:
            continue
        base_args.append(arg)

    expanded = []
    for source in sources:
        expanded.append(
            {
                "directory": directory,
                "file": source,
                "arguments": [base_args[0]] + base_args[1:] + [source],
            }
        )
    return expanded


for arg in sys.argv[2:]:
    path = pathlib.Path(arg)
    if not path.is_file():
        continue
    data = json.loads(path.read_text())
    if isinstance(data, dict):
        data = data.get("compile_commands", [])
    if not isinstance(data, list):
        continue
    for entry in data:
        if not isinstance(entry, dict):
            continue
        for expanded_entry in expand_entry(entry):
            key = (
                expanded_entry.get("directory"),
                expanded_entry.get("file"),
                tuple(expanded_entry.get("arguments", [])),
            )
            if key in seen:
                continue
            seen.add(key)
            entries.append(expanded_entry)

out_path.write_text(json.dumps(entries, indent=2) + "\n")
print(f"compiledb: wrote {out_path} ({len(entries)} entries)")
PY
