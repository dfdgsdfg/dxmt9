#!/usr/bin/env python3
"""Generate/check the TLA table for the PE StateBlock transaction algebra."""

from __future__ import annotations

import argparse
import pathlib
import re


ENUM_RE = re.compile(
    r"enum class (PeStateBlock(?:Phase|Event|Action|CandidateEffect|"
    r"StagedRefEffect|CaptureEffect))[^\{]*\{(?P<body>.*?)\};",
    re.S,
)
ROW_RE = re.compile(r"DXMT9_PE_STATEBLOCK_ROW\((?P<body>[^)]*)\)")


def enum_values(source: str, name: str) -> list[str]:
    for match in ENUM_RE.finditer(source):
        if match.group(1) == name:
            return re.findall(r"\b(\w+)\b", match.group("body"))
    raise SystemExit(f"missing {name}")


def parse_rows(source: str) -> list[tuple[str, ...]]:
    rows: list[tuple[str, ...]] = []
    for match in ROW_RE.finditer(source):
        fields = tuple(field.strip() for field in match.group("body").split(","))
        if fields[0] == "phase":
            continue
        if len(fields) != 7:
            raise SystemExit(f"StateBlock row must have 7 fields: {fields}")
        rows.append(fields)
    return rows


def require_complete(header: str, rows: list[tuple[str, ...]]) -> None:
    phases = enum_values(header, "PeStateBlockPhase")
    events = enum_values(header, "PeStateBlockEvent")
    actions = enum_values(header, "PeStateBlockAction")
    candidate_effects = enum_values(header, "PeStateBlockCandidateEffect")
    staged_effects = enum_values(header, "PeStateBlockStagedRefEffect")
    capture_effects = enum_values(header, "PeStateBlockCaptureEffect")
    known = [
        set(phases), set(events), set(phases), set(actions),
        set(candidate_effects), set(staged_effects), set(capture_effects),
    ]
    for row in rows:
        for field, vocabulary in zip(row, known):
            if field not in vocabulary:
                raise SystemExit(f"unknown StateBlock table atom {field} in {row}")
    keys = [(row[0], row[1]) for row in rows]
    if len(keys) != len(set(keys)):
        raise SystemExit("duplicate StateBlock phase/event row")
    mapped_events = {row[1] for row in rows}
    mapped_actions = {row[3] for row in rows}
    missing_events = set(events) - mapped_events
    missing_actions = set(actions) - mapped_actions
    if missing_events:
        raise SystemExit(f"unmapped PeStateBlockEvent values: {sorted(missing_events)}")
    if missing_actions:
        raise SystemExit(f"unmapped PeStateBlockAction values: {sorted(missing_actions)}")
    mentioned_phases = {row[0] for row in rows} | {row[2] for row in rows}
    missing_phases = set(phases) - mentioned_phases
    if missing_phases:
        raise SystemExit(f"unmapped PeStateBlockPhase values: {sorted(missing_phases)}")


def emit(header_path: pathlib.Path, table_path: pathlib.Path) -> str:
    header = header_path.read_text()
    rows = parse_rows(table_path.read_text())
    require_complete(header, rows)
    lines = [
        "---- MODULE PeStateBlockTransitionTable ----",
        "EXTENDS Naturals, Sequences",
        "(***************************************************************************",
        " * Generated from src/d3d9/d3d9_pe_stateblock_transition_table.inc.",
        " * Do not hand-edit. This is the model/code isomorphism witness.",
        " ***************************************************************************)",
        "StateBlockRows == <<",
    ]
    names = ("phase", "event", "next", "action", "candidateEffect",
             "stagedRefEffect", "captureEffect")
    for row in rows:
        fields = ", ".join(
            f'{name} |-> "{value}"' for name, value in zip(names, row)
        )
        lines.append(f"  [{fields}],")
    lines[-1] = lines[-1].rstrip(",")
    lines += [
        ">>",
        "StateBlockMatches(phase, event, next, action, candidateEffect,",
        "                  stagedRefEffect, captureEffect) ==",
        r"  \E i \in 1..Len(StateBlockRows) :",
        "    LET row == StateBlockRows[i] IN",
        r"      /\ row.phase = phase",
        r"      /\ row.event = event",
        r"      /\ row.next = next",
        r"      /\ row.action = action",
        r"      /\ row.candidateEffect = candidateEffect",
        r"      /\ row.stagedRefEffect = stagedRefEffect",
        r"      /\ row.captureEffect = captureEffect",
        "====",
        "",
    ]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--header", type=pathlib.Path)
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    header = args.header or root / "src/d3d9/d3d9_pe_stateblock_transaction.hpp"
    source = args.source or root / "src/d3d9/d3d9_pe_stateblock_transition_table.inc"
    output = args.output or root / "specs/verification/tla/PeStateBlockTransitionTable.tla"
    expected = emit(header, source)
    if args.check:
        if not output.exists() or output.read_text() != expected:
            print(f"{output} is stale; regenerate from {source}")
            return 1
        return 0
    output.write_text(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
