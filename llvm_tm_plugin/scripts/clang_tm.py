#!/usr/bin/env python3
"""
clang-tm.py — Source-to-source TM instrumenter for C++.

Transforms TM-annotated C++ code into instrumented code that works with a
software transactional memory runtime (e.g., TinySTM).

Approach:
  1. Parse with libclang to identify TM annotations on structs, globals, functions.
  2. Copy the original source line-by-line, applying transformations:
     - Replace container types (std::vector → tm::vector, etc.) inside TM structs.
     - Wrap TM global variable accesses with tm_read / tm_write calls.
     - Clone TX-annotated functions with instrumentation.
  3. Output a self-contained .cpp that compiles with the TM runtime library + container headers.

Usage:
  python3 clang_tm.py input.cpp -o output_dir
  python3 clang_tm.py input.cpp -o output_dir --build  # compile & link with TinySTM

Environment:
  CLANG_LIB   path to libclang shared library (default: /usr/lib/llvm-21/lib/libclang-21.so.1)
  TINYSTM     path to TinySTM source root (default: /mnt/projects/INESCID/clang-tm/llvm_tm_plugin/tinystm)
"""

import argparse
import os
import re
import subprocess
import sys
from pathlib import Path

import clang.cindex
from clang.cindex import CursorKind, TypeKind

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------

CLANG_LIB = os.environ.get(
    "CLANG_LIB",
    "/usr/lib/llvm-21/lib/libclang-21.so.1",
)
clang.cindex.Config.set_library_file(CLANG_LIB)

SCRIPT_DIR = Path(__file__).resolve().parent
STL_CACHE_DIR = SCRIPT_DIR.parent / "stl_cache"
PROJECT_ROOT = SCRIPT_DIR.parent.parent

# TinySTM can be in any of these locations
_TINYSTM_CANDIDATES = [
    Path(os.environ.get("TINYSTM", "")),
    PROJECT_ROOT / "llvm_tm_plugin" / "tinystm",
    PROJECT_ROOT / "llvm_tm_plugin" / "runtime" / "tinystm",
    Path("/usr/local/tinystm"),
    Path("/opt/tinystm"),
]
TINYSTM_DIR = None
for c in _TINYSTM_CANDIDATES:
    if c and (c / "src" / "tinystm.c").exists():
        TINYSTM_DIR = c
        break
if TINYSTM_DIR is None:
    TINYSTM_DIR = _TINYSTM_CANDIDATES[0]

# ---------------------------------------------------------------------------
# Type → tm_read/tm_write suffix
# ---------------------------------------------------------------------------


def tm_suffix(t):
    """Map a clang Type to the tm_read/tm_write suffix (i1, i4, i8, f4, f8, ptr)."""
    k = t.kind
    if k == TypeKind.TYPEDEF:
        return tm_suffix(t.get_canonical())
    if k == TypeKind.ELABORATED:
        return tm_suffix(t.get_named_type()) if hasattr(t, "get_named_type") else "ptr"
    # Integer types
    if k in (TypeKind.CHAR_S, TypeKind.CHAR_U, TypeKind.SCHAR,
             TypeKind.UCHAR, TypeKind.BOOL):
        return "i1"
    if k in (TypeKind.SHORT, TypeKind.USHORT):
        return "i2"
    if k in (TypeKind.INT, TypeKind.UINT):
        return "i4"
    if k in (TypeKind.LONG, TypeKind.ULONG):
        try:
            sz = t.get_size()
            return "i4" if sz <= 4 else "i8"
        except Exception:
            return "i8"
    if k in (TypeKind.LONGLONG, TypeKind.ULONGLONG):
        return "i8"
    # Floating point
    if k == TypeKind.FLOAT:
        return "f4"
    if k == TypeKind.DOUBLE:
        return "f8"
    if k == TypeKind.LONGDOUBLE:
        return "f8"
    # Pointer / array / record → ptr
    if k in (TypeKind.POINTER,
             TypeKind.INCOMPLETEARRAY, TypeKind.VARIABLEARRAY,
             TypeKind.RECORD, TypeKind.CONSTANTARRAY):
        return "ptr"
    return "ptr"


