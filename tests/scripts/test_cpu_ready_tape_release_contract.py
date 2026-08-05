#!/usr/bin/env python3

from pathlib import Path
import re
import sys


MUTATION = (
    r"(?:reserve|probeReserve|abort|sealAndPublish|representReadyPrefix|"
    r"transitionAll|transition|"
    r"completeAll|complete|completeInline|beginReclaim|finishReclaim|"
    r"beginArenaAbort|finishArenaAbort|detachReclaimingArenaOwner|"
    r"finishArenaReclaim|reclaim|stopAdmission)"
)


def main() -> int:
    repo = Path(__file__).resolve().parents[2]
    sources = [
        repo / "src/dxmt9/dxmt9_queue.cpp",
        repo / "src/dxmt9/dxmt9_command_queue.cpp",
    ]
    forbidden = re.compile(
        rf"DXMT_ASSERT\s*\([^;]*?(?:cpuReadyTape|CpuReadyTape)"
        rf"[^;]*?{MUTATION}\s*\(",
        re.DOTALL,
    )
    unchecked_dereference = re.compile(
        r"DXMT_ASSERT\s*\([^;]*?\b(?P<name>[A-Za-z_]\w*)\s*!=\s*nullptr"
        r"[^;]*?\);\s*(?:return\s+\*(?P=name)|(?P=name)->)",
        re.DOTALL,
    )
    positive_fixture = """
DXMT_ASSERT(
    binding.cpuReadyTape->transition(
        source.id, source.storage, State::Ready, State::Represented));
"""
    if not forbidden.search(positive_fixture):
        print("release-contract regex failed its multiline positive self-test")
        return 1
    arena_positive_fixture = """
DXMT_ASSERT(
    binding.cpuReadyTape->finishArenaReclaim(
        source.id, source.storage, std::move(owner)));
"""
    if not forbidden.search(arena_positive_fixture):
        print("release-contract regex failed its arena mutation positive self-test")
        return 1
    dereference_fixture = """
DXMT_ASSERT(
    payload != nullptr && payload == control.payload);
payload->appendPresent(present, sourceHandle);
"""
    if not unchecked_dereference.search(dereference_fixture):
        print("release-contract regex failed its unchecked-dereference self-test")
        return 1

    failures: list[str] = []
    for path in sources:
        source = path.read_text(encoding="utf-8")
        for match in forbidden.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            failures.append(f"{path.relative_to(repo)}:{line}")
        for match in unchecked_dereference.finditer(source):
            line = source.count("\n", 0, match.start()) + 1
            failures.append(f"{path.relative_to(repo)}:{line}")

    if failures:
        print("CpuReadyTape release contract violation:")
        print("\n".join(failures))
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
