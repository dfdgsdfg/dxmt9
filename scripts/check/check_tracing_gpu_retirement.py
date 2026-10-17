#!/usr/bin/env python3
"""Audit the GPU retirement model's production binding and negative fixtures.

The native truth table calls production predicates. This structural audit pins
the CAS/publication order around those predicates; it is not a C++ memory-model
proof, a Metal execution oracle, or a proof of raw impl_ pointer admission.
"""

from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
TLA = ROOT / "specs/verification/tla"
COUNTEREXAMPLES = {
    "early-publication": "NoCompletionBeforePublication",
    "stale-ticket": "NoStaleCompletion",
    "early-reuse": "NoReuseBeforeConsumption",
    "session-reuse": "SessionReuseRequiresQuiescence",
    "early-retire": "NoRetireBeforeGpuCompletion",
    "reader-retire": "NoRetireWithCpuReaders",
}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise SystemExit(message)


def ordered(text: str, fragments: tuple[str, ...], label: str) -> None:
    position = 0
    for fragment in fragments:
        found = text.find(fragment, position)
        require(found >= 0, f"{label} lost ordered fragment {fragment!r}")
        position = found + len(fragment)


def main() -> int:
    model = (TLA / "TracingGpuRetirement.tla").read_text()
    predicates = (ROOT / "src/dxmt9/tracing/gpu_retirement_state.hpp").read_text()
    collector = (ROOT / "src/dxmt9/dxmt9_trace_gpu.cpp").read_text()
    bridge = (ROOT / "src/dxmt9/dxmt9_trace_gpu_bridge.mm").read_text()
    native = (ROOT / "tests/native/backend/tracing_gpu_retirement_spec.cpp").read_text()
    enum = re.search(r"enum class SlotState\s*:\s*std::uint8_t\s*\{([^}]+)\}", predicates)
    states = re.search(r"SlotStates\s*==\s*\{([^}]+)\}", model)
    require(enum is not None and states is not None, "GPU state inventory is missing")
    require(set(re.findall(r'"([A-Za-z]+)"', states[1])) ==
            set(re.findall(r"\b([A-Za-z]+)\s*,", enum[1])),
            "production/model GPU slot state inventory drifted")
    for predicate in ("completionTicketMatches", "canBeginSession", "mayRetireStorage"):
        require(f"constexpr bool {predicate}(" in predicates and predicate in native,
                f"shared GPU predicate/native binding missing: {predicate}")
        require(predicate in collector or predicate in bridge,
                f"GPU production no longer consumes predicate: {predicate}")

    reserve = collector.split("Ticket Collector::reserve(", 1)[1].split(
        "void Collector::recordLostCapacity(", 1)[0]
    ordered(reserve, ("SlotState::Free", "compare_exchange_strong(",
                     "SlotState::Initializing", "slot.work = work;",
                     "slot.completion = {};", "slot.generation.fetch_add(",
                     "slot.state.store(static_cast<std::uint8_t>(SlotState::Reserved)",
                     "return Ticket{"), "GPU reserve publication")
    complete = collector.split("bool Collector::complete(", 1)[1].split(
        "bool Collector::drainOne(", 1)[0]
    ordered(complete, ("SlotState::Reserved", "compare_exchange_strong(",
                      "SlotState::Completing", "completionTicketMatches(",
                      "slot.generation.load(",
                      "ticket.slotGeneration", "slot.completion = completion;",
                      "SlotState::Ready"), "GPU completion generation recheck")
    drain = collector.split("bool Collector::drainOne(", 1)[1].split(
        "void Collector::close(", 1)[0]
    ordered(drain, ("SlotState::Ready", "compare_exchange_strong(",
                   "SlotState::Draining", "sample.work = slot.work;",
                   "sample.gpuEndNs = slot.completion.gpuEndNs;",
                   "SlotState::Free"), "GPU consumer lease")

    normal = (TLA / "TracingGpuRetirement.cfg").read_text()
    for invariant in (*COUNTEREXAMPLES.values(), "TypeOK",
                      "RetirementRequiresConsumedResult", "QuarantineRetainsLiveStorage"):
        require(re.search(rf"^{invariant}\s*==", model, re.M) is not None and
                f"INVARIANT {invariant}" in normal,
                f"GPU normal safety gate missing: {invariant}")
    for suffix, invariant in COUNTEREXAMPLES.items():
        config = (TLA / f"TracingGpuRetirement.{suffix}.counterexample.cfg").read_text()
        require(f"INVARIANT {invariant}" in config and "SPECIFICATION Spec" in config,
                f"GPU expected-failure gate drifted: {suffix}")
    progress = (TLA / "TracingGpuRetirement.progress.cfg").read_text()
    for prop in ("GpuEventuallyConsumed", "QuarantineEventuallyRetires"):
        require(f"PROPERTY {prop}" in progress and prop in model,
                f"GPU progress gate missing: {prop}")
    print("TracingGpuRetirement model/production binding inventory: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