# ---------------------------------------------------------------------------
# Container type helpers
# ---------------------------------------------------------------------------

CONTAINER_TYPES = {
    "std::vector": "tm_stl::vector",
    "std::set": "tm_stl::set",
    "std::unordered_map": "tm_stl::unordered_map",
}

_CONTAINER_KEYS = sorted(CONTAINER_TYPES.keys(), key=len, reverse=True)


def has_container_type(spelling):
    """Check if a type spelling contains any std::vector/set/unordered_map."""
    for prefix in _CONTAINER_KEYS:
        if prefix in spelling:
            return True
    return False


def replace_container_types(line: str) -> str:
    """Replace std::vector/set/unordered_map with tm_stl versions."""
    for old, new in CONTAINER_TYPES.items():
        line = line.replace(old, new)
    return line


# ---------------------------------------------------------------------------
# Source line utilities
# ---------------------------------------------------------------------------


def read_lines(path):
    with open(path, "rb") as f:
        data = f.read()
    data = data.replace(b"\r\n", b"\n").replace(b"\r", b"\n")
    return [l.decode("utf-8", errors="replace") for l in data.split(b"\n")]


# ---------------------------------------------------------------------------
# Annotation helpers
# ---------------------------------------------------------------------------


def has_annot(cursor, name):
    """Return True if cursor has an __attribute__((annotate(name)))."""
    for c in _safe_children(cursor):
        try:
            if c.kind == CursorKind.ANNOTATE_ATTR and c.spelling == name:
                return True
        except ValueError:
            continue
    return False


def _safe_children(node):
    """Get children with error handling for unknown cursor kinds."""
    try:
        return list(node.get_children())
    except ValueError:
        return []
    except Exception:
        return []


def find_tm_structs(tu):
    """Return list of struct/class cursors annotated with 'tm'."""
    out = []

    def walk(node):
        try:
            k = node.kind
        except ValueError:
            return
        if k in (CursorKind.STRUCT_DECL, CursorKind.CLASS_DECL):
            if has_annot(node, "tm"):
                out.append(node)
        for c in _safe_children(node):
            walk(c)

    walk(tu.cursor)
    return out


def find_tm_globals(tu):
    """Return list of variable cursors annotated with 'tm'."""
    out = []

    def walk(node, depth=0):
        try:
            k = node.kind
        except ValueError:
            return
        if k == CursorKind.VAR_DECL and depth <= 2:
            if has_annot(node, "tm"):
                out.append(node)
        for c in _safe_children(node):
            walk(c, depth + 1)

    walk(tu.cursor)
    return out


def find_tx_funcs(tu):
    """Return list of function cursors annotated with 'transaction'."""
    out = []

    def walk(node, depth=0):
        try:
            k = node.kind
        except ValueError:
            return
        if k == CursorKind.FUNCTION_DECL and has_annot(node, "transaction"):
            out.append(node)
        for c in _safe_children(node):
            walk(c, depth + 1)

    walk(tu.cursor)
    return out


# ---------------------------------------------------------------------------
# TM global naming: collect names of all TM-tracked variables
# (both direct TM globals and fields of TM struct instances)
# ---------------------------------------------------------------------------


