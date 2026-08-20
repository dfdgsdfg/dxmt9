#!/usr/bin/env python3
"""Audit the dxmt9c_* bridge wire-symbol set against the machine-readable
classification block in specs/backend/producer-concurrency/spec.md §3.

The wire surface is defined by R-BACK-43.1 / spec.md §3 as:
  - every `extern "C"` *definition* of a `dxmt9c_*` symbol in the five
    src/d3d9/device_c_bridge_*.cpp forwarder files, plus
  - any other `extern "C"` `dxmt9c_*` definition found anywhere else under
    src/ (so a future direct-entry addition outside the bridge files is
    caught rather than silently missed).

The classification block is a fenced ```classification code block directly
below the family table in spec.md §3, with one line per symbol:

    dxmt9c_factory_adapter_count app-return-value

This script fails when:
  - a wire symbol has no entry in the block,
  - a block entry names a symbol that is not a wire symbol,
  - a block entry's class is outside the closed taxonomy,
  - a symbol is listed more than once in the block.

Exit code 0 on success, 1 on any drift.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

WIRE_FILES = [
    "src/d3d9/device_c_bridge_factory.cpp",
    "src/d3d9/device_c_bridge_device_state_draw.cpp",
    "src/d3d9/device_c_bridge_resources.cpp",
    "src/d3d9/device_c_bridge_shader_vdecl.cpp",
    "src/d3d9/device_c_bridge_swapchain_query_stateblock.cpp",
]

SPEC_PATH = "specs/backend/producer-concurrency/spec.md"

CLOSED_CLASSES = {
    "app-return-value",
    "visibility-wait",
    "ordering-fence",
    "state-mutation-ack",
    "record-only",
}

# Matches an `extern "C"` function *definition* (ends in `{`, not `;`), for a
# dxmt9c_* symbol. Parameter lists in this codebase are simple (no nested
# parens), so a non-greedy match up to the first `)` before `{` is safe.
DEFINITION_RE = re.compile(
    r'extern\s*"C"\s+[^;{}]*?\bdxmt9c_([A-Za-z0-9_]+)\s*\([^;{}]*\)\s*\{',
    re.DOTALL,
)

# Also catch definitions whose return type/name split across a `\n` before
# the symbol, e.g.:
#   extern "C" int32_t
#   dxmt9c_device_finish_render_tape_present_source_capture(
# The DEFINITION_RE above already tolerates that via DOTALL + [^;{}]*?.

CLASSIFICATION_BLOCK_RE = re.compile(
    r"```classification\n(.*?)```", re.DOTALL
)


def is_macro_renamed_tu(text: str) -> bool:
    """True when this translation unit includes device_c_provider_macros.hpp
    (directly or via device_c_provider.hpp) WITHOUT also pulling in
    device_c_provider_undefs.hpp (directly or via device_c_provider_api.hpp).

    In such a TU, every `dxmt9c_*` token in the source is preprocessor-renamed
    to `dxmt9p_*` for the whole file (see device_c_provider_macros.hpp /
    device_c_provider_api.hpp), so a textual `extern "C" ... dxmt9c_foo(...)`
    found there does NOT produce a real `dxmt9c_foo` wire symbol -- it compiles
    to `dxmt9p_foo`. The five bridge files include device_c_provider_api.hpp,
    which chains macros.hpp then undefs.hpp, so they are unaffected.
    """
    has_macros = (
        'include "device_c_provider_macros.hpp"' in text
        or 'include "device_c_provider.hpp"' in text
    )
    has_undefs = (
        'include "device_c_provider_undefs.hpp"' in text
        or 'include "device_c_provider_api.hpp"' in text
    )
    return has_macros and not has_undefs


def find_wire_symbols() -> dict[str, list[str]]:
    """Return {symbol_name: [file:line, ...]} for every dxmt9c_* extern "C"
    definition found anywhere under src/, excluding translation units where
    the provider macro rename is active for the whole file (see
    is_macro_renamed_tu) -- those files' `dxmt9c_*` source tokens actually
    compile to `dxmt9p_*` and are not real wire symbols."""
    symbols: dict[str, list[str]] = {}
    for path in sorted((REPO_ROOT / "src").rglob("*.cpp")):
        text = path.read_text(encoding="utf-8", errors="replace")
        if is_macro_renamed_tu(text):
            continue
        for m in DEFINITION_RE.finditer(text):
            name = "dxmt9c_" + m.group(1)
            line_no = text.count("\n", 0, m.start()) + 1
            rel = path.relative_to(REPO_ROOT)
            symbols.setdefault(name, []).append(f"{rel}:{line_no}")
    return symbols


def find_wire_symbols_in(files: list[str]) -> set[str]:
    out: set[str] = set()
    for rel in files:
        path = REPO_ROOT / rel
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in DEFINITION_RE.finditer(text):
            out.add("dxmt9c_" + m.group(1))
    return out


def parse_classification_block(spec_text: str) -> tuple[list[tuple[str, str]], list[str]]:
    """Return (entries, errors). entries is a list of (symbol, class) in file
    order (duplicates included, so the caller can detect them)."""
    m = CLASSIFICATION_BLOCK_RE.search(spec_text)
    if not m:
        return [], [
            f"no fenced ```classification block found in {SPEC_PATH}"
        ]
    entries: list[tuple[str, str]] = []
    errors: list[str] = []
    for lineno, raw in enumerate(m.group(1).splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = line.split()
        if len(parts) != 2:
            errors.append(
                f"classification block line {lineno!r} is not "
                f"'<symbol> <class>': {raw!r}"
            )
            continue
        entries.append((parts[0], parts[1]))
    return entries, errors


def main() -> int:
    errors: list[str] = []

    bridge_file_symbols = find_wire_symbols_in(WIRE_FILES)
    all_symbols = find_wire_symbols()

    # Wire symbols not defined in one of the five bridge files are a drift
    # signal: either a new direct-entry dxmt9c_* symbol was added outside the
    # forwarder files, or one of the forwarder files was renamed.
    stray_files: dict[str, list[str]] = {}
    for name, locs in all_symbols.items():
        if name not in bridge_file_symbols:
            stray_files[name] = locs

    wire_symbols = set(all_symbols.keys())

    spec_path = REPO_ROOT / SPEC_PATH
    spec_text = spec_path.read_text(encoding="utf-8")
    entries, block_errors = parse_classification_block(spec_text)
    errors.extend(block_errors)

    seen: dict[str, int] = {}
    block_symbols: set[str] = set()
    for symbol, klass in entries:
        seen[symbol] = seen.get(symbol, 0) + 1
        block_symbols.add(symbol)
        if klass not in CLOSED_CLASSES:
            errors.append(
                f"{symbol}: class {klass!r} is not in the closed taxonomy "
                f"{sorted(CLOSED_CLASSES)}"
            )

    for symbol, count in seen.items():
        if count > 1:
            errors.append(f"{symbol}: listed {count} times in the classification block")

    missing = sorted(wire_symbols - block_symbols)
    for symbol in missing:
        locs = ", ".join(all_symbols[symbol])
        errors.append(f"{symbol}: wire symbol with no classification block entry ({locs})")

    stale = sorted(block_symbols - wire_symbols)
    for symbol in stale:
        errors.append(f"{symbol}: classification block entry names a nonexistent wire symbol")

    if stray_files:
        note = ", ".join(
            f"{name} ({', '.join(locs)})" for name, locs in sorted(stray_files.items())
        )
        print(
            "[audit-bridge-entry-classification] note: dxmt9c_* definitions "
            f"found outside the five bridge files (treated as wire symbols "
            f"too): {note}",
            file=sys.stderr,
        )

    if errors:
        print("[audit-bridge-entry-classification] FAIL", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        print(
            f"\n  wire symbols found: {len(wire_symbols)}; "
            f"classification block entries: {len(entries)}",
            file=sys.stderr,
        )
        return 1

    print(
        "[audit-bridge-entry-classification] OK: "
        f"{len(wire_symbols)} wire symbols, all classified.",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
