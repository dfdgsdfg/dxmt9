---
domain: present-pacing
workload: STALKER CoP frozen H502/r5 capture
title: "Clear fusion saves 1.8055 ms paired-median source Encode CPU in a finite local screen"
type: evidence
status: private-local-economy-passed-live-transfer-open
updated: 2026-10-15
source: traces/stkcop-clear-economy-h552/{contract.md,run.py,rows.jsonl,summary.json,verify_raw.py,independent-verification.json}; traces/stkcop-clear-backed-null-h551/{build.json,capture-serial-receipt.json,capture-fused-receipt.json,tla-suite.log}
related: present-pacing-clear-backed-null.551.md; present-pacing-clear-caller.549.md; ../../../specs/backend/gap.md
---

# Finite local CPU result, not a live FPS claim

H551's private Clear-fusion candidate passes the predeclared H552 economy
screen. Source 5's whole Encode CPU saves **1.8055 ms paired median**, exceeding
the maximum absolute A/A difference of **0.482 ms**, with **8/8 positive pairs**.
This establishes a removable local cost on one frozen captured input, not the
largest default-live CPU owner or a per-frame/FPS improvement.

## Fixed comparison

The same H551 binary runs with the private extension disabled (A) or enabled
(B). The original H502/r5 capture, source boundaries, predicate and image remain
fixed. Every execution uses a fresh process and device; same-device warm replay
is not valid for this V4 input. Two warmups per arm precede four A/A pairs and
eight alternating AB/BA pairs: 28 executions, with no retries or sample extension.

Witness logging and per-command/per-draw clocks are disabled. Both arms use
the same coarse supply/planning observer. The primary metric is executing-thread
CPU between source-5 Encode begin/end (local sequence 8, 591 commands).
The frozen gate requires median paired saving above maximum absolute A/A
difference and at least seven positive pairs out of eight.

| Scope | A median ms | B median ms | Paired saving median ms | Max absolute A/A ms | Positive pairs |
|---|---:|---:|---:|---:|---:|
| Source 5 whole Encode — primary | 35.9905 | 34.1895 | 1.8055 | 0.482 | 8/8 |
| Source 5 command body — nested secondary | 19.2315 | 17.3960 | 1.7705 | 0.539 | 8/8 |
| Sum of seven original-source Encode scopes — secondary | 59.7995 | 57.9150 | 1.6995 | 0.587 | 8/8 |

These scopes overlap and must not be added. A median of paired differences is
not the difference of arm medians. Primary pair savings in execution-pair order
are 2.566, 1.368, 1.669, 1.117, 2.030, 1.036, 2.077 and 1.942 ms.

## Correctness and independent verification

All 28 executions pass original source/output pixels, seven source mappings,
the production-counter query transcript and balanced resource lifetimes.
H551 separately witnesses all 65 intended fusions (63 depth-only, two color-only)
and tests ordinary/exact-store attachment readbacks and failed-open ownership.
Original source/output SHA-256 remains
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.

`verify_raw.py` independently recomputes all 28 primary measurements directly
from raw CSV Encode begin/end events, checking source mapping, same thread and
owner, complete non-overflowed traces and artifact hashes. It does not import
the timing analyzer. A/A differences, paired deltas and the frozen primary gate
match `summary.json`, SHA-256
`9053d14f926680df4eabb35d8869f523b1e3ed34f9d0f642879120af8326576f`.

## Limits and next gate

The measured saving includes the coarse observer, whose independent OFF/ON
cost is not calibrated. Fine-observer campaigns are not comparable absolute
baselines. Captured content authority remains unknown and timing qualification
is zero; production-counter queries do not prove hardware visibility semantics.
Host snapshots are recorded, not interpreted as proof of an interference-free
live workload. The TLA regression starts only after these measurements end
and exits zero: 242 configurations, including 167 expected counterexamples.
This existing suite does not prove the new Metal attachment behavior or the
private caller's live queue failure settlement.

Canonical encoder sources, Wine staging and runtime defaults remain unchanged.
Before live transfer, qualify queue terminal/failure settlement and ordinary
query/store policy. A supervised live comparison must independently establish
prevalence and savings; default late-tail Primary versus Replay/Encode ownership
remains open. The larger captured draw-loop cost remains a separate candidate.

### Promotion blocker found in the queue wrapper

Static follow-up identifies a concrete boundary for the next fault test.
`dxmt9_command_queue_cpu_ready_session.cpp::runCpuReadySessionEncodeLoop` has
an `encodeWithBorrows` callback that catches encoding exceptions and returns
`std::nullopt`. Its ordinary source loop handles a missing submission through
`completeInlineSnapshot` after pending-session handling. Separately,
`dxmt9_draw_encoder_chunk.mm::encodeChunk` can translate implementation
exceptions into a missing submission for borrowed lookahead or query scopes.

H551's direct caller fixture has neither lookahead borrows nor query scopes.
It proves the private failed-open exception retains pending Clear identity;
it does not prove that the production coordinator preserves that failure.
Thus throwing before clearing the pending identity is not, by itself, a
queue-level terminal guarantee. Do not promote the private caller unchanged.
The next deterministic test must exercise the actual coordinator and distinguish
failed encoding from legitimate no-submission/inline completion, including a
previously encoded pending session. This is code-path evidence, not an observed
live dropped-Clear incident or a new measured performance regression.
