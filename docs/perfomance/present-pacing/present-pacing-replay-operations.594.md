---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Compatibility draw preparation dominates the resolved Replay envelope"
type: evidence
status: live-operation-partition-qualified-removal-open
updated: 2026-10-16
source: traces/stkcop-replay-operations-h594/{contract.md,prepare.py,derivation.json,trace.hpp,native.json,parser-tests.json,build.py,build.json,preflight.json,inputs.json,run.py,parse_partition.py,analyze_live.py,prepare_verify.py,verify.py,verification.json,audit_clock_calls.py,clock-audit.json,draw-cache-disassembly.txt,draw-cache-indirect-symbols.txt}; traces/stkcop-replay-operations-h594/{O0,O1}/{receipt.json,self.bin,partition.bin,analysis.json,post-tail.png,restore/restored.json}
related: present-pacing-replay-partition.593.md; present-pacing-live-batch.592.md; present-pacing-append-tail.591.md; present-pacing-sparse-cpu.584.md
---

# Attribute compatibility draw preparation before optimizing sparse setters

H594 splits [H593](present-pacing-replay-partition.593.md)'s large resolved
Replay envelope. The largest exclusive elapsed phase is now the call to
`Device::submitCompatibilityReplayDrawBatched`: **11.819 / 12.700ms/frame**.
Sparse dispatch outside draw preparation is only 1.450/1.592ms/frame, and
captured binding attachment is 0.762/0.832ms/frame.

This is a narrower operation owner, not a certified removable CPU budget.
Cache reconstruction, run ownership, admission and disabled diagnostic costs
are still combined inside that call. Do not rewrite ownership or claim a
12ms saving from the elapsed measurement.

## Treatment and qualification

The H584 A renderer, Clear OFF, canonical PE and rendering policies remain
unchanged. Four private translation units derive mechanically from H593's
qualified final sources. Both the provider archive and the statically embedded
capture provider are rebuilt. No product sources or canonical binaries change.
The measured unix SHA-256 is
`f2e4f9a929e026574877c1db04beeb9bbbdd362502b84c8ea314aa13d2d0a22a`.

Five new nested elapsed scopes cover sparse dispatch, non-sparse dispatch,
sink draw, binding attachment and compatibility preparation. The existing
queue-batch scope remains a child. Unlike H593, this introduces record/draw
clock reads and atomic updates: all operation times include diagnostic
overhead. There are no new per-record CPU syscalls. CPU endpoints still use
H584's two sparse caller-self observations per worker.

ARM64/Rosetta six-process observer tests and parser positive/ten-negative tests
pass. Original H502/r5 OFF/ON preserves seven source boundaries, exact source
and output hash, query transcript, 670 wrapper lifetimes and normal-state
restoration. Ordinary queue OFF/ON preserves completion/API readbacks with
zero Tape observer calls; that fixture skips draws.

The two fixed live episodes both pass seven active spatial knots and natural
game exit0. There were no replacement runs. Only the predeclared catalogue
`missing_capture` waiver applies; screenshots occur after the selected
interval. The computer-use skill supplies startup focus and post-tail screen
inspection, not input during measurement. Both screenshots are coherent;
this is not whole-scene pixel equality. All 14 shared staging files per run
are restored. Independent raw-byte verification reproduces CPU bounds,
exclusive conservation and 28 restoration hashes; no Wine process remains.

## Observed partition

All operation rows are **exclusive elapsed ms/frame**; only the two explicitly
labelled thread rows are CPU bounds. Operation timings across routes are
aggregated, except the explicitly compatibility-only preparation call.

