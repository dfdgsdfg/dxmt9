#!/usr/bin/env python3
"""Audit perf-counter storage, report-table coverage, and writer evidence.

The storage type and ordered report table intentionally live in separate
translation units. This audit keeps both directions honest:

1. Every Counters field must be referenced by the cumulative report table.
2. Every referenced field must have concrete mutation evidence in a perf
   implementation TU. Multiple report rows do not count as a writer.

Parsing is text-based so the check stays deterministic and needs no clang AST.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
PERF_DIR = REPO_ROOT / "src" / "dxmt9"
INTERNAL_HEADER = PERF_DIR / "dxmt9_perf_counters_internal.hpp"
REPORT_SOURCE = PERF_DIR / "dxmt9_perf_counters_report.cpp"
IMPLEMENTATION_GLOB = "dxmt9_perf_counters*.cpp"

ATOMIC_FIELD_RE = re.compile(
    r"^\s*std::atomic<\s*std::uint64_t\s*>\s+([A-Za-z_][A-Za-z0-9_]*)\s*\{",
    re.MULTILINE,
)
RING_FIELD_RE = re.compile(
    r"^\s*PercentileRing\s+([A-Za-z_][A-Za-z0-9_]*)\s*;",
    re.MULTILINE,
)
COUNTER_REF_RE = re.compile(r"&Counters::([A-Za-z_][A-Za-z0-9_]*)")

# These helpers are the mutation funnels used by the hot counter bodies.
MUTATION_CALL_RE = re.compile(
    r"\b(?:add|addEnabledNonZero|addBatchMissUniformBuild|updateMax|updateMin|"
    r"recordCpuTime|store|recordRing|updatePeakEnabled|addWaitBucket|countIf|"
    r"addIfCategory|addIfAllCategories)"
    r"\s*\(([^;{}]*?)\);",
    re.DOTALL,
)
DIRECT_MUTATION_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\.\s*"
    r"(?:store|fetch_add|exchange|compare_exchange_weak|compare_exchange_strong)\s*\("
)
RETURN_FIELD_RE = re.compile(
    r"\breturn\s+[A-Za-z_][A-Za-z0-9_]*\.([A-Za-z_][A-Za-z0-9_]*)\s*;"
)
ADDRESS_ASSIGNMENT_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*=\s*"
    r"&[A-Za-z_][A-Za-z0-9_]*\.([A-Za-z_][A-Za-z0-9_]*)\b"
)
ADDRESS_FIELD_RE = re.compile(
    r"&[A-Za-z_][A-Za-z0-9_]*\.([A-Za-z_][A-Za-z0-9_]*)\b"
)
POINTER_ARRAY_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\[\s*\]\s*=\s*\{(.*?)\};",
    re.DOTALL,
)
FUNCTION_START_RE = re.compile(
    r"\b([A-Za-z_][A-Za-z0-9_]*)\s*\([^;{}]*\)\s*"
    r"(?:const\s*)?(?:noexcept\s*)?\{",
    re.DOTALL,
)
WORD_RE = re.compile(r"\b[A-Za-z_][A-Za-z0-9_]*\b")
CONTROL_KEYWORDS = {"catch", "for", "if", "switch", "while"}
MUTATION_METHODS = (
    "store|fetch_add|exchange|compare_exchange_weak|compare_exchange_strong"
)


def strip_comments_and_literals(text: str) -> str:
    """Mask C++ comments and literals while preserving layout and newlines."""
    out = list(text)
    size = len(text)
    index = 0

    def mask(start: int, end: int) -> None:
        for position in range(start, end):
            if out[position] != "\n":
                out[position] = " "

    while index < size:
        if text.startswith("//", index):
            end = text.find("\n", index + 2)
            if end < 0:
                end = size
            mask(index, end)
            index = end
            continue
        if text.startswith("/*", index):
            end = text.find("*/", index + 2)
            end = size if end < 0 else end + 2
            mask(index, end)
            index = end
            continue
        if text.startswith('R"', index):
            open_paren = text.find("(", index + 2, min(size, index + 19))
            if open_paren >= 0:
                delimiter = text[index + 2 : open_paren]
                terminator = ")" + delimiter + '"'
                end = text.find(terminator, open_paren + 1)
                end = size if end < 0 else end + len(terminator)
                mask(index, end)
                index = end
                continue
        if text[index] in {'"', "'"}:
            quote = text[index]
            start = index
            index += 1
            while index < size:
                if text[index] == "\\":
                    index = min(size, index + 2)
                    continue
                index += 1
                if text[index - 1] == quote:
                    break
            mask(start, index)
            continue
        index += 1
    return "".join(out)


def find_matching_brace(text: str, open_brace: int) -> int | None:
    depth = 0
    for index in range(open_brace, len(text)):
        if text[index] == "{":
            depth += 1
        elif text[index] == "}":
            depth -= 1
            if depth == 0:
                return index
    return None


def parse_function_bodies(text: str) -> list[tuple[str, str]]:
    """Return named C++ function bodies from already-masked source text."""
    functions: list[tuple[str, str]] = []
    for match in FUNCTION_START_RE.finditer(text):
        name = match.group(1)
        if name in CONTROL_KEYWORDS:
            continue
        open_brace = match.end() - 1
        close_brace = find_matching_brace(text, open_brace)
        if close_brace is not None:
            functions.append((name, text[open_brace + 1 : close_brace]))
    return functions


def alias_is_mutated(alias: str, body: str, mutation_calls: list[str]) -> bool:
    escaped = re.escape(alias)
    dereference = re.compile(rf"\*\s*{escaped}\b(?:\s*\[[^\]]+\])?")
    if any(dereference.search(arguments) for arguments in mutation_calls):
        return True
    pointer_method = re.compile(
        rf"\b{escaped}\b(?:\s*\[[^\]]+\])?\s*->\s*"
        rf"(?:{MUTATION_METHODS})\s*\("
    )
    return pointer_method.search(body) is not None


def collect_indirect_writer_evidence(
    text: str,
    fields: set[str],
    mutation_calls: list[str],
) -> set[str]:
    """Find fields selected indirectly and then mutated in the same function."""
    written: set[str] = set()
    functions = parse_function_bodies(text)

    selector_fields: dict[str, set[str]] = {}
    for name, body in functions:
        selected = fields.intersection(RETURN_FIELD_RE.findall(body))
        if selected:
            selector_fields.setdefault(name, set()).update(selected)
    for arguments in mutation_calls:
        for selector, selected in selector_fields.items():
            if re.search(rf"\b{re.escape(selector)}\s*\(", arguments):
                written.update(selected)

    for _, body in functions:
        body_calls = MUTATION_CALL_RE.findall(body)
        alias_fields: dict[str, set[str]] = {}
        for alias, field in ADDRESS_ASSIGNMENT_RE.findall(body):
            if field in fields:
                alias_fields.setdefault(alias, set()).add(field)
        for match in POINTER_ARRAY_RE.finditer(body):
            selected = fields.intersection(ADDRESS_FIELD_RE.findall(match.group(2)))
            if selected:
                alias_fields.setdefault(match.group(1), set()).update(selected)
        for alias, selected in alias_fields.items():
            if alias_is_mutated(alias, body, body_calls):
                written.update(selected)
    return written


def parse_counters_struct(text: str) -> tuple[set[str], set[str]]:
    """Return atomic and PercentileRing fields from the internal header."""
    start = text.find("struct Counters {")
    if start < 0:
        raise ValueError("cannot locate `struct Counters {`")
    end = text.find("\n};", start)
    if end < 0:
        raise ValueError("cannot locate end of `struct Counters`")
    body = text[start:end]
    return set(ATOMIC_FIELD_RE.findall(body)), set(RING_FIELD_RE.findall(body))


def parse_table_references(text: str) -> set[str]:
    return set(COUNTER_REF_RE.findall(text))


def collect_writer_evidence(
    implementation_texts: list[str], fields: set[str]
) -> set[str]:
    """Return fields named by an actual mutation path, never table reads."""
    written: set[str] = set()
    for text in implementation_texts:
        source = strip_comments_and_literals(text)
        mutation_calls = MUTATION_CALL_RE.findall(source)
        for arguments in mutation_calls:
            written.update(fields.intersection(WORD_RE.findall(arguments)))
        written.update(fields.intersection(DIRECT_MUTATION_RE.findall(source)))
        written.update(
            collect_indirect_writer_evidence(source, fields, mutation_calls)
        )
    return written


def audit_texts(
    internal_text: str,
    report_text: str,
    implementation_texts: list[str],
) -> tuple[set[str], set[str], set[str], set[str], set[str]]:
    """Return fields plus missing, unknown-table, and unwritten sets."""
    atomic_fields, ring_fields = parse_counters_struct(internal_text)
    all_fields = atomic_fields | ring_fields
    referenced = parse_table_references(report_text)
    written = collect_writer_evidence(implementation_texts, all_fields)
    missing = all_fields - referenced
    unknown_references = referenced - all_fields
    never_written = (all_fields & referenced) - written
    return atomic_fields, ring_fields, missing, unknown_references, never_written


def main() -> int:
    required = (INTERNAL_HEADER, REPORT_SOURCE)
    for path in required:
        if not path.exists():
            print(f"audit: source not found: {path}", file=sys.stderr)
            return 2

    implementation_paths = sorted(PERF_DIR.glob(IMPLEMENTATION_GLOB))
    try:
        (
            atomic_fields,
            ring_fields,
            missing,
            unknown_references,
            never_written,
        ) = audit_texts(
            INTERNAL_HEADER.read_text(encoding="utf-8"),
            REPORT_SOURCE.read_text(encoding="utf-8"),
            [path.read_text(encoding="utf-8") for path in implementation_paths],
        )
    except ValueError as error:
        print(f"audit: {error}", file=sys.stderr)
        return 2

    if missing:
        print(
            "audit: kCounterTable is missing rows for these Counters fields:",
            file=sys.stderr,
        )
        for name in sorted(missing):
            kind = "PercentileRing" if name in ring_fields else "atomic"
            print(f"  {name}  ({kind})", file=sys.stderr)
        return 1

    if unknown_references:
        print(
            "audit: kCounterTable references fields absent from Counters:",
            file=sys.stderr,
        )
        for name in sorted(unknown_references):
            print(f"  {name}", file=sys.stderr)
        return 1

    if never_written:
        print(
            "audit: these table-backed Counters fields have no mutation evidence\n"
            "       in any dxmt9_perf_counters*.cpp implementation TU:",
            file=sys.stderr,
        )
        for name in sorted(never_written):
            print(f"  {name}", file=sys.stderr)
        print(
            "\nA declaration plus one or more table rows is not writer evidence;\n"
            "restore the recorder or delete the permanently-zero storage/output.",
            file=sys.stderr,
        )
        return 1

    referenced = parse_table_references(REPORT_SOURCE.read_text(encoding="utf-8"))
    print(
        f"audit: ok — {len(atomic_fields)} atomic + {len(ring_fields)} "
        f"PercentileRing fields are table-backed and written "
        f"({len(referenced)} distinct table references across "
        f"{len(implementation_paths)} implementation TUs)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
