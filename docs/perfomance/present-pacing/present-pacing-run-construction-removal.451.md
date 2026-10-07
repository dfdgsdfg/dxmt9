---
domain: present-pacing
workload: STALKER CoP capture29; default compatibility policy
title: "Owned-run direct construction: measured small CPU removal, not the tail bottleneck verdict"
type: evidence
status: bounded-cpu-removal-measured-no-promotion
updated: 2026-10-07
source: traces/stkcop-run-construction-h451/{run.py,analyze.py,analysis.json,summary.json,arm-build-options.json,candidate-direct-core_draw.cpp,candidate-direct-core_snapshots.hpp,candidate-direct-testlog.json,baseline-current-testlog.json}; traces/stkcop-run-construction-h451/{semantic-A,semantic-B}/result.json; traces/stkcop-run-construction-h451/x86/{summary.json,build-options.json,baseline-current-testlog.json,candidate-direct-testlog.json,baseline-current-run-disassembly.txt,candidate-direct-run-disassembly.txt,baseline-current-indirect-symbols.txt,candidate-direct-indirect-symbols.txt}; traces/stkcop-run-construction-h451/x86/{semantic-A,semantic-B}/result.json; experiments/output/stkcop-v4-immutable-preparation-24/paired-l0-r1/input/manifest.json
related: present-pacing-default-tail-running.450.md; present-pacing-default-tail-cpu.449.md; present-pacing-v4-final-grouping.443.md
---

# Direct owned-run construction removes about one millisecond in this capture

H450's sampled copy-callsite envelope is not a removable-cost measurement.
This experiment removes the intermediate canonical/run temporaries and measures
the existing controlled Replay body on the unchanged capture29. No batching,
resource identity, source partition or capacity policy changes are requested.

The candidate is archived, **not promoted**. Working-tree production source and
both native/x86 build images are restored to the baseline implementation. Wine
prefix staging, PE binaries and runtime defaults were not changed.

## Concrete change and compiled mechanism

The original path copies cached hot state into the by-value
`CanonicalDrawState` constructor, then into a local `CompatibilityBatchRun`,
then moves the inline arrays into the reserved vector element. The experimental
candidate adds a reference-taking, in-place canonical constructor and a private
run constructor, and passes the immutable cache members directly to vector
`emplace_back`. It retains an owned snapshot rather than storing cache pointers.

A first, discarded candidate used default `emplace_back()` followed by member
assignment. Assembly showed a new 21,592-byte zero-fill; it is not the measured
candidate. The direct constructor instead initializes the owned members once.
On the x86_64 capacity-available path, the 8,112-byte hot-state copy appears once
instead of three times. The local-run-to-vector trailing copy disappears, and
there is no whole-run zero-fill. The remaining uniform copy is 10,840 bytes in
this generated path. Mach-O indirect symbols identify the memcpy callees;
both baseline and candidate disassemblies are archived.

This is source-local representation elision, not removal of the snapshot's
required resource ownership. Existing run reservation, binding-span lifetime,
draw count, failure handling and publication cuts remain in place. Promotion
would still require the applicable ownership/refinement and GPU/wild gates;
this diagnostic experiment does not declare them complete.

## Build and execution identity

Both arms are rebuilt with the same installed Xcode compiler and SDK. The
current environment needs explicit `SDKROOT` for Meson's direct compiler path
and a process-local LLVM `ar` shim for the existing `csrD` archive command.
No global toolchain configuration is changed. x86 builds include the
`winemetal_unix_install_name_fixup` target. The restored x86 provider's SHA-256
is exactly H450's measured wild image:
`7f45561ddf0fd22f477e3d3447000913aac83c5b61950726fbe17cffc88aece5`.

Separate image directories retain the executable, native library and provider
for each arm. Every child authenticates the manifest, component blobs, selected
policy and actual loaded images before/after execution. The independent reducer
rechecks every receipt, image hash, clock scope, route count and non-overlapping
child interval. Builds and other profiling jobs are finished before each matrix.
External machine load and driver/cache state are not controlled.