| Scope | O0, 131 frames | O1, 124 frames |
|---|---:|---:|
| Replay thread CPU bounds | 29.160–31.664 | 31.329–31.480 |
| Encode thread CPU bounds | 27.915–30.542 | 29.319–29.521 |
| Compatibility draw preparation | **11.819** | **12.700** |
| Ordinary final queue batch | 8.874 | 9.401 |
| Non-sparse dispatch | 1.870 | 1.967 |
| Sparse dispatch excluding draw | 1.450 | 1.592 |
| Deferred StateBlock item | 1.078 | 1.161 |
| Emission planning | 1.051 | 1.129 |
| Draw remainder, excluding binding/compatibility preparation | 0.974 | 1.106 |
| Compatibility resolved remainder | 0.830 | 0.886 |
| Span driver remainder | 0.815 | 0.944 |
| Captured binding attachment | 0.762 | 0.832 |
| Legacy resource marking | 0.334 | 0.381 |
| Post-replay cleanup | 0.199 | 0.224 |
| Planned-route remainder | 0.197 | 0.216 |
| Raw-entry remainder | 0.097 | 0.102 |
| Worker remainder | 0.081 | 0.086 |
| Wire-view import | 0.035 | 0.038 |
| Direct resolved remainder | 0.014 | 0.018 |
| Mutation work item | 0 | 0.000222 |
| Tape observer | 0 | 0 |
| Sum = worker root wall | 30.483 | 32.782 |

H594's resolved remainder is not comparable directly to H593's larger
resolved envelope: the new child phases have been subtracted. In both live
populations, compatibility preparation inclusive equals exclusive time;
there were no nested final queue batches inside this measured call.
The verifier also checks exact draw/dispatch nesting identities.

The missing-start allowance is 322.144ms for O0 and 8.508ms for O1 over the
whole interval. O1's conservative preparation CPU upper, including that
allowance, is 12.768ms/frame. Its nonzero CPU lower is not established.
Individual uppers repeat the same allowance and must not be summed.
O1 Replay CPU exceeds Encode under this detailed observer, but O0 bounds
overlap; this does not establish a unique observer-free pipeline bottleneck.

| Invocation count | O0 | O1 |
|---|---:|---:|
| Worker roots | 59,821 | 59,474 |
| Resolved Direct | 262 | 259 |
| Resolved compatibility | 30,204 | 30,036 |
| Sink draw calls | 507,562 | 505,075 |
| Compatibility preparation calls | 497,327 | 494,830 |
| Ordinary queue batches | 34,103 | 33,910 |

All roots complete, have zero faults, and belong to the selected Replay
thread/queue; standalone roots are zero. Raw ordinals are contiguous
672,968–732,788 and 656,150–715,623. Cold capture still differs: 199 Direct
versus 61 compatibility resolved invocations; 2,828 of 3,982 sink draw calls
enter compatibility preparation. Do not substitute that route mix for live.

## A concrete diagnostic-cost candidate inside the owner

`src/d3d9/core_draw.cpp`, `PerfScope`, checks whether its callback is non-null
before reading `steady_clock::now()`. Several scopes in
`cachedBaseDrawStateForSubmissionBatch` pass non-null callbacks unconditionally.
The callback eventually reaches `src/dxmt9/dxmt9_perf_counters.cpp`, `add`,
which checks `enabled()` **after the clock reads have already occurred**.
Inner uniform-hash scopes have their own gate; do not treat every scope as
ungated.

The measured dylib's cache function retains calls to the imported
`steady_clock::now()` stub. `clock-audit.json` records the image hash,
disassembly and Mach-O indirect-symbol mapping. The audit finds 49 static
clock call sites in the function, including alternate/error paths: **this is
not 49 calls per draw**, a dynamic frequency, or a measured saving. The initial
audit expected imported names in disassembly comments and rejected that parse;
the final audit resolves the actual stub address through the indirect-symbol
table rather than trusting objdump's misleading nearest-symbol label.

## Next bounded test

1. Price the cache's disabled diagnostic clocks with a same-image/private
   gating treatment, preserving enabled diagnostics and original rendering
   effects. Use caller-self CPU and explicit sampling/observer bounds.
2. Split the remaining compatibility call into cache hit/refresh/rebuild,
   identity/admission, retained no-growth reserve, owned-run construction and
   flat draw append. Keep the final queue batch separate.
3. Only then choose a representation change. `CompatibilityBatchRun` in
   `include/dxmt9/core_snapshots.hpp` owns `CanonicalDrawState` and the full
   `DrawUniformPayload`; borrowed cache pointers cannot replace these without
   preserving generations, spans and source-local run lifetime.

The largest individual default-live CPU operation and correctness-qualified
removable amount remain open. H594 makes no production promotion, FPS claim,
commit or goal-completion claim.
