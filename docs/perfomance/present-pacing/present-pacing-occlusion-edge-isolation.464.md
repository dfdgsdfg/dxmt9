---
domain: present-pacing
workload: PE32 XYZRHW edge oracle, same Sikarugir runtime
title: "The retained edge failure is shared by real and conservative occlusion"
type: evidence
status: shared-raster-gap-isolated-not-fixed
updated: 2026-10-07
source: traces/stkcop-edge-occlusion-h464/{run.py,oracle.cpp,build.log}; traces/stkcop-edge-occlusion-h464/{on-edge,off-edge,wined3d-edge}
related: present-pacing-public-occlusion-oracle.463.md; present-pacing-completed-workload-visibility.455.md
---

# Separate a common rasterization defect from the visibility intervention

The same PE32 executable accepts `--edge` to restore H463's -0.5-to-15.5
XYZRHW rectangle. Three fresh processes use the same Sikarugir CX24.0.7 Wine
root and prefix. Each controlled staging operation is restored atomically and
hash checked afterward. The original WineD3D D3D9 image SHA-256 is
`b7218f499b819de1060beb17d92c86a2491c9c95f439d7ecaa88943d3ebee290`,
identical to the isolated Sikarugir WineD3D runtime copy. Loader logs confirm
builtin `wined3d.dll` in the oracle; dxmt9 processes report their ABI handshake.

| Provider | Visible / hidden / reissued samples | Direct 16x16 mismatches | Scaled 8x8 mismatches |
|---|---|---|---|
| dxmt9 real occlusion ON | 225 / 0 / 225 | 31 each | 15 each |
| dxmt9 conservative OFF | 2 / 2 / 2 | 31 each | 15 each |
| WineD3D | 256 / 0 / 256 | 0 | 0 |

Both dxmt9 policies have matching readback signatures for all nine outputs:
green direct `a96fa0c5`, point `803179c5`, linear `df5c39c5`; blue direct
`d63e72b8`, point `cbe18830`, linear `d49bcbf0`. These 32-bit signatures and
mismatch counts are comparative observations, not collision-free byte equality.
The oracle's full per-pixel checks pass; it also emits a GL clear diagnostic
in stderr, so do not describe its log as error-free.

The original suspicion is narrowed: this input fails on the conservative
reference too; turning on real visibility did not introduce this edge defect.
WineD3D confirms the analytic 256-sample expectation. The pretransformed FFP
branch's missing half-pixel adjustment remains a source-level candidate, not
a tested fix. No runtime code is changed here.

## Scope decision

Do not erase H463's failure or use the oversized passing case to claim full
raster correctness. Track the common XYZRHW issue separately. Also do not
expand the CPU attribution objective into a general FFP repair project: the
four-run H465 Primary comparison uses identical archived default policies,
while any eventual real-visibility promotion retains its correctness gates.

H455's observed CPU reduction is still candidate evidence for a change in
submitted workload. H464 does not turn it into a fully qualified saving or
establish which default thread has the largest renderer CPU cost.
