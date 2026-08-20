#!/usr/bin/env python3
"""Audit R-BACK-43.4/43.5 thread-ownership declarations for drift, in both
directions, against the field-level ownership table in
specs/backend/producer-concurrency/spec.md §2.

R-BACK-43.4 requires every piece of state reachable from more than one of
{producer, replay worker, encode thread, completion path} to carry exactly one
ownership class from the closed taxonomy (`producer-owned`, `worker-owned`,
`owner-published`, `arena-protected`, `queue-shared`,
`immutable-after-init`), declared in a comment adjacent to the owning
struct/field/module. `include/dxmt9/thread_ownership.hpp`'s
`ThreadOwnershipToken` is the concrete debug-assert mechanism for the two
classes that carry no synchronization (`producer-owned`, `worker-owned`); this
script does not (and cannot) mechanically verify the other four classes, which
are documentation-only per R-BACK-43.4 -- it only checks that a
`ThreadOwnershipToken` member declaration is never left unclassified, and that
spec.md §2's declaration-site table has not drifted from the code.

Two checks:

  1. CODE -> COMMENT: every `ThreadOwnershipToken` member declaration
     anywhere under src/ must have a comment naming one of the six closed
     R-BACK-43.4 classes (in backticks, e.g. `` `producer-owned` ``) within a
     bounded window of lines above the declaration. A new multi-actor
     `ThreadOwnershipToken` field that ships without a class comment fails
     here.

  2. SPEC -> CODE: every file path named in the "Declaration site" column of
     spec.md §2's second table ("State | Declaration site | R-BACK-43.5
     assert") must still exist and must still contain either a
     `ThreadOwnershipToken` declaration or at least one of the six class names
     in backticks. A file that was renamed, or whose ownership comment was
     deleted, without updating spec.md fails here.

This is deliberately regex-level and simple, mirroring
scripts/check/audit_bridge_entry_classification.py's shape and error-reporting
style rather than parsing C++ or full Markdown.

Exit code 0 on success, 1 on any drift.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]

SPEC_PATH = "specs/backend/producer-concurrency/spec.md"

CLOSED_CLASSES = [
    "producer-owned",
    "worker-owned",
    "owner-published",
    "arena-protected",
    "queue-shared",
    "immutable-after-init",
]

# Matches a `core::ThreadOwnershipToken` (any namespace-qualification prefix)
# member declaration, e.g.:
#   [[no_unique_address]] core::ThreadOwnershipToken producerOwnership_{
#   dxmt9::core::ThreadOwnershipToken recorderOwnership_{};
TOKEN_DECL_RE = re.compile(
    r'(?:[A-Za-z_][A-Za-z0-9_]*::)*ThreadOwnershipToken\s+([A-Za-z_][A-Za-z0-9_]*)\s*[{;]'
)

# A class name mentioned in backticks anywhere, e.g. `producer-owned`.
CLASS_MENTION_RE = re.compile(
    r'`(' + '|'.join(re.escape(c) for c in CLOSED_CLASSES) + r')`'
)

# How many lines above a ThreadOwnershipToken declaration to search for a
# class-naming comment. The three current sites (dxmt9_command_queue.hpp,
# d3d9_pe_device.cpp, dxmt9_resource_pool.hpp) all carry their class mention
# within a handful of lines above the declaration; 25 gives headroom for a
# multi-paragraph explanatory comment without being unbounded.
CONTEXT_LINES_ABOVE = 25

# The declaration-site table in spec.md §2 lists file paths in backticks,
# e.g. `src/dxmt9/dxmt9_resource_pool.hpp` or `dxmt9_pe_chunk_builder.hpp`
# (bare basenames also appear, alongside a sibling path in the same cell that
# carries the directory). Only accept paths that look like a real source file
# (contain a slash and end in .hpp/.cpp/.h) so we do not chase symbol-only
# backtick spans like `struct Pool`.
FILE_PATH_RE = re.compile(r'`([\w./-]+\.(?:hpp|cpp|h))`')

SECTION2_RE = re.compile(
    r'##\s*2\.\s*Established ownership assignments\n(.*?)\n##\s*3\.',
    re.DOTALL,
)

# Within section 2, the second markdown table (the declaration-site table) is
# the one whose header row contains "Declaration site". Grab from that header
# through the next blank line (tables end at the first line that does not
# start with '|').
DECL_TABLE_RE = re.compile(
    r'(\|[^\n]*Declaration site[^\n]*\n\|[-| ]+\n(?:\|[^\n]*\n)+)'
)


def find_token_declarations() -> list[tuple[Path, int, str]]:
    """Return (file, line_no, member_name) for every ThreadOwnershipToken
    member declaration under src/, excluding the definition header itself."""
    out: list[tuple[Path, int, str]] = []
    for path in sorted((REPO_ROOT / "src").rglob("*")):
        if path.suffix not in (".hpp", ".cpp", ".mm", ".h"):
            continue
        if not path.is_file():
            continue
        text = path.read_text(encoding="utf-8", errors="replace")
        for m in TOKEN_DECL_RE.finditer(text):
            line_no = text.count("\n", 0, m.start()) + 1
            out.append((path, line_no, m.group(1)))
    return out


def has_class_comment_above(path: Path, decl_line: int) -> bool:
    text = path.read_text(encoding="utf-8", errors="replace")
    lines = text.splitlines()
    start = max(0, decl_line - 1 - CONTEXT_LINES_ABOVE)
    window = "\n".join(lines[start:decl_line])
    return CLASS_MENTION_RE.search(window) is not None


def check_code_to_comment() -> list[str]:
    errors: list[str] = []
    for path, line_no, member in find_token_declarations():
        if not has_class_comment_above(path, line_no):
            rel = path.relative_to(REPO_ROOT)
            errors.append(
                f"{rel}:{line_no}: ThreadOwnershipToken member '{member}' has "
                f"no R-BACK-43.4 class comment (one of "
                f"{', '.join('`' + c + '`' for c in CLOSED_CLASSES)}) within "
                f"{CONTEXT_LINES_ABOVE} lines above the declaration"
            )
    return errors


def parse_declaration_site_files(spec_text: str) -> list[tuple[str, list[str]]]:
    """Return [(state_cell, [file_path, ...]), ...] for each data row of
    spec.md §2's declaration-site table."""
    section2 = SECTION2_RE.search(spec_text)
    if section2 is None:
        return []
    table_m = DECL_TABLE_RE.search(section2.group(1))
    if table_m is None:
        return []
    rows = table_m.group(1).splitlines()
    out: list[tuple[str, list[str]]] = []
    for row in rows[2:]:  # skip header + separator
        if not row.strip().startswith("|"):
            continue
        cells = [c.strip() for c in row.strip().strip("|").split("|")]
        if len(cells) < 2:
            continue
        state_cell = cells[0]
        decl_cell = cells[1]
        files = FILE_PATH_RE.findall(decl_cell)
        out.append((state_cell, files))
    return out


