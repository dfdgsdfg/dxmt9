---
domain: present-pacing
workload: STALKER CoP late Normal capture
title: "Closed-query history repair publishes late input; replay query backend is the next gate"
type: evidence
status: capture-authenticated-original-replay-blocked
updated: 2026-10-13
source: traces/stkcop-closed-query-history-h505/{receipt.json,native-final.txt,tla.txt,capture-verification.json,bundle-validation.json,bundle-inspection.json,candidate-build.json,source_probe.cpp,oracle-r1.log,oracle-metal-r2.log,provider-h495-diagnostic.txt}; traces/stkcop-scene-request-h502/r5/{receipt.json,analysis.json,runtime-log.txt,restore/restored.json}; traces/stkcop-scene-request-h502/r5/tape/frame-204307992428400-1/manifest.json
related: present-pacing-closed-query-ordinal.504.md; present-pacing-tail-input-scope.500.md; ../../../specs/experiments/gap.md
---

# The late input now publishes without weakening query identity

H504's lost absolute Issue ordinal is repaired by the 32-byte
`RenderTapeQueryDescriptorV4`. It carries independently maintained initial
cycle and ordinal, not an ordinal inferred from cycle. The closed state grants
no old-result, END-fence or completion authority. Legacy V2 history and the V3
completed-EVENT checkpoint retain their meanings. The private V4 executor maps
this explicit history to a genuinely fresh local query without a synthetic END.
Production query scheduling and the PE/Unix bridge ABI are unchanged.

The retained V2 counterexample still rejects canonical 509/1 against runtime
509/1017. V4 accepts the authentic 509/1017 BEGIN and 509/1018 END and rejects
stale ordinal, generation and cycle. Decoder negatives, old-cycle polls,
missing BEGIN and ordinal overflow remain fail-closed. An exhaustive native
check runs all 4,096 six-action traces through the actual pure reducer and
checks independent cycle/ordinal offsets against fresh-local history. Actual
local execution tests cover closed EVENT END and closed occlusion BEGIN; they
are not a full original-input GPU oracle.

## Authenticated publication

The unchanged H502 spatial selector requests capture at engine frame 1542,
completed count 1453. Arm succeeds 411 ms after request issue. Publication is
observed at frame 1550, completed count 1461. This is an externally observed
camera envelope, not exact per-draw camera identity or default-work equivalence.
The request occurs 116.657–116.718 s after the bracketed running transition;
spatial selection, not elapsed-time equality, selects the episode.

The single bundle is `frame-204307992428400-1`, manifest SHA-256
`d4e84e3ad5daa6fe1d416dc3814f923ea4cf6c38ae2acf975e6eb75c580e40e4`.
Component/blob hashes, structural validation, identity/topology validation and
request-to-token/epoch/queue-to-publication joins pass. No capture abort or
identity attachment rejection remains in this run.

| Admitted input | Count |
|---|---:|
| Events / command records | 5,026 / 5,205 |
| Raw chunks / published sources | 259 / 7 |
| Draws | 3,982 |
| Blobs / total blob bytes | 3,877 / 389,629,136 |
| Query definitions / Issue records / polls | 6 / 9 / 5 |

Both original source and output are 3,686,400-byte BGRX images with SHA-256
`1955a90a4d989708bcbd5a82aa2c3dcd425ed6f94170ec55075c6b2778d68e9b`.
Their presence/authentication is not evidence that replay reproduces them.

Independent post-run hashes match all five restored shared Wine images and all
three supervisor snapshots. No owned engine/helper remains. The private native
runner freezes its own images; loader output confirms those images, and neither
replay attempt stages Wine files.

## New gate: capture and replay use different query backends

The original run explicitly uses `DXMT9_METAL_OCCLUSION=0`. Its four captured
occlusion results are each `a0000000` (160). In `core_resources.cpp`,
`Device::issueQuery` resolves this path from `activeOcclusionCount_`, accumulated
by production draw handling. Those bytes are not hardware visibility samples.

The current provider's `replayGeneralFrameTape` requires `usesMetalOcclusion()`
for **any occlusion definition**, before resource setup. The same-backend r1
therefore returns `query-oracle-unsupported`, with zero created objects, zero
mapped sources and no output. This is an unsupported diagnostic contract, not
a newly observed product pixel failure.

A separate, explicitly non-equivalent r2 enables Metal occlusion to test that
the gate cannot simply be solved with an environment override. It restores all
16 attachment histories, prepares 12 aliases, then rejects
`v4-exact-source-publication` at event 4075, the first occlusion-bearing raw.
Only one captured source has mapped while six actual sources already exist.
All 614 acquired wrappers are released. No final image is reached. Hardware
occlusion changes scope/publication behavior; do not weaken source matching or
use these timings as the default-path cost.

The next implementation must distinguish a **same-production-query diagnostic**
from a **hardware occlusion-result oracle**. It must bind the captured backend
profile to immutable input and verify the constructed local backend, execute
the actual query/counter and real completion path, compare every defined query
result and retain exact source/pixel checks. A default-counter diagnostic must
never set the GPU occlusion-result qualification flag. Do not drop queries,
inject the recorded value 160, switch the original to another backend, or
relax the public Metal-oracle contract merely to admit this bundle.

## Verification and limits

Full canonical Unix, x86 PE and x64 PE builds succeed. The focused suite passes
7/8, including identity, execution, capture and install-name checks. The remaining
provider-spec assertion is the fixed textured-UP Metal digest; the same failure
is reproduced with frozen H495 native/provider images. That comparison is
diagnostic isolation, not a waived oracle or permission to replace its golden.
All 237 existing TLA configurations meet their expected outcomes; the new
history transition binding is the exhaustive native check, not a new TLA model.

Capture authentication is now established for this bounded late-camera input.
Original-image replay, first-read content authority, default-path transfer,
policy timing and the largest default-tail single-thread CPU owner/removable
amount remain unproven. H481 remains excluded from its intended tail window.
