#!/usr/bin/env python3
"""Generate/check the bounded mutation-composition TLA vocabulary.

This intentionally generates vocabulary only.  The ordered predicate relation
is owned by ``classifyComposition`` and is repeated in the bounded TLA
``Expected`` relation; the freshness check must not be mistaken for a
semantic model/code binding.
"""

from __future__ import annotations

import argparse
import pathlib
import re


DECISION_RE = re.compile(r"^\s*([A-Za-z][A-Za-z0-9]*)\s*,?\s*$")


def decisions(source: pathlib.Path) -> tuple[str, ...]:
    inside = False
    names: list[str] = []
    for line in source.read_text().splitlines():
        if line.startswith("enum class CompositionDecision"):
            inside = True
            continue
        if inside and line.strip() == "};":
            break
        if inside:
            match = DECISION_RE.match(line)
            if match:
                names.append(match.group(1))
    if not names:
        raise SystemExit(f"no CompositionDecision values found in {source}")
    return tuple(re.sub(r"(?<!^)(?=[A-Z])", "-", name).lower()
                 for name in names)


def render(source: pathlib.Path) -> str:
    values = ", ".join(f'"{value}"' for value in decisions(source))
    return (
        """---- MODULE MutationCompositionTable ----
(* ***************************************************************************
 * Generated from the production classifier vocabulary in
 * include/dxmt9/mutation_composition_observer.hpp. Do not hand-edit.
 * This module shares names only; predicate ordering/semantics remain in the
 * production classifier and MutationComposition.Expected.
 *************************************************************************** *)
CompositionDecisions == {"""
        + values
        + "}\n====\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--check", action="store_true")
    parser.add_argument("--source", type=pathlib.Path)
    parser.add_argument("--output", type=pathlib.Path)
    args = parser.parse_args()
    root = pathlib.Path(__file__).resolve().parents[2]
    source = args.source or root / "include/dxmt9/mutation_composition_observer.hpp"
    output = args.output or root / (
        "specs/verification/tla/MutationCompositionTable.tla"
    )
    expected = render(source)
    if args.check:
        actual = output.read_text()
        if actual != expected:
            raise SystemExit("MutationCompositionTable.tla is stale")
    else:
        output.write_text(expected)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
