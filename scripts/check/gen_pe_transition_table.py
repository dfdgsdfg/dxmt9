#!/usr/bin/env python3
"""Generate/check the TLA table from the canonical recorder transition rows."""

from __future__ import annotations

import argparse
import pathlib
import re


ROW_RE = re.compile(r"^DXMT9_PE_(STATE_WRITE|APPEND)_ROW\((.*)\)\s*$")


def rows(source: pathlib.Path, kind: str) -> list[list[str]]:
    result: list[list[str]] = []
    for line in source.read_text().splitlines():
        match = ROW_RE.match(line.strip())
        if match and match.group(1) == kind:
            result.append([field.strip() for field in match.group(2).split(",")])
    return result


def tla_atom(value: str) -> str:
    if value == "True":
        return "TRUE"
    if value == "False":
        return "FALSE"
    if value == "Any":
        return '"Any"'
    if value == "AnyNotEqualLive":
        return '"AnyNotEqualLive"'
    return f'"{value}"'


def tla_match_atom(value: str) -> str:
    if value in {"True", "False", "Any"}:
        return f'"{value}"'
    return tla_atom(value)


def emit(source: pathlib.Path) -> str:
    state = rows(source, "STATE_WRITE")
    append = rows(source, "APPEND")
    lines = [
        "---- MODULE PeRecorderTransitionTable ----",
        "(***************************************************************************",
        " * Generated from src/d3d9/d3d9_pe_transition_table.inc.  Do not hand-edit.",
        " ***************************************************************************)",
        "StateWriteTable == <<",
    ]
    for row in state:
        if len(row) != 10:
            raise SystemExit(f"bad state row ({len(row)} fields): {row}")
        fields = [
            ("phase", row[0]),
            ("origin", row[1]),
            ("liveEquals", row[2]),
            ("pendingContains", row[3]),
            ("kind", row[4]),
            ("writeLive", row[5]),
            ("writePending", row[6]),
            ("writeRecorded", row[7]),
            ("directOrderedCall", row[8]),
            ("semanticTransition", row[9]),
        ]
        lines.append("  [" + ", ".join(
            f"{name} |-> {tla_match_atom(value) if name in {'liveEquals', 'pendingContains', 'semanticTransition'} else tla_atom(value)}"
            for name, value in fields) + "],")
    lines[-1] = lines[-1].rstrip(",")
    lines += [
        ">>",
        "AppendTable == <<",
    ]
    for row in append:
        if len(row) != 8:
            raise SystemExit(f"bad append row ({len(row)} fields): {row}")
        fields = [
            ("phase", row[0]),
            ("appendSucceeded", row[1]),
            ("explicitDiscard", row[2]),
            ("next", row[3]),
            ("consumeRepresentedPending", row[4]),
            ("retainPreparedProjection", row[5]),
            ("recordDurable", row[6]),
            ("valid", row[7]),
        ]
        lines.append("  [" + ", ".join(
            f"{name} |-> {tla_match_atom(value) if name in {'appendSucceeded', 'explicitDiscard'} else tla_atom(value)}"
            for name, value in fields) + "],")
    lines[-1] = lines[-1].rstrip(",")
    lines += [">>", "====", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    source = args.source or root / "src/d3d9/d3d9_pe_transition_table.inc"
    output = args.output or root / "specs/verification/tla/PeRecorderTransitionTable.tla"
    expected = emit(source)
    if args.check:
        if not output.exists() or output.read_text() != expected:
            print(f"{output} is stale; regenerate from {source}")
            return 1
        return 0
    output.write_text(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
