---
domain: present-pacing
workload: Isolated Rosetta, Wine PE32 and PE64 clock fixtures
title: "Low-cost ordered elapsed clocks are feasible; root-wall CPU envelope remains unintegrated"
type: evidence
status: clock-and-reducer-preflight-only-no-default-attribution
updated: 2026-10-14
source: traces/stkcop-root-wall-preflight-h522/{contract.md,probe.c,run.py,receipt.json,run.stdout,analyze.py,analysis.json,pe-contract.md,probe-pe.c,pe-receipt.json,execute_pe.py,pe-runner-correction.md,pe-execution.json,analyze_pe.py,pe-analysis.json,root_wall.hpp,root_wall_spec.cpp,test_reducer.py,reducer-tests.json}; traces/stkcop-root-wall-preflight-h522/{i686,x86_64}-execution.stdout; traces/stkcop-root-wall-preflight-h522/{i686,x86_64}.asm.txt
related: present-pacing-primary-gap-live.521.md; present-pacing-root-game.477.md; present-pacing-primary-inplace-bound.460.md
---

# A distinct conservative envelope, not another all-call CPU clock

H521 closes the tiny post-Present subset. H522 tests a different way to bound
mixed app-thread renderer work: **the sum of elapsed durations of disjoint
outer D3D9 calls bounds the CPU executed inside those calls from above**.
Nested callbacks stay included; blocked time also stays included and can make
the bound too loose. This is not an exact CPU estimate, sampled residency,
or occupancy multiplied by whole-thread CPU.

The method would permit rejecting Primary as the largest renderer owner only
if its qualified upper bound were below a same-window worker CPU lower bound,
with full ingress/identity and default-transfer evidence. H522 does not provide
that runtime comparison, a default owner ranking, or removable work.

## Finite-host clock tests

The standalone Rosetta executable compares `lfence; rdtsc; lfence` against
enclosing Mach reads in 64 busy/sleep intervals. Every RDTSC value lies inside
the corresponding absolute Mach bracket on this host. The observed conversion
is one nanosecond per tick. Do not generalize that conversion to other systems
or interpret nanosecond units as nanosecond resolution.

PE32 and PE64 console fixtures repeat 64 busy/sleep intervals each using Wine
QPC as the reference. All contain the same one-nanosecond hypothesis after the
predeclared two-QPC-tick endpoint allowance. Disassembly verifies all four
RDTSC sites per fixture have LFENCE immediately before and after the read.
This checks emitted guest instructions, not a universal Rosetta ordering proof.

Each architecture also runs 36 alternating-order batches (four rounds, three
work sizes, three clock modes), 131,072 operations each. Work checksums agree;
no observed clock reversal occurs. Empty-body batch values are:

| Fixture | Plain | Ordered RDTSC pair | Reference clock pair |
|---|---:|---:|---:|
| Native Rosetta, CPU ns/operation | 1.88–2.81 | 18.43–27.35 | Mach 24.67–36.39 |
| Wine PE32, elapsed ns/operation | 6.04–8.18 | 28.58–37.88 | QPC 257.59–286.70 |
| Wine PE64, elapsed ns/operation | 2.19–2.94 | 20.97–28.03 | QPC 86.61–110.33 |

These complete batch values include loop, call, clock and accumulation work.
They are not individual read latency, default game overhead, or estimates of
renderer savings. PE values are **wall time**, not guest CPU. Nonempty-body
batch ranges overlap and must not be used to subtract a synthetic hook charge.

## Root accounting prototype

`root_wall.hpp` is a thread-confined diagnostic reducer, not production code or
a cross-process publication protocol. It reads only at outer entry/exit,
counts nested application callbacks once, and retains wait duration. A caller-
supplied endpoint uncertainty contributes twice per completed root to the
upper tick sum. The reducer does not establish the clock's uncertainty bound.

Twenty cases pass independently on ARM64 and Rosetta, including one million
inactive calls without clocks, nested calls, disjoint-root gaps, zero duration,
activation/snapshot inside an active root, stale PID/birth/TID/epoch,
out-of-order destruction, excessive nesting, clock reversal, arithmetic
overflow and preserved waits. Invalid results cannot be read as valid totals.

## Provenance and limitations

Both PE builds succeed without warnings. The first runner then rejects a
nonexistent `bin/wine64` path before execution. Its receipt/source are retained;
the executor uses the installed `bin/wine` wrapper and records its real binary.
PE32's image hash is frozen before execution. PE64's hash is recorded afterward
because the interrupted builder had not flushed its second image entry; do not
describe that run as fully pre-frozen binary provenance. It is supplementary
instruction feasibility, not a renderer qualification episode.

No catalogue game, renderer build/staging, runtime knob or production source
changes. The console processes and Wine server terminate. Canonical provider
hashes remain unchanged. No FPS claim, commit or push.

## Next gate

Qualify the short-interval counter enclosure/quantization and integrate this
reducer into H518's already-tested **isolated** ingress wrappers. Require
actual-object baseline parity, nested callback behavior, current-epoch
activation, coherent outside-root cumulative snapshots and conservative window
clipping before live use. Record all wrapper cost in the default-transfer
comparison; clock speed alone is not permission to subtract it. If waits make
the bound overlap worker CPU, report the method inconclusive rather than
subtracting guessed waits or silently switching it into an exact estimator.
