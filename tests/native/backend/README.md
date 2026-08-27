# tests/native/backend

Metal backend specs — descriptor/key transforms, render-pass actions, queue
observer, and Apple-Silicon Stage 2 features (argbuf hybrid, tile FFP, MTLHeap
pooling). Most are CPU-only; a few hold `WMT::Reference<>` defaults so
NSObject release symbols link against winemetal.

| Spec | Covers (R-* anchor) |
|------|---------------------|
| `backend_key_descriptor_spec.cpp` | `buildDrawUniforms()`, depth/stencil + sampler key value-transform (`R-BACK-3.*`) |
| `backend_pipeline_key_spec.cpp` | PSO key (blend, MRT color-write, FVF hash, sRGB format, sampler texture/filter flags) (`R-BACK-3.*`) |
| `dod_replay_observer_spec.cpp` | Warmed ChunkSlot capacity reuse + uniform interning |
| `encode_session_lifecycle_spec.cpp` | Real Legacy/Arena `encodeChunk` session lifecycle and selected-serial effective per-command observer, including selection/fragments/resources; selected-parallel observer binding remains open (`R-VERIF-7.3`) |
| `drawable_token_spec.cpp` | Production stash/take/complete/fail predicates and deterministic wait handoff (`R-VERIF-3.6`) |
| `resource_lifetime_spec.cpp` | Initializer/reference lifetime truth tables plus direct HandleArena ABA reuse (`R-VERIF-3.1`–`3.4`) |
| `resource_hazard_spec.cpp` | Encoder hazard FSM via fake backend (`R-BACK-2.14-2.28`, `R-VERIF-4.4`) |
| `render_pass_actions_spec.cpp` | Color first-use DontCare-load + depth look-ahead + load/store counters (`R-BACK-15.4-15.16`) |
| `allocation_counter_spec.cpp` | Real perf-counter emission for Metal buffer allocations (wrapped via `assert_perf_counters`) (`R-BENCH-2.5`) |
| `metalcapture_spec.cpp` | MetalCapture marker placement + counter |
| `dynamic_rename_ring_spec.cpp` | DYNAMIC DISCARD + MANAGED writable-upload backing versioning, sequence reuse, and concrete draw snapshot (`R-BACK-5.8`, `R-BACK-5.11`) |
| `heap_pooling_spec.cpp` | MTLHeap small-resource pooling classify + lifetime (`R-BACK-5.9, 5.10, 14.*`) |
| `argbuf_hybrid_spec.cpp` | Stage 2 argbuf capability gate + selector + variant key + descriptor (`R-BACK-12.22-12.26`) |
| `argbuf_hybrid_msl_spec.cpp` | FFP/DXBC→MSL emitter routing through ArgbufLayout slot 30 |
| `wmt_generate_mipmaps_metal_spec.mm` | WMT blit RGBA16Float 1024x1024 complete 11-level 2x2 mip-chain oracle (`R-BACK-5.*`) |
| `dxmt9_argbuf_populator_spec.cpp` | Per-encoder argbuf populator (encoder-resource init, dirtyBytesEstimate) |
| `tile_ffp_selector_spec.cpp` | Tile-shader FFP capability gate + per-pass selector (`R-BACK-13.*`) |
| `tile_ffp_msl_spec.cpp` | Tile-FFP MSL emitter source contract |

## Running

```sh
meson test -C build-x86_64-builtin dxmt9-resource-hazard-spec
meson test -C build-x86_64-builtin dxmt9-render-pass-actions-spec
```

`dxmt9-allocation-counter-spec` runs the test exe via the
`scripts/check/assert_perf_counters.py` wrapper to verify expected counter
keys appear in the perf JSON.

## Conventions

- Spec files: `<area>_spec.cpp` (snake_case). Probe-only files use an
  `_probe` suffix.
- Test target name: `dxmt9-<name>` (kebab).
- Specs holding `WMT::Reference<>` instances need `dxmt9_winemetal_dep`
  added to `dependencies:` so NSObject_release links — the test never
  actually constructs a live handle (default `handle == 0`).
- Probes that reach into bridge / commit_chunk paths from native main()
  may surface ASan halt-on-error issues; document non-registered probes
  with a comment in `meson.build`.