def resolve_file(name: str) -> Path | None:
    """A table cell may name a bare basename (e.g. `d3d9_pe_chunk_builder.hpp`)
    or a repo-relative path (e.g. `src/dxmt9/dxmt9_resource_pool.hpp`). Accept
    either: try repo-relative first, then search src/ for a unique basename
    match."""
    direct = REPO_ROOT / name
    if direct.is_file():
        return direct
    if "/" in name:
        return None
    matches = list((REPO_ROOT / "src").rglob(name))
    if len(matches) == 1:
        return matches[0]
    return None


def check_spec_to_code(spec_text: str) -> list[str]:
    errors: list[str] = []
    rows = parse_declaration_site_files(spec_text)
    if not rows:
        errors.append(
            f"no declaration-site table (header row containing "
            f"'Declaration site') found under §2 of {SPEC_PATH}"
        )
        return errors

    seen_any_file = False
    for state_cell, files in rows:
        for name in files:
            seen_any_file = True
            resolved = resolve_file(name)
            if resolved is None:
                errors.append(
                    f"§2 row {state_cell!r}: declaration-site file "
                    f"'{name}' does not exist (renamed or removed without "
                    f"updating {SPEC_PATH})"
                )
                continue
            text = resolved.read_text(encoding="utf-8", errors="replace")
            if "ThreadOwnershipToken" not in text and not CLASS_MENTION_RE.search(text):
                rel = resolved.relative_to(REPO_ROOT)
                errors.append(
                    f"§2 row {state_cell!r}: '{rel}' no longer contains a "
                    f"ThreadOwnershipToken declaration or any R-BACK-43.4 "
                    f"class comment ({', '.join('`' + c + '`' for c in CLOSED_CLASSES)}) "
                    f"-- {SPEC_PATH} §2 is stale"
                )
    if not seen_any_file:
        errors.append(
            f"declaration-site table under §2 of {SPEC_PATH} named no "
            f"recognizable file paths (expected backtick-quoted "
            f".hpp/.cpp/.h names)"
        )
    return errors


def main() -> int:
    errors: list[str] = []

    errors.extend(check_code_to_comment())

    spec_path = REPO_ROOT / SPEC_PATH
    spec_text = spec_path.read_text(encoding="utf-8")
    errors.extend(check_spec_to_code(spec_text))

    if errors:
        print("[audit-thread-ownership-declarations] FAIL", file=sys.stderr)
        for e in errors:
            print(f"  - {e}", file=sys.stderr)
        return 1

    token_count = len(find_token_declarations())
    print(
        "[audit-thread-ownership-declarations] OK: "
        f"{token_count} ThreadOwnershipToken declaration(s) classified, "
        f"{SPEC_PATH} §2 declaration-site table matches code.",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
