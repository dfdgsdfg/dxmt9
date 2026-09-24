---
domain: present-pacing
workload: STALKER CoP Day
title: "H354: Bounded indexed prepared dictionary admits wide sources"
type: evidence
status: partial-attribution
updated: 2026-09-23
source: experiments/output/prepared-index-h354/{native-final.log,x86-tests.log,tla.log,build-unix-final.log,build-pe64.log,build-pe32.log,stalker-runner.log,stalker-on.csv,stalker-on.json}; experiments/output/app-d3d9-stkcop-bench-prepared-index-h354-day/{result.json,actual.png,daybenchmark.result}
related: present-pacing-prepared-cardinality.353.md
---

# H354: 512 identities, bounded construction lookup

Parent `88c41296` plus indexed-dictionary implementation. Prepared source
storage now holds 512 full-width canonical identities. The builder privately
owns 1024 `uint16_t` index+1 open-address slots (zero is empty), using full-key
equality, bounded linear probing and deterministic first-occurrence indices.
It resets scratch on every construction, including workspace reuse. Scratch
never follows the immutable plan owner into Encode. Overflow still invalidates
the whole unpublished plan; command/access/range limits are unchanged.

Immutable storage grows from 149,576 to **151,624 bytes**, exactly 2 KiB.
Builder-only lookup costs another 2 KiB. Shared constants own physical identity
capacity, admission validation and coverage-array sizing. Standalone structural
validation deliberately retains its independent duplicate scan: this step
optimizes interning, not every seal operation or the whole pipeline.

## Deterministic evidence

Native plan/shadow/supply tests 3/3, x86_64 plan/shadow/install-name tests 3/3,
TLA regression 212.36s, canonical Unix and both PE builds pass. Boundary tests
exercise 256/257/512/513 identities and 513 full-generation keys deliberately
colliding into bucket 1023, forcing wrap-around and long collision chains.
An independent first-occurrence oracle checks dictionary order and read/write
access indices. Workspace histories cover 512 success -> 513 rejection ->
single-entry recovery -> 512 success, plus prior exhaustive failure/reuse cases.
Configured 513 capacity rejects before effects. The cardinality fixture retains
an explicit 256 test limit so historical overflow coverage remains non-vacuous.

## Actual-input evidence and exclusion

Same Day/high, renderer_r2.5, 1280x720, Sikarugir cx24.0.7 perf configuration
and encoded Present window 2450–2470 as H353. No build/test ran during the
benchmark. Unix artifact hash prefix `3e2fd9a2bea450ee`; all PE hashes match
H353. Original serial Encode execution remains unchanged.

Runner PASS, return code 0, no timeout, elapsed 393.157s, zero GPU command-buffer
errors. **Visual/performance qualification is not accepted:** the runner could
not find an onscreen STALKER window and `actual.png` captured Orca instead.
The native result reports 8.464481 FPS, but focus/window conditions are not
qualified and input differs. Do not attribute this drop to, or rule out a
regression in, the indexed implementation without a controlled repeat.

The bounded trace closes with zero overflow/incomplete writers:

| Item | Result |
|---|---:|
| Qualified sources | 831 |
| Query / no-work skips | 768 / 21 |
| Structural preparation successes | 42 |
| Capacity or other preparation failures | **0** |
| Successful sources exceeding 256 identities | **21** |
| Wide-source identities | **383–415** |
| Wide-source unresolved rows | **0** |
| New allocation / reuses | 1 / 41 |
| New allocation bytes | 151,624 |
| Total observer CPU | 16.799 ms |
| Acquire / append / seal CPU | 0.207 / 8.171 / 1.868 ms |

This establishes that actual wide sources can now seal; it is not fixed-input
equivalence to H353's rejected sources (412–429 identities), nor an FPS benefit.
No cardinality fallback pass executes because this sample has zero rejections.
Larger accepted plans also perform work the old 256-entry builder abandoned;
inclusive timing cannot isolate lookup savings.

## Remaining gates

Repeat in-scene wild qualification with confirmed focus/window placement.
Compare indexed and linear construction on identical input before claiming CPU
savings. Existing pass/certificate capacity remains 256 and is a separate
contract: joint Slot-plan publication, consumer integration, alias lifetime,
independent Encode differential and non-degenerate GPU exact tests remain open.
No parallel policy promotion or async ownership claim is made.
