---
domain: present-pacing
workload: 3DMark05 GT1
title: "Present-Pacing — display sync, frame latency, and the wallclock cap"
type: domain-index
status: current
updated: 2026-08-05
source: docs/perfomance/overview-3dmark05-gt2.md
related: docs/perfomance/present-pacing/overview.md; docs/perfomance/present-pacing/log.md
---

# Present-Pacing — display sync, frame latency, and the wallclock cap

Latest tracked row: `H233` - a clean `0eaa0d27` / `c8d8f83d` GT2 comparison fixes the segmented-Arena regression boundary: full-arena admission pressure creates 17.16 session releases per Present and raises command buffers 5.93x.

## Start Here

- [Current overview](overview.md) - latest conclusion and active gates only.
- [Historical log](log.md) - long-form chronology moved from the old domain root.
- [Root 3DMark05 GT1 map](../overview-3dmark05-gt1.md)
- [Root 3DMark05 GT2 map](../overview-3dmark05-gt2.md)

## Recent Leaf Documents

- [present-pacing-arena-admission-regression-boundary.233 - Segmented Arena Admission Turns Capacity Pressure Into Session Fragmentation](present-pacing-arena-admission-regression-boundary.233.md)
- [present-pacing-post-defselect-cpu-attribution.05 - The Producer Thread Cannot Be Split By Image](present-pacing-post-defselect-cpu-attribution.05.md)
- [present-pacing-drain-fence-attribution.207 - The Blocked Locks Are Mostly MANAGED](present-pacing-drain-fence-attribution.207.md)
- [present-pacing-drain-fence-attribution.206 - One Of 84 Bridge Entry Points Owns 99.8% Of The Drain Fence](present-pacing-drain-fence-attribution.206.md)
- [present-pacing-sfiv-shader-cost-attribution.205 - The SFIV 88ms Instances Are Real Shader Work, Not A Wait](present-pacing-sfiv-shader-cost-attribution.205.md)
- [present-pacing-sfiv-scene-pass-stall.204 - SFIV Scene-Pass Frame-Period Stall Owns The Frame Wall](present-pacing-sfiv-scene-pass-stall.204.md)
- [present-pacing-engine-default-trio.203 - The Promoted Trio Becomes The Engine Default](present-pacing-engine-default-trio.203.md)
- [present-pacing-archive-prewarm-hardening.202 - Archive Prewarm Hardening Closes The Startup-Flake Class](present-pacing-archive-prewarm-hardening.202.md)
- [present-pacing-inline-const-delta.201 - Inline Const Delta Proves Mechanism But Lands Inside The Noise Band](present-pacing-inline-const-delta.201.md)
- [present-pacing-decimated-pe-stats.200 - Decimated PE Stats Size The Recorder Core At ~8.5ms/present](present-pacing-decimated-pe-stats.200.md)
- [present-pacing-postcache-resample.199 - Post-Cache Producer Re-Sample - The Wall Is Now The Guest Blob](present-pacing-postcache-resample.199.md)
- [present-pacing-readonly-cache-stacking.198 - Readonly Cache Stacks With The Promoted Pair For +26% Cumulative](present-pacing-readonly-cache-stacking.198.md)
- [present-pacing-readonly-managed-buffer-cache.197 - Readonly Managed Buffer Cache Collapses The Lock Bridge Storm](present-pacing-readonly-managed-buffer-cache.197.md)
- [present-pacing-producer-sampling-attribution.196 - Non-Perturbing Producer Sampling Finds The Buffer-Lock Bridge Storm](present-pacing-producer-sampling-attribution.196.md)
- [present-pacing-consolidation-long-confirm.194 - Consolidation Long Confirm - Offload+IndexCache +10% Over The Full Demo](present-pacing-consolidation-long-confirm.194.md)
- [present-pacing-pe-const-overhead-cut.193 - PE Const-Chain Overhead Cuts Land Clean But Move No FPS](present-pacing-pe-const-overhead-cut.193.md)
- [present-pacing-pe-cost-verification.192 - PE Recording Cost Is Real (~10ms/present, Overhead-Corrected)](present-pacing-pe-cost-verification.192.md)
