---
domain: present-pacing
workload: PE32/PE64 Present and canonical COM caches
title: "Canonical cache credit and public COM lifetime"
type: evidence
status: partial
updated: 2026-09-08
source: experiments/output/pe-present-source-owner-20260908/analysis
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

CPU/FPS neutrality is a separate remaining gate. Inline constants stay
default-off; README is unchanged. This ownership repair is not an FPS
optimization claim.
