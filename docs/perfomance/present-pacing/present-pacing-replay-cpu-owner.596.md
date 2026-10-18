---
domain: present-pacing
workload: STALKER Day high 1280x720 max-threads2 snd_targets16 late spatial interval
title: "Sampled Replay CPU isolates cache misses and owned-run materialization"
type: evidence
status: sampled-attribution-complete-removal-unconfirmed
updated: 2026-10-16
source: traces/stkcop-replay-cpu-owner-h596/{contract.md,trace.hpp,native.json,parser-tests.json,parser-tests-v2.json,analysis-amendment.json,build.json,preflight.json,inputs.json,run.py,analyze_live_v2.py,parse_partition_v2.py,verify.py,verification.json,copy-audit.json,canonical-run-disassembly.txt,canonical-indirect-symbols.txt,observed-run-disassembly.txt}; traces/stkcop-replay-cpu-owner-h596/{O0,O1}/{receipt.json,self.bin,partition.bin,analysis.json,post-tail.png,restore/restored.json}
related: present-pacing-cache-clock.595.md; present-pacing-replay-operations.594.md; present-pacing-reserve-recurrence.588.md
---

# The next concrete candidate is owned-run direct construction

Two fixed live episodes separate [H594](present-pacing-replay-operations.594.md)'s
compatibility-preparation envelope. Cache misses and owned-run construction
are substantial CPU owners; admission, reserve and flat draw append are much
smaller in both sampled populations. Final queue batch remains large. Its
CPU bounds overlap cache's, so **no unique largest operation is certified**.

A separate code-generation audit finds a concrete redundant-copy chain in
the unmodified core: the 8,112-byte hot state is copied from cache to a
constructor argument, then to a local run, then into the vector. The next
experiment should construct the owned run directly at its final vector
location. Do not replace ownership with a pointer into the mutable cache.

## Measurement boundary

The private image derives from H594 with additional cache hit/miss, admission,
reserve, owned-run and flat-append scopes. It retains default rendering policy
and the original cache clocks; **H595's clock-removal gate is not included**.
Only five private translation units change. Product source remains untouched.
Unix SHA-256:
`fdb2f752aa90ce41f1eed6d139787b4e534d9e0d5cb375d42a53e5b7bbe0aeef`.

All admitted roots are counted and qualified by process birth, queue, thread
and raw identity. Fixed SplitMix64 selection samples approximately 1/64 of
roots before their execution. Unsampled roots retain nesting identity but
take no detailed phase clocks or per-root CPU readings. Selected roots have
exclusive phase wall accounting and two bracketed caller-self CPU endpoints.
The mmap contains 8,192 bounded rows; neither run drops rows or reports faults.

For root wall `W`, observed CPU difference `D`, and endpoint/read placement
slack `S`, root CPU is bounded by `[max(0,D-4000-S), min(W,D+4000)]`.
A phase/group with exclusive wall `P` receives
`[max(0,rootLower-(W-P)), min(rootUpper,P)]`. The inherited allowance is
2us per CPU endpoint. Bounds include observer overhead; no overhead correction
is invented. **Sample totals are not multiplied by64 or presented as exact
per-frame population costs.** Concurrent Replay/Encode CPU is not additive
frame latency.

## Results

| Whole selected interval | O0 | O1 |
|---|---:|---:|
| Completed frames | 117 | 124 |
| Replay CPU, ms/frame | 30.189–30.330 | 30.019–30.128 |
| Encode CPU, ms/frame | 29.304–29.688 | 29.272–29.498 |
| Admitted roots | 57,877 | 59,345 |
| Sampled roots | 904 | 934 |
| Sampled root CPU, total ms | 55.352–60.770 | 59.532–65.351 |

The following values are **total CPU milliseconds over each sampled
population**, not ms/frame and not matched-work A/B comparisons:

| Exclusive operation/group | O0 CPU bound | O1 CPU bound |
|---|---:|---:|
| Final queue batch | 12.320–18.134 | 12.933–17.836 |
| Cache total | 8.589–13.083 | 9.778–14.443 |
| Cache miss, subset of cache total | 7.258–11.302 | 8.585–13.091 |
| Owned-run construction | 7.347–10.731 | 7.934–11.403 |
| Admission | 0.174–1.013 | 0.162–1.219 |
| Reserve | 0–0.026 | 0–0.029 |
| Flat draw append | 0.005–0.367 | 0.013–0.429 |

Owned-run construction occurs 5,906/6,924 and 6,299/7,687 compatibility
preparation calls: 85.3% and 81.9%. Cache lookups number 2,278/2,630;
1,432/1,696 are misses. Counts describe this sampled live population, not
the cold capture route mix. Final queue batch excludes separately nested
mark/observer scopes. Tape observation calls are zero in both live episodes.

Replay exceeds Encode under this diagnostic image. This is not an
observer-free permanent bottleneck classification or an FPS benefit claim.

## Qualification and disclosed corrections

- Eight ARM64/Rosetta native processes pass sampled/all/absent/open-failure
  coverage, including overflow, identity, nesting and CPU-failure handling.
- Exhaustive bounds tests cover 586,752 trajectory/phase combinations.
  Four capture/ordinary controls pass. Original H502/r5 retains exact source
  and output pixels, seven sources, query transcript and 670 wrapper lifetimes.
  The ordinary fixture skips draws and proves completion/readback only.
- Both live episodes meet all seven active spatial knots and natural game
  exit0. Computer-use is limited to startup inspection/focus and post-tail
  screenshots. Screenshots look coherent; they are not full-scene pixel oracles.
  Only catalogue `missing_capture` is waived.
- The frozen parser rejected O0 reserve rows274/291 because one completed call
  had zero measured duration. This is valid at finite clock resolution.
  Original scripts and inputs remain intact. A separate v2 parser accepts
  positive call counts with zero duration, retains conservation/identity/CPU
  checks, and passes a zero-duration positive case plus 20 negative cases.
  Selection, bounds, runtime image and live population are unchanged.
- `verify.py` imports neither analyzer nor estimator. Raw decoding reproduces
  CPU/group arithmetic, exact hash-selected census, source order and identities;
  all28 restoration hashes pass and no Wine remains.

## Binary evidence and next gate

`copy-audit.json` maps call targets through Mach-O indirect symbols rather
than trusting generic stub labels. In the canonical H584 image, the non-growing
new-run path contains the following hot-state copies, all 8,112 bytes:

1. `0x26574`: cache to stack argument (`rbp-0x7fe0`).
2. `0x26709`: that argument to local run (`rbp-0x5490`).
3. `0x26b3f`: local run to vector destination.

The H596 image retains the same chain. Uniform bytes first copy from cache
to the local run (`0x2a54` bytes), then move within the larger `0x2b90`-byte
trailing run region into the vector. Static copies include real ownership
work; not every byte or all measured construction CPU is removable.

The source owners are `src/d3d9/core_draw.cpp`,
`Device::submitCompatibilityReplayDrawBatched`, and
`include/dxmt9/core_snapshots.hpp`, `CanonicalDrawState` and
`Device::CompatibilityBatchRun`. The canonical constructor takes state/layout
by value; `push_back(std::move(run))` then moves the local aggregate.

Next, test a narrowly scoped borrowed-input constructor that **copies into
owned members** and emplaces the run at its reserved final location. Preserve
shader/stream references, debug values, generation identity, binding-span
stability, capacity behavior and fail-closed exceptions. Compare generated
copies first, then exact capture effects and operation-local CPU. Removing
the intermediate object must not weaken rollback or resource lifetime.

No production promotion, CPU removal amount, FPS improvement or goal completion
is established. The unique largest default-live CPU operation remains open;
the new evidence supplies a concrete implementation candidate, not another
reserve/headroom hypothesis or extended broad FPS population.