class TMAnalyzer:
    """Analyze the AST and collect information for instrumentation."""

    def __init__(self, tu):
        self.tu = tu
        self.tm_structs = find_tm_structs(tu)
        self.tm_globals = find_tm_globals(tu)
        self.tx_funcs = find_tx_funcs(tu)

        # Direct TM global variable names
        self.tm_var_names = {g.spelling for g in self.tm_globals}

        # TM struct *field* names (fields of TM-annotated structs)
        self.tm_struct_fields = {}  # struct_name → {field_name: type_cursor}
        for s in self.tm_structs:
            fields = {}
            for c in s.get_children():
                if c.kind in (CursorKind.FIELD_DECL,):
                    fields[c.spelling] = c
            self.tm_struct_fields[s.spelling] = fields

        # Collect TM struct pointer variable names
        # e.g. `static YadaData* g_yada = nullptr;`
        self.tm_ptr_var_names = set()
        self.tm_ptr_var_types = {}  # name → struct_name
        for c in _safe_children(tu.cursor):
            try:
                if c.kind == CursorKind.VAR_DECL:
                    type_spelling = c.type.spelling
                    for s in self.tm_structs:
                        if f"{s.spelling}*" in type_spelling or f"{s.spelling} *" in type_spelling:
                            self.tm_ptr_var_names.add(c.spelling)
                            self.tm_ptr_var_types[c.spelling] = s.spelling
                            break
            except Exception:
                continue

        # Also collect TX function parameters that are TM struct pointers
        for func in self.tx_funcs:
            for p in func.get_arguments():
                try:
                    type_spelling = p.type.spelling
                    for s in self.tm_structs:
                        if f"{s.spelling}*" in type_spelling or f"{s.spelling} *" in type_spelling:
                            self.tm_ptr_var_names.add(p.spelling)
                            self.tm_ptr_var_types[p.spelling] = s.spelling
                            break
                except Exception:
                    continue

    def has_any_tm(self):
        return bool(self.tm_structs or self.tm_globals or self.tx_funcs)


# ---------------------------------------------------------------------------
# Transformer: line-by-line source transformation
# ---------------------------------------------------------------------------


