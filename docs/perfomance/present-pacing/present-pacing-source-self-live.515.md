---
domain: present-pacing
workload: STALKER CoP Day high 1280x720 controlled default, late spatial knots
title: "Live self endpoints work, but Replay boundary frequency is not low"
type: evidence
status: live-self-coverage-passed-default-ranking-unqualified
updated: 2026-10-14
source: traces/stkcop-source-self-live-h515/{contract.md,run.py,inputs.json,clock.cpp,clocks.json,bounds.py,test_bounds.py,analyze_live.py,analysis.json,run1/receipt.json,run1/self.bin,run1/progress.jsonl,run1/catalogue-result.json,run1/post-tail.png,run1/restore/inventory.json,run1/restore/restored.json,run1/benchmark-results/daybenchmark.result}
related: present-pacing-source-self-report.514.md; present-pacing-self-counter-boundary.513.md; present-pacing-cpu-counter-freshness.512.md
---

# Live coverage without extrapolating a diagnostic ranking

One H514-image episode ran with the H510 controlled policy: Day/high/1280x720,
game worker limit two, audio targets sixteen, production-counter occlusion,
draw chunk limit zero, product counters/frame sampling/supply trace OFF. No
renderer optimization or scheduling policy was changed. These audio/worker
controls are not unrestricted game defaults.

All seven established spatial knots were observed with stable running/active
game state. Knot zero armed the reporter; knots one through six define the
accounting window. The game exited zero without timeout. The catalogue retains
its sole administrative `missing_capture` failure because auto-capture was
delayed; a separate post-tail image shows no obvious corruption, not exact
live-pixel equivalence. Fourteen shared images/support/result files are
hash-restored. Canonical build images were never changed.

## What the trace proves

The post-exit parser accepts 255,242 rows with no loss/fault, unpaired scope,
identity mismatch, stale generation, CPU reversal or CPU-over-enclosing-wall
counterexample. There are two explicit queue bindings; the observed worker
scopes identify one Replay thread and one distinct ordinary Encode thread.

| Recorded scope kind | Pairs |
|---|---:|
| Replay chunk | 64,334 |
| Replay StateBlock | 61,731 |
| Ordinary Encode, no Present | 1,282 |
| Ordinary Encode, with Present | 273 |

All 1,555 Encode pairs have raw ordinal zero. The raw-to-source join remains
unavailable; neither queue sequence nor the latest replay raw supplies that
missing authority. CPU cursor bracketing below does not depend on such a join.

Both ARM64 and Rosetta Mach wall clocks fit ten independently bracketed host
monotonic calls before launch. For each external scene snapshot, the analyzer
uses the preceding self query entirely before it and the following self query
entirely after it. It never interpolates. Monotone cumulative CPU differences
include work between recorded scopes as well as within them. An explicit
conservative +/-2 us allowance per cursor is retained; this remains H513's
finite current-host qualification, not a universal OS error theorem.

## Diagnostic-only worker result

The knot 1--6 window spans 116 observed completed frames. Values below include
the reporter's own CPU and are not removable-cost estimates or critical-path
fractions.

| Worker | Whole-thread CPU / observed completed frame |
|---|---:|
| Replay | 32.1147--32.1453 ms |
| Encode | 31.4408--31.5835 ms |

Replay exceeds Encode in this diagnostic window, but **do not transfer that
ordering to the default binary**. Of the five subintervals, three establish
Replay above Encode and two retain overlapping bounds. Primary is not covered
by this observer, so there is no global dxmt9 owner ranking.

## Falsified assumption and next mechanism

Calling this a low-frequency source observer was premature. The live queue
contains 126,065 Replay items during recording, nearly half StateBlock tasks.
The trace consumes 97.37% of its fixed 262,144-row capacity. Two self queries
per item can materially perturb the small worker difference even though all
counter-validity and output checks pass. No OFF/ON economy transfer is claimed.

The next diagnostic should record only every 256th Replay item while leaving
ordinary Encode boundaries unchanged. Crucially, it must continue using
**cumulative self CPU cursor bounds**, not multiply sampled scope durations by
256. Unsampled work then remains in the interval CPU difference; thinning only
widens the endpoint bracket. This directly reduces the measured observer
mechanism without reviving external stale-counter interpolation.

Six pure bound tests pass, including missing-endpoint rejection, overlapping
queries, no interpolation and sleeping-thread behavior. The original largest
default dxmt9 single-thread owner, Primary attribution and correctness-qualified
removable amount remain unresolved.