Two separate matrices are used; do not compare their absolute times as a pure
architecture experiment:

- ARM64: existing native `debugoptimized` build.
- x86_64 under Rosetta: existing `release` build, matching the wild provider.

Within each matrix: one logical-semantic execution per arm, two warm-ups per
arm, eight A/A pairs, then sixteen A/B pairs alternating AB and BA order. All
108 children across both matrices pass authentication/execution. The existing
`compat-draw-batch-failure` and `cpu-ready-production-routing` tests pass for
both arms on both architectures.

## Preserved work and semantic scope

Every measured arm executes 2,799 compatibility draws in 80 batches, zero
unbatched/Direct/fallback draws. Logical qualification preserves 512 commands,
three sources and 2,806 original-keyed logical items. Within each architecture,
the baseline/candidate logical digest agrees:

| Architecture | Both-arm logical digest |
|---|---|
| ARM64 | `d420b2ef4c6f827c` |
| x86_64 | `a1092af18e4b2e26` |

Cross-architecture digests differ; this experiment does not resolve or qualify
that difference. The existing logical checker is not an independent complete
shader/scalar oracle, and no new GPU exact or wild pixel check is performed.
The legal-NOOVERWRITE/closed-writers assumption is unchanged.

## Measured body CPU

Times cover the **whole captured execution body**, excluding setup,
authentication and teardown, but including Slot work and the immediate-FIFO
controlled CPU consumer through its join. They are process CPU, not exclusive
Replay-thread time, per-Present latency or production Metal Encode time. The
execution-only lane omits expensive semantic observation but retains its
existing perf/route counters; it is not the observer-OFF wild lane.

| Matrix | A/A body CPU medians | Baseline / candidate body CPU medians | Median difference | Faster candidate pairs |
|---|---:|---:|---:|---:|
| ARM64 | 68.725 / 68.816 ms | 68.831 / 67.780 ms | -1.526% | 15 / 16 |
| x86_64 / Rosetta | 103.569 / 103.633 ms | 103.625 / 102.592 ms | -0.996% | 12 / 16 |

Paired CPU savings medians are 1.030 ms on both architectures. AB/BA savings
medians are 0.930/1.066 ms on ARM and 1.063/1.029 ms on x86. Individual x86
pairs range from a 2.281 ms regression to a 3.176 ms saving; its A/A differences
also span -0.771 to +2.198 ms. A/A median drift is 0.132% and 0.062%, respectively,
not a confidence bound. These observations support a small local effect, not a
precise guaranteed saving on every execution.

Corresponding body wall medians are 68.354/67.307 ms on ARM and
102.315/101.235 ms on x86. Neither wall nor summed process CPU may be added to
other pipeline stages to predict FPS.

## Verdict and remaining objective

The intermediate-run copies are real and removable, but this concrete removal
saves only about one millisecond per captured body. H450's 14.65% copy-callsite
sample population must not be presented as an attainable Replay speedup.
Nor is this measurement an upper bound on all possible state/uniform redesigns.

Do not launch another capacity/grouping matrix or promote this candidate from
these numbers. The active question—largest dxmt9 single-thread CPU owner and
its removable amount in the **current default late scene**—remains open:

- H449/H450 rank known Replay/Encode workers, but do not isolate Primary's PE
  share from game/Wine. Raw Rosetta stack artifacts cannot safely fill that gap.
- Capture29 exercises the real compatibility path, but is not a source-identity
  match to H450's six-second tail window. Its local delta cannot price that
  entire window or establish critical-path ownership.
- Encode's `encodeDraw`/DrawRun/pass-entry sample envelopes still include
  required translation and backend work; their removable fractions are unpriced.

The next useful evidence is a current late-window app/PE/Unix CPU split and a
matched-source measurement of the selected remaining owner, not extrapolating
this small local delta into a whole-game explanation. No FPS, README, GPU
utilization improvement, maximum-removal or default-promotion claim is made.
