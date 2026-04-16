#!/usr/bin/env python3
"""
blast_radius.py — JT-Zero cross-layer dependency tracer

Traces a symbol through JT-Zero's 6-layer architecture:
  C++ headers → C++ impl → pybind11 bindings → Python backend → FastAPI → React

Usage:
  python3 blast_radius.py <symbol>
  python3 blast_radius.py position_uncertainty
  python3 blast_radius.py VOResult
  python3 blast_radius.py battery_voltage
  python3 blast_radius.py send_statustext
"""

import sys
import os
import re
import subprocess
from pathlib import Path

# Force UTF-8 output on Windows
if sys.platform == "win32":
    import io
    sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding="utf-8", errors="replace")
    sys.stderr = io.TextIOWrapper(sys.stderr.buffer, encoding="utf-8", errors="replace")

# ── ANSI colors ──────────────────────────────────────────────────────────────
RESET  = "\033[0m"
BOLD   = "\033[1m"
DIM    = "\033[2m"
RED    = "\033[91m"
YELLOW = "\033[93m"
CYAN   = "\033[96m"
GREEN  = "\033[92m"
BLUE   = "\033[94m"
MAGENTA= "\033[95m"

def c(color, text): return f"{color}{text}{RESET}"

# ── Layer definitions ─────────────────────────────────────────────────────────
ROOT = Path(__file__).parent

LAYERS = [
    {
        "name":  "C++ Headers",
        "color": CYAN,
        "dirs":  ["jt-zero/include"],
        "exts":  ["*.h", "*.hpp"],
    },
    {
        "name":  "C++ Implementation",
        "color": BLUE,
        "dirs":  [
            "jt-zero/core",
            "jt-zero/camera",
            "jt-zero/mavlink",
            "jt-zero/sensors",
            "jt-zero/drivers",
            "jt-zero/simulator",
        ],
        "exts":  ["*.cpp", "*.h"],
        "exclude": ["python_bindings.cpp"],
    },
    {
        "name":  "pybind11 Bindings",
        "color": MAGENTA,
        "dirs":  ["jt-zero/api"],
        "exts":  ["*.cpp"],
        "note":  "Cross-layer bridge: C++ symbol → Python export name",
    },
    {
        "name":  "Python Bridge",
        "color": YELLOW,
        "dirs":  ["backend"],
        "exts":  ["*.py"],
        "exclude": ["server.py"],
    },
    {
        "name":  "FastAPI Server",
        "color": GREEN,
        "dirs":  ["backend"],
        "files": ["server.py"],
    },
    {
        "name":  "React Frontend",
        "color": RED,
        "dirs":  ["frontend/src"],
        "exts":  ["*.js", "*.jsx", "*.ts", "*.tsx"],
    },
]

# ── Pure-Python file search (portable, works on Windows) ─────────────────────
def search_in_file(filepath, pattern):
    """Search compiled regex in a file. Returns list of (line_no_str, line)."""
    results = []
    try:
        with open(filepath, encoding="utf-8", errors="replace") as f:
            for i, line in enumerate(f, 1):
                if pattern.search(line):
                    results.append((str(i), line.rstrip()))
    except OSError:
        pass
    return results


def collect_files(layer):
    """Collect all candidate file paths for a layer."""
    files = []

    if "files" in layer:
        for d in layer["dirs"]:
            for fname in layer["files"]:
                p = ROOT / d / fname
                if p.exists():
                    files.append(p)
    else:
        exts = set(e.lstrip("*") for e in layer.get("exts", []))
        for d in layer["dirs"]:
            p = ROOT / d
            if not p.exists():
                continue
            for fp in p.rglob("*"):
                if fp.is_file() and (not exts or fp.suffix in exts):
                    files.append(fp)

    # Apply exclusions
    excludes = layer.get("exclude", [])
    if excludes:
        files = [f for f in files if not any(ex in f.name for ex in excludes)]

    return files


def grep_layer(symbol, layer):
    """Search symbol across a layer's files. Returns (file_str, line_no, content) tuples."""
    # For pybind11 bindings: also match symbol as suffix (e.g. position_uncertainty
    # inside vo_position_uncertainty) — pybind11 often adds layer-prefix to C++ field names
    if layer["name"] == "pybind11 Bindings":
        pattern = re.compile(rf"(?:_|\b){re.escape(symbol)}\b")
    else:
        pattern = re.compile(rf"\b{re.escape(symbol)}\b")

    hits = []
    for fp in collect_files(layer):
        for line_no, content in search_in_file(fp, pattern):
            hits.append((str(fp), line_no, content))
    return hits


