#!/usr/bin/env python3
"""Audit dxmt9_perf_counters.hpp for declared but never-called counter funcs.

Closes the silent-miss regression that the table audit cannot see: a
`count*()` declaration with no production call site under `src/`. Such a
function would link, increment its atomic when called from tests, but
never run in real workloads — counter would always read 0 and the
spec-required signal would be dead.

Dual to scripts/check/audit_perf_counter_table.py:

  table audit     — every Counters field MUST appear in kCounterTable
  callsite audit  — every count*() declaration MUST have >=1 callsite
                    in src/ outside every perf implementation surface

Parsing is text-based for the same reason as the table audit (no clang
dependency, deterministic Meson test).
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
HEADER = REPO_ROOT / "src" / "dxmt9" / "dxmt9_perf_counters.hpp"
SRC_ROOT = REPO_ROOT / "src"

# `void countXxx(` at line start; tolerate trailing args on the same or
# subsequent lines (we only need the function name).
DECL_RE = re.compile(
    r"^void\s+(count[A-Za-z0-9_]*)\s*\(",
    re.MULTILINE,
)

# Perf implementation TUs and their public/private headers are definitions,
# report plumbing, cold output, or test snapshots. None is production callsite
# evidence for the public count* surface.
PERF_DIR = REPO_ROOT / "src" / "dxmt9"


def is_perf_implementation_surface(path: Path) -> bool:
    return path.parent == PERF_DIR and (
        path.name.startswith("dxmt9_perf_counters")
        and path.suffix in {".cpp", ".hpp", ".h"}
    )

# Source extensions to scan for callsites. dxmt9 uses .cpp, .mm (ObjC++),
# and .hpp/.h. We include headers so an inline call from a header counts.
SOURCE_GLOBS = ("**/*.cpp", "**/*.mm", "**/*.hpp", "**/*.h")


def parse_declarations(header_text: str) -> list[str]:
    """Return all `count*` function names declared in the header.

    The header lives entirely inside `namespace dxmt9::perf` so we do
    not need to bound the regex by namespace — every `void count*(` is
    in the public surface.
    """
    names = DECL_RE.findall(header_text)
    # Preserve declaration order, drop dups (overloads share a name).
    seen: set[str] = set()
    ordered: list[str] = []
    for name in names:
        if name in seen:
            continue
        seen.add(name)
        ordered.append(name)
    return ordered


def collect_source_files() -> list[Path]:
    files: list[Path] = []
    for glob in SOURCE_GLOBS:
        for path in SRC_ROOT.glob(glob):
            if is_perf_implementation_surface(path):
                continue
            files.append(path)
    return files


LINE_COMMENT_RE = re.compile(r"//.*$", re.MULTILINE)


def collect_callsite_counts(names: list[str], files: list[Path]) -> dict[str, int]:
    """Count source files that reference each declared counter name.

    Matches both direct calls (`name(...)`) and function-pointer passes
    (`PerfScope scope(perf::name);`) since the latter is a real callsite
    surface — the RAII wrapper invokes the pointer on destruction.

    Line comments are stripped before matching so a stale `// countX
    used to ...` does not count as wired. Block comments are rare in
    dxmt9 sources and the false-positive risk from leaving them in is
    bounded by the meson test running on every change.
    Read each source file once. The former declaration-major scan reopened and
    decoded every file for every counter, which made this audit exceed Meson's
    30-second test timeout as the counter surface grew.
    """
    counts = {name: 0 for name in names}
    pattern = re.compile(
        r"\b(?:" + "|".join(re.escape(name) for name in names) + r")\b"
    )
    for path in files:
        try:
            text = path.read_text(encoding="utf-8")
        except (OSError, UnicodeDecodeError):
            continue
        stripped = LINE_COMMENT_RE.sub("", text)
        for name in set(pattern.findall(stripped)):
            counts[name] += 1
    return counts


def main() -> int:
    if not HEADER.exists():
        print(f"audit: header not found: {HEADER}", file=sys.stderr)
        return 2

    declarations = parse_declarations(HEADER.read_text(encoding="utf-8"))
    if not declarations:
        print("audit: no `count*` declarations parsed from header", file=sys.stderr)
        return 2

    source_files = collect_source_files()

    callsite_counts = collect_callsite_counts(declarations, source_files)
    unwired = [name for name in declarations if callsite_counts[name] == 0]
    total_callsites = sum(callsite_counts.values())

    if unwired:
        print(
            "audit: the following count*() functions are declared but have\n"
            "       no call site under src/ (outside perf implementation files):",
            file=sys.stderr,
        )
        for name in unwired:
            print(f"  {name}", file=sys.stderr)
        print(
            f"\naudit: {len(unwired)} of {len(declarations)} declared count*()\n"
            f"functions are unreachable from production code. Each declared\n"
            f"counter needs both:\n"
            f"  1) the void count*(...) declaration, AND\n"
            f"  2) at least one call site under src/ that drives it from a\n"
            f"     real workload (encoder, queue, presenter, replay, ...).\n"
            f"Without (2), the counter is dead — its value is always 0 and\n"
            f"the spec metric it represents is silently lost.",
            file=sys.stderr,
        )
        return 1

    print(
        f"audit: ok — {len(declarations)} declared count*() functions all have\n"
        f"        >=1 call site under src/ ({total_callsites} call sites total, "
        f"{len(source_files)} files scanned)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
