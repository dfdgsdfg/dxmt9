---
type: "Spec Gap"
title: "winemetal Gap"
description: "Implementation and evidence gaps for the Wine Metal surface bridge."
tags: [specs, gap, winemetal, wsi]
---

# winemetal Gap

The existing provider has a qualified legacy `macdrv_functions` path, but the
`ExtEscape` client and its lifetime protocol are not implemented. At the
2026-08-04 baseline, [Wine MR !11058](https://gitlab.winehq.org/wine/wine/-/merge_requests/11058)
is open/conflicted at `07bb09bd2d3974ec035ec0e49fa5bc7e85a3ab41`, while
[upstream DXMT PR #166](https://github.com/3Shain/dxmt/pull/166) is open at
`a1aa73fd8a8569edcc83378804f94dfbdcfcea10`. The numeric protocol is therefore
revision-pinned and the compatibility target remains open.

| Area | Status | Gap |
|---|---|---|
| Shared escape compatibility declaration | ❌ | Add one declaration-only header for escape values 6790/6791 and the two-`uint64_t` payload, with Wine revision and LGPL attribution |
| PE `QUERYESCSUPPORT` / get-surface path | ❌ | Acquire HDC for `HWND`, probe support, validate positive get result and nonzero tokens, and fail cleanly when unavailable |
| Cold PE/unix layer adoption call | ❌ | Add the pointer-width-independent POD bootstrap/rebind operation without changing hot `CommandChunk` schemas |
| Ordered exactly-once surface release | ❌ | Quiesce unix presenter use before PE issues `MACDRV_ESCAPE_RELEASE_SURFACE`; cover rollback and double-release prevention |
| Reset and additional-swap-chain rebind | ❌ | Implement candidate-first atomic adoption and old-surface release ordering |
| Qualified `macdrv_functions` fallback | ⚠️ partial | The existing path has smoke evidence on a pinned Wine fixture; restrict it to exact `legacy-macdrv-symbols` manifest entries and remove generic Wine 11.x assumptions |
| Runtime protocol manifest | ❌ | Add `metal_surface_protocol`, observed acquisition path, and result preservation |
| Native protocol tests | ❌ | Cover unsupported query, malformed/zero response, adoption rollback, and balanced release without Wine/Metal |
| x64 and WoW64 Wine integration | ❌ | Requires a Wine build carrying the accepted escape implementation and evidence for create/present/reset/destroy |
| Post-change `macdrv_functions` regression | ❌ rerun required | Existing builtin x64/WoW64 legacy smoke evidence predates both ExtEscape dispatch and the `winemetal_dxmt9.*` rename; both lanes must pass R-WMB-17.4 on an exact audited fixture before rollout |
| Unsupported Gcenx/Heroic behavior | ❌ | Prove `D3DERR_NOTAVAILABLE` with one diagnostic instead of success plus a black/no-op window |
| Upstream DXMT coexistence | ❌ | Pair ExtEscape WSI with the `winemetal_dxmt9.*` deployment rename and prove independent D3D9/D3D11 initialization |