class TMTransformer:
    """Transform original source lines into instrumented output."""

    def __init__(self, src_path: Path, analyzer: TMAnalyzer):
        self.src_path = src_path
        self.lines = read_lines(src_path)
        self.a = analyzer

    # ------------------------------------------------------------------
    # Struct body pre-processing
    # ------------------------------------------------------------------

    def _needs_container(self, container_key: str) -> bool:
        """Check if any TM struct field uses a given container type."""
        for s in self.a.tm_structs:
            for fc in self.a.tm_struct_fields.get(s.spelling, {}).values():
                if container_key in fc.type.spelling:
                    return True
        return False

    def _write_modified_source(self, output_dir: Path) -> Path:
        """Write a copy of the original source with TM struct bodies
        using tm_stl container types instead of std:: ones.

        Only the lines inside TM struct bodies are modified; everything
        else stays verbatim. Returns the path to the modified copy.
        """
        if not self.a.tm_structs:
            return self.src_path

        lines = self.lines[:]

        for struct_cursor in self.a.tm_structs:
            extent = struct_cursor.extent
            start_line = extent.start.line - 1
            end_line = extent.end.line - 1

            # Find the first { within the struct range (body start)
            body_start = None
            for i in range(start_line, end_line + 1):
                if '{' in lines[i]:
                    body_start = i
                    break

            if body_start is None:
                continue

            # Transform lines between { and } (exclusive of brace lines)
            for i in range(body_start + 1, end_line):
                lines[i] = replace_container_types(lines[i])

        modified_name = f"{self.src_path.stem}_tm_modified{self.src_path.suffix}"
        output_path = output_dir / modified_name
        output_path.write_text('\n'.join(lines))
        return output_path

    def _modified_source_include(self, output_dir: Path) -> str:
        """Get the #include line for the modified source copy."""
        if not self.a.tm_structs:
            return f'#include "{self.src_path.resolve()}"'
        modified_name = f"{self.src_path.stem}_tm_modified{self.src_path.suffix}"
        modified_path = (output_dir / modified_name).resolve()
        return f'#include "{modified_path}"'

    # ------------------------------------------------------------------
    # Main transformation
    # ------------------------------------------------------------------

    def transform(self, output_dir: Path | None = None) -> list:
        """Generate instrumented output: include modified source + add instrumented clones.

        Strategy:
          1. Pre-process: write modified copy with TM struct bodies using tm_stl types.
          2. Emit runtime headers, macros, and tm_stl container wrappers.
          3. #include the modified source (provides all types, non-TX code).
          4. Emit _tm_clone forward declarations + instrumented function clones.
        """
        out = []

        # Determine which tm_stl wrappers are needed
        has_vec = self._needs_container("vector")
        has_set = self._needs_container("set")
        has_umap = self._needs_container("unordered_map")

        # Header
        out.append("// ------------------------------------------------------------")
        out.append(f"// clang-tm.py — TM-instrumented C++ (source: {self.src_path.name})")
        out.append("// ------------------------------------------------------------")
        out.append("")
        out.append('#include "tm_runtime_cpp.h"')
        if has_vec: out.append('#include "tm_vector.h"')
        if has_set: out.append('#include "tm_set.h"')
        if has_umap: out.append('#include "tm_unordered_map.h"')
        out.append("")
        out.append("#define TM /*tm*/")
        out.append("#define TX /*tx*/")
        out.append("#define THREAD /*thread*/")
        out.append("")

        # Include source (modified copy if TM structs need container substitution)
        if output_dir is None:
            output_dir = self.src_path.parent
        self._write_modified_source(output_dir)
        out.append(self._modified_source_include(output_dir))
        out.append("")

        # Forward declarations
        def _forward_decl(func_cursor):
            try:
                fname = func_cursor.spelling
                fret = func_cursor.result_type.spelling
                fparams = []
                for p in func_cursor.get_arguments():
                    fparams.append(f"{p.type.spelling} {p.spelling}")
                return f"{fret} {fname}_tm_clone({', '.join(fparams)});"
            except Exception:
                return None

        out.append("// ---- Instrumented clones ----")
        seen = set()
        for func in self.a.tx_funcs:
            fd = _forward_decl(func)
            if fd:
                out.append(fd)
                seen.add(func.spelling)
        for c in _safe_children(self.a.tu.cursor):
            try:
                if c.kind == CursorKind.FUNCTION_DECL and has_annot(c, "thread"):
                    if c.spelling not in seen:
                        fd = _forward_decl(c)
                        if fd:
                            out.append(fd)
                            seen.add(c.spelling)
            except Exception:
                continue
        out.append("")

        # Instrumented clones
        for func in self.a.tx_funcs:
            self._emit_instrumented_clone(func, out)
            out.append("")
        for c in _safe_children(self.a.tu.cursor):
            try:
                if c.kind == CursorKind.FUNCTION_DECL and has_annot(c, "thread"):
                    out.append("")
                    out.append("// ---- Instrumented THREAD clone ----")
                    self._emit_instrumented_clone(c, out)
            except Exception:
                continue

        return out

    # ------------------------------------------------------------------
    # TX function cloning
    # ------------------------------------------------------------------


    @staticmethod
    def _normalize_indent(line, base_indent=4):
        """Strip original indent and re-indent to base_indent."""
        stripped = line.lstrip()
        if not stripped:
            return ""
        return " " * base_indent + stripped

    def _emit_instrumented_clone(self, func, out):
        """Emit the instrumented clone of a TX function.

        The clone wraps the original function body with tx_start/tx_end
        and instruments TM accesses inside the body.
        """
        name = func.spelling

        try:
            ret_t = func.result_type.spelling
        except Exception:
            ret_t = "void"

        params = []
        for p in func.get_arguments():
            try:
                ptype = p.type.spelling
            except Exception:
                ptype = "int"
            params.append(f"{ptype} {p.spelling}")
        param_str = ", ".join(params)

        body_start_line = None
        body_end_line = None
        for c in func.get_children():
            if c.kind == CursorKind.COMPOUND_STMT:
                body_start_line = c.extent.start.line - 1
                body_end_line = c.extent.end.line - 1
                break

        out.append(f"// Instrumented clone of '{name}'")
        out.append(f"__attribute__((noinline)) {ret_t} {name}_tm_clone({param_str}) {{")
        out.append("    // ---- transaction begin ----")
        out.append("    tx_start();")
        out.append("")

        has_return = False
        if body_start_line is not None and body_end_line is not None:
            raw_lines = self.lines[body_start_line:body_end_line + 1]

            compound_cursor = None
            for c in func.get_children():
                if c.kind == CursorKind.COMPOUND_STMT:
                    compound_cursor = c
                    break

            if raw_lines and compound_cursor:
                col = compound_cursor.extent.start.column
                if col > 1 and len(raw_lines[0]) >= col - 1:
                    raw_lines[0] = raw_lines[0][col - 1:]
                if raw_lines[0].strip() in ("", "{"):
                    raw_lines.pop(0)

            if raw_lines:
                last = raw_lines[-1].strip()
                if last == "}":
                    raw_lines.pop()

            brace_depth = 0
            for raw_line in raw_lines:
                stripped = raw_line.strip()

                if not stripped:
                    continue

                # Track brace depth to skip returns inside lambdas/nested blocks
                brace_depth += stripped.count("{") - stripped.count("}")
                if brace_depth < 0:
                    brace_depth = 0

                # Pass through brace boundaries
                if stripped in ("{", "}"):
                    out.append(self._normalize_indent(raw_line))
                    continue

                # Instrument the line
                wrapped = self._wrap_tm_accesses(raw_line, func)

                # Redirect TX function calls to their _tm_clone variants
                for tx_func in self.a.tx_funcs:
                    wrapped = re.sub(
                        r'\b' + re.escape(tx_func.spelling) + r'\b(?!_tm_clone)',
                        tx_func.spelling + "_tm_clone",
                        wrapped,
                    )

                # Handle return only at top level (not inside lambda)
                if brace_depth == 0 and (stripped.startswith("return ") or stripped == "return;"):
                    has_return = True
                    out.append("")
                    out.append("    // ---- transaction end ----")
                    out.append("    tx_end();")
                    out.append(self._normalize_indent(wrapped))
                else:
                    out.append(self._normalize_indent(wrapped))

        if not has_return:
            out.append("")
            out.append("    // ---- transaction end ----")
            out.append("    tx_end();")
        out.append("}")

    # ------------------------------------------------------------------
    # TM access wrapping (the core instrumentation logic)
    # ------------------------------------------------------------------

    def _wrap_tm_accesses(self, line: str, func) -> str:
        """Replace TM variable/field accesses in a single line with tm_read/tm_write calls.

        Handles:
          - Direct TM globals:     g_counter = rhs    → tm_write_*(&g_counter, rhs)
          - TM struct ptr fields:  data->scalar = rhs → tm_write_*(&data->scalar, rhs)
          - Read-side uses:        x = g_counter + 1  → x = tm_read_*(&g_counter) + 1
        """
        out = line

        # Phase 1: Direct TM global variables
        for name in sorted(self.a.tm_var_names, key=len, reverse=True):
            out = self._wrap_var(line, name, self._type_of_tm_var(name), out)

        # Phase 2: TM struct pointer fields (g_yada->field, data->field, param->field)
        for ptr_name in sorted(self.a.tm_ptr_var_names, key=len, reverse=True):
            struct_name = self.a.tm_ptr_var_types[ptr_name]
            fields = self.a.tm_struct_fields.get(struct_name, {})
            for field_name in sorted(fields.keys(), key=len, reverse=True):
                fc = fields[field_name]
                out = self._wrap_arrow_field(line, ptr_name, field_name, fc.type, out)

        return out

    def _type_of_tm_var(self, name):
        """Get clang Type for a TM global variable by name."""
        for g in self.a.tm_globals:
            if g.spelling == name:
                return g.type
        return None

    def _is_container_type(self, type_obj):
        """Check if a clang Type is a container type (vector/set/unordered_map)."""
        try:
            spelling = type_obj.spelling
        except Exception:
            return False
        return has_container_type(spelling)

    # ------------------------------------------------------------------
    # Access wrapping helpers
    # ------------------------------------------------------------------

    def _inc_dec_wrap(self, expr, suffix, stripped_prefix=False):
        """Create increment/decrement wrapping.

        If stripped_prefix=True, expr is the full access expression (e.g., 'data->count').
        Returns (tm_write expression for increment, tm_write expression for decrement)
        or None if not applicable.
        """
        inc_write = f"tm_write_{suffix}(&{expr}, tm_read_{suffix}(&{expr}) + 1)"
        dec_write = f"tm_write_{suffix}(&{expr}, tm_read_{suffix}(&{expr}) - 1)"
        return inc_write, dec_write

    def _wrap_write_side(self, out, expr_full, suffix):
        """Try to wrap write-side patterns (assignment, inc/dec) for expr_full.

        Returns (modified_out, True) if wrapped, or (out, False) if not.
        """
        # Post-increment: expr++
        m = re.search(re.escape(expr_full) + r'\s*\+\+', out)
        if m:
            inc, _ = self._inc_dec_wrap(expr_full, suffix)
            return out[:m.start()] + inc + out[m.end():], True
        # Post-decrement: expr--
        m = re.search(re.escape(expr_full) + r'\s*--', out)
        if m:
            _, dec = self._inc_dec_wrap(expr_full, suffix)
            return out[:m.start()] + dec + out[m.end():], True
        # Pre-increment: ++expr
        m = re.search(r'\+\+\s*' + re.escape(expr_full), out)
        if m:
            inc, _ = self._inc_dec_wrap(expr_full, suffix)
            return out[:m.start()] + inc + out[m.end():], True
        # Pre-decrement: --expr
        m = re.search(r'--\s*' + re.escape(expr_full), out)
        if m:
            _, dec = self._inc_dec_wrap(expr_full, suffix)
            return out[:m.start()] + dec + out[m.end():], True
        # Assignment: expr = rhs ;
        m = re.search(
            re.escape(expr_full) + r'\s*=(=?)\s*([^;]+);',
            out,
        )
        if m:
            op = m.group(1)
            rhs = m.group(2).strip()
            if op == "":  # plain assignment, not ==
                wrap = f"tm_write_{suffix}(&{expr_full}, ({rhs}));"
                return out[:m.start()] + wrap + out[m.end():], True
        return out, False

    def _wrap_read_side(self, out, expr_full, suffix):
        """Replace read-side occurrences of expr_full with tm_read_*."""
        # Only replace if not on LHS of assignment (already handled) or inc/dec (handled)
        out = re.sub(
            r'(?<!&)\b' + re.escape(expr_full) + r'\b(?!\s*(?:=|[+]{2}|[-]{2}))',
            f"tm_read_{suffix}(&{expr_full})",
            out,
        )
        return out

    def _make_expr(self, ptr_name, field_name):
        """Create the expression pattern for ptr->field."""
        return f"{ptr_name}->{field_name}"

    def _wrap_var(self, line, var_name, var_type, out):
        """Wrap direct TM global variable accesses in a line."""
        if not var_type:
            return out
        if self._is_container_type(var_type):
            return out

        suffix = tm_suffix(var_type)

        # Try write-side first
        new_out, wrapped = self._wrap_write_side(out, var_name, suffix)
        if wrapped:
            return new_out

        # Read-side
        return self._wrap_read_side(out, var_name, suffix)

    def _wrap_arrow_field(self, line, ptr_name, field_name, field_type, out):
        """Wrap ptr->field accesses (ptr is a named variable)."""
        if self._is_container_type(field_type):
            return out

        suffix = tm_suffix(field_type)
        expr = self._make_expr(ptr_name, field_name)

        # Try write-side first (assignment, inc/dec)
        new_out, wrapped = self._wrap_write_side(out, expr, suffix)
        if wrapped:
            return new_out

        # Read-side
        return self._wrap_read_side(out, expr, suffix)




# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------


def write_output(inp: Path, out_dir: Path, lines: list, analyzer: TMAnalyzer):
    """Write the instrumented source and runtime header to out_dir."""
    # Write instrumented source
    out_path = out_dir / inp.name
    out_path.write_text("\n".join(lines))
    print(f"  wrote {out_path}")

    # Write runtime header
    rt_h = out_dir / "tm_runtime_cpp.h"
    rt_h.write_text(RUNTIME_CPP_H)

    # Copy stl_cache headers if needed
    has_container_usage = any(
        has_container_type(g.type.spelling) for g in analyzer.tm_globals
    )
    if not has_container_usage:
        for s in analyzer.tm_structs:
            for fc in analyzer.tm_struct_fields.get(s.spelling, {}).values():
                if has_container_type(fc.type.spelling):
                    has_container_usage = True
                    break
    if has_container_usage and STL_CACHE_DIR.exists():
        import shutil
        for fn in ["tm_vector.h", "tm_set.h", "tm_unordered_map.h"]:
            src = STL_CACHE_DIR / fn
            if src.exists():
                shutil.copy2(src, out_dir / fn)
        print(f"  copied stl_cache headers to {out_dir}/")

    return out_path


# ---------------------------------------------------------------------------
# Build: compile and link with TinySTM
# ---------------------------------------------------------------------------


