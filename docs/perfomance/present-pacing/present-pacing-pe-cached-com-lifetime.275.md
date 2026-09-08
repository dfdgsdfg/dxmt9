---
domain: present-pacing
workload: PE32/PE64 Present and canonical COM caches
title: "Canonical cache credit and public COM lifetime"
type: evidence
status: partial
updated: 2026-09-08
source: experiments/output/pe-present-source-owner-20260908/analysis; experiments/output/app-d3d9-3dmark{05,06}-pe-cached-com-37621df1-gt2-off-perf-r1-20260908
related: present-pacing-inline-consumers.274.md; present-pacing-inline-constant-pixels.265.md
---

# Canonical cache credit and public COM lifetime

## Causal correction

The Release-2 defect described by H265/H274 is not capture-specific.
`D3D9DeviceImpl::Present` calls public `GetBackBuffer` even with capture OFF.
Device-only controls return zero on both PE widths, while ordinary Present,
Present followed by Reset, explicit getters, and both additional-swap-chain
release orders return two. The ten red executions and two green device-only
controls retain their recipes, fixture/image identities and unmodified failure
assertions under the source root. No extra Release is used to conceal the bug.
The earlier H274 content/fresh-provider evidence remains valid; its attribution
to capture-internal acquisition was incomplete.

The graph has two reverse-reference cycles: the device caches its swapchain,
and the swapchain caches its backbuffer; children retain their parents.
`cachedBackBuffer0_` was also a strong alias despite its non-owning comment.
Reset only dropped the device's cache edges, leaving the inner cycle intact.

## Change and verification boundary

`R-CORE-4.9.1` separates the public count from one private allocation credit.
The shared `planCachedComTransition` reducer drives the atomic lifetime of PE
swapchains and surfaces. Only public 0-to-1 / 1-to-0 transitions acquire or
release parent holds. A cache keeps allocation/raw identity alive; it does not
hold another public reference. Insertion, duplicate losers and allocation
failures use explicit cache-credit policies. No bridge ABI, wire grammar,
per-draw allocation or renderer-default change is introduced.

Release snapshots parent pointers before the transition and touches no child
storage after callbacks can destroy it. Teardown detaches cache entries before
releasing their credit. The device alias is borrowed and cleared before cache
eviction; its GetRenderTarget read/AddRef now shares Reset's recorder lock.
Device public refs are atomic because independent child releases can converge
on that parent. Other COM families are not thereby proved thread-safe.

The native cache spec checks the 64 small-domain state/action combinations,
count extrema and invalid transitions, real unordered-map allocation failure,
private-credit duplicate ownership, and all six caller-release orders for each
of implicit and additional three-object graphs. Heap fakes execute recursive
parent Release/cache eviction, including guarded cache-only reacquisition and
eviction with a live external child. The strong-cache mutant produces exactly
Release 2. Native and standalone ASan/UBSan checks pass.

These are bounded local-state and recursive-order witnesses, not exhaustive
concurrent parent-effect/CAS interleavings, arbitrary COM families, all failed
Reset cases, or a Metal pixel proof. Actual PE capture and fresh-provider
results are tracked independently. The atomic surface path also affects
uncached surfaces; its CPU cost must be measured rather than assumed free.

## Status

Both PE builds and native/Unix builds pass. Native focused tests pass 4/4;
the x86_64 cache/install-name group passes 2/2. The final PE ABI, level-cache,
public-allocation, inline/origin and manifest regression group passes 7/7
(`native-final-r3.log`). The existing TLA regression
passes in 146.97 seconds; it is not a new COM concurrency proof.

All twelve post-change PE lifecycle processes return final device Release 0.
Repeated device/swapchain/GetRenderTarget getters preserve the backbuffer
pointer, including a cache-only interval. A held additional-swapchain surface
still returns its device/container after the caller releases the swapchain.

All four PE32/PE64 inline OFF/ON capture processes now pass teardown and
canonical content validation. Each has 18 independently expected source pixel
samples plus a fresh-provider strict source/output comparison over 24,576 bytes.
All four hashes remain equal to H274:
`299de44bf226e55e6c7a77717e1dac116a0a08f1ace056514195d77fe255cf16`.
Two non-capture ON regression processes add 144 exact samples and Release 0.
The [compact receipt](../../../tests/conformance/d3d9/evidence/pe-cached-com-lifetime-20260908.json)
freezes source, staged images and raw-result hashes. Baseline and candidate
fixtures differ only by added lifecycle checks; capture/pixel bodies are
unchanged. The baseline getter checks precede the additional reacquisition
and held-child-query witnesses, which are independently qualified post-fix.

## Post-commit CPU/FPS spots

Two fresh canonical runs measure `37621df1`, with both PE widths and Unix
staged from the verified build. Sikarugir cx-24.0.7, perf/Warn, replay worker
ON, contiguous transport, CPU-ready tape/index reorder/inline constants OFF;
05 uses 1024×768 and 06 uses 1280×720. Frame sampling and two-second external
process CPU sampling are enabled; detailed PE clocks, stopped-PC sampling and
GPU tracing are disabled. No build, test or second benchmark overlaps either
scene. Earlier desktop-use-contaminated 06 results are not used.

| Scene | Frames / wall ms | FPS | Process CPU ms/frame | CPU frame coverage | CB / pass per frame |
|---|---|---:|---:|---:|---|
| 05 GT2 | 1,916 / 64,624.163 | 29.6484 | 69.8062 | 96.97% | 4.0010 / 15.7844 |
| 06 GT2 | 1,160 / 64,036.352 | 18.1147 | 95.9144 | 96.64% | 4.0017 / 19.8621 |

H274's earlier sample-weighted default-OFF observations were 29.4126/18.1313
FPS and 70.5773/95.1972 CPU ms/frame. These new fixed-order spots are not a
matched A/B: 05 CPU is lower and 06 CPU is higher, so they establish neither
repeatable CPU savings nor neutrality. Historical README recipes also differ.
Inline constants remain default-off and README is unchanged.

CPU uses one cumulative same-PID first/last endpoint delta strictly inside
the scene: 129.70 seconds over 1,858 frames for 05 and 107.52 seconds over
1,121 frames for 06. It includes game, Wine, PE and Unix threads, not PE-only
work. Centisecond timer resolution and polling/log alignment limit precision.
Raw first/last observations, RSS in KiB, recipes and image hashes remain in
`measurement-qualification.json` and `analysis/economy-qualified.json`.

Both runner results pass. 05 exits normally; 06 uses the explicitly allowed
result-GUI timeout after its complete scene (frames 1,035–2,194). Eight observed
error counters are zero. The last periodic 06 Present counter is 2,160, so the
34-frame unreported tail is not health-proven. Summed GPU CB time is
2.0000/11.9214 ms/frame, not GPU critical-path latency. No whole-scene pixel
equivalence or broad COM concurrency claim follows from these runs.
