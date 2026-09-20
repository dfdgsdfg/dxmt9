---
domain: present-pacing
workload: STALKER CoP Day
title: "H337: the blocking head is backend CPU, not prefetch or relock"
type: evidence
status: partial-attribution
updated: 2026-09-20
source: experiments/output/app-d3d9-stkcop-bench-encode-phases-r677-day/{result.json,actual.png,supply.csv,waits.csv,progress.json}; experiments/output/encode-phase-validation-h337/{dxmt9-encode-phase-native-tests.log,dxmt9-encode-phase-unix-tests.log,dxmt9-encode-phase-tla.log}
related: present-pacing-stalker-publication-progress.336.md
---

# H337: sequence-qualified encode phase ownership

Base `73d6b947` plus an observation-only overlay brackets ordinary Slot
prefetch and backend calls in the existing bounded CPU-supply trace. Four
events per source share the outer queue/sequence/TID. Adjacent endpoints
partition the outer interval into source preparation, prefetch eligibility/work,
context preparation, backend processing and return/relock. No runtime policy,
Slot/wire field, per-draw clock or query semantics change is introduced.

## R677 qualification

STALKER Day/high 1280x720, Sikarugir cx24.0.7, perf profile. Metal occlusion,
async fences, covered Pending and publication resource dedup ON. Tape,
scoped CB join, END coalescing, query/receipt/backend observers and PE sampler
OFF; query split and per-frame observations ON. Wait window 2200–2300;
supply window 2190–2310. Runner PASS, 2614 encoded Presents, no timeout,
capture error or recorded failures. The screenshot shows the game; this is
spot evidence, not continuous pixel equivalence.

PID 13796: supply closes with 217,862 rows, zero overflow/incomplete writers;
wait trace closes with zero overflow/incomplete/unknown rows. All 3125 complete
sources have phase endpoints, including all 55 publications of the selected
query-54 target. Staged hashes/build directories are preserved in result.json;
the Unix artifact hash prefix is `e652c4953bf82933`.

No task-owned tests/builds overlap the wild run. Host `dasd` was consuming
substantial CPU before launch; interference-free execution is not claimed.
This diagnostic is not an observer-OFF FPS comparison, nor an overhead A/B.

## Target and disjoint encode phases

Longest matched query-54 receipt: owner 1, raw 387920, frame 2298, replay
TID 1400159. Receipt elapsed 19.135167ms; whole raw worker service 26.020ms
wall / 4.460ms CPU with nested cap wait 19.615ms. These overlap and must
not be added. The raw publishes sequences 11443–11497. The first source has
293 backend commands; the other 54 have one command each. This is the same
prelude-plus-scope-closures pattern as H336, not the identical raw workload.

The first source is also the only retirement observed during the cap wait,
reducing inflight from 31 to 30. Its encode interval is:

| Disjoint scope | Wall ms | Native CPU ms |
|---|---:|---:|
| Source/borrow preparation | 0.015 | 0.015 |
| Prefetch eligibility/work | 0.896 | 0.892 |
| Context/options preparation | 0.057 | 0.012 |
| Backend call | 13.986 | 13.965 |
| Return/relock | 0.007 | 0.006 |
| Outer encode total | 14.961 | 14.890 |

Backend processing owns 93.79% of this measured source CPU; prefetch owns
5.99%. Remaining CPU is 0.033ms. These are scope costs including observation,
not exclusive instruction samples or a claim that all backend work is waste.
The remaining 54 sources have median 0.146ms / maximum 0.523ms encode CPU.

For the head, publication-to-encode is 0.052ms; encode-end-to-submit 0.155ms;
submit-to-completion-dequeue 0.043ms; dequeue-to-completion-observation 5.962ms;
observation-to-retirement 0.073ms. Completion observation is after the CPU
wait, not a hardware GPU timestamp. Across sources these intervals overlap.

## Decision

This example does not support prioritizing queue relock, source borrow or PSO
prefetch over backend processing. It reproduces the two interacting owners:
the expensive prelude and many small scope sources consuming inflight credit.

The next measurement must split `FrameGraphBackend::onSourceReadyImpl`
planning from `encoders::encodeChunk`, retaining queue/sequence identity and
native CPU endpoints. The unscoped prelude may enter framegraph construction
and optimization; scoped sources bypass that planning branch. Therefore the
13.965ms cannot yet be called draw encoding, driver time or framegraph cost.
Only after this split should finer draw/binding phases be considered.

Source batching remains a separate semantic change: per-command scope
ownership and END receipt resolution must be transformed together. Removing
only scope publication moves publication to END; removing publication without
replacing the source-wide scope representation corrupts query membership.
No cap increase, scoped-CB promotion or scope merging is justified by this run.

## Verification

All four canonical builds pass. Native focused checks pass 4/4; x86 Unix
CPU-supply/install-name checks pass 2/2. TLA regression passes in 196.56s,
before CPU calibration and before the wild run. Python analysis tests pass
19/19, covering partition sums, missing/duplicate phase events, cross-thread
and invalid/nonmonotone clock rejection. Old or unsupported-path traces retain
unqualified phases rather than zero-filled costs. These are observation gates,
not a proof for a new query batching policy.