def build_tm(inp: Path, out_path: Path, out_dir: Path):
    """Compile the instrumented output and link with TinySTM runtime."""
    tinystm_src = TINYSTM_DIR / "src" / "tinystm.c" if TINYSTM_DIR else None
    if not tinystm_src or not tinystm_src.exists():
        print(f"  error: TinySTM not found.", file=sys.stderr)
        print(f"  Install TinySTM or set TINYSTM environment variable.", file=sys.stderr)
        print(f"  (Looked in: {', '.join(str(c) for c in _TINYSTM_CANDIDATES)})", file=sys.stderr)
        return False

    # Include paths: output dir, stl_cache, original source dir, TinySTM
    include_dirs = [
        f"-I{out_dir}",
        f"-I{STL_CACHE_DIR}",
        f"-I{inp.parent}",
    ]
    if (PROJECT_ROOT / "backends").exists():
        include_dirs.append(f"-I{PROJECT_ROOT / 'backends'}")
        include_dirs.append(f"-I{PROJECT_ROOT / 'backends' / 'TinySTM'}")

    cmd = (
        ["c++", "-std=c++20", "-O1", "-pthread"]
        + include_dirs
        + ["-DINSTRUMENTED", str(src), str(tinystm_src), "-o", str(binary)]
    )

    print(f"  compile: {' '.join(str(c) for c in cmd)}")
    result = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if result.returncode != 0:
        print("  compilation failed:", file=sys.stderr)
        print(result.stderr[:2000], file=sys.stderr)
        return False
    print(f"  binary: {binary}")
    return True


