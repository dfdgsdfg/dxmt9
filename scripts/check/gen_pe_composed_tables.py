#!/usr/bin/env python3
"""Generate/check composed PE recorder and StateBlock value TLA tables."""

from __future__ import annotations

import argparse
from collections.abc import Callable
import os
import pathlib
import re
import tempfile


ROW_RE = re.compile(r"(?P<macro>DXMT9_PE_[A-Z_]+_ROW)\((?P<body>[^)]*)\)")


def parse_rows(path: pathlib.Path, macro: str, width: int) -> list[tuple[str, ...]]:
    rows: list[tuple[str, ...]] = []
    for match in ROW_RE.finditer(path.read_text()):
        if match.group("macro") != macro:
            continue
        fields = tuple(part.strip() for part in match.group("body").split(","))
        if fields[0] in {"point", "event"}:
            continue
        if len(fields) != width:
            raise SystemExit(f"{path}: expected {width} fields: {fields}")
        rows.append(fields)
    if not rows:
        raise SystemExit(f"{path}: no {macro} rows")
    keys = [(row[0], row[1]) for row in rows] if width == 7 else [row[0] for row in rows]
    if len(keys) != len(set(keys)):
        raise SystemExit(f"{path}: duplicate row key")
    return rows


def atom(value: str) -> str:
    return value.upper() if value in {"True", "False"} else f'"{value}"'


def emit_settlement(path: pathlib.Path) -> str:
    rows = parse_rows(path, "DXMT9_PE_RECORDER_SETTLEMENT_ROW", 7)
    names = ("point", "result", "action", "acceptedRecord", "retryable",
             "rollbackEmitter", "poison")
    lines = [
        "---- MODULE PeRecorderSettlementTable ----",
        "EXTENDS Naturals, Sequences",
        "(***************************************************************************",
        " * Generated from src/d3d9/d3d9_pe_recorder_settlement_table.inc.",
        " * Do not hand-edit. This is the composed model/code row binding.",
        " ***************************************************************************)",
        "SettlementRows == <<",
    ]
    for row in rows:
        fields = ", ".join(
            f"{name} |-> {atom(value)}" for name, value in zip(names, row)
        )
        lines.append(f"  [{fields}],")
    lines[-1] = lines[-1].rstrip(",")
    lines += [
        ">>",
        "SettlementMatches(point, result, action, acceptedRecord, retryable,",
        "                  rollbackEmitter, poison) ==",
        r"  \E i \in 1..Len(SettlementRows) :",
        "    LET row == SettlementRows[i] IN",
        r"      /\ row.point = point /\ row.result = result",
        r"      /\ row.action = action /\ row.acceptedRecord = acceptedRecord",
        r"      /\ row.retryable = retryable",
        r"      /\ row.rollbackEmitter = rollbackEmitter /\ row.poison = poison",
        "====",
        "",
    ]
    return "\n".join(lines)


def emit_stateblock(path: pathlib.Path) -> str:
    rows = parse_rows(path, "DXMT9_PE_STATEBLOCK_VALUE_ROW", 6)
    names = ("event", "action", "preserveTrackedSet", "refreshSnapshot",
             "publishLive", "poison")
    lines = [
        "---- MODULE PeStateBlockValueTable ----",
        "EXTENDS Naturals, Sequences",
        "(***************************************************************************",
        " * Generated from src/d3d9/d3d9_pe_stateblock_value_table.inc.",
        " * Do not hand-edit. This is the repeated-value model/code row binding.",
        " ***************************************************************************)",
        "StateBlockValueRows == <<",
    ]
    for row in rows:
        fields = ", ".join(
            f"{name} |-> {atom(value)}" for name, value in zip(names, row)
        )
        lines.append(f"  [{fields}],")
    lines[-1] = lines[-1].rstrip(",")
    lines += [
        ">>",
        "StateBlockValueMatches(event, action, preserveTrackedSet,",
        "                       refreshSnapshot, publishLive, poison) ==",
        r"  \E i \in 1..Len(StateBlockValueRows) :",
        "    LET row == StateBlockValueRows[i] IN",
        r"      /\ row.event = event /\ row.action = action",
        r"      /\ row.preserveTrackedSet = preserveTrackedSet",
        r"      /\ row.refreshSnapshot = refreshSnapshot",
        r"      /\ row.publishLive = publishLive /\ row.poison = poison",
        "====",
        "",
    ]
    return "\n".join(lines)


def write_atomic(path: pathlib.Path, contents: str) -> None:
    """Replace one generated artifact without exposing a partial file."""
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary: pathlib.Path | None = None
    try:
        with tempfile.NamedTemporaryFile(
            mode="w", encoding="utf-8", newline="\n", dir=path.parent,
            prefix=f".{path.name}.", delete=False
        ) as stream:
            temporary = pathlib.Path(stream.name)
            stream.write(contents)
            stream.flush()
            os.fsync(stream.fileno())
        os.replace(temporary, path)
        temporary = None
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def verify_generation_mode(
    artifacts: tuple[
        tuple[pathlib.Path, pathlib.Path, Callable[[pathlib.Path], str]], ...
    ]
) -> None:
    """Exercise the same separate-file writer without touching repo outputs."""
    with tempfile.TemporaryDirectory(prefix="dxmt9-pe-composed-") as directory:
        generated_root = pathlib.Path(directory)
        generated: set[pathlib.Path] = set()
        for source, output, emit in artifacts:
            expected = emit(source)
            target = generated_root / output.name
            write_atomic(target, expected)
            if target.read_text(encoding="utf-8") != expected:
                raise SystemExit(f"generation verification failed for {target}")
            if source.name not in expected:
                raise SystemExit(
                    f"{target}: generated source comment does not name {source.name}"
                )
            generated.add(target)
        if len(generated) != len(artifacts):
            raise SystemExit("generation verification did not create separate outputs")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--verify-generation", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    artifacts = (
        (root / "src/d3d9/d3d9_pe_recorder_settlement_table.inc",
         root / "specs/verification/tla/PeRecorderSettlementTable.tla",
         emit_settlement),
        (root / "src/d3d9/d3d9_pe_stateblock_value_table.inc",
         root / "specs/verification/tla/PeStateBlockValueTable.tla",
         emit_stateblock),
    )
    if args.verify_generation:
        verify_generation_mode(artifacts)
        return 0
    stale = False
    for source, output, emit in artifacts:
        expected = emit(source)
        if args.check:
            if not output.exists() or output.read_text() != expected:
                print(f"{output} is stale; regenerate from {source}")
                stale = True
        else:
            write_atomic(output, expected)
            print(f"wrote {output} from {source}")
    return 1 if stale else 0


if __name__ == "__main__":
    raise SystemExit(main())
