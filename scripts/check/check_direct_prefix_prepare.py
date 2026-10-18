#!/usr/bin/env python3
"""R-BACK-2.107 structural binding pins, not a C++ concurrency proof.

Native tests execute the shared gate and actual validators. TLC checks the
bounded ownership protocol. These pins detect drift at the private queue
factory/consumer seam, whose exclusion premise still requires code review.
"""
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[2]


def compact(text):
    return re.sub(r"\s+", "", re.sub(r"//[^\n]*|/\*.*?\*/", "", text, flags=re.S))


def check(queue, header, source, gate, model):
    body = queue.split("CommandQueue::commitDirectChunkSlotReplay(", 1)[1]
    prefix = body.split("const bool prepared =", 1)[0]
    guards = (
        "std::unique_lock lock(mutex_)",
        "&*directChunkSlotBuildContext_ != context",
        "context->ticket != ticket", "context->controlIndex != controlIndex",
        "context->ownerThread != std::this_thread::get_id()",
        "context->failed || !context->assembler",
        "*writingSlot_ != controlIndex",
        "control.state != core::ChunkSlot::State::Writing",
        "control.sourceId != ticket.id", "control.storage != ticket.storage",
        "payload != control.payload",
        "nextSeqId_.load(std::memory_order_relaxed) != ticket.seqId",
    )
    for guard in guards:
        assert compact(guard) in compact(prefix), guard
    fields = {
        "rawOrdinal": "ticket.rawOrdinal", "sourceOrdinal": "ticket.sourceOrdinal",
        "seqId": "ticket.seqId", "buildGeneration": "ticket.buildGeneration",
        "sourceGeneration": "ticket.id.generation",
        "storageGeneration": "ticket.storage.generation",
        "controlIndex": "static_cast<std::uint32_t>(controlIndex)",
        "firstPage": "ticket.storage.firstPage", "pageCount": "ticket.storage.pageCount",
        "segmentIndex": "0", "segmentCount": "1",
    }
    for field, value in fields.items():
        assert compact(f"prefixBinding.{field} = {value};") in compact(prefix), field
    seam = body.split("#if defined(DXMT9_EXPERIMENT_PREFIX_PREPARE)", 1)[1].split("#endif", 1)[0]
    assert "lock.unlock" not in seam
    assert compact("prepareOwnedSuffix(core::TransactionalChunkSlotAssembler::"
                   "PrefixPrepareCapability(*context->assembler, *payload, prefixBinding))") in compact(seam)
    assert "#else" in seam and "context->assembler->prepare()" in seam
    assert queue.count("TransactionalChunkSlotAssembler::PrefixPrepareCapability(") == 1
    capability = header.split("class PrefixPrepareCapability final", 1)[1].split("public:", 1)[0]
    assert "PrefixPrepareCapability(" in capability  # constructor is private
    assert "friend class dxmt9::CommandQueue" in capability
    assert "std::exchange(capability.assembler_, nullptr)" in source
    assert "capability.binding_ == *outerBinding_" in source
    assert "DXMT9_DIRECT_CHUNK_SLOT_DIMENSIONS(DXMT9_PREFIX_EXTENT)" in source
    assert compact("if (!buildable) return PrefixPrepareDecision::Reject;") in compact(gate)
    assert compact("if (!capabilityPresent) return PrefixPrepareDecision::Full;") in compact(gate)
    assert compact("return capabilityMatches && extentsPreserved ? PrefixPrepareDecision::Suffix : PrefixPrepareDecision::Reject;") in compact(gate)
    assert compact('IF ~buildable THEN "Reject" ELSE IF ~present THEN "Full" ELSE IF matches /\\ preserved THEN "Suffix" ELSE "Reject"') in compact(model)


def main():
    queue = (ROOT / "src/dxmt9/dxmt9_command_queue.cpp").read_text()
    rest = [(ROOT / path).read_text() for path in (
        "src/dxmt9/dxmt9_source_payload.hpp", "src/dxmt9/dxmt9_source_payload.cpp",
        "src/dxmt9/dxmt9_prefix_prepare.hpp", "specs/verification/tla/DirectPrefixPrepare.tla")]
    check(queue, *rest)
    # The audit must itself distinguish a missing gate and a self-echo binding.
    mutations = (
        ("context->ownerThread != std::this_thread::get_id()", "false"),
        ("prefixBinding.buildGeneration = ticket.buildGeneration;", ""),
        ("prefixBinding.storageGeneration = ticket.storage.generation;", ""),
        ("const bool prepared = context->assembler->prepareOwnedSuffix(",
         "lock.unlock(); const bool prepared = context->assembler->prepareOwnedSuffix("),
    )
    for old, new in mutations:
        assert old in queue
        try:
            check(queue.replace(old, new), *rest)
        except AssertionError:
            continue
        raise AssertionError(f"undetected source mutation: {old}")
    print("prefix prepare source binding: pass; structural counterexamples: 4")


if __name__ == "__main__":
    main()
