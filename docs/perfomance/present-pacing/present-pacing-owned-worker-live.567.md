---
domain: present-pacing
workload: STALKER Day/high 1280x720 diagnostic late-window episode
title: "Owned-worker live episode reaches all knots but fails the game-exit gate"
type: evidence
status: live-episode-rejected
updated: 2026-10-16
source: traces/stkcop-owned-live-h567/{contract.md,build-receipt.json,rejection.json}; traces/stkcop-owned-live-h567/A0/{receipt.json,catalogue-result.json,workers.bin,xray_engine.log,dxmt9.log,post-tail.png,restore/restored.json,recovered.json}
related: present-pacing-owned-worker-preflight.566.md; present-pacing-owner-live.564.md; present-pacing-owner-clock-rejection.560.md
---

# Coverage improved; the live ownership gate did not pass

One predeclared A0 episode uses H563's explicit-domain PE observer and H566's
private worker-instrumented Unix image. The catalogue runner reaches all seven
spatial knots with active, qualified observations. The post-tail screenshot
shows coherent building geometry, not a complete-run pixel oracle.

The game then exits **1**, without timeout or a benchmark result. Its engine
log ends with `compiling shader font2` and two
`CreatePixelShader hr == 0x8876086c` messages. This resembles H560's separate
shader failure, but the cause is not isolated and is not attributed to the
new worker observer. H567's root header reports zero coverage faults and
298 checkpoints, unlike H560's rejected clock-domain observer.

The frozen contract requires game exit zero. `analyze_run.py` therefore must
not be used to certify this run. `reject.py` records rejection and inspects
trace coverage separately; it does not waive the exit gate or produce a new
FPS/default-owner verdict. No unchanged rerun is added to the population.

## What the worker trace establishes

The identity-qualified trace passes its structural/lifecycle reader with
15 worker instances, 654 published rows, one mapping attachment and no
fault/loss. Its role census is:

| Role | Instances | Late-window endpoint coverage |
|---|---:|---|
| Archive load | 2 | Both bodies ended before the target |
| Archive save | 1 | Body ended before the target |
| Finish | 2 | Old body ended; active body has enclosing samples |
| Completion | 2 | Old body ended; active body has enclosing samples |
| Pipeline compilation | 4 | No post-window self endpoint or body exit |
| Library compilation | 4 | No post-window self endpoint or body exit |

The eight compile workers remain **unbounded**, not zero. No acquisition-worker
instance is observed; that is a creation census, not proof of all possible
runtime paths. Thread/closure setup and destruction outside the observed bodies
and shared driver work remain outside this instrument's authority.

## Recovery and next discriminator

All 14 backed-up shared files were restored and verified by SHA-256 after
owned processes/Wine exited; all four temporary clock services were recovered.
No canonical renderer source or default policy was changed.

The next diagnostic must distinguish PE shader-bytecode rejection from a null
Unix shader-create result at the failing `font2` call. Current warn-profile
logs do not identify that branch. Worker lifecycle/endpoints also need closure;
another identical FPS run cannot repair either gap. H564 remains the accepted
live self/root evidence, and H559's matched-observer removal screen remains
separate from default-owner qualification.
