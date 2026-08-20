---
type: "Gap Tracker"
title: "Producer Concurrency Gaps"
description: "Adoption and evidence gaps for the bridge synchronicity classification and thread-ownership contracts."
tags: [specs, backend, producer-concurrency, gap]
---

# Producer Concurrency — Gaps

Domain-owned tracker for `requirements.md` (R-BACK-43.x). Root rollup:
[../../gap.md](../../gap.md).

| Area | Status | Evidence / missing |
|---|---|---|
| Bridge entry classification table (R-BACK-43.1) | classification complete + mechanically audited (spec.md §3, per-entry ```classification block, 161 symbols — the 2026-08-21 "~89" total was an undercount, corrected in the same audit) | Mechanical drift audit landed: `scripts/check/audit_bridge_entry_classification.py` / `dxmt9-bridge-entry-classification-audit` diffs every `extern "C" dxmt9c_*` definition (five `device_c_bridge_*.cpp` files, plus a scan of the rest of `src/` for a stray direct-entry addition) against the spec.md §3 block and fails on any drift or out-of-taxonomy class. G1 (desc-getter no-drain status) is resolved with file:line evidence; four open items remain (G2-G5) — getter-drain necessity G2 is the first R-BACK-43.2 review target, sized by the 0.64 ms get_swap_chain precedent. |
| `record-only` de-synchronization (R-BACK-43.2) | G2 resolved: no getter migration | The 2026-08-21 source audit found the 14 drained device-state getters cost zero on GT2 (nine dead on the steady-state PE-shadow path, five with load-bearing drains on their shadow-miss fallbacks), so the first candidate class yields nothing; no remaining known record-only candidate besides commit_chunk's already-offloaded lane. Reopen only if a profiler surfaces a drained entry with measurable frequency. |
| Ownership declarations at field level (R-BACK-43.4) | partial | §2's table covers the audited surfaces (rename ring, capture read-set, stamps, tickets, slots, pins, reclaim gate). The declarations largely live in this spec + the pool header contract, not yet as adjacent code comments at every owning field. Adopt opportunistically as files are touched. |
| Shared thread-affinity assert helper (R-BACK-43.5) | reference shape only | `assertRecorderThreadConfined` is device-local. Extract a shared helper (owning-thread-id + assert, compiled out in release) with its first additional adopter — T2b (capture move) is the natural first user. |
| T2b capture off the queue mutex | design-licensed, not implemented | Q1 audit (producer-written-only read-set) + pin existence argument; needs a small `ProducerMarkReclaim` CaptureRead extension review, the shared assert helper, and the standard gates. |
| T2c map DISCARD fast path (`completedSeqId_` → owner-published atomic) | not started | Below whole-build A/B noise alone (bundle with T2b); burst-structure check per the .31 rule before any fps expectation. |
| T2d reserve-copy-commit slot append | design stated (design doc §9) | Needs its own bounded model (append/publish/force-publish interleaving with a half-appended-slot Buggy cfg) before implementation. Largest remaining holder (3.4-3.8 ms/present). |
| C++ memory-order harness (R-BACK-43.6 residual) | open | Deterministic interleaving harness over the real atomics (R-VERIF-7.3 direction). The model does not discharge release/acquire pairing or torn reads. Blocks no current default (T2a' writes stay mutex-serialized) but gates further relaxations. |
| Constant-promotion complexity review (R-BACK-43.7) | process rule active from this spec | Two recorded recurrences motivated it; enforcement is review-time. The known open instance: O(n²) commit dedup at 277 handles/call (~0.17 ms/present). |
| Restamp-fire observability | open | No counter observes how often `restampIfTicketAdvancedLocked` actually re-stamps; the window is modelled but unmeasured in the wild. Cheap counter, add with the next queue-side change. |
