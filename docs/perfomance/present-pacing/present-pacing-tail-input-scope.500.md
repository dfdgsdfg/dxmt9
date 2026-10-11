---
domain: present-pacing
workload: STALKER CoP H481 capture provenance versus the requested late CPU window
title: "The current Normal input was published before the 102-110 second tail window"
type: evidence
status: target-window-excluded-next-capture-selection-required
updated: 2026-10-11
source: traces/stkcop-tail-scope-h500/{analyze.py,scope.json}; traces/stkcop-normal-finish-h481/{receipt.json,progress.jsonl,run.py}; traces/stkcop-root-game-h477/default-r1/endpoints.json; src/d3d9/d3d9_pe_device_tape.cpp
related: present-pacing-normal-history-input.481.md; present-pacing-root-game.477.md; present-pacing-vertex-shadow.499.md
---

# Stop using this input to select the largest tail optimization

H499 is a valid local work-removal experiment, not a tail-owner verdict.
H500 strengthens the input-scope limitation: H481's manifest was observed
published **no later than 94.035 seconds after its initial observed running
transition**. Its complete captured interval therefore predates the
102–110-second relative window used in H477. This is not merely missing
confidence about representativeness. No part of that later relative window
was recorded in this bundle.

No game, replay, profiler or build runs in H500. No runtime changes. The audit
reads and hashes surviving external observer artifacts and authenticates the
same manifest used in H486–H499. It does not derive time from directory names,
file timestamps, Present skip counts or CPU clock differences.

## Causal endpoint bound

The capture supervisor records external monotonic begin/end brackets for
coherent game-state snapshots. The last stable non-running snapshot precedes
the first stable running snapshot. Taking the former's **begin** and latter's
**end** conservatively encloses that observed transition; a snapshot's internal
read need not equal either endpoint. No later observed stable reset occurs.

| Observed quantity | Value |
|---|---:|
| Earliest bracketed running edge, monotonic ns | 10,611,266,886,458 |
| Latest bracketed running edge, monotonic ns | 10,611,530,725,958 |
| Manifest publication observed, monotonic ns | 10,705,301,745,041 |
| Publication observation minus first-true endpoint | 93.771019083 s |
| Conservative latest publication elapsed time | 94.034858583 s |
| Margin before 102-second target start | At least 7.965141417 s |

The actual publication may occur earlier than its observation. The captured
interval must finish before manifest publication; consequently an upper bound
on publication is sufficient to exclude this target window. The timestamps'
decimal precision is not a claim of physical timing accuracy.

At publication the external observer sees stable outer-frame 1,404 and
completed SASH-vector count 1,315. Its camera position is retained as an
**observation-at-publication**, not the exact captured draw's pose. These
ordinals are not queue sequences 3,563–3,565, and none is substituted for the
other. The reducer rejects absent false brackets, running resets and overlapping
sample envelopes; all three synthetic counterexamples reject.

This is a within-H481 elapsed-scope exclusion. It does not prove where the
camera would be at 102 seconds in that run, reconstruct the game's physical
simulation clock, or establish camera equivalence with H477. H477 already
shows that equal elapsed times across runs need not imply equal scene work.

## Why the selector cannot close this gap

`D3D9DeviceImpl::armRenderTapeCaptureAtPresentBoundaryInterval` decrements
`renderTapeArmPresentSkipRemaining` at successful eligible Present boundaries
and arms once it reaches zero. It has no external engine/camera request gate.
H481 sets `DXMT9_RENDER_TAPE_SKIP_PRESENTS=1400`; its supervisor stops its owned
engine after the first manifest. Both behaviors are faithful to their contract,
but neither admits a requested late scene envelope.

Increasing the skip count until a desirable screenshot appears would repeat
the same scope ambiguity. Before the next optimization comparison, capture
selection needs a bounded, one-shot external request checked at the existing
safe Present boundary. The supervisor must record game/process identity,
completed-frame/camera brackets, request identity, accepted capture token/epoch,
published source mapping and image hashes. Missing, stale, duplicate, expired
or out-of-window requests must not silently arm a different interval.

Selection is not performance qualification: Normal capture still has seed and
checkpoint drains, PE getter-cache differences and incomplete read-first
content authority. A correctly selected late input still needs original GPU
checks and default-path prevalence/transfer evidence. It also cannot resolve
the mixed Primary game/Wine/PE/synchronous Unix ancestry by itself.

The next work is therefore the late-input selection/identity seam, not another
small cache on H481. Preserve H481 as a useful original-image regression fixture
and retain its local measurements with their original scope. Largest default
dxmt9 single-thread CPU ownership and qualified removable amount remain open.
