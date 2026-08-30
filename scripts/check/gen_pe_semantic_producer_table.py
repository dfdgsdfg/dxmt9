#!/usr/bin/env python3
"""Generate/check the TLA family table from the production PE token table."""

from __future__ import annotations

import argparse
import pathlib
import re


ROW_RE = re.compile(
    r"^X\(([A-Za-z0-9_]+),\s*(D9C_COMMAND_RECORD_[A-Z0-9_]+),\s*"
    r"([A-Za-z0-9_]+)\)"
)


def rows(source: pathlib.Path) -> list[tuple[str, str, str]]:
    result: list[tuple[str, str, str]] = []
    inside = False
    for raw in source.read_text().splitlines():
        line = raw.strip().rstrip("\\").rstrip()
        if line.startswith("#define DXMT9_PE_SEMANTIC_PRODUCER_TABLE"):
            inside = True
            continue
        if inside and not raw.rstrip().endswith("\\"):
            match = ROW_RE.match(line)
            if match:
                result.append(match.groups())
            break
        if inside:
            match = ROW_RE.match(line)
            if match:
                result.append(match.groups())
    if not result:
        raise SystemExit(f"no semantic producer rows found in {source}")
    names = [row[0] for row in result]
    records = [row[1] for row in result]
    if len(names) != len(set(names)) or len(records) != len(set(records)):
        raise SystemExit("semantic producer names and record types must be unique")
    return result


def emit(source: pathlib.Path) -> str:
    producer_rows = rows(source)
    lines = [
        "---- MODULE PeRecorderSemanticProjectionTable ----",
        "(***************************************************************************",
        " * Generated from DXMT9_PE_SEMANTIC_PRODUCER_TABLE in",
        " * src/d3d9/d3d9_pe_semantic_tokens.hpp. Do not hand-edit.",
        " ***************************************************************************)",
        "EXTENDS Naturals, Sequences",
        "SemanticProducerTable == <<",
    ]
    for name, record, category in producer_rows:
        lines.append(
            f'  [producer |-> "{name}", record |-> "{record}", '
            f'category |-> "{category}"],'
        )
    lines[-1] = lines[-1].rstrip(",")
    lines += [
        ">>",
        "SemanticProducers == {SemanticProducerTable[i].producer : "
        "i \\in 1..Len(SemanticProducerTable)}",
        "SemanticRecords == {SemanticProducerTable[i].record : "
        "i \\in 1..Len(SemanticProducerTable)}",
        "SemanticCategories == {SemanticProducerTable[i].category : "
        "i \\in 1..Len(SemanticProducerTable)}",
        "====",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    source = args.source or root / "src/d3d9/d3d9_pe_semantic_tokens.hpp"
    output = args.output or root / (
        "specs/verification/tla/PeRecorderSemanticProjectionTable.tla"
    )
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