# ── pybind11 export name detection ───────────────────────────────────────────
def find_pybind_exports(symbol):
    """
    In pybind11 bindings, find what Python name a C++ symbol is exported as.
    Pattern: "python_name"_a = cpp_expr_containing_symbol
    Returns list of (python_export_name, file, line_no, line)
    """
    bindings_layer = {
        "name": "pybind11 Bindings",
        "dirs": ["jt-zero/api"],
        "exts": ["*.cpp"],
    }
    pattern = re.compile(rf"\b{re.escape(symbol)}\b")
    exports = []

    # Also match symbol as suffix (e.g. _position_uncertainty inside vo_position_uncertainty)
    pattern_suffix = re.compile(rf"(?:_|\b){re.escape(symbol)}\b")

    for fp in collect_files(bindings_layer):
        for line_no, content in search_in_file(fp, pattern_suffix):
            m = re.search(r'"(\w+)"_a\s*=', content)
            if m:
                exports.append((m.group(1), str(fp), line_no, content.strip()))

    return exports


# ── Alias expansion ───────────────────────────────────────────────────────────
def build_aliases(symbol, pybind_exports):
    """
    Build a set of names to search in downstream layers.
    pybind11 may rename: position_uncertainty → vo_position_uncertainty
    """
    aliases = {symbol}
    for (export_name, _, _, _) in pybind_exports:
        aliases.add(export_name)
    return aliases


# ── Output helpers ────────────────────────────────────────────────────────────
def shorten_path(path_str):
    """Make path relative to project root for readability."""
    try:
        return str(Path(path_str).relative_to(ROOT))
    except ValueError:
        return path_str


def print_hits(hits, color, indent="  "):
    if not hits:
        print(f"{indent}{DIM}(no matches){RESET}")
        return

    # Group by file
    by_file = {}
    for (file_, line_no, content) in hits:
        short = shorten_path(file_)
        by_file.setdefault(short, []).append((line_no, content.strip()))

    for filepath, lines in sorted(by_file.items()):
        print(f"{indent}{c(BOLD, filepath)}")
        for (line_no, content) in lines:
            # Highlight the symbol in the line (simple, no regex)
            print(f"{indent}  {c(DIM, line_no + ':')} {content}")


def print_layer_header(layer, aliases=None):
    color = layer["color"]
    name  = layer["name"]
    note  = layer.get("note", "")

    alias_str = ""
    if aliases and len(aliases) > 1:
        alias_str = f"  {DIM}(searching: {', '.join(sorted(aliases))}){RESET}"

    bar = "─" * (50 - len(name))
    print(f"\n{c(color, c(BOLD, f'▶ {name}'))} {c(DIM, bar)}{alias_str}")
    if note:
        print(f"  {c(DIM, note)}")


# ── Main ──────────────────────────────────────────────────────────────────────
def main():
    if len(sys.argv) < 2:
        print(f"{BOLD}Usage:{RESET} python3 blast_radius.py <symbol>")
        print()
        print("Examples:")
        print("  python3 blast_radius.py position_uncertainty")
        print("  python3 blast_radius.py VOResult")
        print("  python3 blast_radius.py battery_voltage")
        print("  python3 blast_radius.py send_statustext")
        print("  python3 blast_radius.py gps_warn_tick")
        sys.exit(1)

    symbol = sys.argv[1]

    print()
    print(c(BOLD, f"  BLAST RADIUS: {c(CYAN, symbol)}"))
    print(c(DIM, f"  JT-Zero cross-layer dependency trace"))
    print(c(DIM, "  " + "─" * 48))

    total_hits = 0
    aliases = {symbol}  # may expand after pybind layer

    for i, layer in enumerate(LAYERS):
        # After pybind11 layer (index 2), use expanded aliases
        search_symbols = aliases if i > 2 else {symbol}

        all_hits = []
        for sym in search_symbols:
            hits = grep_layer(sym, layer)
            all_hits.extend(hits)

        # Deduplicate by (file, line_no)
        seen = set()
        unique_hits = []
        for h in all_hits:
            key = (h[0], h[1])
            if key not in seen:
                seen.add(key)
                unique_hits.append(h)

        print_layer_header(layer, search_symbols if len(search_symbols) > 1 else None)
        print_hits(unique_hits, layer["color"])
        total_hits += len(unique_hits)

        # ── Special: after pybind layer, detect export aliases ──
        if layer["name"] == "pybind11 Bindings":
            exports = find_pybind_exports(symbol)
            if exports:
                new_names = [e[0] for e in exports if e[0] != symbol]
                if new_names:
                    print(f"\n  {c(MAGENTA, c(BOLD, 'Export aliases detected:'))}")
                    for (exp_name, file_, line_no, content) in exports:
                        short = shorten_path(file_)
                        arrow = f"{c(DIM, symbol)} → {c(BOLD, exp_name)}"
                        print(f"    {arrow}  {c(DIM, short + ':' + line_no)}")
                    aliases = build_aliases(symbol, exports)

    # ── Summary ──
    print()
    print(c(DIM, "  " + "─" * 48))
    if total_hits == 0:
        print(f"  {c(YELLOW, 'No matches found.')} Check spelling or try a substring.")
    else:
        print(f"  {c(BOLD, str(total_hits))} references across {c(BOLD, str(len(LAYERS)))} layers")
    print()


if __name__ == "__main__":
    main()
