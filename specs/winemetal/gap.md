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
| Ordered exactly-once surface release | ✅ implementation | A dedicated queue gate rejects active arenas/new Presenter users, waits existing users, and performs a non-deferred GPU fence before registry teardown; PE then consumes the one release obligation |
| Reset and additional-swap-chain rebind | ✅ implementation | Candidate-first adoption preserves the old binding on failure and releases the replaced token only after unix acknowledgement |
| Qualified `macdrv_functions` fallback | ✅ implementation | Only `legacy-macdrv-symbols:<runtime-id>` whose suffix matches the resolved manifest entry selects the aggregate-table path; generic direct-symbol and Cocoa-view fallbacks were removed |
| Runtime protocol manifest | ✅ | Resolver requires `legacy-macdrv-symbols:<matching-id>` and the harness exports both values; unsupported/unknown entries fail before spawn unless the explicit negative-test opt-in is used |
| Native protocol tests | ✅ | Protocol and queue specs cover fixed call shape, retained release capability, candidate/registry failure preservation, actual quiescence dispositions, exactly-once release state, and identity-qualified legacy selection; script tests pin resolver and pre-spawn gates |
| x64 and WoW64 Wine integration | ❌ | Requires a Wine build carrying the accepted escape implementation and evidence for create/present/reset/destroy |
| Post-change `macdrv_functions` regression | ❌ rerun required | Existing builtin x64/WoW64 legacy smoke evidence predates both ExtEscape dispatch and the `winemetal_dxmt9.*` rename; both lanes must pass R-WMB-17.4 on an exact audited fixture before rollout |
| Unsupported Gcenx/Heroic behavior | ⚠️ implementation complete, evidence open | Source and native predicates fail with `D3DERR_NOTAVAILABLE` plus `layer_acquisition=unavailable`; a negative stock-Wine run is still required |
| Loader-pass / WSI-fail composition | ❌ | On stock current Wine, require a successful provider ABI handshake followed by `layer_acquisition=unavailable` and clean WSI creation failure |
| Upstream DXMT coexistence | ❌ | Pair ExtEscape WSI with the `winemetal_dxmt9.*` deployment rename and prove independent D3D9/D3D11 initialization |