# ---------------------------------------------------------------------------
# Comparison
# ---------------------------------------------------------------------------


def print_comparison():
    print()
    print("=" * 60)
    print("  COMPARISON: Python source-level vs LLVM IR instrumenter")
    print("=" * 60)
    print("""
  Python source-level:
    - Operates on C/C++ source → instrumented C++ source
    - Uses libclang AST for structure, line-by-line transformation
    - Output is human-readable, debuggable C++

  LLVM IR plugin:
    - Operates on LLVM bitcode → instrumented IR
    - Instrumentation at the instruction level (every load/store)
    - Full C++ support (templates, overloads, lambdas)
    - Output is LLVM IR (hard to read)

  Choosing between them:
    - Python tool: good for prototyping, debugging, learning
    - LLVM plugin: production use, complete coverage
""")


# ---------------------------------------------------------------------------
# Runtime header
# ---------------------------------------------------------------------------

RUNTIME_CPP_H = """\
#ifndef TM_RUNTIME_CPP_H
#define TM_RUNTIME_CPP_H

#include <cstdint>
#include <cstdlib>

#ifdef __cplusplus
extern "C" {
#endif

void tx_start();
void tx_end();

uint8_t  tm_read_i1(const void *addr);
uint16_t tm_read_i2(const void *addr);
uint32_t tm_read_i4(const void *addr);
uint64_t tm_read_i8(const void *addr);
float    tm_read_f4(const void *addr);
double   tm_read_f8(const void *addr);
void*    tm_read_ptr(const void *addr);

void tm_write_i1(void *addr, uint8_t val);
void tm_write_i2(void *addr, uint16_t val);
void tm_write_i4(void *addr, uint32_t val);
void tm_write_i8(void *addr, uint64_t val);
void tm_write_f4(void *addr, float val);
void tm_write_f8(void *addr, double val);
void tm_write_ptr(void *addr, void *val);

#ifdef __cplusplus
}
#endif

#endif
"""


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def main():
    ap = argparse.ArgumentParser(
        description="Source-to-source TM instrumenter for C++",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""
Examples:
  python3 clang_tm.py test.cpp -o tm_out
  python3 clang_tm.py test.cpp -o tm_out --build
  python3 clang_tm.py test.cpp -o tm_out --compare
        """,
    )
    ap.add_argument("input", help="C/C++ source file to instrument")
    ap.add_argument("-o", "--output-dir", default=None, help="Output directory (default: <input_dir>/tm_out)")
    ap.add_argument("--build", action="store_true", help="Compile and link with TinySTM after instrumentation")
    ap.add_argument("--compare", action="store_true", help="Print comparison with LLVM plugin approach")
    ap.add_argument("--clang-args", default="", help="Extra arguments for libclang parser")
    args = ap.parse_args()

    inp = Path(args.input)
    if not inp.exists():
        print(f"error: {args.input} not found", file=sys.stderr)
        sys.exit(1)

    out_dir = Path(args.output_dir) if args.output_dir else inp.parent / "tm_out"
    print(f"  Input:  {inp}")
    print(f"  Output: {out_dir}")

    # ---- Parse ----
    print()
    print("[1/3] Parsing with libclang...")
    idx = clang.cindex.Index.create()
    extra = args.clang_args.split() if args.clang_args else []
    flags = [
        "-std=c++20",
        "-fno-inline",
        "-x", "c++",
        "-D__extension__=",
        "-D__const=const",
        f"-I{inp.parent}",
        f"-I{STL_CACHE_DIR}",
    ] + extra
    try:
        tu = idx.parse(str(inp), args=flags)
    except Exception as e:
        print(f"  parse error: {e}", file=sys.stderr)
        sys.exit(1)

    analyzer = TMAnalyzer(tu)

    print(f"  TM structs:    {len(analyzer.tm_structs)}")
    for s in analyzer.tm_structs:
        print(f"    struct {s.spelling}")
    print(f"  TM globals:    {len(analyzer.tm_globals)}")
    for g in analyzer.tm_globals:
        print(f"    {g.type.spelling} {g.spelling}")
    print(f"  TM ptr vars:   {len(analyzer.tm_ptr_var_names)}")
    for n in analyzer.tm_ptr_var_names:
        print(f"    {n} -> struct {analyzer.tm_ptr_var_types[n]}")
    print(f"  TX functions:  {len(analyzer.tx_funcs)}")
    for f in analyzer.tx_funcs:
        print(f"    {f.result_type.spelling} {f.spelling}")

    if not analyzer.has_any_tm():
        print("  (no TM/TX annotations found)")
        return

    # ---- Transform ----
    print()
    print("[2/3] Transforming source...")
    out_dir.mkdir(parents=True, exist_ok=True)
    transformer = TMTransformer(inp, analyzer)
    output_lines = transformer.transform(output_dir=out_dir)
    out_path = write_output(inp, out_dir, output_lines, analyzer)

    # ---- Build ----
    if args.build:
        print()
        print("[3/3] Building with TinySTM...")
        ok = build_tm(inp, out_path, out_dir)
        if not ok:
            sys.exit(1)
    else:
        print()
        print("[3/3] Done.")
        print(f"  Instrumented: {out_path}")
        print(f"  Compile with: c++ -std=c++20 -I{out_dir} -I{STL_CACHE_DIR} -I{inp.parent} {out_path} -o program -pthread")
        if TINYSTM_DIR.exists():
            print(f"      With TinySTM: ... -I{TINYSTM_DIR / 'src'} {TINYSTM_DIR / 'src' / 'tinystm.c'}")

    if args.compare:
        print_comparison()


if __name__ == "__main__":
    main()
