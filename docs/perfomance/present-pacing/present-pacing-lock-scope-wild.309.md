---
domain: present-pacing
workload: 3DMark05 GT2; 3DMark06 GT2
title: "Lock-scope wild qualification: fewer acquisitions, promotion withheld"
type: evidence
status: promotion-withheld
updated: 2026-09-09
source: experiments/output/lock-scope-H309-20260909/analysis; experiments/output/lock-scope-H309-20260909/fixture; build-x86_64-builtin/meson-logs/lock-scope-runtime.txt
related: present-pacing-lock-scope-refinement.308.md; present-pacing-upload-arena-locks.306.md
---

# Lock-scope wild qualification

Follow-up: [H311](present-pacing-lock-scope-attribution.311.md) reruns this exact
provider and the H310 follow-up. The large 06 deficit below is not reproduced;
retain these original observations, but do not treat them as an independently
attributed lock-refinement regression.

`DXMT9_LOCK_SCOPE_REFINEMENT` remains default-off. Reduced synchronization
work is observable, but 06 GT2 does not establish the required non-regression.
An FPS increase is not required by `R-BACK-SYNC-5`; correctness, intended work
reduction and matched CPU/latency/FPS non-regression are still required.

## Provenance and qualification

This measures `1f3f03582340d126f8fc2f675851de3c7a031372` plus the uncommitted
H306/H308 implementation overlay, not bare HEAD. All three canonical runtime
builds pass, including both PE architectures and the x86_64 Unix provider.
Six x86_64 runtime specs/audits pass, including OFF/ON range and queue tests.
The native and full TLC evidence remains in [H308](present-pacing-lock-scope-refinement.308.md).

`experiments/output/lock-scope-H309-20260909/analysis/run.py` records recipes,
source/build/runtime/image hashes, staging checks and result/log/CSV hashes.
`summarize.py` revalidates those receipts and reconstructs `summary.json`.
All ten benchmark runs use the same five product images. In particular the
Unix provider SHA256 is
`587db86dd4798d10a7e6cf1de31fed78900e59dcd8999737582f49f3cc796dd7`.

The official experiment runner selects Sikarugir cx24.0.7, 05 GT2 at 1024x768
and 06 GT2 at 1280x720. Replay offload is ON; CPU-ready Tape, segmented
transport, inline-constant delta and opaque-depth index optimization are OFF.
Only the candidate gate differs within each comparison. Perf runs use Warn
logging and frame sampling, with the lock and native CPU observers disabled;
they are not fully uninstrumented `.3dr` measurements. Diagnostic runs use
Info logging, the lock observer and a separate two-second native CPU sampler.
No build/test or other benchmark ran concurrently.

All result artifacts report pass and the eight checked health counters are
zero at covered endpoints. 05 ends normally; 06 finishes the full scene and
is then stopped by the expected result-GUI timeout (exit 143). Full-health
coverage excludes the explicitly listed final 06 frames. Artifact qualification
is not a performance or complete visual-correctness verdict.

## Perf results

Execution order within each product is OFF1, ON1, ON2, OFF2. All runs are
retained; no slow run is discarded. FPS is scene frame count divided by the
sum of positive frame wall times, with consecutive `draw_calls > 100` frames.

| Product | Gate/run | Frames | FPS | Frame p95 / p99 (ms) | Uncovered health tail (frames) |
|---|---|---:|---:|---:|---:|
| 05 GT2 | OFF1 | 1303 | 19.858 | 64.873 / 118.963 | 0 |
| 05 GT2 | ON1 | 1328 | 20.294 | 65.747 / 112.078 | 0 |
| 05 GT2 | ON2 | 1272 | 19.368 | 65.335 / 123.572 | 0 |
| 05 GT2 | OFF2 | 1090 | 16.615 | 75.667 / 146.672 | 0 |
| 06 GT2 | OFF1 | 608 | 9.308 | 144.991 / 211.413 | 29 |
| 06 GT2 | ON1 | 446 | 6.773 | 251.480 / 342.412 | 40 |
| 06 GT2 | ON2 | 398 | 5.952 | 259.929 / 393.222 | 34 |
| 06 GT2 | OFF2 | 436 | 6.553 | 211.885 / 306.533 | 21 |

06 ON/OFF is -27.2% for the first pair and -9.2% for the reversed pair.
However, OFF itself drifts from 9.308 to 6.553 FPS; 05 OFF also drifts from
19.858 to 16.615. This is not clean independent attribution of a candidate
regression. Nor does the positive pooled 05 comparison establish a gain.
06 no-regression and tail gates are not passed, irrespective of attribution.

