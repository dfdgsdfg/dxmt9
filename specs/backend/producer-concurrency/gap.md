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
| Bridge entry classification table (R-BACK-43.1) | initial classification landed (spec.md §3, 2026-08-21, ~89 symbols) | Five open items G1-G5 recorded in §3 (getter-drain necessity G2 is the first R-BACK-43.2 review target, sized by the 0.64 ms get_swap_chain precedent). No mechanical audit yet — a `scripts/check` audit diffing the `dxmt9c_*` symbol set against the table would close the drift risk. |
| `record-only` de-synchronization (R-BACK-43.2) | not started | Candidate follow-up after classification: which `record-only` entries still acquire queue state by convention. Each migration needs the 43.6 ladder. |
| Ownership declarations at field level (R-BACK-43.4) | partial | §2's table covers the audited surfaces (rename ring, capture read-set, stamps, tickets, slots, pins, reclaim gate). The declarations largely live in this spec + the pool header contract, not yet as adjacent code comments at every owning field. Adopt opportunistically as files are touched. |
| Shared thread-affinity assert helper (R-BACK-43.5) | reference shape only | `assertRecorderThreadConfined` is device-local. Extract a shared helper (owning-thread-id + assert, compiled out in release) with its first additional adopter — T2b (capture move) is the natural first user. |
| T2b capture off the queue mutex | design-licensed, not implemented | Q1 audit (producer-written-only read-set) + pin existence argument; needs a small `ProducerMarkReclaim` CaptureRead extension review, the shared assert helper, and the standard gates. |
| T2c map DISCARD fast path (`completedSeqId_` → owner-published atomic) | not started | Below whole-build A/B noise alone (bundle with T2b); burst-structure check per the .31 rule before any fps expectation. |
| T2d reserve-copy-commit slot append | design stated (design doc §9) | Needs its own bounded model (append/publish/force-publish interleaving with a half-appended-slot Buggy cfg) before implementation. Largest remaining holder (3.4-3.8 ms/present). |
| C++ memory-order harness (R-BACK-43.6 residual) | open | Deterministic interleaving harness over the real atomics (R-VERIF-7.3 direction). The model does not discharge release/acquire pairing or torn reads. Blocks no current default (T2a' writes stay mutex-serialized) but gates further relaxations. |
| Constant-promotion complexity review (R-BACK-43.7) | process rule active from this spec | Two recorded recurrences motivated it; enforcement is review-time. The known open instance: O(n²) commit dedup at 277 handles/call (~0.17 ms/present). |
| Restamp-fire observability | open | No counter observes how often `restampIfTicketAdvancedLocked` actually re-stamps; the window is modelled but unmeasured in the wild. Cheap counter, add with the next queue-side change. |
