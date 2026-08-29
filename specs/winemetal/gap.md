---
type: "Spec Gap"
title: "winemetal Gap"
description: "Implementation and evidence gaps for the Wine Metal surface bridge."
tags: [specs, gap, winemetal, wsi]
---

# winemetal Gap

dxmt9 now implements the PE `ExtEscape` client, cold adoption bridge, ordered
surface lifetime, and exact-qualified legacy aggregate-table fallback. At the
2026-08-25 implementation baseline,
[Wine MR !11058](https://gitlab.winehq.org/wine/wine/-/merge_requests/11058)
remains open/conflicted at `07bb09bd2d3974ec035ec0e49fa5bc7e85a3ab41`, while
[upstream DXMT PR #166](https://github.com/3Shain/dxmt/pull/166) is open at
`a1aa73fd8a8569edcc83378804f94dfbdcfcea10`. The numeric protocol is therefore
revision-pinned and the compatibility target remains open.

Wine master `111e5197390aa008789b002222024229fa2b82cf` (2026-08-24) supports
by-name unixlib loading and its WoW64 translation, but does not contain the
proposed Metal-surface escape. Its `winemac.drv` build also uses hidden symbol
visibility, so provider loading does not make dxmt9's legacy `dlsym` path a
stock-Wine ABI. Current upstream Wine is therefore a loader-pass / WSI-fail
compatibility class, not a generally supported runtime.

| Area | Status | Gap |
|---|---|---|
| Shared escape compatibility declaration | ✅ | `winemac_surface_escape.h` pins 6790/6791 and the two-`uint64_t` payload to the cited Wine revision with LGPL attribution |
| PE `QUERYESCSUPPORT` / get-surface path | ✅ | PE retains the acquisition HDC until 6791 is attempted, balances `ReleaseDC` afterward, validates both zero-initialized output fields, defensively releases malformed partial results, and keeps HDC out of every wire record |
| Cold PE/unix layer adoption call | ✅ | Dedicated fixed-width `D9CWsiSurfaceBinding` adopt/teardown operations leave `CommandChunk` unchanged |
| Ordered exactly-once surface release | ✅ implementation / persistent bridge fault leaks by policy | Candidate replacement fails closed on an active arena. Terminal teardown instead arms the gate, waits transient arena and Presenter users, fences live queues or joins stopped workers, invalidates the registry, and only then lets PE consume the release obligation. A Present already accepted by replay waits behind the gate rather than returning a synthetic zero sequence. PE finalization retries a failed unix bridge call only to the fixed bound; without a unix quiescence acknowledgement it deliberately leaks the retained Wine capability instead of hanging or issuing 6791 early. |
| Reset and additional-swap-chain rebind | ✅ implementation | Swap-chain WSI operations, generation-qualified `PresentId` snapshots, and direct cold Presenter mirror calls share one lifecycle lock; no raw owning Presenter accessor remains. Quiescence is armed before legacy candidate acquisition. Because the audited Sikarugir Wine returns the existing `WineMetalView*` without retaining it, physical host views have dxmt9-local pointer-keyed logical claims and only the last claim invokes the macdrv release. Candidate failure preserves the old claim/binding and may return to idle for a later retry; a null `client_cocoa_view` fails closed before the macdrv callback. The replaced ExtEscape token is released only after unix acknowledgement. A different macdrv ABI that returns distinct wrapper pointers for one Cocoa view requires a stronger host identity and remains outside this runtime claim. |
| Qualified `macdrv_functions` fallback | ✅ implementation | Only `legacy-macdrv-symbols:<runtime-id>` whose suffix matches the resolved manifest entry selects the aggregate-table path; generic direct-symbol and Cocoa-view fallbacks were removed |
| Runtime protocol manifest | ✅ | Resolver requires `legacy-macdrv-symbols:<matching-id>` and the harness exports both values; unsupported/unknown entries fail before spawn unless the explicit negative-test opt-in is used |
| Native protocol tests | ✅ deterministic seams / runtime API faults open | The production release algebra is callback-injected and proves one 6791 attempt plus one DC balance, including partial DC-only state and missing-capability preservation. The legacy host-view truth table proves shared-claim and final-release cases, the finalizer retry predicate is bounded, and the source-order audit pins gate-before-candidate, lifecycle-locked readers, removal of the raw Presenter accessor, and null-client-view rejection. Queue tests execute candidate registry failure, gate wait/notify, stop wake, and active-arena finalization; runner tests mock `Popen` and prove environment-only unsupported/unknown identities never spawn. A 2026-08-25 legacy x64 full conformance run completed all 235 cases without a teardown crash/hang (234 pass plus only the unrelated long-standing `visual_process_vertices_xyzhw_policy` failure), with focused reset/surface groups additionally passing 5/5 repetitions trace-off and one trace-on control. Actual Win32 `GetDC`/`ExtEscape`/`ReleaseDC` fault injection, distinct-wrapper/same-Cocoa-view identity, and WoW64/ExtEscape runtime teardown remain open. |
| x64 and WoW64 Wine integration | ❌ | Requires a Wine build carrying the accepted escape implementation and evidence for create/present/reset/destroy |
| Post-change `macdrv_functions` regression | ❌ rerun required | Existing builtin x64/WoW64 legacy smoke evidence predates both ExtEscape dispatch and the `winemetal_dxmt9.*` rename; both lanes must pass R-WMB-17.4 on an exact audited fixture before rollout |
| Unsupported Gcenx/Heroic behavior | ⚠️ implementation complete, evidence open | Source and native predicates fail with `D3DERR_NOTAVAILABLE` plus `layer_acquisition=unavailable`; a negative stock-Wine run is still required |
| Loader-pass / WSI-fail composition | ⚠️ bounded accepted-runtime evidence / stock-Wine case open | The child-current STALKER Day retry records provider factory/ABI activity followed by `layer_acquisition=unavailable` at `stage=query` and clean `CHW::CreateDevice` failure before rendering (`experiments/output/app-d3d9-stkcop-bench-current-head-off-day-20260830-r1`). Child/master Sikarugir `winemac.so`, `ntdll.so`, real Wine binaries, exports, and wrapper semantics match, so no runtime-selection correction is supported; the required stock-current-Wine composition case remains open. |
| Upstream DXMT coexistence | ❌ | Pair ExtEscape WSI with the `winemetal_dxmt9.*` deployment rename and prove independent D3D9/D3D11 initialization |