Host process snapshots show substantial Orca Renderer and WindowServer CPU,
with occasional indexing activity. AC power, disabled low-power mode and no
recorded thermal warning do not prove stable CPU frequency or a quiet host.
No claim is made that external load explains all of the decline.

Locality does not show CB fragmentation: all perf runs stay near four CB/P;
05 passes/P stay 15.756–15.780 and 06 19.859–19.873. GPU CB time stays near
2.60–2.64 ms/P for 05 and 14.84–15.11 ms/P for 06. These aggregates are not
an exhaustive GPU oracle or proof that host load is the sole FPS limiter.

## Lock and native CPU diagnostics

Separate 06 OFF1/ON1 diagnostic runs contain 424/458 scene frames, at
6.384/6.903 FPS. Do not use that positive diagnostic FPS comparison to overrule
the perf result: lock instrumentation itself changes the cost of each lock.
Both lock windows span SubmitPresent ordinals `(660, 1020]`, 360 presents,
log-bracketed inside their respective scenes. Equal ordinals are not equal
game-time/source populations. Endpoint table overflow, dropped, inconsistent
and skipped-row counters are zero. Each row is coherent, not a globally
atomic source-qualified snapshot.

| Scope | OFF acquisitions/P | ON acquisitions/P | OFF wait / hold ms/P | ON wait / hold ms/P |
|---|---:|---:|---:|---:|
| App queue, range upload | 9.294 | 0 (row absent) | 0.264 / 0.0175 | 0 / 0 |
| App queue, full upload | 6.697 | 6.719 | 2.041 / 0.940 | 1.317 / 0.878 |
| App arena, Update | 880.639 | 874.044 | 1.948 / 2.022 | 2.449 / 1.930 |
| Non-app arena, Update | 24036.158 | 12414.358 | 4.613 / 5.749 | 2.360 / 4.449 |
| Non-app arena, UpdateMany | 33.986 | 3867.503 | 0.0085 / 0.0047 | 0.799 / 0.811 |

The observed non-app Update + UpdateMany acquisition sum falls from 24,070
to 16,282/P (-32.4%); reclaim is separate and unchanged at one/P.
This confirms the intended acquisition-economy direction, not an
exact count of deduplicated captured stamps: the site contains other Update
work and the scene samples differ. App arena wait rises in this diagnostic
pair, so do not describe every contention dimension as improved.
Non-app Update's process-cumulative maximum hold is 14.213/17.424 ms OFF/ON;
ON UpdateMany is 0.195 ms. These maxima are neither interval maxima nor p99.
Nested queue/arena times overlap and must not be added as saved CPU time.

Native application-thread CPU is 139.356/130.729 ms/frame in partial envelopes
covering 97.64%/97.16% of the diagnostic scene, at 0.908/0.923 average cores.
This includes game, Wine, PE and synchronous Unix, not PE-exclusive CPU.
The large native CPU values preclude labeling all long frame time as lock
wait. Fixed-order, observer-enabled spots do not establish product CPU savings.

## Correctness boundary and disposition

Inspected 05 OFF/ON mid-scene images and a 06 ON scene image show nonblack
rendering without an obvious large geometry failure. These are spot checks,
not full temporal parity or user-confirmed correctness.

The separate `fixture/lock_scope_pixel_fixture.cpp` runs through the official
PE conformance runner with both PE32/PE64 builtin products and OFF/ON: four
processes pass. A DEFAULT+DYNAMIC VB receives DISCARD A, disjoint-range
NOOVERWRITE B, then DISCARD-rotated A. Actual offscreen render/readback retains
the first two triangles and draws the third at its new location. All twelve
interior samples match exactly: `0xffff2040`, `0xff20a0ff`, `0xffff2040` per
process. The fixture's 60 subsequent diagnostic Presents are outside the
pixel oracle and are not performance measurements. OFF records one range
queue acquisition; ON records none while still reporting one 60-byte range
upload. Per-run staging receipts match the benchmark product images; commands
and hashes are in `fixture/validation-receipt.txt`. The fixture is an ignored
experiment artifact, not a newly integrated permanent native test. It does not
force every possible CPU/GPU overlap or arbitrary multithreaded caller order.

Keep the default OFF and README unchanged. The next economic gate is a quiet,
matched repeat; if the 06 deficit persists, isolate range elision, captured
stamp batching and pre-lock preparation rather than treating the combined
candidate as an indivisible optimization. TLA constrains permitted ordering
and lifetime behavior; it does not establish allocator, mutex or CPU timing.
