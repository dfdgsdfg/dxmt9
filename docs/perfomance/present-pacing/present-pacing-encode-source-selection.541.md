---
domain: present-pacing
workload: STALKER CoP H502/r5 original Normal input
title: "Encode source selection; same-device probe rejected before timing"
type: evidence
status: source-body-localized-warm-probe-invalid-no-removal-budget
updated: 2026-10-15
source: traces/stkcop-warm-encode-h541/{contract.md,source_probe.cpp,build.py,build.log,build.json,run.py,admission.log,admission-receipt.json,audit_failure.py,failure-audit.json,source_costs.py,source-costs.json}; traces/stkcop-prefix-economy-h537/{build-receipt.json,rows.jsonl}
related: present-pacing-prefix-live-result.540.md; present-pacing-prefix-economy.537.md; present-pacing-production-query-diagnostic.506.md; ../../../specs/experiments/gap.md
---

# Localize existing Encode evidence without another live campaign

H540 does not establish live economy for prefix preparation. H541 therefore
examines Encode, preserving all earlier negative scratch, raster-cache and
vertex-immediate experiments. No production source, build image, Wine staging,
runtime default or README result changes. The global default-thread ownership
and removable-CPU objective remains open.

## Existing baseline traces: source 5 owns most command-body CPU

`source_costs.py` rechecks **all 18 H537 A coarse-trace executions**, including
its two warm-up-labelled fresh-device runs, eight A/A runs and eight A/B baseline
runs. This is retrospective descriptive analysis, not a newly frozen effect
test. Every source/output digest and query transcript passes again. The reducer
authenticates current frozen images, retains input-log/trace hashes, joins the
seven original source maps to unique queue sequences, and independently
recomputes 126 raw command-body CPU endpoint pairs. No new timing run occurs.

All scopes use the executing thread's self counter. H513 distinguishes that
from H512's stale external-counter reads. Coarse observers remain included;
their default-path transfer is not established. Setup sources are excluded by
the original V4 source mappings, not by elapsed-time cuts.

| Scope, ms per captured interval | Minimum | Median | Maximum |
|---|---:|---:|---:|
| Whole Encode | 61.138 | 61.742 | 63.569 |
| PSO prefetch, including fresh-device cold work | 29.004 | 29.358 | 30.372 |
| FrameGraph planning/setup | 1.636 | 1.695 | 2.000 |
| Command body | 26.563 | 27.173 | 28.541 |
| Remaining Encode work | 3.450 | 3.531 | 3.765 |

The latter four are disjoint partitions per execution; independently computed
medians need not sum to the median whole. These units are **not ms/frame**,
steady-state game costs or FPS predictions. Prefetch is not a removable budget.

| Original source | Commands | Body CPU median |
|---|---:|---:|
| 1 | 228 | 6.207 ms |
| 5 | 591 | 20.151 ms |

Source 5's median per-execution share is 74.16% of body CPU; sources 1 and 5
together account for 97.15%. The five other sources each have one command.
This selects source 5's actual execution body over another FrameGraph planning
micro-optimization. It does not distinguish required Metal/driver work from
removable preparation inside that body. H486's older intrusive per-draw split
must not be transplanted onto this different input.

## Rejected same-device experiment: a private caller error

Before that reanalysis, a private runner attempted to reuse H537 A's device for
three invocations of the unchanged H502/r5 prepared input. Admission required
exact original pixels and queries before either planned ten-iteration CPU trace.
The first invocation passes. The second returns `output-mismatch`; the runner
exits 5, skips the third invocation and launches **neither timing trace**.

Both completed invocations preserve seven mapped sources, 670 created/released
wrappers, nine Issues, five polls and four required production-counter results.
Those facts do not establish mutable-state reset or image equivalence.
Source and output readbacks both change in 12,703 of 921,600 pixels, comprising
22,437 RGB bytes and zero X-channel bytes. The changed bounding box is
`(0,0)..(1279,337)`; no channel masking or tolerance is accepted.

- First source/output SHA-256:
  `1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
- Second source/output SHA-256:
  `deaa672c8b0f38430c1e145e25d67322105de6248ef336ee39c48c85a9fc9e5c`.

The cause established here is **experiment contract misuse**, not a new product
regression: `replayPreparedRenderTapeIdentityV4` explicitly requires a fresh
device per call in its header and replay specification. Its implementation
records `FreshDevice` and creates fresh invocation-local execution evidence;
it does not reset a reused device. The legacy `resetPreparedDevice` helper is
not invoked by this named V4 API. The existing V4 in-process reset gap already
requires control closure and A-to-B-to-A qualification. The specific retained
state responsible for these pixels has not been isolated.

The runner should have honored that documented precondition before launch.
The failed probe is retained as a warning, not promoted into a warm-device
benchmark, production bug report or performance conclusion. Actual dyld rows,
image/build-input hashes, original receipt and raw pixels are independently
checked by `audit_failure.py`. The Metal environment check passes: Xcode 27,
Metal compiler 32023.921 and Apple M1/Metal 4. No capture or profiler is active.

## Decision

Keep fresh-device V4 semantics. Do not expand reset/cache reuse merely to make
this invalid timing experiment pass. The next bounded owner question is
source 5's DrawRun/pass/driver execution, with coarse command-body CPU retained
as the control and original readbacks required. Any finer observer is for
candidate discovery until its interference is qualified. Live warm-prefetch
ownership, mixed Primary attribution and correctness-qualified work removal
remain separate obligations. No optimization or default promotion follows.
