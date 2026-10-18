---
domain: present-pacing
workload: STALKER CoP Day high 1280x720 late spatial window
title: "Current Primary ownership run rejects a mixed clock-domain contract"
type: evidence
status: rejected-clock-domain-qualification
updated: 2026-10-16
source: traces/stkcop-owner-bound-h560/{contract.md,build-receipt.json,rejection.json,reject.py,clock-domain.csv,clock-domain.json,clock_domain.c,check_clock_domain.py}; traces/stkcop-owner-bound-h560/A0/{receipt.json,catalogue-result.json,gap.bin.196}; experiments/output/app-d3d9-stkcop-bench-owner-bound-h560-a0/{dxmt9.log,xray_engine.log}
related: present-pacing-clear-live.559.md; present-pacing-root-wall-preflight.522.md; present-pacing-process-accounting.561.md
---

# No current renderer Primary upper bound from H560

One new ownership episode used the byte-identical H559 A Unix image and H523
all-root PE observer. All seven spatial knots were observed, but the root
observer rejected its first checkpoint: `coverageFault=1`, zero published
checkpoints, first reason `checkpoint clock/identity`. The normal cost analyzer
was not run and no renderer ownership numbers are admitted.

The game also exited 1 without timeout; the catalogue records `process_exit`
and `missing_capture`. The game log ends with `CreatePixelShader` failures
(`0x8876086c`). Its exit cause has not been isolated and must not be attributed
to the observer fault alone. Cleanup restored all 14 shared files without
errors. No run retry or retrospective waiver was made.

## Concrete clock mismatch

H523 `marker.hpp::Clock` records ordered RDTSC ticks. `Local::publish` compares
those directly with H518 `native.c::wall`, which returns scaled
`mach_absolute_time`, allowing only 100 ns per endpoint. H525
`wall_bounds.py::envelope` also compares root ticks with absolute-domain spatial
endpoints. Changing only the checkpoint predicate would leave that second
comparison invalid.

A subsequent native x86_64 Rosetta diagnostic enclosed each RDTSC read between
both clock domains:

| Check | Result |
|---|---|
| RDTSC inside continuous-time bracket | 32/32 |
| RDTSC outside absolute-time bracket | 32/32 |
| Common continuous-minus-absolute offset bracket | 10,024,639,924,792–10,024,639,924,958 ns |

The roughly 10,024.64-second epoch offset is not measurement noise. This
demonstrates the current native domain mismatch; it does not retrospectively
qualify PE32/PE64 timing, prove when the offset changed, or identify the game
exit cause. Earlier H522 clock qualification is historical evidence, not a
permanent host invariant.

## Required repair

- Record explicit absolute and continuous domains at bridge checkpoints and
  qualify current PE instruction units/ordering through the actual bridge.
- Convert using bounded mapping observations, not an assumed zero offset.
  Preserve absolute time for joins with existing Replay/Encode/spatial rows.
- Reject incompatible mappings, including sleep-induced changes; do not enlarge
  the 100 ns allowance to swallow an epoch offset.
- Repeat ownership qualification only as a new contracted episode after these
  prerequisites pass. H560 remains rejected.

H559's cumulative self-thread CPU results remain separate: they use native
self `proc_pidinfo` with Mach-domain endpoints, not the H523 root-clock join.
The global/default owner and transfer gates remain open.
