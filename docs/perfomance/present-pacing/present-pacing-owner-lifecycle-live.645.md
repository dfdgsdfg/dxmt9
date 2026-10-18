---
domain: present-pacing
workload: STALKER Day/high late camera corridor
title: "H645 — baseline workers are close; combined run rejects missing clock-service deployment"
type: evidence
status: stopped-root-service-attachment-failed
updated: 2026-10-17
source: traces/stkcop-owner-lifecycle-live-h645/{contract.md,inputs.json,run.py,batch.py,batch.json,analyze.py,owners.py,O0/analysis.json,I0/analysis.json,R0/receipt.json,R0/gap.bin.196,failure.md,loader_probe.c,loader_probe.py,loader/results.json}; experiments/output/app-d3d9-stkcop-bench-owner-lifecycle-live-h645-r0/dxmt9.log
related: present-pacing-foreign-disposal.644.md; present-pacing-worker-lifecycle.643.md; present-pacing-snapshot-performance.641.md
---

# H645 — two controls complete; combined attribution is not qualified

The fixed eight-arm population stopped at its third arm, R0, because PE root
clock binding failed. O0 and I0 complete the original-game late scene. R0 is
not a performance sample, and C0/C1/R1/I1/O1 were not launched. No result is
replaced and no global CPU ranking or additional removable amount is claimed.

| Completed arm | Target frames | Replay CPU ms/frame | Encode CPU ms/frame |
|---|---:|---:|---:|
| O0, original PE, added observers OFF | 115 | 32.337–32.497 | 31.694–32.165 |
| I0, diagnostic PE, added observers OFF | 111 | 32.531–32.677 | 31.679–31.888 |

Both use the same H644 Unix image, original engine/R2, Day/high1280x720,
max-threads2, snd_targets16, production query mode and sparse H584 endpoints.
Each reaches seven camera knots, exits normally and restores14 shared files.
O0's inspected post-corridor screen has no obvious gross rendering defect;
this is not a pixel-exact/transient-defect proof. The finite endpoint enclosures
are not confidence intervals. Camera matching does not imply equal AI/draw work.
Replay is above Encode in these two controls, but by a small margin; app and
auxiliary ownership are still missing.

## Diagnosed failure, not an FPS regression

R0's `gap-control` returns6 at knot0. Its root map has coverageFault1 and no
checkpoint rows; native clock identity remains zero. The runtime log says
`incomplete-coverage: clock binding`. H645 used H636's three-service staging,
but H636's private EXE already had a neighboring clock DLL. The original game
does not. Earlier H629 explicitly staged that fourth app-local service.

An isolated loader fixture reproduces the issue in both PE32 and PE64:
Wine-root-only service files yield LoadLibrary error126; adding the same
authenticated clock DLL next to the helper EXE yields a valid native self CPU
packet. This changes discovery, not the service implementation. Four processes
and their exact output are retained in `loader/results.json`.

The failed game's14 shared files and three temporary services are restored;
the loader fixtures also recover their services after process exit. No product
default or game binary is changed. The next population must stage the fourth
service, reject clock attachment failure before the measured corridor, and
preserve the full default-owner composition objective. H641 remains the separate
repeated Replay-removal evidence, not proof that global ranking is complete.
