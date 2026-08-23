#!/usr/bin/env python3
"""Generate/check the TLA commit matrix from settleRecorderCommit."""

from __future__ import annotations

import argparse
import pathlib
import re


ENUM_RE = re.compile(r"enum class (RecorderCommit(?:Phase|Event|Action))[^\{]*\{(?P<body>.*?)\};", re.S)
CASE_RE = re.compile(
    r"case RecorderCommitPhase::(?P<phase>\w+):(?P<body>.*?)"
    r"(?=\n  case RecorderCommitPhase::|\n  \}\n)",
    re.S,
)
EVENT_RE = re.compile(r"facts\.event == RecorderCommitEvent::(\w+)")
RETURN_RE = re.compile(r"return commitPlan\((?P<args>.*?)\);", re.S)


def enum_values(source: str, name: str) -> list[str]:
    for match in ENUM_RE.finditer(source):
        if match.group(1) == name:
            return re.findall(r"\b(\w+)\b", match.group("body"))
    raise SystemExit(f"missing {name}")


def split_args(args: str) -> list[str]:
    result: list[str] = []
    start = 0
    depth = 0
    for index, char in enumerate(args):
        if char == "(":
            depth += 1
        elif char == ")":
            depth -= 1
        elif char == "," and depth == 0:
            result.append(args[start:index].strip())
            start = index + 1
    result.append(args[start:].strip())
    return result


def atom(value: str) -> str:
    value = value.strip()
    if value == "true" or value == "false":
        return value.upper()
    if value.startswith("RecorderCommitPhase::"):
        return '"' + value.rsplit("::", 1)[1] + '"'
    if value.startswith("RecorderCommitAction::"):
        return '"' + value.rsplit("::", 1)[1] + '"'
    raise SystemExit(f"unsupported commit-plan argument: {value}")


def plan(args: str) -> dict[str, str]:
    fields = split_args(args)
    if len(fields) != 2:
        raise SystemExit(f"unexpected commitPlan arity: {fields}")
    action = fields[1].strip().rsplit("::", 1)[-1]
    derived = {
        "preserveRetryBytes": action == "Retry",
        "commandAccepted": action == "AcceptCommand",
        "objectDestroy": action in {"DestroyAlias", "DestroyParent"},
        "resetBuilder": action in {"ResetBuilder", "DiscardAll"},
        "advanceWarmEpoch": action == "AdvanceWarmEpoch",
    }
    return {
        "next": atom(fields[0]),
        "action": atom(fields[1]),
        **{key: str(value).upper() for key, value in derived.items()},
    }


def emit(source_path: pathlib.Path) -> str:
    source = source_path.read_text()
    phases = enum_values(source, "RecorderCommitPhase")
    events = enum_values(source, "RecorderCommitEvent")
    actions = enum_values(source, "RecorderCommitAction")
    rows: list[tuple[str, str, dict[str, str]]] = []

    # The discard/reset branch is intentionally phase-independent in C++.
    discard_match = re.search(
        r"facts\.event == RecorderCommitEvent::ExplicitDiscard\s*\|\|\s*"
        r"facts\.event == RecorderCommitEvent::DeviceReset.*?"
        r"return commitPlan\((?P<args>.*?)\);",
        source,
        re.S,
    )
    if not discard_match:
        raise SystemExit("missing explicit discard/device reset plan")
    discard_plan = plan(discard_match.group("args"))
    rows.extend(("Any", event, discard_plan) for event in ("ExplicitDiscard", "DeviceReset"))

    switch_match = re.search(
        r"switch \(facts\.phase\) \{(?P<body>.*?)\n  \}\n",
        source,
        re.S,
    )
    if not switch_match:
        raise SystemExit("missing commit phase switch")
    for case in CASE_RE.finditer(switch_match.group("body")):
        phase = case.group("phase")
        body = case.group("body")
        for match in RETURN_RE.finditer(body):
            before = body[:match.start()]
            event_match = list(EVENT_RE.finditer(before))
            if not event_match:
                raise SystemExit(f"commit plan without event in {phase}")
            event = event_match[-1].group(1)
            rows.append((phase, event, plan(match.group("args"))))

    known_phases = set(phases)
    known_events = set(events)
    known_actions = set(actions)
    for phase, event, row in rows:
        if phase != "Any" and phase not in known_phases:
            raise SystemExit(f"unknown phase {phase}")
        if event not in known_events:
            raise SystemExit(f"unknown event {event}")
        if row["action"].strip('"') not in known_actions:
            raise SystemExit(f"unknown action {row['action']}")

    lines = [
        "---- MODULE PeRecorderCommitTable ----",
        "EXTENDS Naturals, Sequences",
        "(***************************************************************************",
        " * Generated from src/d3d9/d3d9_pe_commit_transition.hpp.  Do not hand-edit.",
        " * This table is a freshness/isomorphism witness for the PE commit algebra.",
        " ***************************************************************************)",
        "CommitRows == <<",
    ]
    for phase, event, row in rows:
        values = [
            ("phase", '"' + phase + '"'),
            ("event", '"' + event + '"'),
            *row.items(),
        ]
        lines.append("  [" + ", ".join(f"{key} |-> {value}" for key, value in values) + "],")
    lines[-1] = lines[-1].rstrip(",")
    lines += [
        ">>",
        "CommitMatches(phase, event, next, action, preserveRetryBytes,",
        "              commandAccepted, objectDestroy, resetBuilder,",
        "              advanceWarmEpoch) ==",
        "  \\E i \\in 1..Len(CommitRows) :",
        "    LET row == CommitRows[i] IN",
        "      /\\ (row.phase = \"Any\" \\/ row.phase = phase)",
        "      /\\ row.event = event",
        "      /\\ row.next = next",
        "      /\\ row.action = action",
        "      /\\ row.preserveRetryBytes = preserveRetryBytes",
        "      /\\ row.commandAccepted = commandAccepted",
        "      /\\ row.objectDestroy = objectDestroy",
        "      /\\ row.resetBuilder = resetBuilder",
        "      /\\ row.advanceWarmEpoch = advanceWarmEpoch",
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
    source = args.source or root / "src/d3d9/d3d9_pe_commit_transition.hpp"
    output = args.output or root / "specs/verification/tla/PeRecorderCommitTable.tla"
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
