---
domain: state-churn-encode
workload: 3DMark05 GT1
title: "State-Churn Encode — the CPU encode path and draw-run batching - Historical Log"
type: domain-log
status: historical
updated: 2026-07-08
source: docs/perfomance/state-churn-encode/index.md
related: docs/perfomance/state-churn-encode/index.md; docs/perfomance/state-churn-encode/overview.md
---

# State-Churn Encode — the CPU encode path and draw-run batching - Historical Log

> Full historical detail moved from the former top-level `state-churn-encode.md` overview.
> Keep [overview](overview.md) current and compact; append long-running chronology,
> rejected paths, and detailed synthesis here only when it is not already captured in
> one-experiment leaf documents.

---

# State-Churn Encode — the CPU encode path and draw-run batching

> Part of the 3DMark05 GT1 GPU-bottleneck investigation. Root map: [overview-3dmark05-gt1](../overview-3dmark05-gt1.md).

## Scope & question

This domain owns the **CPU encode side** of GT1: why the importer almost never
batches draws into draw-runs, what state actually breaks the runs, and what the
binding-override fix bought. It introduces the per-encoder breakdown
instrumentation (`DXMT9_PERF_ENCODER_BREAKDOWN=1`), measures stream/IB handle
churn, decomposes the draw-run state-delta taxonomy down to the exact stream+IB
pair, lands the `DrawBindingOverride` payload that lets stream/IB-only changes
batch, rechecks after submission batching, and tests disabling auto-expand-indexed.
Every finding here is CPU-throughput. None of them move the GPU frame-time
bottleneck — that is owned by [hidden-backend-storage](../hidden-backend-storage/index.md).

## Hypotheses & verdicts

| # | Hypothesis | Verdict | Evidence |
|---|-----------|---------|----------|
| H1 | The per-draw encode path stays hot because ~99.9% of draws fail to batch | accepted | state-churn-encode-drawrun.01 (580 submits vs 913k draws) |
| H2 | A draw-run cannot blindly cross a constant-upload record (one shared uniform) | accepted | state-churn-encode-drawrun.02 (ConstantUpload stop; per-draw snapshot fallback) |
| H3 | Draw-run breaks are offset/stride churn | rejected | state-churn-encode-stream.01, state-churn-encode-stream.02 (handle churn 81.5-81.9%) |
| H4 | Handle churn is per-draw object creation (lock/rename) | rejected | state-churn-encode-stream.03 (bounded ~184/93 handles, managed-pool alternation) |
| H5 | State-delta breaks are dominated by exact stream+IB pairs | accepted | state-churn-encode-statedelta.01→state-churn-encode-statedelta.03 (82.17% stream+IB-only) |
| H6 | A per-draw stream/IB binding override cuts encode CPU without moving GPU or churn | accepted (CPU win) | state-churn-encode-binding.01 (-30.13% stream-bind CPU, GPU +0.03%) |
| H7 | More draw/submission batching moves the GPU limiter | rejected | state-churn-encode-batch.01 (VS write flat at ~1627 MiB) |
| H8 | Disabling auto-expand-indexed reduces top-pass GPU buffer writes | rejected (GPU); inconclusive (correctness) | state-churn-encode-expand.01, state-churn-encode-expand.02 |
| H9 | Extending `_skipped` bind-cache pattern to vertex_buffer / index_buffer / pipeline / rasterizer / viewport / scissor / depth_state cuts per-CB encode below the 16.67 ms vsync slot | rejected | present-pacing-bind-cache-work-a.01 (0% wallclock movement; `encode_chunk_cpu_ms` rose) |
| H10 | Reducing `mixed_pair_stream_*` draw-run break frequency raises mean run length from 1.88 toward the 32-record cap | open | [present-pacing-encode-budget-fix-proposal.01](../present-pacing/present-pacing-encode-budget-fix-proposal.01.md) |
| H11 | The old unattributed `encode_draw_cpu_ms` remainder is mostly argbuf setup and binding-packet construction/bookkeeping | accepted attribution | state-churn-encode-encode-phase.01, state-churn-encode-encode-phase.02 (`argbuf_cbuf_update=3.92s`, `binding_packet_cache=1.87s`, `argbuf_open=1.21s`) |
| H12 | Skipping clean/no-op argbuf cbuf updates is enough to move the encode budget | rejected as major lever; accepted micro | state-churn-encode-encode-phase.03 (272,956 clean skips, but cbuf-update only -39.7ms; 778,587 dirty writes remain) |
| H13 | Dirty-bit-only partial cbuf repoint can reuse clean categories on argbuf reopen | rejected; new attribution accepted | state-churn-encode-encode-phase.04 (0 cached repoints; 760,157 no-dirty hash mismatches force conservative full dirty upload) |
| H14 | Per-category cbuf identity can repoint cached VS/PS/FFPPS entries on no-dirty payload mismatch | accepted CPU win | state-churn-encode-encode-phase.05 (`encode_draw_cpu_ms` -3.42s, transient bytes -62.05%, dirty cbuf calls -41.88%) |
| H15 | The current category-identity checkout still renders a normal GT1 frame and stable counters | accepted smoke | state-churn-encode-encode-phase.06 (`actual.png` normal robot/flare/HUD frame; cbuf repoint calls +0.02% vs identity-r1) |
| H16 | The remaining binding-packet cache cost is mostly full key probe/equality rather than pure miss/store cost | accepted attribution | state-churn-encode-encode-phase.07 (`probe=939ms`, `key=495ms`, hit rate 85.51%, collision share of misses 99.92%; attribution timers only) |
| H17 | Hashing/probing the already-built binding packet plan directly removes the hot key/probe cost | accepted CPU win | state-churn-encode-encode-phase.08 (`binding_packet_cache` -57.34%, `binding_packet` -30.96%, `encode_draw` -7.40% vs identity smoke; normal GT1 frame) |
| H18 | `argbuf_open` is mostly Metal argument-encoder retarget cost | rejected; attribution accepted | state-churn-encode-encode-phase.09 (`setArgumentBuffer=115ms`; reserve `746ms`, table bind `193ms`, skip `0`) |
| H19 | Skipping transient arena overlap scans on the non-wrapped append path reduces argbuf-open CPU | accepted CPU win | state-churn-encode-encode-phase.10 (`reserve` -51.95%, `argbuf_setup` -16.33%, `encode_draw` -3.87%; same `1680` presents) |
| H20 | Dirty cbuf categories often still match encoder-local cached identities and can be repointed | rejected | state-churn-encode-encode-phase.11 (VS/PS/FFPPS dirty identity hits all `0`; probe removed) |
| H21 | The remaining `stream_bind` parent has one dominant bind class | rejected as single-class; attribution accepted | state-churn-encode-encode-phase.12 (texture/sampler `1065ms`, index `670ms`, shader stream `497ms`, raster `389ms`; FFP stream negligible) |
| H22 | Texture/sampler bind cost is spread across fragment/vertex/resource-array/LOD-bias lanes | rejected as broad split; attribution accepted | state-churn-encode-encode-phase.13 (fragment resolve `575ms`, fragment direct `317ms`; resource-array, vertex textures, and LOD bias all `0`) |
| H23 | Sampler binds can skip `samplerStateFor()` before materializing a Metal handle | accepted CPU win | state-churn-encode-encode-phase.14 (`sampler_lookup_skipped_prehandle=2,108,453`, texture/sampler parent -18.84%, encode_draw -117ms) |
| H24 | Direct sampler shadowing should reuse the packet's sampler-state hash instead of rehashing each entry | accepted CPU win | state-churn-encode-encode-phase.15 (`fragment_direct` -68.425ms, texture/sampler parent -69.616ms, `encode_draw` -126.622ms; heavy split counters are opt-in) |
| H25 | Fragment texture resolve can skip `findTexture()` / `textureForShaderRead()` by matching D3D texture handle + sRGB before materializing the Metal handle | rejected as default CPU win; removed from hot path | state-churn-encode-encode-phase.16 (`1,206,015` pre-resolve skips and local resolve -28.902ms, but texture/sampler parent +48.224ms; temporary default-off smoke proved skip counter `0`; after branch removal, texture/sampler parent returned to baseline `822.864 -> 821.007`) |
| H26 | Remaining cbuf update time is mainly Metal `setBuffer` or transient upload | rejected; attribution accepted | state-churn-encode-encode-phase.17 (`setBuffer=114.568ms`, upload `276.019ms`, build `477.921ms`, inferred residual `954.163ms`; VS residual `618.150ms`) |
| H27 | The phase.17 cbuf residual is mostly upload-plan or observer cost | rejected; binding-hash attribution accepted | [state-churn-encode-encode-phase.18](state-churn-encode-encode-phase.18.md) (`binding_hash=570.070ms`, VS `489.627ms`; `upload_plan=43.287ms` nested in build; observer `0`) |
| H28 | `hashConstantBufferBytes()` is still required in the default cbuf cache path | rejected; CPU win accepted | [state-churn-encode-encode-phase.19](state-churn-encode-encode-phase.19.md) (`binding_hash=570.070 -> 0ms`; cbuf update `1.216 -> 0.875ms/present`; encode_draw `10.359 -> 10.006ms/present`) |
| H29 | Dirty cbuf upload must first materialize full `VsConsts` / `PsConsts` structs | rejected; prefix-preserving CPU win accepted | [state-churn-encode-encode-phase.20](state-churn-encode-encode-phase.20.md) (build `0.333815 -> 0.175342ms/present`; cbuf update `0.875284 -> 0.679652ms/present`; live-range-only prefix zeroing failed visual smoke) |
| H30 | Binding-packet sampler plans must rehash `FlatStateSet` payloads after snapshot key build | rejected; CPU win accepted | [state-churn-encode-encode-phase.21](state-churn-encode-encode-phase.21.md) (`binding_packet_plan` `0.666122 -> 0.599724ms/present`; packet parent `-4.42%`; full sampler equality retained) |
| H31 | Forcing full VS/PS cbuf uploads clearly fixes the suspected black/translucent GT1 geometry | inconclusive visual check; full-upload fallback rejected | [state-churn-encode-encode-phase.22](state-churn-encode-encode-phase.22.md) (`argbuf_hybrid_bytes_per_encoder` +519.59%; no obvious visual normalization; `actual.png` frame drift prevents exact verdict) |
| H32 | Draw submission batches can reuse stale argbuf cbuf slices when per-draw uniform payloads change but base hot constant hashes do not | accepted correctness fix; visual smoke restored | [state-churn-encode-encode-phase.23](state-churn-encode-encode-phase.23.md) (disable-batch A/B localizes artifact; payload component hash identity keeps batching and cuts dirty-fix traffic `-34.24%`) |
| H33 | Current stream/IB churn is a direct backend-storage Xcode candidate | rejected-current; handle-stable A/B required | [state-churn-encode-stream.04](state-churn-encode-stream.04.md) (`60/2` binding tuple changes `160/187`, unique tuples `58`, stream1 extra changes `111`, explicit writers `0.089 B/vertex`) |
| H34 | Row-scoped staging can isolate stream/IB handle churn without changing draw/PSO/argbuf shape | accepted diagnostic; GPU win unproven | [state-churn-encode-stream.08](state-churn-encode-stream.08.md) (`60/2` stream handle changes `271 -> 0`, IB `160 -> 0`, PSO `48 -> 48`, argbuf table `5056 -> 5056`, but staged copy `7.38 MiB` and offset churn remains) |
| H35 | Stream/IB handle identity owns the Xcode hidden backend write bucket | rejected as first-order GPU owner | [state-churn-encode-stream.09](state-churn-encode-stream.09.md) (`60/2` stream/IB handle changes `271/160 -> 0/0`, but GPU `19.184 -> 19.278 ms`, VS write `981.159 -> 981.166 MiB`, VS invocations unchanged) |
| H36 | `bridge_commit_latency_ns` is raw Wine bridge/ABI overhead | rejected as bridge owner; commit_chunk replay accepted | state-churn-encode-encode-phase.24 (`bridge_commit_latency=22.473s`, replay `21.839s`, import `88ms`, handle `542ms`, draw-batch-submit `3.234s`) |
| H37 | `commit_chunk_replay_cpu_ms` is mostly draw-run scan/state/const dispatch | rejected; queued submission/snapshot accepted | [state-churn-encode-encode-phase.25](state-churn-encode-encode-phase.25.md) (replay `22.224s`, queue submission `9.927s`, nested snapshot `7.697s`, draw-batch submit `3.229s`, draw-run submit `2.094s`) |
| H38 | `CommandQueue` submit residual can be localized before changing batching behavior | accepted attribution | [state-churn-encode-encode-phase.26](state-churn-encode-encode-phase.26.md) (`commit_chunk_draw_batch_submit_cpu_ms=3629.383`; batch append `2379.837ms`, compat scan `559.625ms`; resource mark/slot/chunk commit are small) |
| H39 | Snapshot cache misses may be inflated by declared draw-packet deltas that do not actually change non-binding state | rejected-current | state-churn-encode-encode-phase.28 (`draw_packet_declared_nonbinding=419,990`, `actual_nonbinding=419,990`, `redundant_nonbinding=0`) |
| H40 | Snapshot/uniform cost is mostly real payload construction and hashing, not redundant invalidation | accepted local CPU win; FPS flat | state-churn-encode-encode-phase.28, snapshot-cache-snapshot.10 (`uniform_refresh 2014.263ms→814.507ms`, snapshot submission `7622.807ms→6495.069ms`, sampled FPS `15.717→15.752`) |
| H41 | The batch append owner is raw payload byte copy | rejected; state/uniform attribution accepted | state-churn-encode-encode-phase.29 (`state=958ms`, `uniform=902ms`, `payload=65ms` despite `232.5MB` copied) |
| H42 | Removing extra `CanonicalDrawState` value hops cuts the state append child | accepted CPU win | state-churn-encode-encode-phase.30 (`append` `2708→2116ms`, `state` `958→720ms`, GPU flat) |
| H43 | Remaining state append cost is PSO/invariant construction | rejected; SoA append attribution accepted | state-churn-encode-encode-phase.31 (`state=879ms`, `SoA=707ms`, `PSO=50ms`, `invariant=22ms`; split timers add overhead) |
| H44 | Slot-local full-state interning amortizes the state SoA push | rejected; frontend generation fast path accepted as next proof | state-churn-encode-encode-phase.32 (`0.150866%` state reuse hit rate; SoA `707ms→962ms`; first add generation/lane opportunity counters) |
| H45 | Adjacent same stable-generation/lane submissions can skip the deep compat compare | accepted proof | state-churn-encode-encode-phase.33 (`52.524409%` same-generation/lane pairs; `385,120/385,120` compatible; `0` incompatible) |
| H46 | Using the generation/lane stamp as a compat fast path removes the compat scan bucket | accepted CPU win | state-churn-encode-encode-phase.34 (`compat_scan` `557.621ms→44.923ms`; `draw_batch_submit` `3491.771ms→3070.200ms`) |
| H47 | Filling queued `DrawRunSubmission` entries in place removes a first-order queue copy | accepted small CPU win; rejected as first-order | state-churn-encode-encode-phase.35 (`queue-snapshot residual` `2286.890ms→2193.875ms`; total queue `-0.60%`) |
| H48 | Reusing a device-owned pending submission vector removes per-chunk allocation churn | rejected-current | state-churn-encode-encode-phase.36 (`queue-snapshot residual` `2193.875ms→2200.657ms`; scratch branch removed) |
| H49 | Reusing draw binding snapshot scratch removes per-group vector allocation churn | accepted small CPU win | state-churn-encode-encode-phase.37 (`batch_binding_snapshot` `0.119755→0.089566ms/present`; total replay flat) |
| H50 | Shader bytecode value ownership makes layout/state copies allocate and copy bytecode per draw | accepted CPU win | state-churn-encode-encode-phase.38 (`state_copy` `0.421921→0.271225ms/present`; queue submit `5.176033→4.792983ms/present`) |
| H51 | Sampler `FlatStateSet` capacity can be reduced from ID-space 64 to public D3DSAMP 16 | accepted CPU win | state-churn-encode-encode-phase.39 (`FlatDrawStateRecord` `18,736→11,056B`; `state_copy` `0.271225→0.181239ms/present`) |
| H52 | Texture-stage `FlatStateSet` active-entry capacity can be split from the 64-key DeviceState id space | accepted targeted CPU win | state-churn-encode-encode-phase.40 (`FlatDrawStateRecord` `11,056→9,008B`; `append_state_soa` `0.263628→0.220626ms/present`; `encode_draw` mixed) |
| H53 | Render-state `FlatStateSet` needs an active-entry count proof before compaction | accepted proof; shrink held | state-churn-encode-encode-phase.41 (`render_state_entries_max=62`, `gt64=0`, but default table already has 62 entries) |
| H54 | Render-state flat payload can use a 128-slot priority active-entry set while keeping the full 256-state digest | accepted bounded CPU width win | [state-churn-encode-encode-phase.42](state-churn-encode-encode-phase.42.md) (`FlatDrawStateRecord` `9,008→7,984B`; GT1 render max `62`, overflow `0`; GPU flat) |
| H55 | Same-stamp non-front submissions can skip copied canonical state if batching is stamp-only | accepted proof; strict grouping remains diagnostic | [state-churn-encode-encode-phase.44](state-churn-encode-encode-phase.44.md) (`DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1`; `4.10GB` state bytes elided, state-copy CPU `-46.07%`, sampled FPS flat) |
| H56 | Elided submissions may still pay large `DrawRunSubmission` carrier construction cost | accepted bounded attribution | [state-churn-encode-encode-phase.45](state-churn-encode-encode-phase.45.md) (`emplace=1,123.253ms`, `13.99%` of queued submission, `0.646ms/present`; phase36 scratch reuse remains rejected) |
| H57 | Optional queued-submission carrier storage removes default construction without reusing vector capacity | accepted bounded CPU win | [state-churn-encode-encode-phase.46](state-churn-encode-encode-phase.46.md) (`emplace` `1,123.253→573.056ms`, `-48.98%`; queue submission `-2.04%`; sampled FPS `+0.64%`) |
| H58 | Default path still materializes non-front state that batch append discards | accepted attribution | [state-churn-encode-encode-phase.47](state-churn-encode-encode-phase.47.md) (`411,362` records / `4.209GB`, `46.70%` of batch records) |
| H59 | Same-stamp N-1 state-copy elision can be promoted without stamp-only batching | accepted default CPU cleanup | [state-churn-encode-encode-phase.48](state-churn-encode-encode-phase.48.md) (`discarded_state_records 411,362→0`, `state_elided=412,180`, state-copy CPU `261.001→138.856ms`, FPS flat/noisy) |
| H60 | Binding-packet cache associativity can reduce direct-map collision misses enough to cut encode CPU | rejected-current | state-churn-encode-encode-phase.49 (misses/collisions `189k→133k`, but cache CPU `706.875→816.355ms` and encode CPU `+86ms`; code reverted) |
| H61 | The default indexed path spends CPU preparing diagnostic index byte spans even when all diagnostics/reorder paths are off | accepted CPU cleanup | state-churn-encode-encode-phase.50 (`index_setup` `636.514→342.602ms`, index phase `775.311→480.350ms`, total encode only `-0.73%`, FPS flat/noisy) |
| H62 | Uniform payload append cost is dominated by repeated lookup-reserve checks | rejected-current | state-churn-encode-encode-phase.51 (`append_uniform/present` `0.474488→0.474157ms`, parent append `+2.05%`; code reverted) |
| H63 | The remaining uniform append child is mostly payload copy/materialization rather than reserve/linking | accepted attribution | [state-churn-encode-encode-phase.52](state-churn-encode-encode-phase.52.md) (`append_copy=813.196ms`, lookup `294.215ms`, reserve `53.018ms`; attribution timers only) |
| H64 | Uniform payload append copy includes an avoidable aggregate-record temporary | accepted CPU cleanup | state-churn-encode-encode-phase.53 (`append_copy` `813.196→602.274ms`, append-uniform parent `-13.15%`; FPS flat/noisy) |
| H65 | Reusing resolved prefetched PSO handles removes the remaining pipeline-lookup parent | rejected-current | state-churn-encode-encode-phase.54 (`152,261` cache hits, but `pipeline_lookup` `934.420→950.626ms`; code reverted) |
| H66 | The `argbuf_open` parent is mostly actual Metal argument-buffer open work | rejected as phrased; post-open attribution accepted | state-churn-encode-encode-phase.55 (`open_call=573.804ms`, `reopen_post=891.359ms`, table bind `178.803ms`, cached repoint `269.898ms`, content probe `123.303ms`) |
| H67 | Pre-open component identity can skip whole argbuf table reopen in GT1 | rejected-current | state-churn-encode-encode-phase.56 (`961,473` candidates, `0` skips; VS misses `812,520`; identity check cost `956.102ms`) |
| H68 | The phase55 post-open residual is a single hidden argbuf child | rejected; distributed bookkeeping accepted | state-churn-encode-encode-phase.57 (table probe `50.933ms`, byte account `51.990ms`, cbuf cache/dirty scans `118.813ms`, force dirty `104.757ms`; attribution timers add overhead) |
| H69 | The remaining binding-packet plan parent has one large child worth primary optimization | rejected as primary lever; attribution accepted | state-churn-encode-encode-phase.58 (`DXMT9_PERF_BINDING_PACKET_PLAN_SPLIT=1`: fragment plan largest at `0.204682ms/present`; default-off guard keeps child counters `0` and parent near baseline) |
| H70 | Slot-local `DrawUniformPayload` dedup lookup is worth its GT1 CPU cost | rejected as default assumption; accepted diagnostic micro-win | state-churn-encode-encode-phase.59 (`DXMT9_DISABLE_DRAW_UNIFORM_PAYLOAD_DEDUP=1`: lookup `276.107→0ms`, appends `877,508→930,994`, append-uniform `1041.108→799.528ms`, but queue submission flat) |
| H71 | The `encode_draw_issue_cpu_ms` bucket hides dxmt9 wrapper or diagnostic overhead | rejected; indexed Metal draw-call attribution accepted | state-churn-encode-encode-phase.60 (`DXMT9_PERF_DRAW_ISSUE_SPLIT=1`: all draws indexed, visibility/expanded/split/nonindexed `0`, Metal draw call `897.049ms` / `77.0%` of issue parent) |
| H72 | Argbuf cached-repoint/content-probe residual has one stage worth primary optimization | rejected as primary lever; attribution accepted | state-churn-encode-encode-phase.61 (`DXMT9_PERF_ARGBUF_CBUF_PROBE_SPLIT=1`: FFPPS repoint `899,453` calls / `345.390MB` but `137.306ms`; VS probe `143,728` hits / `788,015` misses and `83.048ms`; dirty VS update remains `936.123ms`) |
| H73 | Dirty VS cbuf updates are stale-cache repeats that can be repointed or skipped by identity | rejected-current | state-churn-encode-encode-phase.62 (`DXMT9_PERF_ARGBUF_CBUF_DIRTY_IDENTITY=1`: dirty VS probes `808,845`, hits `0`, misses `788,347`, no-cache `20,498` matching render-pass begin; cached dirty VS miss rate `100%`) |
| H74 | Argbuf table reopen is mostly caused by over-broad non-shader payload hash changes | rejected-current; shader-constant attribution accepted | state-churn-encode-encode-phase.63 (`DXMT9_PERF_ARGBUF_PAYLOAD_DELTA=1`: payload changes `931,917` exactly match no-dirty reopen rows, `nonconst_only=0`, VS/PS explain all changes; resource-array forced reopen `0`) |
| H75 | Dirty VS cbuf upload width is mostly the dirty register range | rejected-current; usage-prefix attribution accepted | state-churn-encode-encode-phase.64 (frame60 avg dirty float regs/upload `0.702`, usage `45.147`, plan `57.483`, VS bytes/upload `984.712`; indexed-float full fallback `20.21%`) |
| H76 | Shader-specific packed constants for non-indexed shaders are the next large cbuf width target | rejected-current; indexed BLENDINDICES window proof remains open | state-churn-encode-encode-phase.65 (frame60 bytecode corpus safe packed save `0`; theoretical gap is entirely indexed VS, `59` draws / `59` full uploads; all hot indexed rows use static offsets `0;1;2` with `a0.x/a0.y`, requiring vertex BLENDINDICES dynamic-window proof before packing) |
| H77 | Indexed VS `a0.x/a0.y + 0..2` can be bounded to a narrow BLENDINDICES window | rejected-current for top indexed VS sample | state-churn-encode-encode-phase.66 (`--dump-indexed-geometry-vs 0x18ffaf75e52f4615`: `12` payloads, `75,395` vertices sampled; many draws need <=`50` regs, but draw `30615` observes `a0.x=0..255`, `a0.y=0..254`, requiring full-range fallback) |
| H78 | Stage 2 argument-buffer hybrid is a net CPU win over Stage 1 direct cbuf binds for current GT1 | rejected-current as CPU policy; accepted attribution | state-churn-encode-encode-phase.67 (`DXMT9_DISABLE_ARGBUF_HYBRID=1`: same `1740` presents / `1.285M` draws; `encode_draw_cpu_ms` `17,399.519 -> 12,847.687`, argbuf setup `4,322.402 -> 0`, transient bytes `909.169MB -> 62.660MB`, but completion wait rises `46.913s -> 49.233s`) |
| H79 | Disabling Stage 2 argument-buffer hybrid improves average FPS once measured with low-overhead frame sampling | rejected-current as FPS policy | state-churn-encode-encode-phase.68 (warm `encode_draw_cpu_ms` p50 `8.621 -> 5.545ms`, but warm `completion_wait_ms` p50 `27.409 -> 30.010ms`; warm FPS p50 `17.202 -> 17.323`, tail-600 FPS p50 `16.855 -> 16.817`) |
| H80 | Publish-time PSO prefetch is the current Present-record replay owner | accepted diagnostic placement; superseded by H81 default | state-churn-encode-encode-phase.69 (`commit_chunk_replay_present_record_cpu_ms=2.757ms/present`; `prepare_slot_pso_prefetch_cpu_ms=2.497ms/present`, `90.6%` of the Present record. `DXMT9_DISABLE_PUBLISH_PSO_PREFETCH=1` moves the work to encode lookup but improves repeated warm FPS p50 by `+0.564`) |
| H81 | Encode-worker slot-copy PSO prefetch removes the serialized Present cost while preserving prefetched handles | accepted default | state-churn-encode-encode-phase.70 (new default: `prepare_slot_pso_prefetch_cpu_ms=0`, `encode_slot_pso_prefetch_cpu_ms=2.605ms/present`, `encode_draw_pso_prefetch_handle_missing=0`, warm FPS avg `17.628 -> 18.345`) |
| H82 | The remaining encode-slot PSO prefetch cost is repeated draw PSO lookup/key work, not selector or depth lookup overhead | accepted attribution | state-churn-encode-encode-phase.71 (`encode_slot_pso_prefetch_cpu_ms=2.806ms/present`, draw lookup `2.506ms/present`, depth lookup `0.127ms/present`, `591,477` eligible candidates, `503` draw PSO slots, handle misses `0`) |
| H83 | Most encode-slot draw PSO lookups resolve to final handles already seen in the same slot | accepted opportunity | state-churn-encode-encode-phase.72 (`584,441` observed final handles; `484,107` slot-repeat hits = `82.832%`; adjacent hit ratio `35.550%`; overflow `0`; repeated-handle share of current draw lookup ~= `2.061ms/present`) |
| H84 | Slot-local probe-key memo removes repeated global PSO cache lookup but exposes resolved-key construction as the owner | accepted CPU cleanup | state-churn-encode-encode-phase.73 (`draw_lookup` `2.489 -> 0.225ms/present`, memo hit ratio `82.796%`, overflow `0`, handle misses `0`, but parent only `2.803 -> 2.660ms/present`; `draw_key_resolve=2.015ms/present`) |
| H85 | Slot-local semantic PSO memo removes about half of repeated resolved-key/source-context work | accepted CPU cleanup | state-churn-encode-encode-phase.74 rejects pointer identity (`0 / 586,299` hits), then accepts semantic memo default: hits `310,499 / 587,325` (`52.867%`), overflow `0`, handle misses `0`, `draw_key_resolve` `2.237 -> 1.060ms/present`, and parent prefetch `2.942 -> 1.766ms/present` |
| H86 | Semantic memo key/probe/store overhead is the next PSO-prefetch bottleneck | rejected as primary; attribution accepted | state-churn-encode-encode-phase.75 splits the default semantic memo: key `0.058ms/present`, probe `0.036ms/present`, store `0.008ms/present`, subtotal `0.102ms/present`; hit ratio remains `52.805%`, overflow and prefetched-handle misses stay `0`, and the remaining owner is still draw-key resolve at `1.062ms/present` |
| H87 | Conservative semantic misses that collapse to probe-key hits are mostly texture-handle exactness | accepted attribution | state-churn-encode-encode-phase.76 classifies semantic-miss -> probe-key-hit rows: `179,072` collapses, `diff_texture_handles=176,291` (`98.447%`), `diff_texture_handles_only=169,729` (`94.783%`), `diff_hash_only=0`, `diff_unknown=0`; visual smoke remains normal |
| H88 | A texture-handle-blind resource-shape memo can cover most remaining semantic misses without changing final PSO shape | accepted opportunity | state-churn-encode-encode-phase.77 probes semantic non-overflow misses with a pre-resolve resource-shape key that ignores exact texture handles, then validates after the normal resolve: `167,983 / 276,842` candidates hit (`60.678%`), all `167,983` hits match the final canonical probe key, every mismatch bucket is `0`, overflow is `0`, skipped pipeline and Metal errors stay `0`, and visual smoke remains normal |
| H89 | Default-off resource-shape memo behavior bypasses the expected resolved-key work | accepted default-off smoke | state-churn-encode-encode-phase.78 runs `DXMT9_ENABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1`: resource-shape hits `167,974 / 276,912`, overflow `0`, `draw_key_resolve` `1.098 -> 0.442ms/present` vs the validation run, parent prefetch `2.019 -> 1.335ms/present`, canonical probe-key hits collapse to `7,870` because the shape memo consumes the repeated rows first, and handle-missing/skipped-pipeline/Metal-error counters stay `0`; repeat a paired current-code default A/B before default promotion |
| H90 | Resource-shape memo repeats the CPU win against a current-code default baseline | accepted CPU win; not FPS proof | state-churn-encode-encode-phase.79 pairs current default vs `DXMT9_ENABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1`: parent prefetch `1.865 -> 1.335ms/present`, `draw_key_resolve` `1.060 -> 0.442ms/present`, `draw_resolve_variant_key` `0.667 -> 0.276ms/present`, resource-shape hits `167,974`, probe-key hits `175,758 -> 7,870`, overflow/handle-missing/skipped-pipeline/Metal-error counters stay `0`, and visual smoke remains normal; sampled FPS `16.924 -> 16.929` is noise |
| H91 | Resource-shape memo repeated A/B confirms a stable local CPU cleanup but not an FPS owner | accepted repeat CPU win | state-churn-encode-encode-phase.80 repeats the low-overhead pair: r2 parent prefetch `1.853 -> 1.332ms/present`, `draw_key_resolve` `1.053 -> 0.441ms/present`, and `draw_resolve_variant_key` `0.664 -> 0.275ms/present`; r1 and r2 both save about `0.52-0.53ms/present` in the PSO-prefetch parent, resource-shape hits stay stable (`167,974`, `167,252`), overflow/handle-missing/skipped-pipeline/Metal-error counters stay `0`, and visual smoke remains normal, but sampled FPS moves `+0.005` then `-0.073`, so this is a default-promotion candidate only as CPU cleanup, not as the broader GT1 FPS fix |
| H92 | Resource-shape memo can be default-on as a CPU cleanup with opt-out validation | accepted default CPU cleanup; not FPS proof | state-churn-encode-encode-phase.81 promotes the resource-shape memo to the default encode-slot PSO prefetch path, with `DXMT9_DISABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1` as opt-out. The no-env smoke records shape candidates/hits `277,156 / 168,200`, overflow `0`, probe-key hits `7,863`, handle-missing/skipped-pipeline/Metal-error counters `0`, visual smoke normal, and local timing in the expected band (`prefetch=1.336ms/present`, `draw_key_resolve=0.436ms/present`); sampled FPS `16.911` and completion wait `26.377ms/present` keep this out of the FPS-owner lane |
| H93 | Resource-shape memo default promotion should not add per-slot heap allocation | accepted hot-path cleanup; not FPS proof | state-churn-encode-encode-phase.82 replaces the default-on resource-shape memo table's per-slot `make_unique` with thread-local epoch scratch. The table is about `671,744B`; the 120s smoke keeps the phase81 mechanism band (`276,393` candidates, `167,727` hits, overflow `0`, probe-key hits `7,868`, handle-missing/skipped-pipeline/Metal-error counters `0`) and visual output normal. Local timing remains in the enabled band (`prefetch=1.289ms/present`, `draw_key_resolve=0.433ms/present`), while sampled FPS `16.880` and completion wait `27.002ms/present` remain noisy/unchanged |
| H94 | Remaining encode-slot PSO memo tables should not zero-init per slot | accepted hot-path cleanup; not FPS proof | state-churn-encode-encode-phase.83 converts final-handle, semantic, and probe-key memo tables to thread-local epoch scratch. The 120s smoke keeps the same mechanism band: semantic hits/misses `306,884 / 277,109`, resource-shape hits/misses `167,036 / 110,073`, probe-key hits/misses `7,875 / 102,198`, all memo overflows `0`, handle-missing/skipped-pipeline/Metal-error counters `0`, and visual output normal. Local timing stays enabled-band (`prefetch=1.290ms/present`, `draw_key_resolve=0.439ms/present`); sampled FPS `16.861` and completion wait `28.443ms/present` remain noisy/unchanged |
| H95 | Stage 2 slot-30 argbuf table bind shadowing is still not useful | accepted cleanup; rejected FPS lever | state-churn-encode-encode-phase.84 replaces the redundant `argbufTableHash` / `argbufTableValid` side shadow with the existing exact slot-30 `(buffer handle, offset)` vertex-buffer bind shadow. The 120s smoke is visually normal and keeps correctness counters clean (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, `render_split_hazard=0`), but `encode_draw_argbuf_table_bind_skipped` remains `0` over `987,526` table binds. The argbuf setup band remains structural (`setup=2.522ms/present`, `open=1.397ms/present`, `cbuf_update=0.979ms/present`), so further Stage 2 work must target table storage/reopen shape or cbuf update frequency, not slot-30 shadowing |
| H96 | Stage 2 argbuf reopen child timers should be opt-in only | accepted hot-path cleanup; not FPS proof | state-churn-encode-encode-phase.85 adds `DXMT9_PERF_ARGBUF_REOPEN_SPLIT=1` as the opt-in guard for the phase57 table/cbuf/dirty child timers. The 120s smoke is visually normal, all child split counters are `0.000ms` by default, aggregate argbuf counters remain live (`reopen_post=0.514ms/present`, `cbuf_update=0.979ms/present`), and correctness counters stay clean. This reduces default-profile perturbation but leaves the structural Stage 2 argbuf model unchanged |
| H97 | Stage 2 cbuf cached-repoint/content-probe timers should be opt-in only | accepted hot-path cleanup; not FPS proof | state-churn-encode-encode-phase.86 gates the aggregate cached-repoint and content-probe timers behind `DXMT9_PERF_ARGBUF_CBUF_PROBE_SPLIT=1`, matching the stage children. The 120s smoke is visually normal, the timer counters are `0.000ms` by default, and sizing counters remain live (`cached_repoint_calls=1,726,364`, `cached_repoint_bytes=400,548,256`, `content_probe_calls=965,978`). This removes another attribution-only timer layer from the default profile; the remaining structural owners are still argbuf setup/cbuf update, encode-slot PSO prefetch, queued draw submission, and completion wait |
| H98 | Encode-slot PSO semantic memo child timers should be opt-in only | accepted hot-path cleanup; not FPS proof | state-churn-encode-encode-phase.87 adds `DXMT9_PERF_ENCODE_SLOT_PSO_SEMANTIC_SPLIT=1` for the phase75 semantic key/probe/store child timers. The 120s smoke is visually normal, those child counters are `0.000ms` by default, and semantic/resource-shape mechanism counters remain live (`semantic_hits=316,115`, `semantic_misses=281,818`, `resource_shape_hits=171,046`, overflow/handle-missing/skipped-pipeline/error counters `0`). This removes another attribution-only timer layer; remaining PSO-prefetch work is miss-side key resolve and lookup frequency |
| H99 | Uniform-payload child timers are not safe to make default-off | rejected-current; visual timing-sensitive | state-churn-encode-encode-phase.88 tries the same cleanup for `draw_uniform_payload_lookup`, bucket, reserve, copy, and link timers. The mechanism counters remain live and explicit correctness counters stay clean, but two default-off 120s runs produce HUD-only black scene frames. A timer-restored run in the same code state renders normal bloom/particles/scene. The candidate was reverted; this is now a perf/visual-coupling clue around queue append -> slot publish -> encode timing, not a safe cleanup |
| H100 | Current low-overhead refresh keeps encode/snapshot as CPU gates but not independent FPS proof | accepted current attribution | present-pacing-lowoverhead-refresh.33 records a normal visual frame with `18.878fps` average, `gpu_command_buffer_time_ms=3.072ms/present`, and `completion_wait_ms=28.834ms/present`. The current local CPU children are `encode_draw_cpu_ms=8.730ms/present`, `encode_draw_argbuf_setup=1.879`, `argbuf_cbuf_update=0.971`, `binding_packet=1.023`, `stream_bind=1.388`, `encode_slot_pso_prefetch=1.228`, and snapshot/cache lookup `3.057ms/present`. Future state-churn work must reduce one named child and also move completion wait or frame sampling before being promoted as an average-FPS fix |
| H101 | Stream-bind phase child timers should be opt-in only | accepted hot-path cleanup; not FPS proof | [state-churn-encode-encode-phase.89](state-churn-encode-encode-phase.89.md) adds `DXMT9_PERF_STREAM_BIND_PHASE_SPLIT=1`. The default-off 120s smoke keeps normal bloom/particles/scene/HUD output, leaves aggregate `encode_draw_stream_bind_cpu_ms` and phase call counters live, and reports all five phase child timers as `0.000ms`. The opt-in run restores the raster/FFP-stream/shader-stream/texture/index attribution shape. Correctness counters stay clean in both runs, while average FPS/completion wait remain noisy/flat, so this is profile perturbation cleanup rather than a structural `stream_bind` fix |
| H102 | Commit-chunk pending draw submissions should reuse scratch capacity | accepted hot-path cleanup; not FPS proof | [state-churn-encode-encode-phase.90](state-churn-encode-encode-phase.90.md) replaces the per-`dxmt9c_device_commit_chunk` local `pendingDrawSubmissions` vector with thread-local replay scratch. Two low-overhead smokes keep normal bloom/particles/scene/HUD output and repeat the intended local direction versus present-pacing-lowoverhead-refresh.33: r1/r2 average `commit_chunk_replay_cpu_ms/present` `8.457 -> 8.313ms`, queue draw submission `4.314 -> 4.190ms`, and snapshot/cache lookup `3.057 -> 2.957ms`. FPS remains sub-percent/noisy (`18.878 -> 18.930` average), and the r1 completion-overlap signal does not repeat (`completion_wait_with_enqueue_ms` r1/r2 `1.063/0.148ms`), so this is accepted as allocation-churn cleanup only |
| H103 | Current-head next-owner scout keeps the target on P2/P3 serial work plus P4 overlap | accepted current attribution | [state-churn-encode-encode-phase.91](state-churn-encode-encode-phase.91.md) reruns current head after phase90 with low-overhead frame sampling. Visual output is normal, frame CSV avg/p50/p95/tail600 is `18.914 / 18.731 / 26.912 / 17.278fps`, and clean counters stay clean. The current top owners are still `completion_wait_ms=27.579ms/present` with only `0.120ms/present` overlapped enqueue, `encode_chunk_cpu_ms=10.440ms/present`, `commit_chunk_replay_cpu_ms=8.244ms/present`, queue draw submission `4.159ms/present`, and snapshot `3.465ms/present`. Snapshot batch miss remains `2.147ms/present`; backend encode is distributed across argbuf setup `1.880`, PSO prefetch `1.220`, stream bind `1.161`, binding packet `1.024`, and cbuf update `0.965ms/present`. This confirms phase90 did not change the next-owner class |
| H104 | Current copy-elision smoke proves state-copy cleanup is active but no longer the owner | accepted current validation | [state-churn-encode-encode-phase.92](state-churn-encode-encode-phase.92.md) reruns the low-overhead no-gputrace path and verifies `413,344` adjacent same-generation/lane submissions, `4.23GiB` of elided state copies, and `0` same-generation/lane incompatibilities. State copy is down to `0.077ms/present`, while uniform materialization remains `885,613` rows / `9.07GiB`, `d3d9_snapshot_uniform_elided=0`, uniform hash is `1.028ms/present`, and batch uniform append is `0.627ms/present`. Visual output is normal and sampled FPS remains in the same noisy band (`18.898fps` avg), so the next owner is uniform/hash/hot-build or P4 overlap, not more N-1 canonical-state copying |
| H105 | Adjacent uniform-payload hash reuse is too rare to justify a broad uniform elision path | rejected-current | [state-churn-encode-encode-phase.93](state-churn-encode-encode-phase.93.md) adds observation-only `DrawRunSubmission::uniformPayloadHash` counters and reruns the low-overhead no-gputrace path. The first run has a HUD-only black screenshot and is kept only as a numeric scout; the second run uses `--capture-delay-sec 40` and captures a normal GT1 scene. Both runs agree: same-payload-hash adjacent rows are only `5,512 / 784,666` (`0.702%`) and `5,531 / 783,821` (`0.706%`) respectively, all under different uniform generations. Uniform materialization remains about `9.04GiB`, uniform hash remains `~1.15ms/present`, and append-uniform remains `~0.64ms/present`. Do not implement a broad adjacent-payload uniform copy elision from this signal; target hash/build cost, payload storage shape, or P4 overlap instead |
| H106 | Uniform component hashes show PS reuse is common but VS hashing remains the larger local owner | accepted attribution; design open | [state-churn-encode-encode-phase.94](state-churn-encode-encode-phase.94.md) splits the same adjacent-uniform probe into VS and PS constant component hashes. The normal visual no-gputrace run records `801,819` adjacent pairs. Full payload equality remains rare (`5,037`, `0.628%`), but PS constant hash equality is common (`514,938`, `64.221%`) and mostly same-state-lane (`412,182`, `51.406%`). VS constant equality is only `141,295` (`17.622%`). The measured cost shape keeps VS hashing first (`d3d9_snapshot_uniform_build_vs_const_hash_cpu_ms=0.610ms/present`) while PS copy+hash is smaller (`0.155ms/present` combined). Current code still has one `drawUniformGeneration_` from `mutableShaderConstantsState()`, so component reuse needs explicit VS/PS generation tracking before it can skip copy/hash work; do not infer a safe optimization from post-build hash equality alone |
| H107 | VS/PS component generations are a valid local cleanup but not the current FPS lever | accepted local cleanup; not FPS proof | [state-churn-encode-encode-phase.95](state-churn-encode-encode-phase.95.md) adds explicit VS/PS shader-constant generations and reuses unchanged cache-owned shader-constant halves during uniform refresh. The 120s no-gputrace run is visually normal and correctness counters stay clean. Against phase94, PS constant copy drops `0.084 -> 0.048ms/present`, PS constant hash drops `0.071 -> 0.048ms/present`, and total uniform hash drops `1.027 -> 0.989ms/present`. VS constant hash stays effectively flat (`0.610 -> 0.610ms/present`), encode chunk is flat/noisy (`10.477 -> 10.525ms/present`), and completion wait remains dominant (`27.546 -> 27.281ms/present`). Keep the component-generation patch as a low-risk CPU cleanup, but the next average-FPS proof still needs VS-hash reduction, payload/append storage shape, or P4/serial-stage overlap |
| H108 | Cache-miss shader-constant hash reuse is safe but too small to be the VS/FPS lever | accepted local cleanup; rejected FPS lever | [state-churn-encode-encode-phase.96](state-churn-encode-encode-phase.96.md) lets direct and binding-agnostic cache misses reuse previous VS/PS component hashes when both stage constant generation and scanned usage bounds are unchanged. The normal 120s no-gputrace run lowers total uniform hash `0.989 -> 0.955ms/present`, VS hash `0.610 -> 0.592ms/present`, and batch-miss VS hash `0.300 -> 0.284ms/present`, but full indexed VS hashes remain `165,734` and completion wait stays dominant (`27.655ms/present`). Treat this as bounded cleanup only; the next proof should target full indexed VS hash/storage frequency, uniform payload append width, larger encode children, or P4 overlap |
| H109 | Indexed-float VS hash can safely trim unused int/bool tails but remains a small local cleanup | accepted local cleanup; rejected FPS lever | [state-churn-encode-encode-phase.97](state-churn-encode-encode-phase.97.md) hashes full float constants for indexed-float-only shaders but only the scanned int/bool prefixes. The 120s no-gputrace run is visually normal and lowers VS hash bytes `791.56MB -> 747.09MB`, VS hash CPU `0.592 -> 0.561ms/present`, and total uniform hash `0.955 -> 0.920ms/present`. Full indexed VS hashes still occur `165,873` times, uniform materialization remains `8.46GiB`, append-uniform remains `0.628ms/present`, and completion wait remains `27.538ms/present`, so this closes the safe tail reduction without changing the current FPS owner |
| H110 | Uniform append storage width should be visible without enabling timing-sensitive child timers | accepted instrumentation | [state-churn-encode-encode-phase.98](state-churn-encode-encode-phase.98.md) adds `draw_uniform_payload_append_bytes`, a non-timer counter incremented by `sizeof(DrawUniformPayloadRecord)` (`10,256B`) on each unique uniform payload append. This does not optimize the path; it lets the next low-overhead scout directly size backend uniform SoA copy/storage width alongside `d3d9_snapshot_uniform_materialized_bytes` and `submit_draw_run_batch_append_uniform_cpu_ms` |
| H111 | Uniform payload summary should separate frontend materialization from backend append width | accepted summary tooling | [state-churn-encode-encode-phase.99](state-churn-encode-encode-phase.99.md) adds a `Uniform Payload Derived` block to `3dmark05-perf-summary.md`. It reports materialized bytes/present, append bytes/present, bytes/append, append records per materialized snapshot, append-byte share, and snapshot-elision share. This is not a new runtime probe; it makes the phase98 counter directly comparable with the residual P2/P3 buckets on the next unlocked 120s scout |
| H112 | Uniform payload A/B should gate frontend and backend byte-width movement separately | accepted compare tooling | [state-churn-encode-encode-phase.100](state-churn-encode-encode-phase.100.md) extends `compare_3dmark05_perf_counters.py` so the same uniform split appears in A/B reports and can be required with `--require-uniform-materialized-bytes-decrease` or `--require-uniform-append-bytes-decrease`. This turns the phase98/99 observability into a reusable verification gate for the next uniform storage, hash, or copy-width candidate |
| H113 | Uniform/hash/append owner gates should target the residual CPU children, not only byte width | accepted compare tooling | [state-churn-encode-encode-phase.101](state-churn-encode-encode-phase.101.md) extends the run-level A/B compare and wrapper/finalizer pass-through with per-present gates for `d3d9_snapshot_cache_uniform_build_cpu_ms`, `d3d9_snapshot_cache_uniform_hash_cpu_ms`, batch-miss VS/PS/nonconst hash children, `d3d9_snapshot_uniform_copy_cpu_ms`, `submit_draw_run_batch_append_uniform_cpu_ms`, and the uniform payload lookup/append-copy children. Use these gates for the next no-gputrace uniform/hash/storage candidate so a local CPU win proves the intended owner moved before spending another Xcode capture |
| H114 | Full uniform snapshot copy may be wider than the usage-live shader constant payload | accepted instrumentation, measured | [state-churn-encode-encode-phase.102](state-churn-encode-encode-phase.102.md) adds `d3d9_snapshot_uniform_materialized_compact_candidate_bytes` and `_compact_saved_bytes`, derived from the usage-aware VS/PS constant hash byte counts plus the fixed non-shader uniform fields. The 2026-06-15 no-gputrace scout measured `9.17 GB` (`8.54 GiB`) materialized uniform bytes and `6.54 GB` (`6.09 GiB`) conservative compact-copy saved bytes (`71.31%`). This does not change storage yet; it sizes the compact/interned owned-uniform payload carrier before implementation. `--require-current-uniform-compact-saved-bytes-present` gates standalone scouts; `--require-uniform-compact-saved-bytes-present` remains a compare/finalizer gate with `--compare-baseline-output` |
| H115 | Single-run summaries should rank encode CPU children after P4 exposes backend encode | accepted summary tooling | [state-churn-encode-encode-phase.103](state-churn-encode-encode-phase.103.md) adds an `Encode CPU Derived` block to `3dmark05-perf-summary.md`. It reports encode chunk/draw/prefetch CPU per present, `encode_draw` share of encode chunk, and the top coarse/child encode counters sorted by total CPU. This is not additive and not an FPS proof; it turns the next low-overhead scout into an immediate argbuf/stream/binding/PSO/pipeline/issue owner selection before choosing an A/B gate |
| H116 | Argbuf setup should be split in the standalone summary because it is the current top encode row | accepted summary tooling, measured | [state-churn-encode-encode-phase.104](state-churn-encode-encode-phase.104.md) adds an `Argbuf CPU Derived` block. The current `encode-summary-current` scout records `argbuf_setup=1.888ms/present`, split into `cbuf_update=0.968`, `open=0.783`, `reopen_post=0.356`, `open_call=0.337`, and `VS cbuf update=0.529ms/present`. Mechanism counters show no table-bind skips (`0 / 988,876`), frequent dirty cbuf updates (`988,876 / 1,332,067`), and a VS content-probe hit share of only `149,628 / 967,651`. The next code candidate should reduce fresh table open/reopen cost or VS cbuf update frequency/bytes, then prove P4/frame movement |
| H117 | Argbuf A/Bs need target-specific run-level compare gates | accepted compare tooling | [state-churn-encode-encode-phase.105](state-churn-encode-encode-phase.105.md) adds derived compare metrics and gates for `argbuf_setup`, `argbuf_open`, `argbuf_cbuf_update`, and `argbuf_cbuf_update_vs` CPU per present. Use these with existing P4/no-enqueue/frame gates so the next argbuf optimization proves both the intended local owner and the broader average-FPS path moved |
| H118 | Argbuf owner gates must be available through the normal probe/finalizer path | accepted probe tooling | [state-churn-encode-encode-phase.106](state-churn-encode-encode-phase.106.md) wires the four argbuf per-present CPU gates through `run_3dmark05_perf_probe.sh` and `finalize_3dmark05_perf_probe.sh`. The next argbuf candidate can now use one wrapper command to require local `argbuf_setup` / `argbuf_open` / `argbuf_cbuf_update` movement plus the existing P4/frame gates, instead of hand-running the compare script after the probe |
| H119 | Dirty FFP PS argbuf cbuf updates should follow the direct-build storage policy | accepted CPU cleanup; runtime proof pending | [state-churn-encode-encode-phase.107](state-churn-encode-encode-phase.107.md) adds `buildFfpPsConstsUploadBytes()` and uses it in the dirty FFP PS argbuf lane, removing the stack `FfpPsConsts` plus copy for that update path. Native coverage proves byte identity with the value builder. This is a narrow hot-path cleanup only; the next 120s A/B still needs the argbuf cbuf-update gate, and broader P4/frame movement is not implied |
| H120 | Dirty FFP PS direct-build is visually safe but too small to be the FPS owner | accepted smoke; rejected FPS owner | state-churn-encode-encode-phase.108 runs current head with 120s no-gputrace after the direct-build change. The screenshot is a normal GT1 scene with bloom/muzzle/tracer/particle effects, `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, and `render_split_hazard=0`. The changed FFP PS dirty lane is only `0.046ms/present`, while current owners remain `completion_wait_without_enqueue=27.028ms/present`, `encode_chunk=10.804ms/present`, `argbuf_setup=1.901ms/present`, VS cbuf update `0.534ms/present`, and argbuf open `0.784ms/present` |
| H121 | Argbuf reopen/probe children must rank in the standalone summary | accepted summary tooling | [state-churn-encode-encode-phase.109](state-churn-encode-encode-phase.109.md) adds reopen/post child rows and cbuf cached-repoint/content-probe rows to the `Argbuf CPU Derived` ranking. This is not a runtime win; it keeps the next 120s argbuf scout from burying reopen/post attribution under aggregate `argbuf_open`, and lets the next candidate choose between table open/reopen, cached repoint, content probe, or dirty VS cbuf update before spending an A/B run |
| H122 | Argbuf reopen/post is now the clearest local encode subtree under setup | accepted attribution; not FPS proof | state-churn-encode-encode-phase.110 reruns a 120s no-gputrace argbuf split scout with normal GT1 visual output. It records `argbuf_setup=2.811ms/present`, `argbuf_open=1.679`, `reopen_post=1.248`, `cbuf_update=0.990`, dirty VS cbuf update `0.545`, cached repoint `0.265`, and content probe `0.240`. Because split timers perturb the run, do not use this FPS as a baseline comparison. Use it to prioritize table open/reopen frequency or the no-dirty cbuf hash-mismatch path before another narrow FFP PS cleanup |
| H123 | Source-changed VS/PS argbuf probes can fast-miss safely but only trim a leaf | accepted local cleanup; not FPS proof | state-churn-encode-encode-phase.111 passes previous/current VS and PS source-hash change booleans into the no-dirty hash-mismatch path. Changed lanes now miss and become dirty without recomputing identity. The split attribution run keeps visual output normal and reduces `content_probe=0.240 -> 0.159ms/present`, mainly VS probe CPU `0.046 -> 0.006ms/present`, while hit/miss counts remain stable. This is useful cleanup under `reopen_post`, but the larger targets remain table open/reopen frequency and dirty VS cbuf update |
| H124 | Current uniform compact-carrier opportunity remains large after argbuf cleanup | accepted current baseline | state-churn-encode-encode-phase.112 reruns a low-overhead 120s scout with normal heavy-particle GT1 output and `--require-current-uniform-compact-saved-bytes-present`. The current run still materializes `9.081GB` of full `DrawUniformPayload` snapshots and appends `9.697GB` of backend uniform records (`5.04MB` and `5.39MB` per present). A conservative usage-live carrier would save `6.474GB` (`71.29%`) of materialized bytes, while adjacent uniform reuse remains rejected (`same_generation=0`, same-payload-hash only `4,861`). P4 is still no-enqueue dominated (`26.764ms/present`, `99.557%` no-enqueue share), so the next uniform change must target compact/interned storage and prove both byte/CPU gates plus P4 or frame movement |
| H125 | Backend uniform append is not primarily losing to full-payload equality after semantic hash match | rejected as first-order owner; accepted attribution | [state-churn-encode-encode-phase.113](state-churn-encode-encode-phase.113.md) adds behavior-preserving counters for `ChunkSlot::findDrawUniformPayload()` and immediately measures them in a 120s scout. The run is visually normal and reports `15,945` semantic-hash/full-byte lookup misses, but only `163.5MB` total / `88.7KB` per present, about `1.66%` of `draw_uniform_payload_append_bytes`. This means semantic-key dedup is a real cleanup but not the main append-width owner; the next uniform work should target compact/interned owned storage and frontend materialization width directly |
| H126 | Compact-carrier residual floor is mostly fixed payload, not PS constants | accepted current attribution | [state-churn-encode-encode-phase.114](state-churn-encode-encode-phase.114.md) adds fixed/VS/PS composition counters and measures them in a 120s no-gputrace scout after restaging Wine builtin artifacts. The run timeout-finalizes with `status=pass`, a normal high-effect GT1 screenshot, and clean health counters. Full uniform materialization is still `5.04MB/present`, while a compact carrier would save `3.60MB/present` (`71.34%`). The retained compact candidate is `1.45MB/present`, split fixed `68.70%`, VS live constants `29.09%`, and PS live constants only `2.21%`. This rejects PS-range work as the first compact-storage lever and points the implementation toward split owned storage: fixed-payload interning/sharing plus segmented shader-constant ranges |
| H127 | Fixed-payload reuse needs its own runtime gate before split storage | accepted current attribution | [state-churn-encode-encode-phase.115](state-churn-encode-encode-phase.115.md) adds `DrawUniformPayload::fixedPayloadHash`, adjacent fixed-payload reuse counters, and summary/compare derived shares, then measures a 120s no-gputrace scout. Adjacent fixed-payload reuse is `809,459 / 809,459` (`100.00%`), while fixed+VS+PS reuse is only `0.63%`. The next implementation should split owned uniform storage into fixed-payload handles plus segmented shader-constant ranges rather than chase whole-payload elision |
| H128 | Fixed-payload split storage narrows append bytes but is not enough to move FPS/P2/P3 | accepted local byte-width; rejected FPS owner | [state-churn-encode-encode-phase.116](state-churn-encode-encode-phase.116.md) interns fixed non-shader uniform fields behind a fixed-payload handle and stores compact per-uniform records. The runtime scout keeps normal high-effect GT1 output and reduces `draw_uniform_payload_append_bytes` `9.968GB -> 7.594GB` (`5.36MB -> 4.36MB/present`), but `sampled_avg_fps` does not improve (`17.003 -> 16.357`) and `commit_chunk_replay` / `encode_chunk` worsen within run noise. The next storage step must split usage-live VS/PS constant segments and let prefetch/encoder consumers read compact records directly before claiming a CPU or FPS win |
| H129 | Command-front uniform payload copies should be removed after fixed split | accepted local copy cleanup; rejected FPS owner | [state-churn-encode-encode-phase.117](state-churn-encode-encode-phase.117.md) removes `ChunkSlot::drawRunUniformPayloads`, so draw-run command views no longer retain a second full `DrawUniformPayload` copy for the front draw. PSO prefetch, draw encoding, framegraph emission, and queue diagnostics now resolve the command/front payload through `DrawUniformHandle -> DrawUniformPayloadRecord + DrawUniformFixedPayloadRecord` and materialize into caller-owned scratch only where an existing consumer still needs the legacy full view. The 120s scout is visually normal and clean, but `completion_wait_without_enqueue` stays dominant (`26.939ms/present`), `encode_chunk` is flat (`11.195ms/present`), and `uniform_append_bytes_per_append` is unchanged (`8,291.830B`). This closes one leftover full-copy lane from phase116, but the next real storage step remains segmented VS/PS constants or direct compact consumption |
| H130 | VS/PS stage constants should be split out of `DrawUniformPayloadRecord` | accepted storage-width cleanup; rejected FPS owner | [state-churn-encode-encode-phase.118](state-churn-encode-encode-phase.118.md) replaces the embedded VS/PS arrays in `DrawUniformPayloadRecord` with fixed/VS/PS component handles and adds stage-specific append counters. The per-payload record is now `96B`; the 120s scout keeps normal high-effect GT1 output and lowers aggregate uniform append width from phase117 `8,291.830B -> 4,558.972B/append` (`4.35MB -> 2.39MB/present`). The payload-record-only copy path drops to `96B/append` and `draw_uniform_payload_append_copy_cpu_ms_per_present=0.030`, but aggregate append bytes are now explicitly VS constants (`80.01%`) plus PS constants (`17.80%`), and `submit_draw_run_batch_append_uniform` does not improve (`0.982ms/present`). This closes the payload-record-width issue; remaining uniform work is usage-live/segmented VS constants, direct compact consumers, or upstream constant-churn reduction |
| H131 | Stage-constant append amplification should drive the next uniform storage target | accepted residual attribution | [state-churn-encode-encode-phase.119](state-churn-encode-encode-phase.119.md) adds summary/compare derived metrics that divide actual VS/PS stage append bytes by the existing usage-live compact candidate. The phase118 r2 summary now reports `uniform_stage_constants_append_bytes_per_present=2,340,544.018`, VS append amplification `4.524x`, PS append amplification `13.319x`, and combined stage amplification `5.142x`. This confirms the payload record body is no longer the storage owner; the next uniform candidate must attack full stage constants, preferably VS-first for absolute bytes or direct compact consumers to avoid legacy materialization |
| H132 | Compact stage-constant storage closes the uniform append-width owner | accepted local storage cleanup; rejected FPS owner | [state-churn-encode-encode-phase.120](state-churn-encode-encode-phase.120.md) stores VS/PS stage constants as usage-live byte prefixes in slot-local arenas, leaving stage records as handle + offset/count metadata and preserving legacy scratch materialization for existing consumers. The 120s scout is visually normal and clean, lowers `uniform_append_bytes_per_present` to `490,549.644`, and brings stage amplification to the compact floor (`uniform_stage_append_amplification_vs_compact_stage=0.971x`, VS `1.004x`). Average FPS does not promote (`16.742fps`), and P4/P2/P3 remain dominant (`completion_wait_without_enqueue=26.500ms/present`, `commit_chunk_replay=8.297ms/present`, `encode_chunk=10.972ms/present`). Treat uniform append width as closed; next work should reduce frontend materialization, stage append count, replay/encode serialization, or P4 overlap |
| H133 | Backend compact storage still feeds full legacy uniform scratch consumers | accepted attribution; not FPS proof | [state-churn-encode-encode-phase.121](state-churn-encode-encode-phase.121.md) adds backend materialization counters around `drawRunUniformPayloadForHandle()`. The 120s scout is visually normal and clean, with `0` materialize fallbacks. It reports `17.542MB/present` of backend full `DrawUniformPayload` scratch materialization and `0.616ms/present` CPU, while append storage is already down to `0.490MB/present`. This is a real byte-amplification cleanup target for direct compact consumers, but not the current average-FPS owner; P4/P2/P3 remain dominant (`completion_wait_without_enqueue=26.555ms/present`, `commit_chunk_replay=8.131ms/present`, `encode_chunk=10.991ms/present`) |
| H134 | Command-front uniform scratch can be reused for base-handle backend consumers | accepted local cleanup; rejected FPS owner | state-churn-encode-encode-phase.122 reuses the already materialized command/front `DrawUniformPayload` scratch in draw encoder and framegraph paths when a draw param has no override handle or the same handle as the command front. The 120s A/B stays visually normal and clean, cuts backend legacy materialization `17.542MB/present -> 12.345MB/present` (`-29.62%`) and materialization CPU `0.616 -> 0.449ms/present` (`-27.17%`), with `0` fallbacks. FPS/P4 does not promote: `completion_wait_without_enqueue` stays about `26.7ms/present`, `encode_draw` is noise-flat, and the local gain is only `0.167ms/present`. Keep the cleanup, but rank remaining work behind direct compact consumers or larger P2/P3/P4 serialization |
| H135 | Dirty VS argbuf cbuf updates can often repoint identical cached slices | rejected-current | [state-churn-encode-encode-phase.123](state-churn-encode-encode-phase.123.md) reruns the dirty VS identity probe on current code after compact uniform storage and command-front scratch reuse. The run is clean and normal, but the probe records `862,747` dirty VS candidates with `0` identity hits, `840,847` misses, `21,900` no-cache cases, and `992.154MB` miss bytes. Do not implement a dirty VS identity skip for GT1; the current dirty VS cbuf lane is real VS source churn. Next argbuf work should target table open/reserve/bind frequency, upstream constant update frequency, or broader P2/P3/P4 serialization |
| H136 | Argbuf table open can reuse a chunk completed-seq snapshot | accepted local reserve cleanup; rejected FPS owner | state-churn-encode-encode-phase.124 carries a chunk-local `transientCompletedSeqId` through `EncodeContext` and uses it for argbuf table reservations, avoiding a queue-mutex completion read per table open. The 120s candidate is visually normal and clean, and the narrow reserve child drops `0.196 -> 0.158ms/present`, but the parent does not promote: `argbuf_open` worsens `0.779 -> 0.862ms/present`, `argbuf_setup` `1.845 -> 2.076`, `encode_chunk` `10.820 -> 12.054`, and sampled FPS is only `14.830`. Keep the bounded cleanup, but do not spend more iterations on completed-seq snapshot plumbing; the next argbuf work must reduce fresh table frequency, true cbuf dirty frequency, or broader P2/P3/P4 serialization |
| H137 | Encode-slot PSO prefetch does not need legacy uniform payload materialization | accepted local cleanup; rejected FPS owner | [state-churn-encode-encode-phase.125](state-churn-encode-encode-phase.125.md) removes `drawRunUniformPayloadForHandle()` from `prefetchSlotPipelines()` and proves the prefetch key builders are uniform-value independent. The 120s A/B stays visually normal and clean, reduces backend uniform materialization `12.345MB/present -> 9.011MB/present` (`-27.01%`), materialization CPU `0.449 -> 0.337ms/present`, and PSO prefetch state-copy CPU `0.150 -> 0.016ms/present`. The broader owners stay flat: `encode_chunk` `10.820 -> 10.789ms/present`, `commit_chunk_replay` `8.110 -> 8.078`, and no-enqueue completion wait `26.695 -> 26.632`. Keep the cleanup, but treat remaining FPS work as direct compact consumers only if larger legacy scratch paths disappear, or as serial P2/P3/P4 overlap work |
| H138 | Remaining backend uniform materialization is split between draw encoder base, queue observation, and per-draw params | accepted attribution; not FPS proof | [state-churn-encode-encode-phase.126](state-churn-encode-encode-phase.126.md) adds site tags to `drawRunUniformPayloadForHandle()` and measures a 120s no-gputrace scout. Materialization is `1,469,379` calls / `8.977MB/present` / `0.382ms/present`, split draw-encoder command `36.81%`, queue observation `36.79%`, and draw-encoder param `26.40%`; framegraph and other are `0`. Visual output is normal and health counters are clean, but P4 remains no-enqueue dominated. The next local candidate should remove queue-observation's full payload dependency by caching/compacting the projected-texture compat input, then consider direct compact reads in draw-encoder command paths |
| H139 | VS/FFPVS cbuf content-history scans should not run in default encoder breakdown baselines | accepted instrumentation cleanup; runtime verified | [state-churn-encode-encode-phase.129](state-churn-encode-encode-phase.129.md) gates `recordArgbufCbufUploadContent()` behind `DXMT9_PERF_ENCODER_BREAKDOWN_CBUF_CONTENT=1`, leaving argbuf table/cbuf byte totals live in `DXMT9_PERF_ENCODER_BREAKDOWN=1`. The 120s r3 scout keeps a normal GT1 scene, reports `encode_draw_argbuf_cbuf_observer_cpu_ms=0`, and still emits encoder CSV `argbuf_cbuf_bytes=882,765,800` / `argbuf_table_bytes=26,087,584`; the byte-by-byte observer only returns when explicitly requested |
| H140 | Current materialization scout keeps queue observation closed and ranks argbuf/P4 ahead of uniform scratch | accepted current baseline | state-churn-encode-encode-phase.130 reruns a low-overhead, no-encoder-breakdown scout after the queue-observation and cbuf-content cleanups. The screenshot is visually normal, `QueueObservation` materialization remains `0`, and remaining backend scratch is draw encoder command/param only (`5.688MB/present`, `0.228ms/present`). The larger current owners are still `argbuf_setup=1.838ms/present`, `encode_chunk=10.949ms/present`, `commit_chunk_replay=8.104ms/present`, and `completion_wait_without_enqueue=26.586ms/present`, so the next FPS work should target argbuf dirty/open frequency, replay/snapshot, or P4 overlap rather than another materialization-site cleanup |
| H141 | Current argbuf reopen pressure is not caused by non-cbuf payload hash changes | rejected shortcut; accepted current attribution | state-churn-encode-encode-phase.131 reruns the payload-delta probe on current code with `DXMT9_PERF_ARGBUF_PAYLOAD_DELTA=1` and `DXMT9_PERF_ARGBUF_REOPEN_SPLIT=1`. The normal GT1 run reports `changed_nonconst_only=0`, `payload_changed=995,097`, and all reopens split into VS-only `667,298`, PS-only `155,386`, and VS+PS `172,413`. Replacing the full payload hash with only cbuf-source hashes would save no GT1 reopens; remaining argbuf work is real VS constant churn, persistent/segmented cbuf storage, or a table model that can update cbuf pointers without mutable-table reopen side effects |
| H142 | Current argbuf tables cannot be shared across changed draws by mutating cbuf entries in place | accepted design gate | [state-churn-encode-encode-phase.132](state-churn-encode-encode-phase.132.md) reviews the Stage 2 argbuf lifetime code. Resource-array mode must use fresh tables because texture/sampler IDs are written inline; constants-only mode may reuse a table only when the uniform payload is unchanged. When cbuf pointers change, a shared mutable table would make earlier draws observe the last pointer written at GPU execution time. Future argbuf work must either keep per-draw immutable table lifetime and make it cheaper, split cbufs out of the mutable table, or introduce a stable indirection model with explicit visual, argbuf CPU, and P4/frame gates |
| H143 | Splitting cbufs out of Stage 2 argbuf is a shader/PSO ABI project, not a host-only microfix | accepted next-scope | [state-churn-encode-encode-phase.133](state-churn-encode-encode-phase.133.md) traces Stage 2 through the shader prelude, programmable entry points, FFP emitters, tile FFP kernel, and host descriptor table. All Stage 2 shader lanes read the four cbuf pointers through `ArgbufLayout` at slot 30, so direct cbuf binding needs a new PSO key bit and generated MSL variant. That overlaps the already-measured Stage 1 direct-cbuf policy, which cut local encode CPU but failed the low-overhead FPS/P4 gate. The next small work should target upstream VS constant churn or a deterministic Stage 2b ABI test plan before GT1 |
| H144 | Payload-delta attribution splits changed shader constants into float/int/bool categories | accepted current attribution | [state-churn-encode-encode-phase.134](state-churn-encode-encode-phase.134.md) reruns the heavy `DXMT9_PERF_ARGBUF_PAYLOAD_DELTA=1` probe with opt-in VS/PS float, int, and bool prefix hashes. The normal GT1 run reports `changed_vs=843,136 == changed_vs_float`, `changed_ps=328,826 == changed_ps_float`, and all int/bool changed counters are `0`, with `changed_nonconst_only=0`. The current Stage 2 argbuf reopen churn is therefore float-constant source turnover, not int/bool invalidation or non-cbuf payload hash noise. Next argbuf work should target VS float churn, cheaper immutable table/cbuf storage, or a Stage 2b cbuf ABI; do not chase int/bool invalidation without a new non-zero counter |
| H145 | The first VS float argbuf width counter used unsafe previous-payload ownership | superseded | [state-churn-encode-encode-phase.135](state-churn-encode-encode-phase.135.md) extends the phase 134 probe with changed float4 register-width counters, but the implementation retained a pointer to the previous `DrawUniformPayload`. Per-draw override payloads can live in loop-local scratch, so its width totals are not authoritative. Keep phase 134's float/int/bool component attribution, but use [state-churn-encode-encode-phase.136](state-churn-encode-encode-phase.136.md) for width distribution |
| H146 | Corrected VS float argbuf width is mixed: small rows are common, but the wide tail is significant | accepted current attribution | [state-churn-encode-encode-phase.136](state-churn-encode-encode-phase.136.md) fixes the probe by keeping an owned previous payload copy and adds exclusive `<=1`, `2..4`, `5..16`, `17..64`, and `>64` buckets. VS `<=16` covers `644,146 / 839,414` VS-float-changed rows (`76.86%`), but `180,275` rows (`21.48%`) are `>64`; PS is entirely `<=16` with max `10`. Dirty VS cbuf uploads are `826,524,384B` (`959.6B/update`) versus `729,196,368B` observed changed-reg bytes (`1.13x`). This weakens pure byte-width segmentation as an FPS hypothesis; prioritize reducing argbuf table reopen frequency or VS constant update frequency, and require local argbuf/cbuf plus P4/frame gates for any segmented prototype |
| H147 | Wide VS argbuf deltas own almost all changed-register bytes | accepted current attribution | [state-churn-encode-encode-phase.137](state-churn-encode-encode-phase.137.md) adds bucket-specific register-sum counters. The `>64` VS bucket is only `179,322 / 835,207` rows (`21.47%`) but owns `41,300,477 / 45,350,717` changed float4 registers (`91.07%`). Conversely, VS `<=16` rows are `76.87%` by count but only `8.15%` by registers. This rejects a small-delta-only segmented cbuf path as the first-order argbuf lever; next work should attribute and reduce the wide VS update source or table/reopen frequency |
| H148 | Wide VS argbuf deltas are mostly full-prefix contiguous churn | accepted current attribution | [state-churn-encode-encode-phase.138](state-churn-encode-encode-phase.138.md) adds VS float prefix/span/full-prefix counters. The run reports `span / changed = 1.057x`, so the wide tail is not primarily sparse high-index changes. `full_prefix` rows are `589,543 / 820,563` (`71.85%`) and own `34,811,978 / 44,366,977` changed float4 registers (`78.46%`). This pushes the next useful probe toward shader-pair/source-hash or D3D9 setter-range attribution, not small-delta-only cbuf slicing |
| H149 | Full-prefix VS argbuf churn is concentrated in a few shader-pair source buckets | accepted current attribution | [state-churn-encode-encode-phase.139](state-churn-encode-encode-phase.139.md) adds `DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE=1` and aggregates changed VS float payload rows by VS hash, PS hash, and active prefix width. The normal GT1 run reports `95` source buckets with no overflow. `prefix_regs=256` is only `28.60%` of source rows but owns `42,244,795 / 45,666,770` changed regs (`92.51%`) and `33,297,664 / 35,878,500` full-prefix regs (`92.81%`). The top two shader-pair buckets own `54.63%` of full-prefix regs, and the top five own `71.37%`. The hottest pairs include the known hidden-backend row `60/1` (`0xcf219872fdbbb398 / 0x6f39a816200d9efe`) and the known BLENDINDICES matrix-palette indexed VS (`0x18ffaf75e52f4615 / 0x6f39a816200d9efe`). This rejects diffuse small-delta/table-hash explanations and moves the next gate to D3D9 setter-range attribution for hot VS hashes or a Stage 2b cbuf ABI that avoids mutable-table reopen on cbuf pointer turnover |
| H150 | Wide VS constant records are mostly created by PE dirty-span flush merging, not full-range app setters | accepted current attribution | [state-churn-encode-encode-phase.140](state-churn-encode-encode-phase.140.md) adds `DXMT9_PERF_VS_CONST_SETTER_RANGE=1` and wrapper flag `--probe-vs-const-setter-range` to aggregate app `SetVertexShaderConstantF` calls and flushed `SET_VS_CONST_F` records by VS/PS hash and range. The normal GT1 run reports equal changed-register totals for `call` and `flush` (`17,426,287`), but `flush` emits `24,670,044` range regs (`1.416x` changed) versus app-call `21,578,169` (`1.238x`). Non-overflow app calls have no large ranges, while flush `count >= 64` rows own `6,682,392` changed regs (`38.35%`) and `10,304,650` range regs (`41.77%`). The hot concrete flush rows are `start=0,count=196/201/205`, while hot call rows are `count=3/4`. This promotes sparse dirty-run VS const flushing as the next local experiment before a shader/PSO Stage 2b cbuf ABI |
| H151 | Sparse VS const dirty-run flushing proves the width mechanism but not current FPS movement | rejected-current FPS lever; accepted mechanism | [state-churn-encode-encode-phase.141](state-churn-encode-encode-phase.141.md) reruns the existing `DXMT9_SPLIT_SPARSE_CONST_RECORDS=1` path with setter-range attribution. Flush record width becomes exact (`range/changed 1.416x -> 1.000x`) and flush range regs per present drop `-28.59%`, but flush record events per present rise `+20.98%`, VS cbuf update bytes per present are flat (`+0.64%`), and P2/P3/P4 worsen/noise in the wrong direction (`sampled_avg_fps 16.360 -> 14.938`, replay `8.128 -> 8.949ms/present`, encode `10.847 -> 11.643`, no-enqueue wait `27.999 -> 29.112`). Keep sparse splitting diagnostic-only; the next argbuf/FPS work needs Stage 2 cbuf ABI/storage change or larger replay/snapshot/encode/P4 overlap movement |
| H152 | GT1 argbuf table churn is constants-only cbuf pointer turnover, not resource-array pressure | accepted attribution; not FPS proof | [state-churn-encode-encode-phase.142](state-churn-encode-encode-phase.142.md) adds payload-delta disjoint reopen counters for Stage 2b sizing. The no-gputrace run reports `resource_array=0`, `reopen_cbuf_only=1,004,713`, `cbuf_only_first=21,720`, `cbuf_only_payload_changed=982,993`, and `argbuf_table_bind_calls=1,004,713`. This proves a direct-cbuf Stage 2b ABI would target the whole current slot-30 table-bind count for GT1, but the run screenshot was mostly black with HUD visible, so keep it as counter-only evidence. Stage 2b still needs deterministic shader/PSO ABI tests and a normal-visual P4/frame gate |
| H153 | Stage 2b direct-cbuf now has a deterministic shader/PSO ABI gate | accepted ABI gate; runtime path open | [state-churn-encode-encode-phase.143](state-churn-encode-encode-phase.143.md) adds `argbufDirectCbufMode` to shader source context and PSO key identity, plus native tests for FFP, translated programmable, tile-FFP, host slot constants, and key equality/hash separation. Stage 2b MSL now keeps direct cbuf slots `0/3` and emits no `ArgbufLayout`, while the PSO key cannot alias Stage 1 or Stage 2. Runtime selection and encoder binding are still open and must prove local argbuf table/open movement plus normal visual/P4 frame gates before any FPS claim |
| H154 | Stage 2b direct-cbuf runtime selection removes constants-only argbuf table churn | accepted local CPU win; rejected as average-FPS owner | [state-churn-encode-encode-phase.144](state-churn-encode-encode-phase.144.md) adds the default-off `DXMT9_ARGBUF_DIRECT_CBUF=1` runtime scout. The normal-visual no-gputrace run keeps `588,953` Stage 2 candidates and `0` resource-array candidates, while `encode_draw_argbuf_table_bind_calls`, `argbuf_open`, `argbuf_setup`, and argbuf cbuf update counters all drop to `0`. The run is clean (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`), but still reports `under-pipelined-no-enqueue`, `sampled_avg_fps=16.864`, and `completion_wait_without_enqueue=28.565ms/present`. This closes the table/open mechanism and moves the next FPS proof back to P4/P2/P3 cadence, stream-bind/PSO-prefetch/binding-packet only if they move frame sampling and completion wait |
| H155 | Legacy uniform scratch does not need a pre-materialization zero-fill | accepted local CPU cleanup; rejected as average-FPS owner | state-churn-encode-encode-phase.145 removes full `DrawUniformPayload{}` value-initialization before compact uniform materialization in the draw encoder. The candidate is visually normal and clean (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`), and `encode_draw_cpu_ms_per_present` moves `8.580 -> 8.426` while `sampled_avg_fps` stays noise-flat (`16.865 -> 16.931`). Keep the cleanup, but do not rank legacy scratch zero-fill as the FPS owner; the next proof still needs P4/P2/P3 cadence or a larger encode child |
| H156 | Current direct-cbuf repeat shifts exposed time from encode to replay/publish, not into overlap | accepted local CPU win; rejected FPS owner; visual open | [state-churn-encode-encode-phase.146](state-churn-encode-encode-phase.146.md) reruns `DXMT9_ARGBUF_DIRECT_CBUF=1` against the current state-elision baseline. It cuts `encode_chunk_cpu_ms_per_present` `11.110 -> 8.500` (`-23.49%`) and removes argbuf setup/open/cbuf update/table binds, but `wait -> next enqueue` stays flat (`30.482 -> 30.703ms/present`) because `commit entry -> publish` grows `13.672 -> 16.260ms/present`. The screenshot is not black but is much darker than baseline and shows a large white band, so direct-cbuf remains default-off and correctness-open. Next work should target replay/snapshot/publish cadence or true P4 overlap before more argbuf local cleanup |
| H157 | Current direct-cbuf fails the `v0.0.3` visual-safety gate even though it removes argbuf encode cost | accepted local CPU win; rejected correctness and FPS owner | [state-churn-encode-encode-phase.147](state-churn-encode-encode-phase.147.md) repeats the Stage 2b direct-cbuf scout against a same-day `v0.0.3`-anchored baseline. It again removes argbuf setup/open/cbuf-update/table-bind counters and cuts `encode_chunk_cpu_ms_per_present` `11.311 -> 8.471` (`-25.10%`) and `encode_draw_cpu_ms_per_present` `8.750 -> 6.023` (`-31.17%`). The visual output is severely corrupted, with black scene regions and overexposed white geometry/bands versus the normal baseline frame, while `completion_wait_without_enqueue` worsens `26.839 -> 28.250ms/present`. Keep direct-cbuf default-off; next direct-cbuf work must debug runtime PSO/source/bind correctness before another FPS claim |
| H158 | Direct-cbuf corruption was stale direct cbuf binding on per-draw payload source changes | accepted correctness fix and local CPU win; rejected FPS owner | [state-churn-encode-encode-phase.148](state-churn-encode-encode-phase.148.md) marks direct VS/PS cbuf slots dirty whenever the argbuf cbuf-source hashes change, even if no D3D constant-set dirty bit is live in the current encoder state. The `v0.0.3`-anchored rerun returns to a normal GT1 frame (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`) while still removing argbuf setup/open/cbuf-update/table-bind counters. CPU remains a local win (`encode_chunk_cpu_ms_per_present` `11.311 -> 8.871`, `-21.57%`; `encode_draw_cpu_ms_per_present` `8.750 -> 6.359`, `-27.33%`), but `sampled_avg_fps` is noise-flat (`16.832 -> 16.894`) and `completion_wait_without_enqueue` worsens `26.839 -> 28.354ms/present`. This promotes the fix as correctness-required for the opt-in path, not as an average-FPS owner; keep ranking P4/replay-publish and residual uniform/hash/append work ahead of more argbuf cleanup |
| H159 | `v0.0.3`-anchored current run keeps N-1 state elision closed; residual submit-side owner is uniform/hash/append | accepted current attribution | [state-churn-encode-encode-phase.149](state-churn-encode-encode-phase.149.md) re-reads a same-day current run gated by the `v0.0.3` visual anchor after the direct-cbuf correctness pass. The raw DrawRun state critique is no longer the next implementation target: `d3d9_snapshot_state_elided=412,984` (`4.226GiB`), same-generation/lane compatibility has `410,551` compatible and `0` incompatible pairs, and residual `submit_draw_run_batch_discarded_state_records=3,925` is only about `40.16MiB` for the whole run. The remaining submit-side CPU is elsewhere: `d3d9_snapshot_uniform_elided=0`, `d3d9_snapshot_uniform_materialized=885,840` (`9.092GiB`), `d3d9_snapshot_uniform_build_hash_cpu_ms=1702.902`, `submit_draw_run_batch_append_uniform_cpu_ms=1176.066`, and queue submission snapshot still costs `3.102ms/present`. Next work should not reopen N-1 state materialization unless a regression counter appears; target VS/full-indexed uniform hash, compact consumers, append count/storage shape, or a larger P4 overlap design |
| H160 | `v0.0.3`-anchored setter-range refresh keeps sparse const width closed as an FPS lever | accepted current attribution | [state-churn-encode-encode-phase.150](state-churn-encode-encode-phase.150.md) reruns `DXMT9_PERF_VS_CONST_SETTER_RANGE=1` against a current run gated by the `v0.0.3` visual anchor. The old shape repeats: app calls are mostly small, while concrete flush `count >= 128` rows own `10,192,088` range regs (`81.09%` of concrete flush range regs), and hot rows include `start=0,count=196/201`. But H151 already proved sparse dirty-run splitting only makes records exact; it leaves backend VS cbuf/table pressure and P4/FPS flat or worse. Treat setter-width/full-indexed hash as attribution, not the next primary implementation axis. The next average-FPS proof needs P4 overlap or a larger serial replay/snapshot/encode cadence win with locality gates |
| H161 | Current uniform append storage is secondary; frontend materialization/hash and legacy compact consumers remain larger | accepted current attribution | [state-churn-encode-encode-phase.151](state-churn-encode-encode-phase.151.md) audits the same `v0.0.3` baseline and code paths behind H149. Backend append storage is already componentized: append bytes are `884.199MiB` (`491,221.742B/present`, `9.72%` of frontend materialized bytes), payload records are `96B/append`, and stage append amplification is at the compact floor. The visible append children are smaller than the parent headline (`lookup=270.282ms`, reserve/copy/link `177.252ms` total), while frontend uniform hash remains `1702.902ms` and backend legacy scratch materialization remains `423.341ms`. Do not chase previous-handle candidate plumbing or append reserve/copy/link microfixes first; target frontend compact-owned snapshots, direct compact draw-encoder consumers, VS constant churn with a `v0.0.3` visual gate, or P4/serial-cadence overlap |
| H162 | Command-local uniform materialize caching is correctness-safe but not yet a runtime owner | accepted scoped cleanup; runtime owner open in H163 | [state-churn-encode-encode-phase.152](state-churn-encode-encode-phase.152.md) adds a draw-encoder `DrawUniformPayloadMaterializeCache` for compact draw-run commands. The cache reuses a materialized scratch only when a non-front per-draw override uniform handle repeats inside the same command, keys reuse by compact payload-record source, and resets at command boundaries because `DrawUniformHandle` values are slot-local. The command-front payload stays in a separate scratch so override cache misses cannot overwrite a retained command pointer. Native coverage proves compact command views can resolve through the helper and that repeated override handles reuse the override scratch without changing the command-front payload. Follow-up runtime evidence is H163 |
| H163 | H162 command-local uniform cache does not move GT1 materialize or FPS counters | rejected GT1 owner; cleanup remains correctness-safe | [state-churn-encode-encode-phase.153](state-churn-encode-encode-phase.153.md) runs the H162 current worktree as `app-d3d9-3dmark05-h162-uniform-cache-r1` against the `v0.0.3` baseline with supervised `--no-gputrace --timeout 120 --frame-sampling`. The screenshot gross-check is normal with muzzle flash/bloom and `gpu_command_buffer_errors=0`, but the target counters do not improve: `uniform_backend_materialize_cpu_ms_per_present` is `0.235 -> 0.243`, draw-encoder param materialize CPU is `0.096 -> 0.101`, backend materialized bytes are `5.709M -> 5.749M/present`, `encode_chunk_cpu_ms_per_present` worsens `11.311 -> 12.997`, and sampled FPS is `16.832 -> 16.417`. Treat repeated override-handle materialize as too rare/noisy for current GT1; keep H161's next targets at frontend compact-owned snapshots, direct compact consumers with proven counter movement, or P4/serial-cadence overlap |
| H164 | Frontend compact uniform snapshots are real but require a submission/storage boundary change | accepted design gate | [state-churn-encode-encode-phase.154](state-churn-encode-encode-phase.154.md) audits the `v0.0.3` baseline after H163. Full uniform submission snapshots still copy `9.092GiB/run`, while the conservative compact candidate is `2.616GiB` (`28.77%`) and would save `71.23%` of snapshot bytes. Adjacent fixed payload hashes are identical for `787,998 / 787,998` previous-payload pairs, but both shader-constant hashes match only `4,928` times (`0.63%`), so same-generation/full-payload elision correctly stays at zero. The next implementation is not a partial `std::optional<DrawUniformPayload>` copy or hash-only fixed reuse; it needs a compact owned `DrawRunSubmission` payload plus a `ChunkSlot` append API that consumes fixed/stage components directly, with native equality/materialization tests and a `v0.0.3` visual gate before Xcode spend |
| H165 | `ChunkSlot` can append compact uniform payloads without a full `DrawUniformPayload` input | accepted native gate; runtime producer side open | [state-churn-encode-encode-phase.155](state-churn-encode-encode-phase.155.md) adds `DrawUniformCompactPayloadView` plus compact fixed/stage/payload find/append overloads in `ChunkSlot`. The compact path uses the same hash chains and last-handle fast paths as the full path, but still compares fixed payload contents and stage bytes before reusing handles. `testChunkSlotCompactUniformPayloadAppendMatchesFullPath()` proves a compact view produced from the full path appends equivalent fixed/VS/PS/payload records, reuses a semantic lookup handle, and materializes a legacy `DrawUniformPayload` with the expected fixed fields, stored VS/PS prefixes, zero-filled outside-prefix values, and payload hash. This is an API/storage gate only; `snapshotDrawSubmissionFromCurrentState()` still needs a compact owned producer carrier before any GT1 counter can move |
| H166 | Producer compact uniform submissions cut bytes but regress normalized CPU | accepted mechanism; rejected runtime promotion; default-off | state-churn-encode-encode-phase.156 wires the H165 compact append API into chunk replay behind `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` with `DrawSubmissionUniformScratch` and `DrawUniformCompactSubmissionPayload`. Native tests pass and the no-gputrace `v0.0.3`-anchored smoke is visually coherent, but the runtime gate is negative: materialized uniform bytes per present fall `5.051MB -> 1.423MB`, yet `commit_chunk_queue_draw_submission_cpu_ms_per_present` worsens `3.776 -> 4.461ms`, `d3d9_snapshot_draw_submission_cpu_ms_per_present` worsens `3.042 -> 3.611ms`, and `sampled_avg_fps` falls `16.832 -> 14.441`. Keep the compact append/storage API, but do not promote the producer compact path until fixed-payload dedup, arena pre-sizing, or direct compact build removes the new scratch-copy overhead |
| H167 | Compact fixed-payload reuse proves the H156 scratch-copy opportunity but does not beat `v0.0.3` | accepted bounded CPU improvement; default-off | state-churn-encode-encode-phase.157 adds last-fixed-payload reuse inside `DrawSubmissionUniformScratch` and counters for appends/reuses/saved bytes. The runtime gate reports `763,709` reuses versus `94,889` appends and improves normalized CPU versus H156 (`commit_chunk_replay` `9.323 -> 8.307ms/present`, queue submission `4.461 -> 3.982ms/present`, append-uniform `0.715 -> 0.628ms/present`). It still trails the `v0.0.3` visual-safe baseline on replay and queue submission, while `sampled_avg_fps` is `16.377` versus baseline `16.832`; keep `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` opt-in and move the next compact work toward direct compact build or a smaller submission carrier |
| H168 | Compact producer breakdown ranks fixed-payload work above stage-byte copy | accepted attribution; diagnostic-only | state-churn-encode-encode-phase.158 adds nested compact snapshot timers and reruns the opt-in path. The timers themselves slow the path (`uniform_copy` `0.257 -> 0.501ms/present` versus H167), so this is not a performance candidate. The attribution is still useful: compact total is `0.442ms/present`, fixed-payload construction/equality is `0.207ms/present`, VS stage copy `0.071ms/present`, and PS stage copy `0.042ms/present`. This ranks fixed-lane elimination and direct compact construction ahead of arena pre-sizing or stage-copy-only cleanup |
| H169 | Compact fixed-payload direct compare trims the H168 fixed lane but does not promote compact submissions | accepted bounded CPU cleanup; default-off | state-churn-encode-encode-phase.159 skips constructing a temporary `DrawUniformFixedPayload` on the adjacent fixed-hash reuse path and directly compares the stored fixed scratch entry against the current payload fields. Native coverage proves shader-constant-only snapshots reuse fixed storage and transform changes append new fixed storage. The opt-in runtime gate improves the H168 target child (`d3d9_snapshot_uniform_compact_fixed_cpu_ms_per_present` `0.207 -> 0.189`, compact parent `0.442 -> 0.422`, queue submission `4.305 -> 4.255`) and the broad screenshot is normal, but H169 still trails the `v0.0.3` visual-safe baseline and FPS is not promoted (`16.123`). Keep `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` default-off; next compact work is direct compact build, smaller submission carrier, or removing diagnostic timers before a repeat gate |
| H170 | Compact breakdown timers should be opt-in attribution only | accepted instrumentation hygiene; compact path still default-off | state-churn-encode-encode-phase.160 gates the H158/H159 compact parent/fixed/VS/PS timers behind `DXMT9_PERF_UNIFORM_COMPACT_BREAKDOWN=1`. The H170 opt-in compact run confirms the child timer rows are `0` by default and `d3d9_snapshot_uniform_copy_cpu_ms_per_present` returns to the H167 band (`0.254ms`). Queue submission and replay recover versus H169 (`4.255 -> 4.119ms/present`, `8.667 -> 8.537`) but remain above H167 and do not move FPS (`16.142`) or completion wait (`28.292ms/present`). `actual.png` is very dark/occluded, so treat visual smoke as inconclusive rather than a `v0.0.3` visual-safe proof; keep compact submissions default-off and rank P4/replay-publish or larger serial cadence work ahead unless a direct-compact/smaller-carrier probe proves a larger CPU win |
| H171 | Current compact uniform submission repeat still reduces bytes but not FPS/P4 | rejected current FPS lever | state-churn-encode-encode-phase.161 reruns same-HEAD baseline and `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` after H98/H99. The candidate passes broad effects-heavy visual smoke and proves the compact mechanism (`uniform_materialized_bytes_per_present` `5.046MB -> 1.430MB`, `-71.65%`), but normalized producer CPU regresses (`snapshot_draw_submission` `3.053 -> 3.171ms/present`, queue submission `3.801 -> 3.917`, replay `8.147 -> 8.223`) and P4 overlap worsens (`completion_wait_with_enqueue` `1.562 -> 0.139`, no-enqueue `26.860 -> 27.554`). Keep compact submissions default-off; the next compact candidate must avoid full `cached.uniforms` materialization or shrink the `DrawRunSubmission` carrier, otherwise prioritize P4/replay-publish work |
| H172 | Submission carrier counters prove compact uniform bytes do not shrink queued draw storage | accepted attribution; rejected current compact FPS lever | state-churn-encode-encode-phase.162 adds `d3d9_snapshot_submission_carrier_*` counters and reruns baseline versus `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1`. Both paths carry the same fixed queued-draw footprint: `DrawRunSubmission=21,176B/record`, including `10,320B` state storage, `10,272B` full-uniform storage, and only `128B` compact-uniform storage. The compact path still cuts logical producer uniform bytes `5.076MB -> 1.427MB/present` (`-71.88%`), but queue submission CPU regresses `3.876 -> 4.120ms/present`, snapshot CPU regresses `3.195 -> 3.410`, encode chunk regresses `11.090 -> 11.606`, and useful completion overlap stays absent. This turns H171's "shrink the carrier" into a measured requirement: a future compact candidate must remove the full optional uniform/state carrier or build compact payloads before full `cached.uniforms` exists |
| H173 | Full-uniform sidecar carrier shrinks bytes but fails the runtime and visual-safe promotion gate | rejected and reverted | [state-churn-encode-encode-phase.163](state-churn-encode-encode-phase.163.md) tests a smaller `DrawRunSubmission` carrier by moving full uniforms out of the inline optional storage. The shared-pointer prototype cuts carrier width `21,176B -> 10,928B` but drops sampled FPS `18.381 -> 14.499`; the raw-pointer prototype cuts it further to `10,912B` but drops sampled FPS to `7.334` and only reaches `660` presents in the same supervised window. Restoring the inline optional returns carrier width to `21,176B`. The standardized `--keep-frontmost` repeat h179 reaches the h174 progress band (`1,800` presents, `18.527` FPS mean) and keeps local CPU rows close to baseline, but no P4 overlap appears (`completion_wait_without_enqueue` `27.922 -> 28.032ms/present`, `encode_ready_depth_avg=1.000`). Keep `v0.0.3` as the visual-safe anchor, treat h175/h176 as failed experiments, and require wrapper `--keep-frontmost` repeats before reading no-gputrace frame progression as a code regression |
| H174 | Unused full-uniform carrier lane counter proves compact path still pays the full lane | accepted attribution; rejected current compact promotion | state-churn-encode-encode-phase.164 adds `d3d9_snapshot_submission_carrier_unused_uniform_storage_*` counters and reruns default h180 against `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` h181 under the standardized `--keep-frontmost --timeout 120` no-gputrace path. Default has `0` unused full-uniform carrier bytes because every submission carries `uniforms`. Compact reduces logical uniform materialization `5.070MB -> 1.435MB/present` (`-71.69%`), but the carrier remains `21,176B/record`, full-uniform storage remains `10,272B/record`, and the unused full-uniform lane becomes `492.633 records/present`, `4.826MiB/present`, `10,272B/record`, `100%` of that lane. Queue/snapshot CPU still regress slightly (`3.875 -> 3.964ms/present`, `3.112 -> 3.203`), so the next compact attempt must remove the inline lane with a direct compact carrier or split carrier before another FPS claim |
| H175 | Compact-only draw submission carrier removes the full-uniform lane but does not move FPS/P4 | accepted carrier-shape implementation; rejected current promotion | state-churn-encode-encode-phase.165 adds `DrawRunCompactSubmission` and routes `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` through a compact queue/append path when render trace is off. The h182 -> h183 r2 gate proves the carrier-shape target: `submission_carrier_bytes_per_record` `21,176 -> 10,904` (`-48.51%`), full-uniform storage `10,272 -> 0B/record`, and unused full-uniform storage stays `0` because the lane no longer exists. Logical uniform materialization still falls `5.052MB -> 1.427MB/present` (`-71.75%`). But replay/snapshot remain worse (`queue_draw_submission` `3.857 -> 4.025ms/present`, snapshot `3.096 -> 3.186`), `completion_wait_without_enqueue` is flat/slightly worse (`26.840 -> 26.994ms/present`), and `encode_ready_depth_avg` stays `1.000`. Keep the compact carrier default-off; the remaining compact work is direct compact construction that removes the temporary full snapshot, not another carrier-width experiment |
| H176 | Direct compact submission snapshot cleans the carrier path but still does not promote compact submissions | accepted cleanup; rejected current promotion | state-churn-encode-encode-phase.166 removes the phase 165 temporary full-submission bridge by adding a direct `DrawRunCompactSubmission` snapshot overload and filling the compact vector in place. Native coverage proves compact materialization plus adjacent same-generation compact state/uniform elision. The h183 -> h184 compact comparison shows a small queue-submission cleanup (`4.025 -> 3.927ms/present`), but snapshot draw submission worsens (`3.186 -> 3.301`), encode chunk worsens (`12.829 -> 13.222`), sampled FPS falls (`16.332 -> 15.971`), and ready depth stays `1.000`. The h182 -> h184 storage gate remains valid (`21,176 -> 10,904B/record`, full-uniform lane `10,272 -> 0B/record`), but normalized CPU still does not beat default. Keep compact submissions default-off and stop adding carrier variants until the cached uniform source itself becomes compact or P4/replay-publish work resumes |
| H177 | Resource-shape memo ProbeKey validation does not explain the current visual regression | rejected as visual owner; rejected default validation | [state-churn-encode-encode-phase.167](state-churn-encode-encode-phase.167.md) temporarily validates every resource-shape memo hit by resolving the canonical draw `probeKey` before reuse. The h189 no-gputrace run records `161,025 / 161,025` validated hits, `validated_misses=0`, every resource-shape mismatch bucket `0`, skipped-pipeline and Metal-error counters `0`, and a gross-normal `actual.png`. The validation itself is too expensive for default use (`draw_key_resolve` rises to `1.104ms/present` versus the recent `0.437-0.497` band, prefetch parent `1.872ms/present` versus `1.212-1.347`), so the code experiment was reverted. Keep the default memo path; use `DXMT9_DISABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1` or `DXMT9_PERF_ENCODE_SLOT_PSO_RESOURCE_SHAPE_OPPORTUNITY=1` only as targeted A/B if a `v0.0.3` visual-gated artifact reproduces |
| H178 | Disabling the resource-shape memo does not fix the then-suspected black-foreground visual class | rejected visual fix; rejected FPS lever | [state-churn-encode-encode-phase.168](state-churn-encode-encode-phase.168.md) runs the targeted opt-out `DXMT9_DISABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1`. The h190 counters prove the memo is fully disabled (`resource_shape_memo_* = 0`) and repeated work moves to probe-key memo hits (`169,745`), but `actual.png` still contains the sampled dark foreground/silhouette class. The run remains in the same cadence class as h189: command buffers per present `3.999`, ready depth `1.000`, completion wait `26.723ms/present`, and `draw_key_resolve=1.108ms/present`. H182 later shows this window class also exists in `v0.0.3`; keep the stale resource-shape memo branch closed unless a future same-frame A/B contradicts it |
| H179 | Full-cbuf oracle does not remove the sampled 1060..1100 black foreground silhouettes | rejected cbuf visual owner; rejected default workaround | [state-churn-encode-encode-phase.169](state-churn-encode-encode-phase.169.md) captures the current h191 frame window `1060..1100:5` and repeats it with `DXMT9_FORCE_FULL_CBUF_UPLOADS=1` as h192. The oracle is active (`argbuf_hybrid_bytes_per_encoder` `937,374,776 -> 4,958,743,232`, cbuf update `1.010 -> 1.332ms/present`), but the offset-paired h191 `N` vs h192 `N+5` contact sheet still shows the same dark foreground silhouette class. FPS stays noise-flat (`16.391 -> 16.379`), skipped-pipeline/Metal-error counters stay `0`, and ready depth/P4 class does not change. Do not reopen compact uniform ABI-prefix/cbuf width based on this time-based window; next visual proof needs same-frame final-writer/pass or binding-source isolation |
| H180 | Batch-miss uniform payload attribution points at shader-constant refresh, not another carrier-width experiment | accepted attribution; no FPS claim | state-churn-encode-encode-phase.170 adds runtime counters for the three binding-agnostic batch-miss uniform paths and runs h200 under the standard `--keep-frontmost --timeout 120` no-gputrace gate. The selected paths are `reuse_full=3,449`, `reuse_nonconst=306,976`, and `full_build=34,903`; `reuse_nonconst + full_build` matches `uniform_build_calls=341,879`. The residual build timer is `1071.926ms/run` (`0.715ms/present`), with hash work `764.934ms` and VS constant hashing `488.736ms`. This narrows the local target to VS constant hash/copy width or a direct compact constant representation. It does not change the global owner: h200 remains in the no-enqueue class (`completion_wait_without_enqueue=24.474ms/present`, `with_enqueue=0.000`, `encode_chunk=14.819ms/present`, sampled FPS `13.934`) |
| H181 | Batch-miss shader-constant hashes are mostly rebuilt, but remain a bounded local CPU cleanup | accepted attribution; no FPS claim | [state-churn-encode-encode-phase.171](state-churn-encode-encode-phase.171.md) adds VS/PS hash reuse/build path counters and runs h202 under the same `--keep-frontmost --timeout 120` no-gputrace gate. Batch-miss selected paths total `407,597`; VS hash builds are `272,229` (`66.79%`) and PS hash builds are `292,162` (`71.68%`). The CPU owner is real but bounded: batch-miss VS+PS hash timers are `0.315ms/present`, while h202 stays no-enqueue dominated (`completion_wait_without_enqueue=27.505ms/present`, `with_enqueue=0.000`, `encode_chunk=11.230ms/present`, replay `8.318ms/present`). Treat shader-constant hash memoization as a P2/P3 cleanup candidate, not the FPS-facing owner; byte-width-only reduction is smaller because indexed-float VS full-hash potential saved bytes are only `21.24MB` against `345.34MB` batch-miss VS hash bytes |
| H182 | The sampled black-foreground firefight window also exists in `v0.0.3` | accepted normal-scene class; regression not proven | [state-churn-encode-encode-phase.172](state-churn-encode-encode-phase.172.md) compares the current h199 window with the `v0.0.3` h196 release capture at the same HUD time class (`0:59.36` vs `0:59.30`). Both frames contain the strong spark/bloom pass, crates, foreground silhouettes, and dark limb/prop shapes. Raw image diff is large (`94.217%` full, `97.147%` cropped) because the scene is not frame-locked, so the numeric diff is not a gate; the qualitative black-foreground class itself is present in the safe tag. Treat this window as scene/post-process unless a separate same-frame weapon/lighting artifact is reproduced |
| H183 | Batch-miss shader-hash memo opportunity is measurable and effectively absent | rejected opportunity; instrumentation accepted | [state-churn-encode-encode-phase.173](state-churn-encode-encode-phase.173.md) adds default-off `DXMT9_PERF_BATCH_MISS_SHADER_HASH_MEMO_PROBE=1` and runs h203 under the standard `--keep-frontmost --timeout 120` no-gputrace gate. The run is clean (`present_encoded=1,800`, `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, broad visual smoke normal), but the real memo opportunity is not there: VS probes/hits are `278,940 / 0` (`0.000%`) and PS probes/hits are `300,577 / 253` (`0.084%`). The bounded hash bucket remains about `0.316ms/present`, while the run stays no-enqueue dominated (`completion_wait_without_enqueue=27.493ms/present`, `with_enqueue=0.024`, replay `8.183ms/present`, encode `11.203ms/present`). Do not implement a real per-stage shader-hash memo for GT1; keep the probe as opt-in attribution only and move back to P4/replay-publish or larger serial-cadence work, using `v0.0.3` as the visual-safe anchor |
| H184 | Pending draw submission flush reason split initially mislabeled draw-run drains as fallback drains | superseded instrumentation label error | [state-churn-encode-encode-phase.174](state-churn-encode-encode-phase.174.md) split `commit_chunk_replay_pending_flush_cpu_ms` by reason and ran h204 under the standard no-gputrace 120s foreground gate. The broad owner is real (`1.660ms/present`, `20.57%` of replay), but the first reason mapping tagged direct/indexed draw-run preflushes as fallback. The large h204 `draw_fallback=47.71%` value is therefore invalid. | Do not use h204's fallback conclusion. Use H185/h205 for the corrected split. |
| H185 | Pending draw submission flushes are draw-run/end drains, not fallback or non-draw boundary churn | accepted runtime attribution | [state-churn-encode-encode-phase.175](state-churn-encode-encode-phase.175.md) reruns h205 after fixing the reason labels. The pending-flush bucket remains real (`1.677ms/present`, `20.64%` of replay), but the corrected split is `draw_run=47.42%`, chunk `end=47.30%`, `before_record=5.03%`, real `draw_fallback=0.25%`, and failure `0`. The run stays no-enqueue dominated (`completion_wait_without_enqueue=27.163ms/present`, `with_enqueue=0`). | Do not chase fallback-draw classification or broad non-draw-boundary churn. The next replay/snapshot work should target pending-submission to draw-run boundary churn, `submitDrawRunBatch()` / `appendDrawRunBatch()` cost, or larger replay/snapshot/P4 movement. This is CPU-only evidence; no `.gputrace` spend without no-gputrace P4 movement and the `v0.0.3` visual-safe gate. |
| H186 | Pending draw submission flushes are frequent small draw-run/end drains | accepted runtime attribution | [state-churn-encode-encode-phase.176](state-churn-encode-encode-phase.176.md) adds reason-specific flush and record counters and runs h206. The corrected CPU split repeats (`draw_run=47.36%`, `end=47.29%`, fallback `0.25%`), and the new volume data shows `draw_run` has `57,367` flushes / `416,211` records (`7.255` records/flush, `32.970` flushes/present) while `end` has `32,330` flushes / `408,196` records (`12.626` records/flush, `18.580` flushes/present). Combined `draw_run+end` is `51.55` flushes/present, not a few large drains. | Treat the owner as high-frequency small-batch carrier churn. Next work should investigate merging pending submissions with explicit draw-run command replay, safe cross-chunk drain delay/merge, or reducing `submitDrawRunBatch()` / `appendDrawRunBatch()` per-group and per-record width, especially uniform append. No `.gputrace` spend without no-gputrace P4 movement and the `v0.0.3` visual gate. |
| H187 | Draw-run preflushes are immediate carrier-combine opportunities | accepted runtime attribution | [state-churn-encode-encode-phase.177](state-churn-encode-encode-phase.177.md) pairs non-empty `draw_run` pending flushes with the following explicit imported draw-run. h207 records `57,128` opportunities, exactly matching `draw_run` flushes (`100.00%`), with `414,472` pending records plus `219,283` following run records. Combined shape is `11.094` records/boundary and `364.227` records/present. | Treat pending-submission plus explicit draw-run carrier merge as a concrete next CPU candidate for the `draw_run` half of pending-flush churn. It does not address chunk `end` drains; cross-chunk/end merge remains separate. Any mutation still needs no-gputrace P4/cadence movement and the `v0.0.3` visual gate. |
| H188 | Materializing the following imported draw-run into pending submissions removes the boundary but is not promotable | mechanism accepted; runtime promotion rejected | state-churn-encode-encode-phase.178 adds default-off `DXMT9_ENABLE_DRAW_RUN_PREFLUSH_MERGE=1` and compares h208 against same-code h209. The candidate eliminates `draw_run` pending flushes (`59,109 -> 0`) and collapses explicit draw-run build/submit CPU (`268.074/2,091.400ms -> 68.348/506.944ms`), but shifts the work to chunk `end` (`1,406.691 -> 2,816.407ms`) and increases per-draw queued-submission materialization (`queue_draw_submission=3.805 -> 4.218ms/present`, snapshot `3.123 -> 3.301ms/present`, batch records `882,567 -> 1,217,493`). | Keep the knob default-off as a diagnostic prototype. The useful next design is a carrier that merges pending submissions with the following imported draw-run while preserving explicit-run shared-state behavior, or a separate cross-chunk/end drain merge. Do not spend `.gputrace` on this candidate; promotion still needs no-gputrace P4/replay movement plus the `v0.0.3` visual gate. |
| H189 | Imported canonical draw-run submit can bypass public fan-normalization but does not move the runtime owner | rejected-current | state-churn-encode-encode-phase.179 adds default-off `DXMT9_ENABLE_DRAW_RUN_CANONICAL_FAST_PATH=1`, routing scanner-accepted imported runs through `drawPrimitiveRunCanonical()` instead of public `drawPrimitiveRun()`. Native coverage proves canonical `TriangleList` submission and defensive `TriangleFan` rejection. The h210/h211 runtime A/B shows the targeted row is effectively flat after present-count normalization (`commit_chunk_draw_run_submit_cpu_ms_per_present` `1.169 -> 1.161`), while FPS falls slightly (`16.546 -> 16.412`) and P4 remains no-enqueue dominated (`completion_wait_with_enqueue=0`). | Keep the knob default-off as a diagnostic only. This closes public fan-normalization as a first-order owner; next state-churn work should return to preserving explicit-run shared-state behavior while merging carriers, or to direct N-1 state/uniform materialization elision. No `.gputrace` spend without no-gputrace P4/replay movement and the `v0.0.3` visual gate. |
| H190 | Current compact-uniform carrier halves queued carrier width but still fails the P4 gate | mechanism accepted; runtime promotion rejected | state-churn-encode-encode-phase.180 reruns `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` on the current H189 worktree against h211. The carrier target is achieved: `submission_carrier_bytes_per_record` `21,176 -> 10,904`, full-uniform carrier storage `10,272 -> 0B/record`, and logical materialized uniform bytes `5.027MB -> 1.431MB/present`. Backend append-uniform CPU improves modestly (`0.654 -> 0.613ms/present`), but snapshot CPU worsens (`3.125 -> 3.212ms/present`), `d3d9_snapshot_uniform_copy_cpu_ms_per_present` worsens (`0.143 -> 0.236`), ready depth stays `1.000`, and completion wait worsens (`27.195 -> 27.740ms/present`). | Keep compact submissions default-off. Another carrier-width variant is not the next useful unit; the remaining compact path must avoid first building/copying full `cached.uniforms`, or else the next FPS work should return to serial-cadence/P4 overlap. Do not spend `.gputrace` without no-gputrace P4 movement and the `v0.0.3` visual gate. |
| H191 | Direct compact construction is a cache-representation change, not a snapshot-only patch | accepted design gate | state-churn-encode-encode-phase.181 audits the current source path after H190. The backend already accepts compact uniform views and the compact carrier is narrow, but `cachedBaseDrawStateForSubmissionBatch()` still refreshes or rebuilds `CachedBaseDrawState::uniforms` as a full `DrawUniformPayload` before `snapshotCompactDrawUniformPayload(cached.uniforms, ...)` runs. h212 confirms the remaining source-side owner: `d3d9_snapshot_uniform_build_hash_cpu_ms=1,701.597`, VS const hash `1,025.998`, non-constant hash `293.326`, and copy scope `425.185`. | Do not implement another narrow compact carrier or snapshot-only variant. The next compact implementation must split the uniform cache source of truth so fixed payload and stage bytes can be produced directly from `DeviceState` plus cached component hashes, with full materialization only for full-submission/render-trace/debug consumers. Until then, prioritize P4/no-enqueue or render-pass-safe overlap work. |
| H192 | Pending-plus-explicit-run merge remains viable only as a mixed carrier | accepted design gate | state-churn-encode-encode-phase.182 audits why H188 failed. The opportunity is real (`draw_run` preflushes are immediately followed by explicit imported draw-runs), but the prototype used `queueImportedDrawRunAsSubmissions(...)`, expanding the following shared-state run into per-record submissions. That removed `draw_run` flushes (`59,109 -> 0`) but increased queued submission CPU (`3.805 -> 4.218ms/present`), snapshot CPU (`3.123 -> 3.301`), batch records (`882,567 -> 1,217,493`), and chunk-end pending flush CPU (`1,406.691 -> 2,816.407ms`). | The next draw-run carrier should accept pending submissions plus a canonical `DrawParam` run together, preserving the explicit run's single shared state instead of materializing it as submissions. Gate it with no-gputrace P4/no-enqueue movement, locality flatness, and the `v0.0.3` visual anchor before any Xcode spend. |
| H193 | Corrected mixed pending-plus-explicit-run carrier removes the boundary but does not move P4/FPS | mechanism accepted; runtime promotion rejected | state-churn-encode-encode-phase.183 implements default-off `DXMT9_ENABLE_DRAW_RUN_PREFLUSH_MIXED_CARRIER=1` and compares h213 control against timer-fixed h215. The carrier removes the targeted `draw_run` pending flush CPU (`1435.098ms -> 0`) while preserving the following imported run as a canonical `DrawParam` span. The top-level batch-submit row rises (`commit_chunk_draw_batch_submit_cpu_ms 2987.523 -> 4097.587`), but child counters show the main backend append/resource/compat costs are flat (`submit_draw_run_batch_append_cpu_ms 2295.245 -> 2290.760`, uniform append `1182.069 -> 1180.647`, resource mark `25.945 -> 26.083`), so this is mostly timer reclassification from the old draw-run-submit parent (`2113.798 -> 944.804`). Frame-facing rows stay flat or slightly worse: replay only improves `8.153 -> 8.092ms/present`, `completion_wait_with_enqueue` remains `0`, ready depth remains `1.000`, GPU CB time worsens `+0.61%`, and tile preservation worsens `+641.121MiB`. | Keep the mixed carrier default-off as a diagnostic mechanism. Do not spend `.gputrace` on this candidate. The next useful state-churn target is underlying N-1 state/uniform materialization or direct uniform-cache representation, or a separate P4 overlap design; any promotion still needs flat locality and the `v0.0.3` visual-safe gate. |
| H194 | After mixed-carrier rejection, the next owner is direct compact uniform cache or locality-preserving P4 overlap | accepted next-owner review | [state-churn-encode-encode-phase.184](state-churn-encode-encode-phase.184.md) rereads H193 child counters and ranks the next branches. More draw-run preflush carriers are closed unless paired with underlying materialization removal. Direct compact uniform cache remains plausible because H181 proves full `CachedBaseDrawState::uniforms` is still built before compacting; P4 overlap remains the average-FPS lane but must not repeat previous CB/pass/tile/final-reopen regressions. | Use `v0.0.3` as the visual-safe anchor. Run 120s no-gputrace gates before Xcode: direct uniform work must reduce snapshot/replay rows without worsening P4/frame, and P4 overlap work must create enqueue-during-wait while keeping command buffers, render passes, tile preservation, final same-key reopens, and load/store traffic flat. |
| H195 | Hot-state construction can now consume uniform hashes without a full payload argument | accepted prerequisite; no runtime claim | [state-churn-encode-encode-phase.185](state-churn-encode-encode-phase.185.md) adds `FlatDrawStateUniformInputs` and routes current hash-ready hot-build call sites through `FlatDrawStateUniformInputs{.hashes = &uniformHashes}`. This removes the function-signature blocker identified by H184: `FlatDrawStateRecord` construction no longer inherently requires a full `DrawUniformPayload` object when hashes are already available. Focused native tests pass. | Keep this as a direct compact uniform-cache prerequisite only. The cache still builds full `CachedBaseDrawState::uniforms`; the next mutation must split that source of truth and then pass the 120s no-gputrace plus `v0.0.3` visual gate before any Xcode/gputrace spend. |
| H196 | Direct compact uniform cache source lowers local replay/snapshot CPU but does not move P4 | mechanism accepted; runtime promotion rejected | state-churn-encode-encode-phase.187 pairs h216 control with h217 `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` after H186's direct-source implementation. The mechanism is real: carrier width drops `21,176 -> 10,904B/record`, full-uniform carrier storage drops `10,272 -> 0B/record`, uniform materialized bytes fall `5.070 -> 1.428MB/present`, snapshot uniform build drops `0.467 -> 0.405ms/present`, queue draw submission drops `3.919 -> 3.629`, and replay drops `8.247 -> 7.902`. | Keep compact direct as bounded P2/P3 cleanup behind the existing opt-in knob, but do not promote it as the average-FPS lever. Completion remains no-enqueue dominated (`25.801ms/present` without enqueue, `0.036` with enqueue), ready depth is still `1.000`, encode chunk slightly worsens, and sampled FPS is noise (`16.429 -> 16.545`). Return to producer/replay/encode overlap or serial-cadence work, still gated by CB/pass/tile locality and `v0.0.3` visual safety. |
| H197 | Chunk-end pending flushes are real opportunities, but naive cross-chunk carry is not safe | opportunity accepted; naive carry rejected | state-churn-encode-encode-phase.188 adds default-off `DXMT9_PERF_CHUNK_END_FLUSH_PROBE=1` and runs h218 under the standard 120s no-gputrace gate. The `End` drain is the same order as draw-run preflush (`1,393.521ms` vs `1,411.785ms`, about `0.801ms/present`), with `32,283` stored end flushes / `406,005` pending records (`12.576` records/flush). Almost every stored flush resolves immediately to a next draw-shaped record (`29,582` first submissions, `2,623` first imported draw-runs, `78` blockers). | Do not implement a plain cross-chunk pending-vector carry. Only `48.25%` of first-submission candidates share stable state/lane, uniform generation matches only `7.04%`, and whole uniform payload hash matches only `11.40%`. The viable next design must preserve per-draw uniform ownership and explicit-run shared state, or return to a larger P4 overlap contract. The run remains no-enqueue dominated (`completion_wait_without_enqueue=26.898ms/present`, `with_enqueue=0`), so no `.gputrace` spend from this probe alone. |
| H198 | State-compatible chunk-end carry remains plausible, but uniform-stable carry is too narrow | opportunity accepted; uniform-stable carry rejected | [state-churn-encode-encode-phase.189](state-churn-encode-encode-phase.189.md) adds the missing H188 intersection counters and runs h221 under the 120s no-gputrace foreground gate. The opportunity repeats: `32,740` stored end flushes / `408,432` records (`12.475` records/flush), `30,198` first-submission candidates, and `2,470` first imported draw-runs. State/lane compatibility is still about half (`15,070`, `49.90%`). | Do not implement a carry that requires both same state and same uniform. The actual state+uniform intersection is only `7.22%` by both generation and whole-payload-hash predicates, so a useful carrier must preserve per-draw uniform ownership while carrying only the shared state lane, or preserve the following explicit run as a shared-state span. H221 itself is not a perf win: sampled FPS is `16.305`, `completion_wait_without_enqueue=30.051ms/present`, ready depth is `1.000`, replay is `8.458ms/present`, and encode is `11.096ms/present`; no `.gputrace` spend from this probe alone. |
| H199 | Cross-chunk end carry needs owned uniform scratch and resource re-marking | accepted design gate | [state-churn-encode-encode-phase.190](state-churn-encode-encode-phase.190.md) audits the current replay and queue code before mutating H221. The pending vectors and `DrawSubmissionUniformScratch` are thread-local scratch cleared at commit exit, while compact submissions may hold arena-view spans into that scratch. Separately, chunk replay suppresses per-draw resource marking after `markChunkResources()` because same-chunk draws share the bulk-mark `seqId`. | Do not move the local vectors to `D9CDevice` as a standalone patch. A safe end carry must own submissions plus uniform scratch across calls, keep full/compact lanes exclusive, flush before next non-draw boundary, and submit carried work with forced per-draw resource marking at the actual submit sequence. Without that submit mode, prefer same-call mixed carriers or P4 overlap work over cross-chunk deferral. |
| H200 | Forced resource-marking submit is now available as a cross-chunk carry prerequisite | prerequisite accepted; no runtime claim | [state-churn-encode-encode-phase.191](state-churn-encode-encode-phase.191.md) adds explicit forced-mark submit methods through `core::Device`, `BackendDevice`, `dxmt9::Device`, and `CommandQueue`. The queue variants reuse the existing batch submit implementation with per-draw resource marking enabled even if chunk replay is currently in bulk-mark skip mode. Focused native coverage proves the seams route through the dedicated backend methods and preserve draw params. | This removes only the resource-lifetime blocker from H199. It does not enable end carry, reduce CPU, or justify `.gputrace`. The next mutation must add owned carried submission/uniform scratch storage and then pass the usual 120s no-gputrace P4 plus `v0.0.3` visual gate before promotion. |
| H201 | Owned chunk-end carry is implemented as a default-off runtime experiment | implementation accepted; runtime result pending | state-churn-encode-encode-phase.192 adds `DXMT9_ENABLE_CHUNK_END_CARRY=1`. The replay path can move a small chunk `End` pending-submission drain plus compact-uniform scratch into `D9CDevice`, adopt it into the next draw-shaped record, or flush it through forced resource marking on non-draw/failure boundaries. New summary counters report stored/adopted/flushed carry records. | Keep the knob default-off and unpromoted. Default-off native/TLA/script coverage passes, but the useful proof is still missing: run a 120s no-gputrace 3DMark05 A/B with `--keep-frontmost`, verify carry adoption dominates flushing, verify P4/no-enqueue and replay/encode rows improve or stay flat, and compare visually against `v0.0.3`. Broad env-on unit tests are not the promotion gate because isolated draw-only final chunks can be intentionally deferred with no following boundary. |
| H202 | Owned chunk-end carry removes the local end-flush bucket but not the P4/FPS owner | mechanism accepted; runtime promotion rejected | state-churn-encode-encode-phase.193 pairs h222 control with h223 `DXMT9_ENABLE_CHUNK_END_CARRY=1` under the 120s no-gputrace foreground gate. The mechanism works: `649,242` records are stored, `648,183` are adopted (`99.84%`), and chunk-end pending flush CPU falls `0.817 -> 0.045ms/present`. The frame-facing rows do not move: replay is `8.497 -> 8.492ms/present`, encode is `13.060 -> 13.001`, ready depth stays `1.000`, and completion wait remains no-enqueue dominated (`26.943 -> 26.402ms/present` without enqueue, `0.106 -> 0.000` with enqueue). | Keep the knob default-off. The saved end-flush work shifts into larger submit batches (`draw_batch_submit` `1.714 -> 1.983ms/present`, submission records per submit `9.053 -> 12.497`) instead of becoming an FPS lever. Do not spend `.gputrace` on this candidate; next work should attribute the submit-cost shift, combine carry only with real N-1 materialization elision, or return to a locality-preserving P4 overlap design gated by `v0.0.3` visual safety. |
| H203 | Forced resource-marking pending flushes need direct attribution after H202 | instrumentation accepted; runtime partially attributed; visual gate failed | [state-churn-encode-encode-phase.194](state-churn-encode-encode-phase.194.md) adds behavior-neutral counters for pending flushes that submit through forced per-draw resource marking, then h224 reruns `DXMT9_ENABLE_CHUNK_END_CARRY=1`. The mechanism repeats (`557,652` stored records, `557,140` adopted), and the new counter reports `0.144ms/present`, `1.089` flushes/present, and `30.401` records/present through forced resource marking. That is a real local cost (`20.09%` of pending flush CPU), but it is not the whole submit shift: `draw_batch_submit` remains `2.007ms/present` and batch resource marking remains `0.114ms/present`. H224's screenshot is HUD plus black scene (`mean_luma=6.289`), so it is counter evidence only, not visual-safe performance evidence. | Keep `DXMT9_ENABLE_CHUNK_END_CARRY=1` default-off. Do not mutate carry again from this alone. The next branch is either a narrower resource-marking residual proof, queue lock / outer submit / batch-width attribution for the rest of `draw_batch_submit`, or P4 overlap work. Any timing claim still needs a repeated no-gputrace run with `v0.0.3` visual safety before `.gputrace` or promotion. |
| H204 | Current wall review narrows the next owner to P4 or submit residual attribution | accepted current direction | [state-churn-encode-encode-phase.195](state-churn-encode-encode-phase.195.md) reviews the current visual-safe evidence: same-generation state-copy elision is already live (`410,814` states / `4.203GiB` saved), adjacent uniform generation reuse is `0`, compact uniform source already failed P4 promotion, and chunk-end carry shifts rather than removes work. | Treat the project as narrowed, not stuck. The next branch is queue lock / outer submit / batch-width residual attribution, append materialization, or a render-pass-safe overlap carrier. `.gputrace` is only for GPU-hot-frame questions or a candidate that moves P4/locality gates. |
| H205 | Queue mutex acquisition owns the draw-batch-submit residual | rejected | [state-churn-encode-encode-phase.196](state-churn-encode-encode-phase.196.md) adds `submit_draw_run_batch_queue_lock_cpu_ms` around the queue lock in `submitDrawRunBatch*` and reruns the current visual-safe 120s no-gputrace probe. The lock is only `0.018ms/present`, while `draw_batch_submit=1.682ms/present`, replay `8.424`, encode `11.249`, and no-enqueue completion wait `27.837` remain. | Do not optimize the submit mutex for GT1. The next local CPU branch is append/materialization width or snapshot/cache; the average-FPS branch remains P4 overlap. |
| H206 | The remaining draw-batch-submit row is mostly an unmeasured outer-submit gap | rejected by derived reanalysis | [state-churn-encode-encode-phase.197](state-churn-encode-encode-phase.197.md) extends the summary tool with parent-minus-child residual rows and re-summarizes H225. Existing children explain `89.96%` of `commit_chunk_draw_batch_submit_cpu_ms`; append alone is `76.63%` of the parent. Inside append, uniform is `51.51%`, state is `26.00%`, and append residual is `9.73%`. | Treat append uniform/state materialization as the next local submit branch. Do not chase a broad outer-submit unknown or `.gputrace` from this CPU-only evidence. Any append-width win still needs P4/no-enqueue movement before FPS promotion. |
| H207 | Uniform append is mostly just payload lookup and payload-record append | rejected by derived reanalysis | [state-churn-encode-encode-phase.198](state-churn-encode-encode-phase.198.md) adds derived uniform-append CPU rows and re-summarizes H225. The parent is `0.664ms/present`, but payload lookup is only `0.152`, payload append storage is only `0.101`, and the known child share is `38.02%`. The residual is `0.411ms/present`, likely stage-level find/append/vector maintenance around vertex/pixel constant payloads. | Do not optimize only the final payload-record append copy. The next local branch is stage-level uniform append materialization or N-1 state/uniform materialization elision. This remains a local CPU branch until replay/encode/P4 rows move under the visual-safe no-gputrace gate. |
| H208 | The remaining uniform-append residual needs component-level attribution before mutation | instrumentation accepted; runtime gate completed | [state-churn-encode-encode-phase.199](state-churn-encode-encode-phase.199.md) adds behavior-neutral counters for fixed/VS/PS component find and append scopes inside `appendDrawUniformPayload()`, plus summary rows for known-with-components share and component residual. Existing H225 data predates the counters, so the new rows correctly report `n/a` rather than zero. | The follow-up run in H209 makes fixed-payload find the first local cleanup target. Keep the component split as attribution tooling; it is not `.gputrace` evidence by itself. |
| H209 | Fixed-payload handle carry reduces the targeted component but does not break the FPS wall | accepted local cleanup; not FPS proof | [state-churn-encode-encode-phase.200](state-churn-encode-encode-phase.200.md) stamps submissions with `uniformFixedPayloadGeneration` and lets `appendDrawRunBatch()` reuse the previous slot-local fixed handle when the generation is unchanged and the record hash still matches the current fixed payload. The targeted row moves: `uniform_component_fixed_find_cpu_ms_per_present` `0.229 -> 0.150`, and total component find `0.323 -> 0.257`. | Keep the carry path. Do not promote it as a wall-breaker: `uniform_append_parent_cpu_ms_per_present` is flat (`0.882 -> 0.880`), sampled FPS is noisy/regressed (`16.170 -> 14.261`), and no-enqueue completion wait remains dominant. The next FPS-facing branch remains P4/no-enqueue overlap or larger replay/encode materialization elision. |
| H210 | Uniform append residual after fixed-handle carry is bounded local cleanup | accepted direction | [state-churn-encode-encode-phase.201](state-churn-encode-encode-phase.201.md) audits H209's current run and the `appendDrawUniformPayload()` source. The remaining parent is `0.880ms/present`; known scopes plus component scopes explain `77.75%`, leaving `0.196ms/present` residual. VS stage append is the largest named remaining component (`0.116ms/present`) because `661,640` VS stage records are appended (`0.833` per payload append), while full uniform generation reuse remains `0` and full payload hash reuse is only `3,970 / 672,993` adjacent payloads. | Do not spend `.gputrace` on uniform append residual alone. A VS-stage split or stage-handle tweak is optional local cleanup with a small ceiling. The FPS branch remains P4/no-enqueue overlap or a larger replay/encode materialization change that moves serial rows under the visual-safe no-gputrace gate. |

## Verification methods

- **`DXMT9_PERF_ENCODER_BREAKDOWN=1`** — emits `[dxmt9-perf-encoder]` (one row
  per render-encoder close) and `[dxmt9-perf-encoder-stream]` (per used stream)
  lines: stream/IB samples, Metal binds, handle/offset/stride changes, argbuf
  table/cbuf bytes, `setVertexBytes`, geometry transient vertex/index bytes,
  unique-handle counts/bytes/pool buckets. Proves churn is handle-dominated.
  `DXMT9_PERF_ENCODER_BREAKDOWN_CBUF_CONTENT=1` is a separate heavy extension
  for VS/FFPVS cbuf content-history first/rewrite/field splits; keep it off for
  normal CPU baselines because it scans uploaded cbuf bytes on the hot path.
- **`analyze_shader_constant_sparsity.py`** — offline analyzer for
  `--dump-shaders` D3D bytecode dumps. It records exact float/int/bool constant
  register sets, tracks indexed constant access, and joins optional
  `3dmark05-perf-indexed-probe-draws.csv` rows to distinguish safe non-indexed
  packed savings from theoretical indexed gaps.
- **`analyze_blendindices_geometry.py`** — offline analyzer for
  `--dump-indexed-geometry` payloads. It reads `.meta`, `.index.bin`, and
  `.streamN.bin`, finds `D3DDECLUSAGE_BLENDINDICES`, and measures the actual
  vertex values feeding `a0.x/a0.y` for indexed constant-window proofs.
- **`analyze_stream_ib_backend_churn.py`** — hot-row preflight for stream/IB as
  a possible hidden-backend denominator experiment. It proved the frame60 rows
  were handle-churn-dominant and named the row-scoped staging A/B target. The
  later Xcode gate rejects handle identity as the first-order GPU owner, so use
  this analyzer for CPU/draw-run attribution or for new stream/IB mechanisms
  only after they change more than handle identity.
- **`commit_chunk_draw_run_*` counters** — `_submits`, `_records`, break-type
  rows (`_const_upload`), and the state-delta sub-buckets (`_stream_only`,
  `_ib_only`, `_texture_only`, `_mixed`, `_mixed_group2/3/4plus`,
  `_mixed_pair_stream_ib`, `_stream_ib_only`). Size each draw-run break class
  exactly. Pending-flush reason rows split chunk replay's draw submission path:
  `commit_chunk_replay_pending_flush_cpu_ms` remains the broad owner, while
  `commit_chunk_replay_pending_flush_{before_record,draw_run,draw_fallback,failure,end}_cpu_ms`
  decide whether a candidate should attack non-draw boundaries, draw-run
  interaction, real fallback drains, or ordinary chunk-end draining.
  `DXMT9_PERF_CHUNK_END_FLUSH_PROBE=1` is a default-off opportunity probe for
  the ordinary chunk-end class: it stores only the end-flush generation/lane
  stamp and resolves it against the next chunk's first draw-shaped record. Use
  its `commit_chunk_replay_end_flush_probe_*` rows to size cross-chunk carry
  designs, not as permission to carry pending submissions across chunks without
  per-draw uniform and state-compatibility proof.
- **`commit_chunk_*_cpu_ms` stage counters** — split the historical
  `bridge_commit_latency_ns` wall time into wire import, handle/resource
  marking, record replay, and nested draw-batch submit. Use these before
  treating a large bridge latency number as an ABI or Wine thunking problem.
- **Replay child counters** — `commit_chunk_queue_draw_submission_cpu_ms`,
  `commit_chunk_draw_run_scan_cpu_ms`, `commit_chunk_draw_run_build_cpu_ms`,
  `commit_chunk_draw_run_submit_cpu_ms`, `commit_chunk_apply_draw_state_cpu_ms`,
  and `commit_chunk_const_upload_cpu_ms` split the replay owner after the broad
  stage counter has already named `commit_chunk` replay. Use
  `commit_chunk_queue_draw_submission_emplace_cpu_ms` only to size the
  default-construction/reallocation part of queued submission creation before an
  optional-state/direct-construct carrier refactor. Pair that with
  `d3d9_snapshot_submission_carrier_*` when compact/uniform work reduces logical
  payload bytes but queue-submission CPU stays flat; those counters expose the
  fixed inline `DrawRunSubmission` storage that still has to be allocated,
  default-constructed, moved, and scanned. The
  `d3d9_snapshot_submission_carrier_unused_uniform_storage_*` rows specifically
  count records where the full-uniform optional lane is empty but still
  contributes inline carrier width; use them to size the direct-compact or
  carrier-split target before another implementation attempt. For
  compact-carrier A/Bs, require
  `--require-submission-carrier-bytes-per-record-decrease`; when the candidate
  specifically removes full inline uniform storage, also require
  `--require-submission-carrier-uniform-storage-per-record-decrease`.
  Native `dxmt9-core-device-com-spec` coverage locks the carrier-footprint
  helper contract against `sizeof(DrawRunSubmission)` and the current inline
  state/full-uniform/compact-uniform storage lanes, so future carrier refactors
  have a deterministic pre-runtime gate before another GT1 scout.
- **`CommandQueue` submit child counters** —
  `submit_draw_run_*_cpu_ms` and `submit_draw_run_batch_*_cpu_ms` split the
  queued draw submission residual into binding snapshot, payload byte scan,
  slot/payload-arena preparation, resource marking, append, and chunk-commit
  stages. Use them after `commit_chunk_queue_draw_submission_cpu_ms` or
  `commit_chunk_draw_*_submit_cpu_ms` is proven hot.
- **Present publish split counters** —
  `commit_chunk_replay_present_record_cpu_ms`, `submit_present_*_cpu_ms`,
  and `prepare_slot_{publish,resource_mark,pso_prefetch}_cpu_ms` split a hot
  Present record between drawable acquire, queue commit/publish, boundary
  policy, resource marking, and publish-time PSO prefetch. Use these before
  treating Present replay CPU as a display or Wine pacing problem.
- **Encode-slot PSO prefetch counter** —
  `encode_slot_pso_prefetch_cpu_ms` is the default home for slot-level PSO /
  depth-state handle resolution after state-churn-encode-encode-phase.70.
  It should replace, not add to, `prepare_slot_pso_prefetch_cpu_ms` in normal
  runs. A non-zero publish prefetch counter now means
  `DXMT9_ENABLE_PUBLISH_PSO_PREFETCH=1` or a policy regression. Its child
  counters (`encode_slot_pso_prefetch_{draw_lookup,depth_lookup,state_copy,
  tile_select,argbuf_select}_cpu_ms`) are the first attribution layer for
  reducing the encode-worker prefetch cost; phase 71 names draw PSO lookup/key
  work as the current owner.
- **Encode-slot PSO handle reuse counters** —
  `encode_slot_pso_prefetch_draw_handle_{adjacent,slot}_*` are opportunity
  counters only. They classify the final `PsoHandle` after the authoritative
  lookup has already completed, so they prove repeated results but not a safe
  reduced memo key. Use them to size an exact-key/final-handle slot-local memo
  candidate; do not treat `DrawPsoSubview` alone as enough PSO identity.
- **Encode-slot PSO semantic memo counters** —
  `encode_slot_pso_prefetch_draw_semantic_memo_{hits,misses,overflow}` classify
  the default pre-resolve slot-local memo added in
  state-churn-encode-encode-phase.74. Use
  `DXMT9_DISABLE_ENCODE_SLOT_PSO_SEMANTIC_MEMO=1` for A/B. A valid run must keep
  overflow at `0`, `encode_draw_pso_prefetch_handle_missing=0`, and normal visual
  smoke before treating lower `encode_slot_pso_prefetch_draw_key_resolve_cpu_ms`
  as a real cleanup. The optional child timers
  `encode_slot_pso_prefetch_draw_semantic_{key,probe,store}_cpu_ms` are
  attribution probes added in state-churn-encode-encode-phase.75; they add
  per-candidate clock calls, so use them to rank owners, not as standalone FPS
  evidence. For miss-side attribution, enable
  `DXMT9_PERF_ENCODE_SLOT_PSO_SEMANTIC_MISS_SPLIT=1` and read
  `encode_slot_pso_prefetch_draw_semantic_miss_probe_key_*`. The split is
  non-mutating and classifies only semantic misses that later hit the
  slot-local probe-key memo. Its `diff_texture_handles_only` bucket is the
  current sizing gate for a resource-shape / texture-handle-blind memo; do not
  relax the default semantic key until active texture mask/type, X8-alpha shape,
  attachment format, sampler/TSS, and visual smoke gates are proven. The next
  opt-in guard is `DXMT9_PERF_ENCODE_SLOT_PSO_RESOURCE_SHAPE_OPPORTUNITY=1`,
  which probes a texture-handle-blind resource-shape key and validates every hit
  against the final canonical probe key after the normal resolve. Treat it as a
  non-mutating opportunity counter only: a valid proof needs resource-shape
  overflow `0`, validated mismatches `0`, no skipped pipelines, no Metal
  command-buffer errors, and normal visual smoke before the resource-shape
  shortcut can be read as safe. The shortcut is now default-on after
  state-churn-encode-encode-phase.81; set
  `DXMT9_DISABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1` for opt-out A/B. When
  the default shortcut runs without the opportunity knob, resource-shape hits
  reuse the memoized `PsoHandle` and intentionally skip final-key construction,
  so `validated_*` counters remain `0` by design.
  Use `draw_key_resolve` movement plus handle-missing/skipped/error counters for
  behavior smokes. state-churn-encode-encode-phase.80 repeats the paired A/B
  and confirms a stable local CPU win (`~0.52-0.53ms/present` in the
  prefetch parent), but not an FPS owner.
- **Submission generation/lane counters** —
  `submit_draw_run_batch_submission_adjacent_*` and
  `submit_draw_run_batch_compat_same_generation_lane_*` prove whether frontend
  stable-state cache identity agrees with the queue's deep compatibility
  comparison before enabling a generation fast path.
- **Discarded-state counters** —
  `submit_draw_run_batch_discarded_state_{records,bytes}` count non-front
  `DrawRunSubmission` states that were materialized in the default path but are
  not stored by `appendDrawRunBatch()`. After
  [state-churn-encode-encode-phase.48](state-churn-encode-encode-phase.48.md), the large N-1 state materialization
  waste should stay collapsed; the `v0.0.3`-anchored current run still has a small
  residual class (`3,925` records / about `40MiB` in H159), so regression runs
  should gate `d3d9_snapshot_state_elided` as present and
  `submit_draw_run_batch_discarded_state_{records,bytes}` as non-increasing
  rather than requiring exact zero.
- **Uniform payload append split counters** —
  `draw_uniform_payload_lookup_cpu_ms`,
  `draw_uniform_payload_lookup_bucket_cpu_ms`, and
  `draw_uniform_payload_append_{reserve,copy,link}_cpu_ms` attribute the
  `submit_draw_run_batch_append_uniform_cpu_ms` child. These timers are hot-path
  attribution probes, so read parent movement as measurement overhead unless a
  separate no-extra-timer A/B confirms it. `draw_uniform_payload_append_bytes`
  is the non-timer storage-width companion: it counts appended
  `DrawUniformPayloadRecord` bytes (`10,256B` each on the current layout) so a
  low-overhead run can size the uniform SoA copy/storage lane without enabling
  the timing-sensitive child timers.
- **Backend uniform materialization counters** —
  `draw_uniform_payload_materialized`,
  `draw_uniform_payload_materialized_bytes`,
  `draw_uniform_payload_materialize_cpu_ms`, and
  `draw_uniform_payload_materialize_fallbacks` size legacy
  `DrawUniformPayload` scratch reconstruction after compact backend storage.
  A non-zero fallback means a caller bypassed compact records and should be
  treated as a correctness/coverage issue before reading the CPU number.
- **Snapshot flat-state entry counters** —
  `d3d9_snapshot_flat_render_state_entries{,_max,_gt64,_gt128}`,
  `d3d9_snapshot_flat_tss_{entries,stage_entries_max}`, and
  `d3d9_snapshot_flat_sampler_{entries,slot_entries_max}` prove whether a
  copied active-entry capacity change is safe for the weighted draw-submission
  path. Overflow counters must stay zero before accepting any cap split.
- **Draw-packet actual-change counters** —
  `draw_packet_declared_*`, `draw_packet_actual_*`, and
  `draw_packet_redundant_*` are opt-in via
  `--probe-draw-packet-actual-change`. Use them to decide whether snapshot
  cache invalidation is inflated by declared non-binding/uniform deltas that
  repeat the current `DeviceState` value.
- **`DrawBindingOverride` payload** — per-draw serialized stream/IB binding range
  in `DrawParam`; lets `scanImportedDrawRun()` accept stream-only and
  stream+IB-only runs. Counters: `commit_chunk_draw_run_binding_override_{records,bytes,stream_records,ib_records}`.
- **`RenderPass[seq=N,enc=N,rt=,depth=]` labels** — join dxmt per-encoder
  attribution to Xcode counters without row-order assumptions.
- **Run-level CPU gates** — `--require-binding-overrides-present`,
  `--require-draw-submission-batch-present`,
  `--require-draw-run-records-increase`, `--require-encode-draw-cpu-decrease`
  prove the intended CPU mechanism by `result.json` before reading Xcode frame
  counters.
- **`DXMT_DISABLE_AUTO_EXPAND_INDEXED=1`** — removes the indexed-expansion
  transient vertex amplifier (correctness-risky; needs image proof).
- **`DXMT9_FORCE_FULL_CBUF_UPLOADS=1`** — diagnostic-only cbuf visual bisection
  knob. Forces full VS/PS cbuf uploads to test whether prefix sizing owns a
  suspected artifact; do not use as a default perf workaround without
  same-input image proof.

## Experiment dependency graph

```mermaid
flowchart TD
  classDef accepted fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef rejected fill:#f8d7da,stroke:#a33,color:#600
  classDef open fill:#fff3cd,stroke:#a80,color:#640

  Drawrun1["drawrun.01\nfailure shape\n580 submits / 913k draws"]:::accepted
  Drawrun2["drawrun.02\nconst-upload boundary\nper-draw uniform"]:::accepted
  Enc1["encoder.01\nfirst breakdown\ncbuf 4.64GB / transient 1.05GB"]:::accepted
  Enc2["encoder.02\nbinding/bytes\ntransient=0 no-auto-expand"]:::accepted
  Stream1["stream.01\nstream split\nhandle 81.9% / IB 81.5%"]:::rejected
  Stream2["stream.02\ndelta breakdown\nIB delta all handle"]:::rejected
  Stream3["stream.03\nunique handles\nbounded alternation"]:::rejected
  Stream4["stream.04\ncurrent backend gate\nhandle-stable A/B required"]:::accepted
  Stream8["stream.08\nrow-scoped staging A/B\n60/2 handles -> 0\noffset churn remains"]:::accepted
  Stream9["stream.09\nXcode handle-stable gate\nGPU/VS write unchanged"]:::rejected
  Churn["churn.01\nDrawBindingOverride design"]:::accepted
  SD1["statedelta.01\nbucket split\n85.66% mixed"]:::accepted
  SD2["statedelta.02\nmixed pairs\n96.61% 2-group"]:::accepted
  SD3["statedelta.03\nexact stream+IB\n82.17%"]:::accepted
  Bind["binding.01\noverride fix\n-30.13% stream CPU / GPU +0.03%"]:::accepted
  Batch["batch.01\nHEAD recheck\nVS write flat 1627MiB"]:::rejected
  Join["encoder.03\nlabel-join\ndxmt 450KiB vs 1.63GiB"]:::accepted
  Exp1["expand.01\nno-auto-expand\nGPU 3.64->3.54s"]:::open
  Exp2["expand.02\nsame-frame Xcode\nwrites unchanged"]:::rejected
  EncodePhase["encode-phase.01\nargbuf 5.07s / packet 2.57s"]:::accepted
  EncodePhase2["encode-phase.02\ncbuf update 3.92s;\npacket cache 1.87s"]:::accepted
  EncodePhase3["encode-phase.03\nclean cbuf gate\n272k skips / -39.7ms"]:::accepted
  EncodePhase4["encode-phase.04\nreopen mask attribution\n760k no-dirty hash mismatches"]:::accepted
  EncodePhase5["encode-phase.05\ncategory identity repoint\nencode_draw -3.42s"]:::accepted
  EncodePhase6["encode-phase.06\ncurrent smoke\nnormal GT1 frame"]:::accepted
  EncodePhase7["encode-phase.07\nbinding packet split\nprobe 939ms / key 495ms"]:::accepted
  EncodePhase8["encode-phase.08\nplan-direct cache\ncache -57.34%"]:::accepted
  EncodePhase9["encode-phase.09\nargbuf open split\nreserve 746ms / setArg 115ms / table skip 0"]:::accepted
  EncodePhase10["encode-phase.10\ntransient fast append\nreserve -51.95% / encode_draw -3.87%"]:::accepted
  EncodePhase11["encode-phase.11\ndirty identity probe\n0 hits / rejected"]:::rejected
  EncodePhase12["encode-phase.12\nstream bind split\ntexture 1065ms / index 670ms\nshader 497ms / raster 389ms"]:::accepted
  EncodePhase13["encode-phase.13\ntexture sampler split\nfragment resolve 575ms\nfragment direct 317ms"]:::accepted
  EncodePhase14["encode-phase.14\nsampler pre-handle skip\n2.11M lookups avoided\ntexture parent -18.84%"]:::accepted
  EncodePhase15["encode-phase.15\nsampler-state hash reuse\nfragment direct -68ms"]:::accepted
  EncodePhase16["encode-phase.16\ntexture pre-resolve skip\n1.206M skips but parent +48ms"]:::rejected
  EncodePhase17["encode-phase.17\ncbuf category operation split\nresidual 954ms / VS residual 618ms"]:::accepted
  EncodePhase18["encode-phase.18\ncbuf residual split\nbinding hash 570ms\nVS hash 490ms"]:::accepted
  EncodePhase19["encode-phase.19\ncbuf content hash off\nbinding hash 0ms\ncbuf -0.341ms/present"]:::accepted
  EncodePhase20["encode-phase.20\nprefix-preserving cbuf builder\nbuild -47%/present\nzero-unused prefix rejected"]:::accepted
  EncodePhase21["encode-phase.21\nbinding-packet sampler key hash reuse\nplan -9.97%/present"]:::accepted
  EncodePhase22["encode-phase.22\nforce full cbuf diagnostic\nbytes +519%\nvisual inconclusive"]:::open
  EncodePhase23["encode-phase.23\nper-draw payload cbuf identity\nvisual smoke restored\nbytes -34% vs dirty fix"]:::accepted
  EncodePhase24["encode-phase.24\ncommit_chunk stage split\nreplay 21.84s / import 88ms"]:::accepted
  EncodePhase25["encode-phase.25\nreplay child split\nqueue 9.93s / snapshot 7.70s"]:::accepted
  EncodePhase26["encode-phase.26\nCommandQueue submit split\nappend 2379ms\ncompat scan 560ms"]:::accepted
  EncodePhase27["encode-phase.27\nsnapshot invalidation candidate\nactual-change probe designed"]:::open
  EncodePhase28["encode-phase.28\nactual-change probe\nredundant nonbinding 0"]:::rejected
  EncodePhase29["encode-phase.29\nbatch append split\nstate 958ms / uniform 902ms\npayload 65ms"]:::accepted
  EncodePhase30["encode-phase.30\nrvalue state append\nappend -21.85%\nstate -24.82%"]:::accepted
  EncodePhase31["encode-phase.31\nstate inner split\nSoA 707ms\nPSO 50ms / invariant 22ms"]:::accepted
  EncodePhase32["encode-phase.32\nslot-state intern rejected\n0.15% reuse hits\nnext: generation/lane counters"]:::accepted
  EncodePhase33["encode-phase.33\ngeneration/lane proof\n52.5% pairs; 0 incompatible\nnext: compat fast path"]:::accepted
  EncodePhase34["encode-phase.34\ngeneration/lane fast path\ncompat scan -91.9%\nqueue still copy-bound"]:::accepted
  EncodePhase35["encode-phase.35\nin-place submission fill\nresidual -93ms\nnot first-order"]:::accepted
  EncodePhase36["encode-phase.36\npersistent pending scratch\nno residual win\nremoved"]:::rejected
  EncodePhase37["encode-phase.37\nTLS binding snapshot scratch\nsnapshot bucket -27.3%\nreplay flat"]:::accepted
  EncodePhase38["encode-phase.38\nshared shader bytecode storage\nstate copy -35.7%\nqueue -7.4%"]:::accepted
  EncodePhase39["encode-phase.39\nsampler state compact\nFlatDrawState -41%\nqueue -12.3%"]:::accepted
  EncodePhase40["encode-phase.40\nTSS active-entry compact\nFlatDrawState -18.5%\nSoA -16.3%"]:::accepted
  EncodePhase41["encode-phase.41\nrender-state count proof\nmax 62 / gt64 0\nshrink held"]:::accepted
  EncodePhase42["encode-phase.42\nrender-state priority compact\nFlatDrawState -11.4%\noverflow 0 / GPU flat"]:::accepted
  EncodePhase43["encode-phase.43\nfront resource marking\nresource mark -8.9%"]:::accepted
  EncodePhase44["encode-phase.44\nsame-stamp state elision\n4.10GB elided\nFPS flat"]:::accepted
  EncodePhase45["encode-phase.45\ncarrier emplace split\n13.99% of queue"]:::accepted
  EncodePhase46["encode-phase.46\noptional carrier storage\nemplace -49%"]:::accepted
  EncodePhase47["encode-phase.47\ndiscarded materialized state\n411k / 4.21GB"]:::accepted
  EncodePhase48["encode-phase.48\ndefault state elision\ndiscarded -> 0"]:::accepted
  EncodePhase49["encode-phase.49\n2-way binding cache\nmisses down\nCPU regresses"]:::rejected
  EncodePhase50["encode-phase.50\nindexed fast path\nindex setup -46%\nFPS flat"]:::accepted
  EncodePhase51["encode-phase.51\nuniform lookup prereserve\nappend_uniform flat\nreverted"]:::rejected
  EncodePhase52["encode-phase.52\nuniform append split\ncopy 813ms\nreserve 53ms"]:::accepted
  EncodePhase53["encode-phase.53\nuniform record emplace\ncopy -25.9%"]:::accepted
  EncodePhase54["encode-phase.54\nPSO resolve cache\nhits exist\nlookup no win"]:::rejected
  EncodePhase55["encode-phase.55\nargbuf reopen split\nopen_call 574ms\npost 891ms"]:::accepted
  EncodePhase56["encode-phase.56\nwhole-table argbuf reuse\n961k checks / 0 skips"]:::rejected
  EncodePhase57["encode-phase.57\npost-open residual split\nsmall bookkeeping children"]:::accepted
  EncodePhase58["encode-phase.58\nbinding-plan split\nfragment largest child\nopt-in only"]:::accepted
  EncodePhase59["encode-phase.59\nuniform dedup off\nappend_uniform -242ms\nqueue flat"]:::accepted
  EncodePhase60["encode-phase.60\nissue split\nMetal indexed draw\nowns issue"]:::accepted
  EncodePhase61["encode-phase.61\nargbuf cbuf probe split\nno single stage owner"]:::accepted
  EncodePhase62["encode-phase.62\ndirty VS identity\n0 hits / 788k misses"]:::rejected
  EncodePhase63["encode-phase.63\npayload delta\nVS/PS constants own reopen"]:::rejected
  EncodePhase64["encode-phase.64\nVS cbuf plan shape\nusage prefix dominates"]:::accepted
  EncodePhase65["encode-phase.65\nconstant sparsity\nsafe packed save 0"]:::rejected
  EncodePhase66["encode-phase.66\nBLENDINDICES window\ntop VS full-range draw"]:::rejected
  EncodePhase67["encode-phase.67\nStage2 off scout\nencode_draw -4.55s\ncompletion wait up"]:::accepted
  EncodePhase68["encode-phase.68\nlow-overhead FPS gate\nencode -3ms/frame\nFPS flat"]:::rejected
  EncodePhase69["encode-phase.69\npublish PSO prefetch\npresent replay owner"]:::accepted
  EncodePhase70["encode-phase.70\nencode-slot PSO prefetch\ndefault placement"]:::accepted
  EncodePhase71["encode-phase.71\nPSO prefetch split\ndraw lookup/key owner"]:::accepted
  EncodePhase72["encode-phase.72\nfinal-handle reuse\n82.8% repeats"]:::accepted
  EncodePhase73["encode-phase.73\nprobe-key memo\ndraw lookup -91%"]:::accepted
  EncodePhase74["encode-phase.74\nsemantic memo\nkey resolve -53%"]:::accepted
  EncodePhase75["encode-phase.75\nsemantic split\noverhead not owner"]:::rejected
  EncodePhase76["encode-phase.76\nsemantic miss split\ntexture handles only"]:::accepted
  EncodePhase77["encode-phase.77\nresource-shape proof\n0 mismatches"]:::accepted
  EncodePhase78["encode-phase.78\nresource-shape behavior\nshortcut live"]:::accepted
  EncodePhase79["encode-phase.79\nresource-shape A/B\nCPU win"]:::accepted
  EncodePhase80["encode-phase.80\nresource-shape repeat\nCPU win"]:::accepted
  EncodePhase81["encode-phase.81\nresource-shape default\nCPU cleanup"]:::accepted
  EncodePhase82["encode-phase.82\nresource-shape scratch\nno per-slot heap"]:::accepted
  EncodePhase83["encode-phase.83\nPSO memo scratch epochs\nno per-slot zero-init"]:::accepted
  EncodePhase84["encode-phase.84\nargbuf table shadow\nskip still 0"]:::rejected
  EncodePhase85["encode-phase.85\nargbuf reopen split\nopt-in child timers"]:::accepted
  EncodePhase86["encode-phase.86\nargbuf cbuf probe\nopt-in timers"]:::accepted
  EncodePhase87["encode-phase.87\nPSO semantic split\nopt-in timers"]:::accepted
  EncodePhase88["encode-phase.88\nuniform payload split\nblack scene"]:::rejected
  EncodePhase89["encode-phase.89\nstream-bind split\nopt-in timers"]:::accepted
  EncodePhase90["encode-phase.90\npending submissions scratch\nlocal cleanup; FPS flat"]:::accepted
  EncodePhase91["encode-phase.91\ncurrent next-owner scout\nP2/P3 serial + P4 overlap"]:::accepted
  EncodePhase92["encode-phase.92\nstate-copy elision live\nstate copy no longer owner"]:::accepted
  EncodePhase93["encode-phase.93\nuniform payload hash reuse\n0.7% adjacent only"]:::rejected
  EncodePhase94["encode-phase.94\nuniform component hashes\nPS 64%, VS 17.6%"]:::accepted
  EncodePhase95["encode-phase.95\nVS/PS generations\nPS hash/copy cleanup"]:::accepted
  EncodePhase96["encode-phase.96\nmiss hash reuse\nsmall VS cleanup"]:::accepted
  EncodePhase97["encode-phase.97\nindexed-float hash trim\nsmall VS cleanup"]:::accepted
  EncodePhase98["encode-phase.98\nuniform append bytes\nnon-timer storage counter"]:::accepted
  EncodePhase99["encode-phase.99\nuniform derived summary\nmaterialized vs append width"]:::accepted
  EncodePhase100["encode-phase.100\nuniform compare gates\nfrontend vs backend byte proof"]:::accepted
  EncodePhase101["encode-phase.101\nuniform owner gates\nbuild/hash/append child proof"]:::accepted
  EncodePhase102["encode-phase.102\nuniform compact opportunity\nusage-live byte proof"]:::accepted
  Snapshot10["snapshot.10\nuniform-refresh fast path\nrefresh -59.6%\nFPS flat"]:::accepted

  Drawrun1 -->|"split-into"| Drawrun2
  Drawrun1 -->|"measured-by"| Enc1
  Enc1 -->|"stream-split"| Stream1
  Stream1 -->|"delta-confirm"| Stream2
  Stream2 -->|"unique-handle"| Stream3
  Stream3 -->|"current frame60 gate"| Stream4
  Stream4 -->|"row-scoped diagnostic"| Stream8
  Stream8 -->|"Xcode sensitivity gate"| Stream9
  Stream1 -->|"handle-churn->design"| Churn
  Drawrun1 -->|"state-delta-split"| SD1
  SD1 -->|"mixed-pairs"| SD2
  SD2 -->|"exact-pair"| SD3
  SD3 -->|"motivated"| Bind
  Churn -->|"mechanism-for"| Bind
  Bind -->|"recheck"| Batch
  Enc2 -->|"label-join"| Join
  Stream3 -->|"flagged-amplifier"| Exp1
  Exp1 -->|"same-frame-validate"| Exp2
  Exp2 -->|"motivated-labels"| Join
  Batch -->|"GPU unmoved"| Join
  Batch -->|"CPU remainder split"| EncodePhase
  EncodePhase -->|"child buckets"| EncodePhase2
  EncodePhase2 -->|"safe no-op gate"| EncodePhase3
  EncodePhase3 -->|"dirty path split"| EncodePhase4
  EncodePhase4 -->|"category identity"| EncodePhase5
  EncodePhase5 -->|"fresh smoke"| EncodePhase6
  EncodePhase6 -->|"next hot child split"| EncodePhase7
  EncodePhase7 -->|"plan-direct cache"| EncodePhase8
  EncodePhase8 -->|"argbuf open split"| EncodePhase9
  EncodePhase9 -->|"remove arena scan"| EncodePhase10
  EncodePhase10 -->|"dirty upload reuse probe"| EncodePhase11
  EncodePhase11 -->|"split next named bucket"| EncodePhase12
  EncodePhase12 -->|"split texture/sampler child"| EncodePhase13
  EncodePhase13 -->|"avoid skipped sampler lookup"| EncodePhase14
  EncodePhase14 -->|"reuse sampler state hash"| EncodePhase15
  EncodePhase15 -->|"try texture source pre-resolve"| EncodePhase16
  EncodePhase16 -->|"return to cbuf bucket"| EncodePhase17
  EncodePhase17 -->|"split residual"| EncodePhase18
  EncodePhase28 -->|"real payload work"| Snapshot10
  EncodePhase18 -->|"remove unused hash"| EncodePhase19
  EncodePhase19 -->|"avoid full cbuf build"| EncodePhase20
  EncodePhase20 -->|"next residual packet plan"| EncodePhase21
  EncodePhase20 -->|"visual bisection"| EncodePhase22
  EncodePhase22 -->|"diff + disable-batch A/B"| EncodePhase23
  EncodePhase23 -->|"commit call owner split"| EncodePhase24
  EncodePhase24 -->|"replay child split"| EncodePhase25
  EncodePhase25 -->|"submit path split"| EncodePhase26
  EncodePhase25 -->|"snapshot owner analysis"| EncodePhase27
  EncodePhase27 -->|"no-op delta proof"| EncodePhase28
  EncodePhase26 -->|"append child split"| EncodePhase29
  Snapshot10 -->|"queue append next"| EncodePhase29
  EncodePhase29 -->|"remove value hops"| EncodePhase30
  EncodePhase30 -->|"split remaining state child"| EncodePhase31
  EncodePhase31 -->|"slot interning rejected;\nmove upstream"| EncodePhase32
  EncodePhase32 -->|"stamp and count generation/lane"| EncodePhase33
  EncodePhase33 -->|"skip proven deep compares"| EncodePhase34
  EncodePhase34 -->|"remove temporary submission move"| EncodePhase35
  EncodePhase35 -->|"test vector capacity reuse"| EncodePhase36
  EncodePhase36 -->|"fix submit scratch allocation"| EncodePhase37
  EncodePhase37 -->|"remove bytecode value copies"| EncodePhase38
  EncodePhase38 -->|"reduce sampler state width"| EncodePhase39
  EncodePhase39 -->|"split TSS id space vs active entries"| EncodePhase40
  EncodePhase40 -->|"prove render-state count before shrinking"| EncodePhase41
  EncodePhase41 -->|"use 128-slot priority payload"| EncodePhase42
  EncodePhase42 -->|"avoid duplicate resource marking"| EncodePhase43
  EncodePhase43 -->|"skip N-1 copied states behind gate"| EncodePhase44
  EncodePhase44 -->|"measure remaining carrier construction"| EncodePhase45
  EncodePhase45 -->|"avoid default construction"| EncodePhase46
  EncodePhase46 -->|"size default-path discarded state"| EncodePhase47
  EncodePhase47 -->|"promote safe same-stamp elision"| EncodePhase48
  EncodePhase21 -->|"test cache associativity later"| EncodePhase49
  EncodePhase12 -->|"remove default diagnostic index bytes"| EncodePhase50
  EncodePhase48 -->|"probe uniform append reserve overhead"| EncodePhase51
  EncodePhase51 -->|"split append miss path"| EncodePhase52
  EncodePhase52 -->|"remove record temporary"| EncodePhase53
  EncodePhase53 -->|"probe prefetched PSO resolve residual"| EncodePhase54
  EncodePhase54 -->|"return to larger argbuf child"| EncodePhase55
  EncodePhase55 -->|"try pre-open whole-table skip"| EncodePhase56
  EncodePhase56 -->|"split residual instead"| EncodePhase57
  EncodePhase57 -->|"check packet-plan residual"| EncodePhase58
  EncodePhase53 -->|"test uniform payload dedup value"| EncodePhase59
  EncodePhase58 -->|"split draw issue bucket"| EncodePhase60
  EncodePhase57 -->|"split cbuf probe/repoint stages"| EncodePhase61
  EncodePhase61 -->|"refresh dirty VS skip proof"| EncodePhase62
  EncodePhase62 -->|"split argbuf reopen payload delta"| EncodePhase63
  EncodePhase63 -->|"measure cbuf plan shape"| EncodePhase64
  EncodePhase64 -->|"scan exact register sparsity"| EncodePhase65
  EncodePhase65 -->|"dump geometry + measure a0 range"| EncodePhase66
  EncodePhase66 -->|"fallback to storage-model scout"| EncodePhase67
  EncodePhase67 -->|"low-overhead FPS A/B"| EncodePhase68
  EncodePhase68 -->|"move present PSO work"| EncodePhase69
  EncodePhase69 -->|"relocate prefetch to encode worker"| EncodePhase70
  EncodePhase70 -->|"split residual prefetch"| EncodePhase71
  EncodePhase71 -->|"measure handle reuse"| EncodePhase72
  EncodePhase72 -->|"memo by probe key"| EncodePhase73
  EncodePhase73 -->|"memo before resolved-key build"| EncodePhase74
  EncodePhase74 -->|"split semantic overhead"| EncodePhase75
  EncodePhase75 -->|"classify semantic misses"| EncodePhase76
  EncodePhase76 -->|"validate resource-shape key"| EncodePhase77
  EncodePhase77 -->|"enable default-off shortcut"| EncodePhase78
  EncodePhase78 -->|"paired A/B"| EncodePhase79
  EncodePhase79 -->|"repeat A/B"| EncodePhase80
  EncodePhase80 -->|"promote default"| EncodePhase81
  EncodePhase81 -->|"remove memo allocation"| EncodePhase82
  EncodePhase82 -->|"remove remaining memo zero-init"| EncodePhase83
  EncodePhase83 -->|"return to argbuf lane"| EncodePhase84
  EncodePhase84 -->|"make attribution timers opt-in"| EncodePhase85
  EncodePhase85 -->|"make cbuf probe timers opt-in"| EncodePhase86
  EncodePhase86 -->|"make PSO semantic timers opt-in"| EncodePhase87
  EncodePhase87 -->|"try uniform payload timers"| EncodePhase88
  EncodePhase88 -->|"safe sibling split"| EncodePhase89
  EncodePhase89 -->|"remove replay scratch allocation"| EncodePhase90
  EncodePhase90 -->|"refresh current owner"| EncodePhase91
  EncodePhase91 -->|"validate promoted state elision"| EncodePhase92
  EncodePhase92 -->|"probe residual uniform copy reuse"| EncodePhase93
  EncodePhase93 -->|"split VS/PS component reuse"| EncodePhase94
  EncodePhase94 -->|"add component generations"| EncodePhase95
  EncodePhase95 -->|"reuse on cache miss"| EncodePhase96
  EncodePhase96 -->|"trim indexed-float int/bool tail hash"| EncodePhase97
  EncodePhase97 -->|"size append/storage width"| EncodePhase98
  EncodePhase98 -->|"make next scout readable"| EncodePhase99
  EncodePhase99 -->|"make A/B verdict enforceable"| EncodePhase100
  EncodePhase100 -->|"split residual owner gates"| EncodePhase101
  EncodePhase101 -->|"size compact payload opportunity"| EncodePhase102
```

## Results synthesis

The CPU encode story is settled. The per-draw encode path stays hot because
~99.94% of draws fail to batch into draw-runs (state-churn-encode-drawrun.01):
constant-upload boundaries are the largest break class, and state-delta breaks
are second. The per-encoder breakdown (state-churn-encode-encoder.01,
state-churn-encode-stream.01) proved the state-delta churn is **handle
churn** (81.5-81.9% of stream/IB samples), not offset/stride, and that the
handles are a bounded set repeatedly *alternated* — not per-draw created
(state-churn-encode-stream.03). The state-delta taxonomy
(state-churn-encode-statedelta.01→state-churn-encode-statedelta.03)
narrowed the dominant break to the *exact stream+IB pair* (82.17% of all
state-delta), naming the precise payload target. The `DrawBindingOverride` path
(state-churn-encode-churn.01, state-churn-encode-binding.01) then carried
per-draw stream/IB bindings inside a run, cutting stream-bind encode CPU
`-30.13%` and total encode CPU `-10.44%` with no churn increase — the one
**accepted CPU win** of this domain.

What is also settled is the negative: every GPU frame-time check stayed flat.
The binding-override A/B moved `gpu_command_buffer_time_ms` only `+0.03%`; the
post-submission-batch HEAD recheck (state-churn-encode-batch.01) left the
top-three VS buffer write at ~`1627.3 MiB` (unchanged); disabling auto-expand
(state-churn-encode-expand.01, state-churn-encode-expand.02) removed the
CPU transient amplifier but left the Xcode top-pass buffer/device writes
unchanged at ~`1.63 GiB`. The label-join validation
(state-churn-encode-encoder.03) is the clean proof: the top three encoders
own ~`98.4%` of frame GPU and ~`1.63 GiB` of buffer writes, while their entire
dxmt CPU/upload payload is ~`450 KiB`. These are CPU-throughput wins, orthogonal
to the GPU bottleneck. The only open item is the correctness of disabling
auto-expand-indexed, which still needs visual proof.

The current stream/IB backend preflight
([state-churn-encode-stream.04](state-churn-encode-stream.04.md)) keeps the GPU-side question alive but narrows
the spend gate. Frame60 hot rows are handle-churn-dominant, not offset/stride
noise: `60/2` has combined stream+IB handle changes/draw `2.305` versus
offset+stride/draw `0.053`, with explicit dxmt writers only `0.089 B/vertex`.
`60/1` and `60/0` show the same pattern. The probe-draw join confirms draw-level
stream0/IB alternation (`60/2`: `160` stream0 changes and `160` IB changes),
and the new `stream_extra_bindings` field confirms stream1 as another active
row-local source (`111` extra-stream changes, `25` unique extra-stream
bindings). The complete binding tuple changes `160` times across `187` draws
with only `58` unique tuples, max tuple run `6`, and average run length `1.161`.
The follow-up tuple-structure pass ([state-churn-encode-stream.05](state-churn-encode-stream.05.md)) makes the
shape more concrete: `60/2` has `168/187` stream0/IB pairs with `IB =
stream0 + 2`, and `132/187` full stream0/stream1/IB triplets with `stream1 =
stream0 + 1` and `IB = stream0 + 2`; `60/1` and `60/0` are `100%` stream0/IB
`+2` pairs. This makes stream/IB a plausible handle-stabilizing A/B target, but
also rejects a simple bind-cache fix. The feasibility pass
([state-churn-encode-stream.06](state-churn-encode-stream.06.md)) closes two invalid shortcuts:
force-expanding indexed rows changes the index/VS-invocation denominator, and a
per-draw transient copy adds explicit writer traffic. A future run must present
the same geometry bytes through fewer/stable Metal buffer identities while
holding geometry, index order, VS invocations, render state, and visible shader
layout stable before it can claim hidden-backend bytes/invocation movement. The
staging-cost preflight ([state-churn-encode-stream.07](state-churn-encode-stream.07.md)) says row-stable
staging is feasible as a diagnostic but carries confounders: `60/2` would copy
about `8.2 MiB` (`7.035 B/vertex`), roughly `78.7x` the row's current explicit
dxmt writers, and handle changes would become `2.305` expected offset
changes/draw. That is acceptable for a no-gputrace A/B, not for a direct Xcode
claim. The implemented row-scoped A/B ([state-churn-encode-stream.08](state-churn-encode-stream.08.md)) proves
that isolation is real: `60/2` keeps `187` draws, PSO changes `48 -> 48`,
argbuf table bytes `5056 -> 5056`, argbuf cbuf bytes `96424 -> 96424`, and
`setVertexBytes` bytes `2992 -> 2992`, while stream handle changes drop
`271 -> 0` and IB handle changes drop `160 -> 0`. The cost is explicit
staging traffic (`6.94 MiB` stream + `0.44 MiB` IB) and remaining offset churn
(`stream_offset_changes=271`). The Xcode follow-up
([state-churn-encode-stream.09](state-churn-encode-stream.09.md)) rejects the GPU-side owner hypothesis:
target row `60/2` is shape-stable with stream/IB handle changes `271/160 ->
0/0`, but GPU time is effectively unchanged (`19.184 -> 19.278 ms`), VS buffer
write is unchanged (`981.159 -> 981.166 MiB`), and VS invocations stay fixed
at `642,001`. Stream/IB handle churn remains a CPU/draw-run batching problem,
not the first-order GT1 hidden-backend GPU limiter.

The current CPU-side open path has also narrowed. The broad bind-cache proposal
from [present-pacing-encode-budget-fix-proposal.01](../present-pacing/present-pacing-encode-budget-fix-proposal.01.md) is rejected by
present-pacing-bind-cache-work-a.01. Follow-up encode-phase timers first
named argbuf setup and binding-packet construction/bookkeeping
(state-churn-encode-encode-phase.01), then split them into child buckets:
dirty cbuf mirroring (`3.92s`), binding-packet cache lookup/store (`1.87s`),
and argbuf table open/repoint (`1.21s`) (state-churn-encode-encode-phase.02).
The first mutating A/B then skipped `272,956` clean/no-op argbuf cbuf updates
but saved only `39.7ms` from that bucket (state-churn-encode-encode-phase.03).
The dirty-path split then rejected dirty-bit-only partial repoint:
state-churn-encode-encode-phase.04 saw `0` cached repoints and `760,157`
no-dirty whole-payload hash mismatches that force conservative VS/PS/FFPPS
uploads. The follow-up category identity implementation closed that bet:
state-churn-encode-encode-phase.05 repoints cached VS/PS/FFPPS entries on
no-dirty payload mismatch and cuts `encode_draw_cpu_ms` by `3.42s`
(`-17.74%`), transient upload bytes by `-62.05%`, and dirty cbuf calls by
`-41.88%` versus reopen mask. The fresh current-state smoke
state-churn-encode-encode-phase.06 keeps the same counter shape and produces
a normal visible GT1 frame (`actual.png` robot/flare/HUD), so the path is no
longer counter-only. It is still weaker than same-input exact image proof. Treat
stream/texture/index bind counters as useful but partly nested aggregates.
The next child split also narrowed the binding-packet cache: in
state-churn-encode-encode-phase.07, `cacheDrawBindingPacket()` spends most
of its measured local time in probe/full key equality (`939ms`) and key
construction (`495ms`), while store/copy is `292ms`. Hits are already 85.51%,
and 99.92% of misses are direct-map collisions, so associativity is plausible
but secondary until a compact packet identity/signature reduces hit-side
comparison cost. The next CPU implementation bet should therefore target
binding-packet identity/probe cost or D3D9 snapshot/state rebuild path, with
only smaller cbuf follow-ups such as narrowing FFPPS identity. That
binding-packet bet is now closed as a CPU win:
state-churn-encode-encode-phase.08 hashes/probes the existing
`DrawBindingPacketPlan` directly and compares only active binding/sampler
prefixes, cutting `encode_draw_binding_packet_cache_cpu_ms` by `57.34%` and
total `encode_draw_cpu_ms` by `7.40%` versus current identity smoke with a
normal GT1 frame. After snapshot-cache-snapshot.10, the priority has shifted
again: the uniform-refresh fast path drops D3D9 snapshot submission
`7622.807ms→6495.069ms` and refresh cost `2014.263ms→814.507ms` over `1680`
presents, but sampled FPS remains flat (`15.717→15.752`). The packet-cache
bucket itself is no longer first-order, and residual snapshot is now a named
CPU cleanup rather than a run-level limiter by itself. The broader encode path
still is: argbuf setup (`3357.980ms`), binding-packet plan/cache
(`2705.893ms`), stream/index bind, and queue append are the next named buckets
to split or reduce before another generic bind-cache guess. The later
state-churn-encode-encode-phase.60 closes issue cost as indexed Metal draw
call attribution rather than wrapper overhead.

The first follow-up split of that broader encode path is
state-churn-encode-encode-phase.09. It rejects the simple "argbuf open is
mostly `MTLArgumentEncoder.setArgumentBuffer`" explanation: the argument-encoder
retarget is only `115.192ms` over the run, while transient table reservation is
`745.942ms`, slot-30 render-table bind is `192.913ms`, cached cbuf repoint is
`326.434ms`, and content probing is `99.576ms`. `encode_draw_argbuf_table_bind_skipped`
is `0`, because the fresh-table design gives every reopened argbuf a distinct
offset. That makes more slot-30 bind shadowing the wrong next bet; the remaining
argbuf work has to be structural table allocation/open reduction or narrower
cache/repoint decision work.

The first structural table-allocation bet is accepted in
state-churn-encode-encode-phase.10. `ResourceArena` now skips the O(n)
live-allocation overlap scan when a request appends at the current slab cursor
and the live allocation deque is non-wrapped; wrapped ring states still use the
old scan. In a same-present A/B (`1680 -> 1680` presents), this cuts
`transient_upload_cpu_ms` `961.534 -> 223.304`, `encode_draw_argbuf_open_reserve_cpu_ms`
`745.942 -> 358.422` (`-51.95%`), `encode_draw_argbuf_setup_cpu_ms`
`4259.704 -> 3564.075` (`-16.33%`), and total `encode_draw_cpu_ms`
`17593.130 -> 16911.650` (`-3.87%`). GPU command-buffer time and
`completion_wait_ms` stay flat/noisy, so this is a CPU-throughput win only.
The residual argbuf work is now cbuf upload/build/repoint decision cost rather
than simple transient reservation scanning.

The first dirty-cbuf reuse follow-up is rejected in
state-churn-encode-encode-phase.11. A temporary dirty-category identity
probe tried to reuse cached VS/PS/FFPPS slices when a dirty reopen still matched
the current category identity. GT1 produced `0` hits for all three categories
over `19,769` partial-dirty reopen candidates, while adding `8.609ms` of probe
overhead. The branch was removed. Treat the dirty VS/PS/FFPPS update path as
real dirty work for this workload; the next cbuf direction has to reduce
upstream dirty frequency, make build/upload cheaper, or change the constants
layout, not add another identity repoint check.

The next retained attribution split is state-churn-encode-encode-phase.12.
It decomposes `encode_draw_stream_bind_cpu_ms` after the fast-append baseline.
The run processed more presents (`1680 -> 1740`), so it is not an A/B CPU win,
but per-present GPU/`completion_wait_ms` stayed flat/noisy and the visible frame
was normal. The parent `stream_bind` bucket is distributed across multiple
classes: texture/sampler binding (`1065.369ms`, 35.66% of parent), index phase
(`669.907ms`, 22.43%), shader stream binding (`496.708ms`, 16.63%), and
raster/base-state work (`389.388ms`, 13.03%). FFP stream binding is only
`6.845ms`. The next stream-side work should therefore split texture/sampler
skip opportunities, index setup/source resolve, and shader-stream binding
diversity before attempting a broad bind-cache or FFP stream optimization.

That texture/sampler split is now recorded in
state-churn-encode-encode-phase.13. It is a same-present attribution run
(`1740 -> 1740`) with normal visual smoke. The path is almost entirely
fragment-stage: fragment resolve costs `575.228ms` and fragment direct
bind/shadow/set costs `316.761ms`, while resource-array binding, vertex
textures, and LOD-bias upload are all zero for this GT1 run. Existing skip
counters also show the direct path is already skipping heavily: texture binds
skip ~`52.69%`, and sampler binds skip ~`92.05%`. The useful next hypothesis is
not "add more sampler bind shadowing" after the handle exists; it is to avoid
materializing sampler handles/cache lookups before the shadow identity proves
the sampler bind will be skipped.

The follow-up implementation state-churn-encode-encode-phase.14 accepts that
hypothesis as a CPU win. `TextureSamplerBindShadow` now stores an exact sampler
identity (`FlatStateSet`, LOD, argument-buffer support bit) in addition to the
final Metal handle, so the direct fragment sampler path can skip before calling
`samplerStateFor()`. In a same-present A/B, pre-handle skip count exactly
matches skipped sampler binds (`2,108,453`), remaining lookup calls exactly
match real sampler binds (`181,844`), `encode_draw_texture_sampler_bind_cpu_ms`
drops `1099.703 -> 892.480` (`-18.84%`), and total `encode_draw_cpu_ms` drops
`17842.278 -> 17724.955` (`-117.323ms`). GPU and completion wait remain
flat/noisy, so this is still a CPU-throughput win rather than a GPU/fps proof.

state-churn-encode-encode-phase.15 closes the remaining sampler-side hash
tax in that direct lane. The binding packet now carries `samplerStateHash`, so
the sampler shadow key can mix the precomputed hash instead of rehashing the
`FlatStateSet` for every direct texture/sampler entry. In the default perf
profile this cuts `encode_draw_texture_sampler_fragment_direct_cpu_ms`
`495.039 -> 426.614` (`-68.425ms`) and the texture/sampler parent
`892.480 -> 822.864` (`-69.616ms`) in a same-present run. The heavy per-entry
direct split counters are preserved only behind
`DXMT9_PERF_TEXTURE_SAMPLER_DIRECT_SPLIT=1`.

The follow-up texture-side source match is rejected in
state-churn-encode-encode-phase.16. Matching D3D texture handle + sRGB before
`findTexture()` proves the mechanism (`1,206,015` skipped texture resolves) and
cuts the local fragment resolve bucket `186.426 -> 157.524`, but it regresses the
parent `encode_draw_texture_sampler_bind_cpu_ms` `822.864 -> 871.088` and leaves
total `encode_draw_cpu_ms` slightly worse. A temporary default-off smoke proved
the skip counter was `0`, but still showed parent texture/sampler instability,
so the rejected source-identity branch, counter, env flag, and extra shadow
fields were removed from the hot path. The removed-branch default run returns
texture/sampler parent CPU to baseline (`822.864 -> 821.007`) with a normal
visible frame, so default perf keeps the Metal-handle texture shadow from
state-churn-encode-encode-phase.15.

With the texture path back at baseline, state-churn-encode-encode-phase.17
returns to the remaining cbuf-update bucket and splits build/upload/setBuffer by
VS/PS/FFP category. The result rejects the simple "Metal `setBuffer` owns cbuf
update" explanation: `setBuffer` is only `114.568ms`, transient upload is
`276.019ms`, and cbuf struct build is `477.921ms` over the `1680`-present run.
The inferred update residual is larger than all of them (`954.163ms`), dominated
by VS residual (`618.150ms`). [state-churn-encode-encode-phase.18](state-churn-encode-encode-phase.18.md) performs
that residual split and rejects upload-plan/observer as the owner:
`upload_plan` is only `43.287ms` and is nested inside build, while observer cost
is `0`. The newly named dominant child is binding content hash:
`encode_draw_argbuf_cbuf_binding_hash_cpu_ms=570.070`, with VS alone at
`489.627ms`. `writtenBindings` writeback is only `41.288ms`. That leaves a real
but smaller residual (`523.767ms` total; VS `200.752ms`). The next cbuf
implementation bet should therefore target safe `hashConstantBufferBytes()`
avoidance or deferral before changing constants layout, and any cache-key
semantic change needs same-input image proof because time-based GT1
`actual.png` is not a correctness oracle (baselines-visual-capture.01).
[state-churn-encode-encode-phase.19](state-churn-encode-encode-phase.19.md) closes that bet as a default CPU win:
uploaded argbuf cbuf bindings now leave `contentHash=0` unless
`DXMT9_ARGBUF_CBUF_CONTENT_HASH=1` explicitly requests the legacy byte hash, and
`contentMatches()` treats zero as a non-match sentinel. The live cache decisions
remain full `payloadHash`, per-category `identityHash`, and FFPVS byte compare.
In a watchdog-finalized no-gputrace run, `binding_hash` drops `570.070 -> 0ms`,
cbuf update drops `1.216 -> 0.875ms/present`, and backend encode drops
`10.359 -> 10.006ms/present`; GPU time remains flat/noisy. The next cbuf work
should therefore move to build/upload, content-probe/cached-repoint, binding
writeback, or residual dispatch/timer cost.
[state-churn-encode-encode-phase.20](state-churn-encode-encode-phase.20.md) closes the build sub-bet, with an
important correctness boundary. Dirty VS/PS cbuf updates now build raw upload
bytes instead of first materializing full `VsConsts` / `PsConsts`, but the raw
builder preserves the exact MSL-visible byte prefix that the old full builder
would have uploaded. The first live-range-only version zeroed bytes inside that
prefix and produced visibly dark/black GT1 geometry, so usage bounds are only
safe for choosing the prefix size, not for rewriting bytes inside the prefix.
The accepted prefix-preserving version restores the normal smoke frame and cuts
cbuf build `0.333815 -> 0.175342ms/present`, cbuf update
`0.875284 -> 0.679652ms/present`, and backend encode
`10.005939 -> 9.853414ms/present`. The next cbuf targets are cached repoint,
upload/setBuffer, content probe, and residual timer/dispatch cost, not another
full-builder reduction.
[state-churn-encode-encode-phase.21](state-churn-encode-encode-phase.21.md) then returns to the broader encode
frontier and closes a small binding-packet plan tax. The packet plan previously
rehashed active sampler `FlatStateSet` payloads even though the D3D9 snapshot
had already computed `hot.key.samplerStateHashes[]`. Reusing the canonical key
hash, while still storing and comparing the full sampler state set for packet
equality, cuts `binding_packet_plan` `0.666122 -> 0.599724ms/present`, the
packet parent `1.646770 -> 1.573957ms/present`, and backend encode
`9.853414 -> 9.662653ms/present`. This is a CPU-only cleanup; the visible smoke
frame is normal but not an exact image proof because the two `actual.png`
captures drifted by frame/time.
[state-churn-encode-encode-phase.22](state-churn-encode-encode-phase.22.md) adds a diagnostic fallback for the
reported black/semi-transparent-looking geometry. `DXMT9_FORCE_FULL_CBUF_UPLOADS=1`
forces VS/PS cbuf plans back to full `VsConsts` / `PsConsts` uploads, but the
same-present smoke rejects it as a default workaround: argbuf cbuf/transient
traffic jumps by about `+519%`, backend encode rises `+2.61%`, and the visual
smoke does not obviously normalize the artifact. Because the images drifted
from frame 1003 to frame 994, this is not exact correctness proof; future visual
debugging should use mini-replay or same-input semantic image gates rather than
time-based `actual.png`.
[state-churn-encode-encode-phase.23](state-churn-encode-encode-phase.23.md) closes that visual bisection with the
actual root cause. Disabling draw submission batching normalizes the GT1 smoke,
and the code diff from the visual-good tag lineage, now anchored at `v0.0.3`,
points at the batched
per-draw uniform path. The bug is not full cbuf size; it is stale cbuf cache
identity. Batched draws can carry a current `DrawUniformPayload` while the base
`FlatDrawStateRecord` still has the first draw's VS/PS constant hashes, so the
argbuf cbuf probe could false-hit a stale slice. The accepted fix stores VS/PS
component hashes in `DrawUniformPayload` and uses those for argbuf cbuf identity.
It keeps normal visual smoke, retains batching, and cuts the temporary
all-dirty correctness fix's argbuf traffic by `34.24%`.

state-churn-encode-encode-phase.24 corrects the next CPU attribution
mistake. The historical `bridge_commit_latency_ns` key is not raw Wine bridge
or ABI crossing cost; it measures the whole synchronous
`dxmt9c_device_commit_chunk()` call up to return to PE. A no-gputrace stage
split shows `22.473s` total bridge-call time, but `21.839s` is record replay,
only `88ms` is import validation, `542ms` is handle/resource marking, and
`3.234s` is nested `submitDrawSubmissionBatch()` work. The next CPU split
therefore belongs inside `commit_chunk` replay - record dispatch, draw-run scan,
constant-upload pass-through, draw packet state application, batch construction,
and snapshot/payload lookup - not in bridge ABI tuning.

[state-churn-encode-encode-phase.25](state-churn-encode-encode-phase.25.md) names the first child inside that replay
bucket. A watchdog-finalized partial-log run still reached `1680` presents and
produced a normal-looking `actual.png`. `commit_chunk_replay_cpu_ms` was
`22.224s`; the dominant named child is queued draw submission
(`9.927s`), and its nested `snapshotDrawSubmissionFromCurrentState()` timer is
`7.697s`. The next tier is `submitDrawSubmissionBatch()` (`3.229s`) and
`drawPrimitiveRun()` (`2.094s`). Draw-run scan (`168ms`), draw-state apply
(`369ms`), draw-run build (`223ms`), constant upload record dispatch
(`152ms`), and final binding (`14ms`) are not first-order owners. The next CPU
work should therefore split/reduce snapshot cache lookup/miss hot-build,
uniform refresh/build/hash, payload lookup collisions, and the two draw submit
paths before changing scan heuristics.

[state-churn-encode-encode-phase.26](state-churn-encode-encode-phase.26.md) and
state-churn-encode-encode-phase.29 split the queued submit path far enough
to reject the simplest copy-volume hypothesis. The batch append parent remains
large (`2707.789ms` in the 120-second append-split run), but payload byte copy
is only `65.088ms` while copying `232.5MB`. The dominant children are full
draw-state append / PSO subview / invariant construction (`958.031ms`) and
per-submission uniform payload lookup/append (`901.830ms`). The average batch is
only `1.877` records per group, so one copied `CanonicalDrawState` and one
uniform lookup pass are poorly amortized. The next queue-side implementation
bet should therefore compact or intern the run state, add a uniform-handle
reuse fast path, or improve coalescing enough to raise records/group; do not
spend the next iteration optimizing raw payload arena byte copies.

state-churn-encode-encode-phase.30 closes the first state-append
implementation bet. `appendDrawState()` now consumes `CanonicalDrawState&&`, and
`appendDrawRunBatch()` moves the first submission state directly into the slot
instead of passing it through an extra local value and by-value parameter. That
cuts the batch append parent `2707.789ms -> 2116.270ms` and the state child
`958.031ms -> 720.274ms` over the same `1680` presents, while GPU time remains
flat. The next queue-side work should split the remaining state child before a
larger compact-state design, and should also revisit the now-largest child:
per-submission uniform lookup/append (`837.360ms`) with mostly unique payloads.

state-churn-encode-encode-phase.31 closes that split as attribution. The
extra nested timers are hot enough to add measurement overhead (`state/present`
`0.428735 -> 0.505263ms`), so the run is not a CPU win. The child distribution is
still decisive: `appendDrawState()` SoA storage is `707.490ms` (`80.47%` of the
state parent), while `makeDrawPsoSubview()` is only `50.156ms` and invariant
construction is `22.451ms`. The next implementation bet should therefore change
the stored state shape or amortization, not micro-optimize PSO subview or
run-invariant construction.

state-churn-encode-encode-phase.32 rejects the first stored-state
amortization attempt and refines the target. Slot-local full-state interning
found only `663` hits across `439,464` batch groups (`0.150866%`) and made the
state SoA child worse (`707.490ms -> 961.947ms`) because the fingerprint/probe
work ran after the queue had already copied and compared the large submission
states. The useful critique is upstream: `cachedBaseDrawStateForSubmissionBatch`
already proves stable-state identity by generation, while
`submitDrawRunBatch()` rediscovers compatibility through deep `FlatStateSet`
and shader-layout comparisons before storing only `submissions.front().state`.
The next non-mutating proof should stamp submissions with stable generation and
snapshot lane, then count adjacent same-generation/lane opportunities and their
agreement with the existing deep compare. Only after that counter is high should
the compat scan use a generation fast path or the snapshot code skip N-1
state/layout copies. Per-draw binding overrides and dynamic backing snapshots
must remain per-draw until resource-lifetime marking proves they can be safely
coalesced.

state-churn-encode-encode-phase.33 closes that non-mutating proof. In a
120s no-gputrace scout, `submitDrawRunBatch()` saw `733,221` adjacent compat
pairs; `385,120` pairs (`52.524409%`) had the same stable generation and
snapshot lane, and all `385,120` were compatible by the existing deep compare.
Same-generation/lane incompatible cases were `0`. This makes a guarded compat
scan fast path safe for the next implementation: return compatible immediately
for same generation/lane, otherwise keep the current deep compare. Its direct
target is the `557.621ms` compat-scan bucket, not the entire queued-submission
cost. The same counter also sizes a future copy-elision opportunity for
`385,120` non-front records, but state/layout copy elision must keep per-draw
uniforms, binding overrides, dynamic backing snapshots, and resource marking
correct.

state-churn-encode-encode-phase.34 applies the guarded compat fast path.
Same-generation/lane pairs now return compatible without the deep
`FlatStateSet` and shader-layout comparison; debug builds still assert that the
old deep comparison would agree. The 120s no-gputrace scout keeps a normal
visible GT1 frame with machine-gun muzzle flash/bloom, and cuts
`submit_draw_run_batch_compat_scan_cpu_ms` `557.621ms -> 44.923ms`
(`0.331917 -> 0.025818ms/present`). The parent
`commit_chunk_draw_batch_submit_cpu_ms` drops `3491.771ms -> 3070.200ms`
(`2.078435 -> 1.764483ms/present`). The larger
`commit_chunk_queue_draw_submission_cpu_ms` remains effectively dominated by
snapshot/state copy and append work, so the next CPU work should move to F2
in-place queue fill, persistent replay scratch, and then stronger state/layout
copy elision for same-generation non-front submissions.

state-churn-encode-encode-phase.35 closes the first F2 microfix. The replay
queue now fills `pendingDrawSubmissions.emplace_back()` directly instead of
constructing a stack `DrawRunSubmission` and `push_back(move)`-ing it. The run
is same-present versus phase.34 (`1740`), keeps a normal visible GT1 frame, and
cuts the queue residual after subtracting nested snapshot time
`2286.890ms -> 2193.875ms` (`1.314305 -> 1.260848ms/present`). The total
`commit_chunk_queue_draw_submission_cpu_ms` moves only `8873.818ms ->
8820.641ms` (`-0.60%`), so the old move was real waste but not the first-order
owner. Continue with persistent replay scratch only as an isolated allocation
test; the major remaining work is still snapshot/cache lookup, state/layout
copy width, and append/state storage.

state-churn-encode-encode-phase.36 rejects the remaining F2 persistent
scratch bet for the current path. A temporary `D9CDevice`-owned
`pendingDrawSubmissions` vector reused capacity across `commit_chunk` calls and
kept a release fallback for reentrancy, but it did not reduce the residual
after subtracting nested snapshot time (`2193.875ms -> 2200.657ms`). The total
queue timer moved with snapshot noise, while draw-batch submit and append
slightly worsened. The scratch branch was removed. This closes F2 allocation
churn as a near-term owner; the next queue work should target snapshot/cache
lookup, state/layout copy width, and same-generation copy elision rather than
per-chunk vector capacity.

state-churn-encode-encode-phase.37 accepts the narrower F6 hygiene fix but
not as a first-order owner. `submitDrawRun()` and `submitDrawRunBatch()` no
longer create local `DrawBindingSnapshot` / `DrawParamPayloadView` vectors under
the submit hot path; they reuse thread-local scratch so payload spans remain
valid even if queue chunk commit temporarily releases the queue mutex. The
batch binding-snapshot bucket drops `0.119755 -> 0.089566ms/present`, and the
non-batch snapshot bucket drops `0.054349 -> 0.043882ms/present`. The broader
`commit_chunk_replay_cpu_ms` and `commit_chunk_queue_draw_submission_cpu_ms`
stay flat within run noise on a per-present basis, so this closes F6 as perf
hygiene and leaves the large owners unchanged: snapshot/cache lookup,
uniform/hash work, state/layout copy width, and same-generation copy elision.

state-churn-encode-encode-phase.38 accepts the first F3 ownership fix.
`ShaderBytecode` no longer owns a mutable `std::vector<u8>` by value inside
copied draw-state/layout records; it now carries shared immutable byte storage,
so cache hits, snapshot submissions, and per-draw shader-layout overrides do
not allocate and memcpy VS/PS bytecode payloads. The 120s no-gputrace scout
keeps a normal visible GT1 frame and cuts the targeted copy-shaped buckets:
`d3d9_snapshot_state_copy_cpu_ms` `0.421921 -> 0.271225ms/present`,
`d3d9_snapshot_cache_miss_shader_layout_cpu_ms`
`0.532211 -> 0.409452ms/present`, and
`commit_chunk_queue_draw_submission_cpu_ms`
`5.176033 -> 4.792983ms/present`. This confirms the review's bytecode-copy
critique, but it does not remove the remaining fixed-width `FlatDrawStateRecord`
SoA push or N-1 same-generation state/layout copies; those remain separate F1/F4
design work.

state-churn-encode-encode-phase.39 accepts the first F4 width fix. Render
state capacity stays `256` because D3DRS ids reach `209`, and texture-stage
capacity stays `64` because dxmt9 uses internal `TSS_TEXTURE_TYPE=63`. Sampler
states are different: public D3DSAMP ordinals are `1..13`, so
`kMaxSamplerStates` and the PE sampler shadow can shrink from `64` to `16`
without changing identity mapping. This cuts `FlatStateSet<sampler>` from
`536B` to `152B`, `FlatDrawStateRecord` from `18,736B` to `11,056B`,
`CanonicalDrawState` from `21,080B` to `13,384B`, and `DrawRunSubmission` from
`31,744B` to `24,064B`. In the rebuilt-unix 120s scout, per-present
`d3d9_snapshot_state_copy_cpu_ms` drops `0.271225 -> 0.181239`,
`submit_draw_run_batch_append_state_soa_cpu_ms` drops
`0.387479 -> 0.263628`, and
`commit_chunk_queue_draw_submission_cpu_ms` drops
`4.792983 -> 4.205132`. The visible smoke frame is normal. This validates
sampler-state compaction as a real CPU win, but the larger F4 render-state/TSS
compaction still requires sparse id remapping or a different compact record
shape.

state-churn-encode-encode-phase.40 refines that last sentence: the
DeviceState texture-stage id space must stay `64`, but the copied
`FlatDrawStateRecord` active-entry set can use a separate capacity. PE validates
public `D3DTSS_*` ids to `18` possible entries and dxmt9 adds one internal
`TSS_TEXTURE_TYPE=63`, so `kMaxFlatTextureStageStates=32` keeps a conservative
active-entry bound without remapping keys. The structural width drops
`FlatDrawStateRecord` `11,056 -> 9,008B`, `CanonicalDrawState`
`13,384 -> 11,336B`, and `DrawRunSubmission` `24,064 -> 22,016B`. The primary
same-present scout shows targeted copy wins:
`d3d9_snapshot_state_copy_cpu_ms` `0.181239 -> 0.158232ms/present`,
`submit_draw_run_batch_append_state_cpu_ms`
`0.367501 -> 0.323385ms/present`, and
`submit_draw_run_batch_append_state_soa_cpu_ms`
`0.263628 -> 0.220626ms/present`. Broader encode remains mixed
(`encode_draw_cpu_ms` `+1.55%` per present), so this is accepted as a
state-width reduction, not a GPU/encode-bottleneck fix. The next F4 target,
render-state compaction, still needs a sparse/remapped record or supported-ID
proof before reducing any capacity.

state-churn-encode-encode-phase.41 adds that proof layer without changing
render-state storage. New snapshot flat-state counters show the weighted GT1
submission path has `883,062` samples, `render_state_entries_max=62`,
`render_state_entries_gt64=0`, and no render/TSS/sampler overflow. TSS and
sampler caps are additionally validated by `tss_stage_entries_max=11` and
`sampler_slot_entries_max=9`. This proves `FlatStateSet<64>` would fit current
GT1, but it does not yet justify changing the default: `DeviceState::reset()`
already initializes `62` render-state entries, leaving only two spare entries
for arbitrary `SetRenderState()` ids. Render-state compaction therefore remains
either a wider active-entry cap with broader app evidence (`96`/`128`) or a
sparse/remapped record that stores encoder-consumed states separately from the
full compatibility digest.

[state-churn-encode-encode-phase.42](state-churn-encode-encode-phase.42.md) implements the conservative variant of
that design: `DeviceState` keeps the full `256` render-state id space, while the
copied `FlatDrawStateRecord::renderStates` payload becomes a `128`-entry
priority active set. Backend-consumed high-id states are appended first,
remaining ids fill spare slots, and the full render-state hash remains the
compatibility/debug digest. The structural width drops `FlatDrawStateRecord`
`9,008 -> 7,984B`, `CanonicalDrawState` `11,336 -> 10,312B`, and
`DrawRunSubmission` `22,016 -> 20,992B`. The 120s GT1 scout stays visually
normal and reports `render_state_entries_max=62`, `gt128=0`, and overflow `0`.
This is accepted as another bounded state-width reduction: direct state-copy and
state-SoA children improve, but broader queue/snapshot and GPU totals remain
run-variance dominated.

[state-churn-encode-encode-phase.43](state-churn-encode-encode-phase.43.md) accepts a smaller resource-retention
cleanup from the F1 critique. `appendDrawRunBatch()` stores one front
`CanonicalDrawState` plus N draw params, so `CommandQueue::submitDrawRunBatch`
now marks `batch.front().state.hot` once and keeps only binding
override/snapshot payload marking per submission. The companion test update
records the current contract for single imported batch submissions: base
stream/index hot fields are binding-agnostic, and effective bindings live in
`DrawBindingOverride` payloads. The 120s no-gputrace scout remains visually
normal and moves the intended bucket:
`submit_draw_run_batch_resource_mark_cpu_ms` `27.146 -> 24.739` (`-8.87%`).
Broader `submit_draw_cpu_ms` drops `-1.87%`, while `gpu_command_buffer_time_ms`
is flat (`+0.32%`) and completion wait worsens (`+2.00%`), so this is a
targeted CPU micro-win, not a GPU bottleneck fix.

[state-churn-encode-encode-phase.44](state-churn-encode-encode-phase.44.md) implements the F1 N-1 state-copy elision
behind `DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1`. Same-stamp continuations now skip
the `state.hot` / `shaderLayout` cache copy, and the queue groups by stamp only
so elided non-front state is never deep-compared. The 2026-06-14 no-gputrace
GT1 A/B proves the intended mechanism (`400,838` elisions, `4.10GB` elided
state bytes, `d3d9_snapshot_state_copy_cpu_ms` `258.969 -> 139.672`), but it is
not an FPS lever by itself: sampled mean FPS is flat (`18.435 -> 18.403`) and
GPU/completion wait move slightly the wrong way. At this point the strict
stamp-only mode stayed diagnostic; phase48 later promotes only the safe
same-stamp copy-elision subset while preserving normal compatibility grouping.

[state-churn-encode-encode-phase.45](state-churn-encode-encode-phase.45.md) adds the next split before that larger
carrier refactor: `commit_chunk_queue_draw_submission_emplace_cpu_ms` times only
`submissions.emplace_back()` in the primitive and indexed queued-submission
paths. The 120s GT1 scout accepts this as a bounded target:
`commit_chunk_queue_draw_submission_emplace_cpu_ms=1,123.253`, `13.99%` of
queued-submission CPU and `0.646ms/present`, with a normal output frame. This
does not repeat the rejected phase36 capacity-reuse branch and does not explain
the remaining FPS limit by itself; it only justifies an optional-state or
direct-construct carrier experiment if we want the next CPU cleanup.

[state-churn-encode-encode-phase.46](state-churn-encode-encode-phase.46.md) implements that cleanup without changing
vector ownership: `DrawRunSubmission::state` and `uniforms` become optional
storage, materialized only by `snapshotDrawSubmissionFromCurrentState()`. This
slightly increases `DrawRunSubmission` width (`20,992 -> 21,008B`) but avoids
default construction for the large carrier fields. The no-gputrace A/B moves the
target bucket directly: `commit_chunk_queue_draw_submission_emplace_cpu_ms`
`1,123.253 -> 573.056` (`-48.98%`), with queued-submission CPU down `-2.04%`.
Output stayed visually normal. Broader FPS movement is small (`+0.64%`) and
should be treated as supportive but not a final average-FPS fix. The after-run
residual also closes the carrier branch for now: queue submission is still
`4.522ms/present`, but snapshot accounts for `3.900ms/present`, emplace accounts
for `0.329ms/present`, and the remainder is only `0.292ms/present`.
`d3d9_snapshot_cache_lookup_cpu_ms` remains `3.343ms/present`, so the next CPU
owner is snapshot/cache lookup or backend encode cadence, not more
queued-carrier construction.

[state-churn-encode-encode-phase.47](state-churn-encode-encode-phase.47.md) then adds a non-mutating counter for the
copy-policy frontier that remains after optional carrier storage. In the default
path, `submit_draw_run_batch_discarded_state_records=411,362` and
`submit_draw_run_batch_discarded_state_bytes=4,209,055,984`, which is `46.70%`
of batch records and materialized state bytes. This independently matches the
phase44 opt-in elision magnitude while leaving runtime behavior unchanged. The
next state-copy implementation should therefore target direct construction into
queue-owned storage or an interned compact draw-state carrier for this
materialized-but-discarded non-front class. It should not keep digging in
queued-carrier default construction unless a new counter names another child.

[state-churn-encode-encode-phase.48](state-churn-encode-encode-phase.48.md) promotes the safe subset of that target:
same-stamp non-front submissions now elide their canonical state copy in the
default binding-agnostic snapshot path, while the queue keeps the normal
compatibility policy and accepts elided candidates through the previously
accepted draw when needed. The 120s GT1 scout removes the phase47 waste
directly: `submit_draw_run_batch_discarded_state_records` `411,362 -> 0`,
`d3d9_snapshot_state_elided=412,180`, and `d3d9_snapshot_state_copy_cpu_ms`
`261.001 -> 138.856` (`-46.80%`). Broader queue submission improves
`7,463.771 -> 7,023.458ms` (`-5.90%`), the output frame remains visually
normal, and pipeline skips/errors stay zero. This is accepted as a default
copy-policy cleanup, not as the average-FPS owner: GPU command-buffer time and
completion wait stay flat/noisy. `DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1` remains a
strict stamp-only batching diagnostic, not the required default elision gate.

state-churn-encode-encode-phase.49 then rejects the next obvious
binding-packet cache idea. A 2-way set cache does remove direct-map misses and
collisions (`189,178/189,050 -> 132,947/132,819`), but the extra probe,
promotion, shift, and larger packet-store work costs more than the saved misses:
`encode_draw_binding_packet_cache_cpu_ms` regresses `706.875 -> 816.355` and
the parent `encode_draw_binding_packet_cpu_ms` regresses `1,835.316 ->
1,939.451`. The candidate code was reverted; do not chase cache associativity
again unless a new plan reduces the packet identity/probe width rather than
retaining more full packet entries.

state-churn-encode-encode-phase.50 accepts the indexed draw default fast
path as a local cleanup. With encoder breakdown, index-cache/reorder/split, dump,
and stream/IB staging all off, the encoder no longer prepares reusable CPU index
byte spans just to skip every diagnostic path. This cuts
`encode_draw_index_setup_cpu_ms` `636.514 -> 342.602` and the index phase
`775.311 -> 480.350`, but total `encode_draw_cpu_ms` moves only
`16,023.609 -> 15,906.915` and sampled FPS/GPU/completion remain flat/noisy.
Treat the default diagnostic-byte-span class as closed; remaining index work
needs a new counter-named owner rather than another broad pass through the same
block.

state-churn-encode-encode-phase.51 rejects the simple
`DrawUniformPayload` lookup prereserve cleanup. `appendDrawRunBatch()` already
reserves uniform storage/lookup for the whole batch, so the probe skipped the
inner `reserveDrawUniformPayloadLookup()` call on the miss append path. The run
was visually normal, but normalized counters do not support the change:
`submit_draw_run_batch_append_uniform_cpu_ms / present` is effectively flat
(`0.474488 -> 0.474157ms`) and the parent append bucket regresses
(`1.047767 -> 1.069220ms`). The code was reverted. If the uniform-append child
is pursued again, split lookup bucket walk, payload equality compare, payload
copy, and lookup linking first; do not re-try the reserve-check shortcut.

[state-churn-encode-encode-phase.52](state-churn-encode-encode-phase.52.md) performs that split as attribution-only
instrumentation. It confirms the reserve shortcut was the wrong lever:
`draw_uniform_payload_append_reserve_cpu_ms` is only `53.018ms`, while
`draw_uniform_payload_append_copy_cpu_ms` is `813.196ms`. Lookup remains visible
(`294.215ms`, with `154.102ms` in bucket chains), but the miss path is dominated
by materializing the owned `DrawUniformPayloadRecord`. Future work on this child
should target fewer/narrower payload copies or a proven lifetime-safe interned
payload representation, not another reserve/linking micro-optimization.

state-churn-encode-encode-phase.53 closes the easy copy-construction part of
that bucket. Constructing `DrawUniformPayloadRecord` in-place avoids the
aggregate temporary on append miss and cuts
`draw_uniform_payload_append_copy_cpu_ms` `813.196 -> 602.274ms`; the
append-uniform parent follows `1299.014 -> 1128.212ms`. The run is visually
normal and GPU/completion/FPS stay flat/noisy, so this is a local submit-side
cleanup, not the frame-rate owner. Any next uniform-payload change needs a
storage-shape proof: fewer append misses, component interning, or compact owned
payload storage.

state-churn-encode-encode-phase.54 rejects a simple prefetched-PSO resolve
cache as the next encode target. The transient cache saw `152,261` same-handle
hits, but `encode_draw_pipeline_lookup_cpu_ms` moved the wrong way
(`934.420 -> 950.626ms`) while FPS/GPU/completion stayed flat. The experiment
code was reverted. Keep `pipeline_lookup` below the larger `argbuf_setup`,
`stream_bind`, `binding_packet`, snapshot/replay, and completion-pacing buckets
unless a split counter first names a larger subchild. `issue` is now split in
state-churn-encode-encode-phase.60 and reads as per-draw Metal indexed
draw-call cost.

state-churn-encode-encode-phase.55 then fixes the argbuf-open attribution.
The legacy `encode_draw_argbuf_open_cpu_ms` counter is a per-draw reopen-block
parent, not just the `openArgbuf()` call. In the 120s GT1 scout,
`open_cpu_ms=1625.608`, actual `openArgbuf()` is `573.804ms`, and post-open
table/cache bookkeeping is larger at `891.359ms`. The live post-open children
are table bind (`178.803ms`), cached cbuf repoint (`269.898ms`), no-dirty
component probe (`123.303ms`), and an inferred `~319ms` residual; the full-cbuf
repoint branch is dead for GT1 (`0`). This is attribution-only and does not
move FPS/GPU/completion. Future argbuf work should avoid treating `openArgbuf()`
as the single owner; reduce reopen frequency only with a pre-open component
identity proof that preserves per-draw table lifetime, or split the post-open
residual further before optimizing it.

state-churn-encode-encode-phase.56 rejects that pre-open whole-table skip
for GT1. A temporary default-off gate checked only constants-only argbuf draws
where the previous slot-30 table shadow was still valid, the cbuf cache was
complete, no cbuf dirty bits were pending, and VS/PS/FFPVS/FFPPS identities all
matched. It reached `961,473` candidates but skipped `0`: VS identity misses
dominated (`812,520`), followed by PS (`310,696`) and FFPPS (`33,233`). The
check cost `956.102ms` when enabled and regressed `encode_draw_cpu_ms` by
`+4.46%`, while FPS/GPU/completion stayed noisy-flat. The temporary code was
removed after the run. Do not pursue whole-table argbuf reuse as the next GT1
lever; either split the phase55 post-open residual further or prove that the VS
identity misses are false misses before touching this area again.

state-churn-encode-encode-phase.57 performs that residual split. It adds
attribution-only timers inside `encode_draw_argbuf_reopen_post_cpu_ms` and keeps
behavior unchanged. The run passes with a normal machine-gun muzzle-bloom frame,
`draw_skipped_no_pipeline=0`, and `gpu_command_buffer_errors=0`. The added
hot-path timers are not free (`encode_draw_us_per_draw` rises
`11.926 -> 12.833us`), so parent movement is measurement overhead rather than a
runtime regression. The useful signal is internal distribution: table probe
(`50.933ms`), byte accounting (`51.990ms`), cbuf cache/dirty scans
(`60.990 + 57.823ms`), and force-dirty writes (`104.757ms`) explain the old
phase55 residual as distributed bookkeeping. `table_shadow_store=48.420ms` is a
sub-slice of the table-bind parent. Do not chase a single hidden post-open API
child; the next argbuf work must either reduce reopen frequency with a
correctness proof or consolidate the required cbuf decision/repoint control
flow.

state-churn-encode-encode-phase.58 performs the same attribution-only split
for the binding-packet plan parent. The useful signal is negative: fragment
texture/sampler planning is the largest named child (`356.146ms`,
`0.204682ms/present`, `48.3%` of named children), but the absolute size is
smaller than the larger default owners such as argbuf setup, stream bind,
binding-packet cache/probe, snapshot/replay, and present under-pipelining. The
child timers are explicitly opt-in via
`DXMT9_PERF_BINDING_PACKET_PLAN_SPLIT=1`; the default-off validation keeps the
child counters at `0` and the parent near baseline (`0.314560ms/present`).
Do not make binding-packet plan reuse the next primary FPS bet unless a future
patch can reuse the fragment plan without a per-entry check that recreates the
texture pre-resolve regression from state-churn-encode-encode-phase.16.

state-churn-encode-encode-phase.59 closes the adjacent-uniform follow-up on
the backend append side. Since snapshot-cache-snapshot.20 reports `0`
adjacent same-`uniformGeneration` submissions, the next question was whether the
slot-local `DrawUniformPayload` dedup table is doing useful work at all. The
diagnostic `DXMT9_DISABLE_DRAW_UNIFORM_PAYLOAD_DEDUP=1` skips lookup/reserve/link
and appends every materialized payload. In a same-present no-gputrace run,
lookup drops `276.107 -> 0ms` and append-copy rises `574.305 -> 629.058ms`;
the targeted append-uniform parent improves `1041.108 -> 799.528ms`
(`0.619707 -> 0.475910ms/present`). But
`commit_chunk_queue_draw_submission_cpu_ms` is flat (`6813.183 -> 6817.526ms`)
and completion/GPU movement is noise, so this is a diagnostic micro-win, not a
new FPS owner. Keep the env for A/Bs; do not promote a default policy change
without repeated runs or a memory-pressure check.

state-churn-encode-encode-phase.60 closes the `encode_draw_issue_cpu_ms`
owner as attribution. The opt-in `DXMT9_PERF_DRAW_ISSUE_SPLIT=1` run is
heavy (`present_encoded=1620` instead of the adjacent `1680`), so it is not an
A/B performance result. The internal signal is still decisive: GT1 issue work
is entirely indexed (`draw_indexed=draw_calls=1,199,600`), with
non-indexed, expanded-indexed, split-indexed, and visibility-scout children all
`0`. The Metal draw-call child is `897.049ms`, which is `77.0%` of the issue
parent and `86.9%` of the indexed-path child. Do not treat issue as a hidden
dxmt9 wrapper bucket; moving it materially requires fewer submitted Metal draws
or a different submission model, not micro-optimizing the call wrapper.

state-churn-encode-encode-phase.61 closes the next argbuf cbuf attribution
question as a negative primary lever. The opt-in
`DXMT9_PERF_ARGBUF_CBUF_PROBE_SPLIT=1` run adds nested hot-path timers, so the
parent bucket movement is instrumentation overhead. The useful stage split says
FFPPS repoint has the largest byte/call count (`899,453` calls,
`345.390MB`) but only `137.306ms`; PS repoint is `108.299ms`; and VS repoint is
only `28.678ms`. VS content probing has a low hit rate
(`143,728 / 931,743`) and costs `83.048ms`, but skipping it would convert those
hits into more VS uploads. Dirty VS update remains larger
(`936.123ms`) than any probe/repoint child. Do not pursue a default "skip VS
probe" or FFPPS repoint micro-optimization without a stronger A/B; the next
argbuf work should reduce table reopen frequency, dirty VS upload frequency, or
the table storage model itself.

state-churn-encode-encode-phase.62 refreshes the dirty VS cbuf question
against the current post-phase61 code and closes the local cached-identity skip
variant. The opt-in `DXMT9_PERF_ARGBUF_CBUF_DIRTY_IDENTITY=1` run probes every
dirty VS cbuf update and sees `808,845` probe calls, `0` hits, `788,347`
misses, and `20,498` no-cache rows. The no-cache count matches
`render_pass_begin`, so those are the expected first writes that seed each
encoder-local cache; every cached dirty VS update is a real identity miss. Do
not add a dirty-mirror cached-repoint fast path for GT1 without a new upstream
semantic change. The remaining argbuf work is now specifically table reopen
frequency, cheaper per-draw VS constant storage, or reducing upstream VS dirty
frequency.

state-churn-encode-encode-phase.63 closes the broad-payload-hash variant of
the argbuf reopen question. The opt-in
`DXMT9_PERF_ARGBUF_PAYLOAD_DELTA=1` probe shows `payload_changed=931,917`,
exactly matching `encode_draw_argbuf_cbuf_reopen_no_dirty_hash_mismatch`, while
`payload_same=330,687` exactly matches the clean-skip rows and first draws
match `render_pass_begin=20,475`. Most importantly, `changed_nonconst_only=0`:
all changed-payload reopens are explained by VS and/or PS constant component
hash movement (`624,768` VS-only, `144,058` PS-only, `163,091` both). Do not
try to save GT1 argbuf reopens by replacing the full payload hash with only
shader-constant component hashes; the current workload is already shader-
constant driven. The remaining choices are upstream constant churn reduction,
cheaper changed-constant cbuf storage, or a table model that avoids per-change
reopen side effects without reusing mutable table contents unsafely.

state-churn-encode-encode-phase.64 then rejects the narrower "dirty range
is still too large" explanation for dirty VS cbuf width. In a scoped frame60
encoder-breakdown run, VS uploads average `984.712` bytes, but the dirty
high-water range averages only `0.702` float regs per upload. The width is
instead dominated by shader usage and fallback shape: usage averages `45.147`
float regs, the final plan averages `57.483` regs, and indexed-float access
forces full-struct uploads for `20.21%` of frame60 VS uploads. The current
prefix-preserving builder is already using the safe range trim available to
the MSL-visible `VsConsts` ABI. Further large cbuf wins need upstream constant
churn reduction, persistent/segmented constant storage, or shader-specific
packed constant layouts rather than another dirty-range micro-trim.
state-churn-encode-encode-phase.65 lowers that last packed-constant branch:
the exact bytecode scan finds no safe non-indexed packed savings in frame60.
The theoretical gap is large only for indexed VS shaders (`59` draws matching
the `59` full/indexed VS uploads). Those rows are not arbitrary indexed
constant access: every draw-weighted indexed VS row uses static offsets
`0;1;2` with relative sources `a0.x/a0.y`, the matrix-palette skinning shape.
Packing is still illegal until a per-draw or per-resource vertex BLENDINDICES
range proves the runtime `a0` window and the translator/ABI can rewrite
`c[a0+n]` safely. The current cbuf direction therefore stays on upstream
constant churn, segmented/persistent storage, or a hard indexed-window proof
rather than a generic non-indexed packed layout.
state-churn-encode-encode-phase.66 then probes that indexed-window branch
with real geometry payloads for the hottest indexed VS. The bytecode side is
indeed matrix-palette skinning (`BLENDINDICES` UBYTE4 at stream0 offset `12`,
stride `24`), but the sampled draw set contains one full-range draw:
`a0.x=0..255`, `a0.y=0..254`, making the required `c[a0 + 0..2]` window
`0..257`. That rejects packed indexed VS constants as a current broad safe
target. The cbuf track should now focus on reducing constant-change frequency,
argbuf table reopen frequency, or storage shape that preserves full indexed
access semantics.
state-churn-encode-encode-phase.67 converts that storage-model question into
a direct Stage2-vs-Stage1 scout. With `DXMT9_DISABLE_ARGBUF_HYBRID=1`, the same
120s GT1 shape (`1740` presents and `1.285M` draws) drops
`encode_draw_cpu_ms` from `17,399.519` to `12,847.687` and removes the
`4,322.402ms` argbuf setup bucket plus `846.5MB` of transient traffic. Direct
uniform build and pipeline lookup grow, and completion wait rises, so this is
not an immediate default flip. It is a stronger architectural signal that the
current Stage 2 constants-only argbuf table model is CPU-negative for GT1 unless
it gets a cheaper immutable-per-draw table or persistent/segmented cbuf storage
strategy.
state-churn-encode-encode-phase.68 runs the required low-overhead FPS gate
for that same policy question. Disabling Stage 2 still removes encode work
(warm `encode_draw_cpu_ms` p50 `8.621 -> 5.545ms` and warm
`encode_chunk_cpu_ms` p50 `10.453 -> 7.326ms`), but completion wait grows in the
same window (`27.409 -> 30.010ms` p50, `40.548 -> 45.961ms` p95). Warm FPS p50
moves only `17.202 -> 17.323`, and tail-600 FPS p50 is effectively flat
(`16.855 -> 16.817`). This rejects an argbuf-hybrid default flip as the current
average-FPS lever. The Stage 2 table model remains a CPU/storage cleanup target,
but the next FPS proof must move completion overlap, producer cadence, or earlier
PE/unix publish rather than only local encode CPU.
The current dirty VS identity recheck
([state-churn-encode-encode-phase.123](state-churn-encode-encode-phase.123.md)) keeps the same conclusion after the
compact uniform-storage work: `862,747` dirty VS candidates produced `0`
cache-identity hits and `992.154MB` of miss bytes. Treat the dirty VS cbuf lane
as real source churn unless a future upstream constant-generation probe proves
otherwise.
[state-churn-encode-encode-phase.127](state-churn-encode-encode-phase.127.md) removes the queue-observation legacy
uniform materialization site identified in phase 126 by carrying
`nonIdentityTextureTransformStageMask` in compact hot state. The runtime proof is
clean: `uniform_backend_materialize_queue_observation_*` is now `0`, while
`FlatDrawStateRecord` remains `7,984B` and queue diagnostics still report
projected compatibility from hot state. This is accepted as a local cleanup, not
an FPS owner: the remaining materialization is draw-encoder command/param work
and the run is still no-enqueue dominated
(`completion_wait_without_enqueue_ms_per_present=25.390`,
`completion_wait_overlap_share=0.000%`).
state-churn-encode-encode-phase.128 then rejects the adjacent lazy-lookup
idea for the remaining `DrawEncoderCommand` site. A temporary local patch delayed
command uniform materialization until the draw loop proved that a `DrawParam`
used the run-level uniform, but the 120s scout stayed effectively flat:
`draw_uniform_payload_materialized_draw_encoder_command/present` moved only `323.548 -> 323.233`.
That means the command site is not mostly pass-open observer waste; GT1's
draw-runs normally consume the command uniform in the draw loop. Further
uniform-storage work must change the consumer shape, such as direct compact
fixed/stage cbuf builders, rather than only delaying the lookup.
state-churn-encode-encode-phase.130 refreshes the current low-overhead owner
split after the cbuf content observer was made opt-in. The queue-observation
site remains closed (`0` calls), backend legacy scratch is now only
draw-encoder command/param work (`0.228ms/present`), and the larger encode
owners are `argbuf_setup=1.838ms/present`, `cbuf_update=0.963ms/present`, and
`argbuf_open=0.733ms/present`. The P4 shape remains under-pipelined
(`completion_wait_without_enqueue_ms_per_present=26.586`), so the next FPS
candidate should be argbuf dirty/open frequency, replay/snapshot, or overlap
rather than another uniform materialization-site pass.
state-churn-encode-encode-phase.131 then rechecks the most obvious argbuf
open-frequency shortcut on the current code. `DXMT9_PERF_ARGBUF_PAYLOAD_DELTA=1`
still reports `changed_nonconst_only=0`: every payload-change reopen is explained
by VS and/or PS constant-source hash movement (`667,298` VS-only, `155,386`
PS-only, `172,413` both). Replacing the full payload hash with only cbuf-source
hashes would therefore save no GT1 reopens. The remaining argbuf work is real
VS constant churn, persistent/segmented cbuf storage, or a table model that can
change cbuf pointers without reopening mutable argbuf table state.
[state-churn-encode-encode-phase.134](state-churn-encode-encode-phase.134.md) adds the missing attribution surface for
that upstream-churn branch and then runs it. The existing payload-delta probe now
has opt-in VS/PS float/int/bool changed counters, and the current 120s scout
reports `changed_vs=843,136 == changed_vs_float`, `changed_ps=328,826 ==
changed_ps_float`, and all int/bool changed counters at `0`. That closes the
int/bool invalidation shortcut for current GT1: the remaining Stage 2 argbuf
churn is float-constant source turnover, led by VS.
[state-churn-encode-encode-phase.135](state-churn-encode-encode-phase.135.md) then measures the width of that float
turnover. VS float changes average only `13.665` changed float4 registers per
VS-float-changed draw, but dirty VS cbuf uploads still average about
`959.2B/update`, or about `4.50x` the observed changed-register bytes. That
keeps storage width amplification alive, but the `256`-register max means the
next no-gputrace probe should histogram the tail before implementing segmented
cbuf storage.
[state-churn-encode-encode-phase.132](state-churn-encode-encode-phase.132.md) and
[state-churn-encode-encode-phase.133](state-churn-encode-encode-phase.133.md) close the unsafe table-sharing branch.
The current constants-only Stage 2 table cannot be shared across changed cbuf
pointers because the GPU reads the mutable descriptor table later and would see
last-write-wins. Moving cbufs out of the table is also not a host-only patch:
programmable shaders, FFP shaders, and tile FFP all receive `ArgbufLayout` at
slot 30, so a direct-cbuf Stage 2b needs its own shader/PSO ABI and tests. Rank
that behind upstream VS constant-churn reduction unless the ABI work is taken as
an explicit structural experiment.
[state-churn-encode-encode-phase.143](state-churn-encode-encode-phase.143.md) closes that ABI precondition without
turning on the runtime path: Stage 2b now has a separate PSO key bit and
source-contract tests for FFP, programmable, tile-FFP, and direct host slots.
[state-churn-encode-encode-phase.144](state-churn-encode-encode-phase.144.md) then adds that bounded runtime scout
behind `DXMT9_ARGBUF_DIRECT_CBUF=1`. It removes the entire constants-only
argbuf table/open/cbuf-update counter set in a normal visual run, but the
summary still reports `under-pipelined-no-enqueue` with
`completion_wait_without_enqueue_ms_per_present=28.565` and
`sampled_avg_fps=16.864`. Treat Stage 2b as a successful local cleanup and
mechanism proof, not the current average-FPS owner. The next proof must move
P4/P2/P3 cadence or producer overlap, with stream-bind, PSO-prefetch, and
binding-packet now the larger remaining encode children after argbuf removal.
state-churn-encode-encode-phase.145 keeps this classification: removing
redundant full `DrawUniformPayload` zero-fill before compact materialization
cuts the local encode parent slightly (`8.580 -> 8.426ms/present`) in a normal
visual run, but sampled FPS remains noise-flat and no-enqueue completion wait
still dominates.
state-churn-encode-encode-phase.156 then wires compact uniform submissions
into the producer path behind `DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1`. It
proves the storage mechanism and cuts measured materialized uniform bytes to the
compact candidate (`10264B -> ~2922B` per materialized submission), but the
current scratch carrier is not a CPU win: queue draw submission, snapshot, and
append-uniform CPU all regress per present, and the no-gputrace FPS sample is
worse. Treat compact producer submission as a default-off design checkpoint; the
follow-up should first prove whether fixed-payload deduplication removes enough
scratch-copy overhead before another promotion gate.
state-churn-encode-encode-phase.157 runs that first follow-up. Last
fixed-payload reuse is real (`763,709` reuses versus `94,889` appends) and
recovers much of H156's normalized CPU regression (`commit_chunk_replay`
`9.323 -> 8.307ms/present`, queue submission `4.461 -> 3.982ms/present`), but
the opt-in path still trails the `v0.0.3` visual-safe baseline on replay,
queue submission, and `sampled_avg_fps` (`16.377` versus baseline `16.832`).
Keep
`DXMT9_ENABLE_COMPACT_UNIFORM_SUBMISSIONS=1` default-off; the next compact
uniform attempt needs direct compact construction from the uniform builder,
arena sizing, or a smaller submission carrier rather than more fixed-payload
dedup inside the same scratch shape.
state-churn-encode-encode-phase.158 adds diagnostic timers for that compact
path and shows why arena sizing alone is unlikely to be the next main lever:
fixed-payload construction/equality is the largest measured compact child
(`0.207ms/present`) and is larger than VS+PS stage byte copying combined
(`0.113ms/present`). The timers add overhead and make the run slower, so treat
H158 as attribution only. The next compact-uniform implementation should either
skip the fixed projection/equality on proven adjacent fixed-hash reuse or build
the compact snapshot directly from the uniform builder with a smaller
submission carrier.
state-churn-encode-encode-phase.162 adds explicit carrier-footprint counters
for that last clause. The counters show that compact uniform submissions change
the logical uniform bytes, but not the queued draw carrier width: baseline and
compact runs both report `21,176B/record`, with `10,272B/record` still reserved
for full-uniform storage. This makes further compact append/storage polishing a
secondary target unless it is paired with direct compact construction or a
smaller `DrawRunSubmission` carrier.
state-churn-encode-encode-phase.164 adds the missing distinction between
"reserved" and "used" full-uniform carrier storage. The compact opt-in run
sets `submission.uniforms` empty for every queued submission, but the fixed
record still reserves `10,272B/record`; the new counter reports that as
`4.826MiB/present` of unused full-uniform lane. This turns the next compact
attempt into a carrier-shape problem, not a copy-loop polishing problem.
state-churn-encode-encode-phase.165 then implements that carrier-shape
split with a compact-only queued submission. The compact path removes the full
uniform lane from the queued record (`21,176 -> 10,904B/record`,
`10,272 -> 0B/record` full-uniform storage) while preserving broad no-gputrace
smoke (`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`). It is
still not a promotion candidate: the path builds through a temporary full
snapshot and then converts to the compact carrier, so queue/snapshot CPU remain
slightly worse and P4 no-enqueue wait does not move. The next compact-uniform
work should construct compact payloads directly from the uniform builder, or
defer compact work behind the larger P4/replay-publish owner.
state-churn-encode-encode-phase.166 removes the temporary full-submission
bridge and fills `DrawRunCompactSubmission` directly. That is a cleaner carrier
implementation and trims the compact-vs-compact queue-submission row slightly
(`4.025 -> 3.927ms/present`), but it does not change the larger conclusion:
the compact path still relies on `cached.uniforms` as the source of truth,
snapshot CPU remains worse than default, sampled FPS does not improve, and
`encode_ready_depth_avg` remains `1.000`. This closes the carrier-shell cleanup
thread. The next compact proof must change the uniform cache representation
itself; otherwise prioritize P4/replay-publish cadence.
[state-churn-encode-encode-phase.167](state-churn-encode-encode-phase.167.md) checks whether the default-on
resource-shape PSO memo could explain the post-`v0.0.3` black/translucent
visual reports. A temporary validation path resolved the canonical draw
`probeKey` for every shape hit before reuse and found `161,025` validated hits,
`0` validated misses, and all mismatch buckets at `0`. The gross visual smoke
was normal, but this is not a same-frame visual proof. Because the validation
raises `draw_key_resolve` to `1.104ms/present` and the prefetch parent to
`1.872ms/present`, it was reverted and should not become default. Reproduce the
artifact with `DXMT9_DISABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1` before
blaming this memo again.
[state-churn-encode-encode-phase.168](state-churn-encode-encode-phase.168.md) runs that opt-out A/B and closes the
loop. With `DXMT9_DISABLE_ENCODE_SLOT_PSO_RESOURCE_SHAPE_MEMO=1`, all
resource-shape memo counters drop to `0`, probe-key memo hits rise to `169,745`,
and the same broad no-enqueue cadence remains. The h190 screenshot still shows
the then-suspected dark foreground/silhouette class, so the resource-shape memo
is no longer a good owner for that report. H172 later shows the sampled class
also exists in `v0.0.3`; move any future visual-correctness investigation to a
reproduced same-frame object artifact, binding/source proof, or final-writer
state instead of spending more Xcode budget on this memo.
[state-churn-encode-encode-phase.169](state-churn-encode-encode-phase.169.md) then checks whether the sampled black
foreground window is a recurrence of the compact uniform / cbuf-prefix visual
bug. The h191 current capture window `1060..1100:5` and h192 full-cbuf oracle
drift by about one capture step, so the sheet pairs h191 `N` against h192
`N+5`. Full-cbuf is active and expensive (`argbuf_hybrid_bytes_per_encoder`
about `0.94GB -> 4.96GB`, cbuf update `1.010 -> 1.332ms/present`), but it does
not visually remove the dark foreground silhouette class. Treat this as a
negative cbuf-prefix oracle for the current window, not as a pixel-exact visual
pass. The next proof needs same-frame final-writer/pass or binding-source
isolation.
state-churn-encode-encode-phase.170 closes that proof gate with h200. The
new rows split the binding-agnostic snapshot-cache miss path into full-payload
reuse, non-constant-payload reuse, and full-payload rebuild; h200 is dominated
by non-constant payload reuse (`306,976 / 345,328` selected paths), so the
residual `d3d9_snapshot_cache_batch_miss_uniform_build_*` timers mostly come
from shader-constant refresh and hash width rather than complete fixed-payload
construction. This is useful attribution, but not a promotion signal: h200 still
has no completion overlap (`with_enqueue=0`) and remains governed by P4/no-enqueue
serial cadence plus replay/encode CPU. Next uniform work should be VS constant
hash/copy width or a direct compact constant representation, not another queued
carrier-shape variant, and it still needs a P4/no-enqueue movement proof before
any Xcode capture.

[state-churn-encode-encode-phase.177](state-churn-encode-encode-phase.177.md) closes the next pending-flush
attribution step. Every non-empty `draw_run` pending flush in h207 is
immediately followed by an explicit imported draw-run, so pending-submission
plus explicit-run carrier merge is now a measured CPU candidate for that half
of the bucket. Chunk `end` drains remain a separate branch and still need a
cross-chunk delay/merge proof or per-record submit-width reduction. Any
mutating version still uses `v0.0.3` as the visual-safe gate before FPS or
Xcode promotion.

state-churn-encode-encode-phase.183 implements the corrected H192 mixed
carrier and rejects it as a runtime promotion. The new API preserves the
following imported draw-run as one canonical shared-state span and removes the
`draw_run` pending flush row. The apparent batch-submit increase is mostly
top-level timer reclassification: the child append/resource/compat rows stay
flat while the old draw-run-submit parent shrinks. P4 does not move
(`completion_wait_with_enqueue=0`, ready depth `1.000`). Treat preflush carrier
merging as a mechanism proof only; the remaining owner is underlying
state/uniform materialization or a separate locality-preserving P4 overlap
design. [state-churn-encode-encode-phase.184](state-churn-encode-encode-phase.184.md) records the resulting owner
review and keeps `v0.0.3` as the current visual-safe anchor.

[state-churn-encode-encode-phase.185](state-churn-encode-encode-phase.185.md) implements the first direct-compact
prerequisite from that review. `FlatDrawStateRecord` hot-build now accepts a
hash-only `FlatDrawStateUniformInputs` view, and hash-ready call sites no longer
pass a full uniform payload just to build the key/hot state. This is a
source-shape change only: full `CachedBaseDrawState::uniforms` is still built
upstream, so there is no FPS or `.gputrace` claim yet.

state-churn-encode-encode-phase.186 implements the next compact-source
step. The opt-in compact carrier can now build compact uniform submissions from
cached fixed payloads, component hashes, and current `DeviceState` shader
constants instead of first materializing a full `DrawUniformPayload`. Full
submission paths still materialize a valid full payload before copying, so the
default behavior is unchanged. Native parity tests cover direct compact stage
spans against the full compact materializer, but this remains an
implementation-only result until a 120s no-gputrace GT1 smoke passes the
`v0.0.3` visual gate and moves the relevant snapshot/P4 counters.

state-churn-encode-encode-phase.187 runs that gate and rejects runtime
promotion. The direct compact source removes the full-uniform carrier lane
(`10,272 -> 0B/record`) and lowers local uniform/replay costs, including
materialized uniform bytes (`5.070 -> 1.428MB/present`), snapshot uniform build
(`0.467 -> 0.405ms/present`), queue draw submission (`3.919 -> 3.629`), and
replay (`8.247 -> 7.902`). The frame owner remains unchanged: ready depth is
still `1.000`, completion wait is still no-enqueue dominated
(`25.801ms/present` without enqueue, `0.036` with enqueue), encode chunk does
not improve, and sampled FPS movement is noise. Treat compact direct as bounded
P2/P3 cleanup only; the next average-FPS work returns to P4/serial-cadence
overlap with CB/pass/tile locality and the `v0.0.3` visual-safe gate.

state-churn-encode-encode-phase.188 sizes the separate chunk `End` drain.
The opportunity is real: end pending flush costs `1,393.521ms/run`
(`0.801ms/present`) and carries `406,005` records across `32,283` flushes.
Nearly all end flushes meet a next draw-shaped record, but the first-submission
compatibility shape is weak: only `48.25%` share state/lane, only `7.04%` share
uniform generation, and only `11.40%` share whole uniform payload hash. Treat
this as a carrier-design limit for a naive cross-chunk carry, not as a hardware
wall. The next mutation must preserve per-draw uniforms and explicit-run shared
state or move to a stricter P4 overlap design; the probe alone does not justify
`.gputrace` because P4 remains fully no-enqueue dominated.

[state-churn-encode-encode-phase.189](state-churn-encode-encode-phase.189.md) adds the missing state+uniform
intersection proof for that same chunk-end shape. H221 repeats the opportunity
(`32,740` stored end flushes / `408,432` records, `30,198` first-submission
candidates), but proves that uniform-stable carry is too narrow: state/lane
compatibility is `49.90%`, while the state-and-uniform intersection is only
`7.22%` by both generation and whole-payload-hash predicates. A promotable
end-drain carrier therefore cannot require uniform stability. It must keep each
draw's uniform payload owned while sharing only the stable state lane, or
preserve the following explicit run as a shared-state span. H221 remains
no-enqueue dominated and is not a `.gputrace` candidate by itself.

[state-churn-encode-encode-phase.190](state-churn-encode-encode-phase.190.md) audits the implementation shape before
turning H221 into a mutation. A naive cross-chunk carry is unsafe for two
independent reasons: the pending submission vectors and compact-uniform arena
scratch are commit-call-local, and previous-chunk `markChunkResources()` stamps
resources for the old chunk's expected submission sequence. A safe carried
submit would need owned `D9CDevice` storage for submissions plus uniform scratch
and a queue submit mode that forces carried work through per-draw resource
marking at the actual submit sequence. Until that exists, do not implement
end-drain carry by merely extending vector lifetime.

[state-churn-encode-encode-phase.191](state-churn-encode-encode-phase.191.md) adds that forced resource-marking submit
mode as a prerequisite. The new frontend/backend/queue seams let carried work
enter `CommandQueue` with per-draw resource marking enabled even while ordinary
same-chunk replay is using the bulk-mark skip path. This is not a performance
result and does not carry any submissions yet; it only removes the resource
lifetime blocker for a future owned end-carry object.

state-churn-encode-encode-phase.192 implements that owned end-carry object
behind `DXMT9_ENABLE_CHUNK_END_CARRY=1`. This is still default-off and not
promoted: deterministic coverage only proves the existing path and the summary
plumbing. The next decision requires a 120s no-gputrace GT1 A/B proving that
stored carry records are adopted, not mostly flushed, and that P4/cadence plus
the `v0.0.3` visual gate remain healthy.

state-churn-encode-encode-phase.193 performs that runtime gate and rejects
promotion. The carry path adopts `99.84%` of stored records and collapses the
target chunk-end flush bucket (`0.817 -> 0.045ms/present`), but total replay per
present is flat (`8.497 -> 8.492ms`) because submit batching grows wider:
`commit_chunk_draw_batch_submit_cpu_ms` rises `1.714 -> 1.983ms/present` and
submission records per submit rise `9.053 -> 12.497`. P4 also stays in the same
no-enqueue class: ready depth remains `1.000` and
`completion_wait_with_enqueue_ms_per_present` falls to `0.000`. Treat the owned
carry as a proven mechanism, not a current FPS lever. The next branch should
either remove the underlying submit/materialization work or return to a
locality-preserving P4 overlap design.

[state-churn-encode-encode-phase.194](state-churn-encode-encode-phase.194.md) adds and runs the missing attribution
for that branch. H224 repeats the carry mechanism but shows forced
resource-marking pending flushes are only a partial owner:
`0.144ms/present`, `30.401` records/present, and `20.09%` of pending flush CPU.
The larger submit row remains (`draw_batch_submit=2.007ms/present`), ready depth
stays `1.000`, and the output screenshot is HUD plus black scene, so this is
not visual-safe promotion evidence. Treat resource marking as a local residual
to reduce only after a narrower proof; the average-FPS branch remains queue
submit residual/batch width or P4 overlap.

[state-churn-encode-encode-phase.195](state-churn-encode-encode-phase.195.md) summarizes the current "wall" review.
The latest visual-safe scout confirms that same-generation state-copy elision is
already active (`410,814` elided states / `4.203GiB` saved) while adjacent
uniform elision remains unavailable (`same uniform generation = 0`) and compact
uniform / chunk-end carry branches have already failed the FPS/P4 gate. This is
not a proven hard limit: it narrows the next work to queue lock / outer submit /
batch-width residuals for local CPU cleanup, or a render-pass-safe P4 overlap
carrier that actually moves no-enqueue rows. The first follow-up counter is
`submit_draw_run_batch_queue_lock_cpu_ms`, which splits queue mutex acquisition
from the existing `submit_draw_run_batch_*` child rows.

[state-churn-encode-encode-phase.196](state-churn-encode-encode-phase.196.md) runs that follow-up counter and rejects
queue mutex acquisition as the owner. H225 reports only
`0.018ms/present` in `submit_draw_run_batch_queue_lock_cpu_ms`, while the same
run still has `completion_wait_without_enqueue_ms_per_present=27.837`,
`commit_chunk_replay_cpu_ms_per_present=8.424`,
`encode_chunk_cpu_ms_per_present=11.249`, and
`submit_draw_run_batch_append_cpu_ms_per_present=1.289`. Visual captures from
frames `900` and `920` are coherent and the run has
`draw_skipped_no_pipeline=0` / `gpu_command_buffer_errors=0`, so this is a valid
negative attribution result. Do not spend GT1 work on this mutex path; the next
local CPU branch is snapshot/cache materialization or append payload width, and
the next average-FPS branch remains P4/run-ahead overlap.

[state-churn-encode-encode-phase.197](state-churn-encode-encode-phase.197.md) then reuses the same H225 run with a
summary-only parent-minus-child split. The draw-batch-submit parent is not a
large unmeasured outer-submit gap: known children explain `89.96%` of the
parent, and append alone is `76.63%`. Inside append, uniform append owns
`51.51%`, state append `26.00%`, and the residual is only `9.73%`. This keeps
the local submit branch focused on append materialization width rather than
queue locking or broad submit wrapper cost. It remains a local CPU branch unless
a follow-up no-gputrace run moves P4/no-enqueue rows.

[state-churn-encode-encode-phase.198](state-churn-encode-encode-phase.198.md) further narrows the uniform half of that
append row. Payload lookup plus payload-record append storage explain only
`38.02%` of `submit_draw_run_batch_append_uniform_cpu_ms`; the remaining
`0.411ms/present` is stage-level uniform append work. The useful local branch is
therefore N-1 materialization elision or stage-level uniform append reduction,
not payload-record copy width alone. The average-FPS branch still requires
P4/no-enqueue movement.

[state-churn-encode-encode-phase.199](state-churn-encode-encode-phase.199.md) adds the missing component split for the
next run. It separates fixed/VS/PS component find and append scopes inside
`appendDrawUniformPayload()` and makes the summary report
known-with-components share plus remaining component residual. Older H225 data
shows `n/a` for those rows, preserving the H207 conclusion while making the next
probe decisive.

[state-churn-encode-encode-phase.200](state-churn-encode-encode-phase.200.md) runs that probe and implements the
small fixed-payload handle carry that the split suggested. The targeted row
moves: `uniform_component_fixed_find_cpu_ms_per_present` falls
`0.229 -> 0.150`, and total component find falls `0.323 -> 0.257`. The run stays
clean on skipped-pipeline and command-buffer errors, but the parent uniform
append row is flat (`0.882 -> 0.880ms/present`), sampled FPS is noisy/regressed,
and completion wait remains no-enqueue dominated. Treat fixed-handle carry as a
valid local cleanup, not the wall-breaking FPS lever. The next average-FPS
branch remains P4/no-enqueue overlap or larger replay/encode materialization
elision.

[state-churn-encode-encode-phase.201](state-churn-encode-encode-phase.201.md) then puts a ceiling on the remaining
uniform append path after that cleanup. The H209 run still appends `794,314`
uniform payload records, including `661,640` VS constant stage records
(`0.833` per payload append), but the normalized local CPU is small:
the parent is `0.880ms/present`, known scopes plus component scopes explain
`77.75%`, the remaining component residual is `0.196ms/present`, and the
largest named child left is VS stage append at `0.116ms/present`. That makes
more uniform-append work optional local cleanup, not a reason to spend Xcode or
claim a wall-breaker. The next FPS-facing branch remains P4/no-enqueue overlap
or a larger replay/encode materialization change that moves serial rows.

## How to run
Every experiment here is a 3DMark05 GT1 run via the standard wrapper. This is a
CPU draw-run / handle-churn domain: enable the per-encoder breakdown, run a cheap
`--no-gputrace` A/B, and prove the batching mechanism with run-level CPU gates:

```sh
DXMT9_PERF_ENCODER_BREAKDOWN=1 \
bash scripts/tools/run_3dmark05_perf_probe.sh --suffix state-churn --frame 60 \
  --no-gputrace --timeout 120

bash scripts/tools/finalize_3dmark05_perf_probe.sh --suffix state-churn --frame 60 \
  --baseline-output experiments/output/<baseline>/result.json \
  --require-binding-overrides-present --require-draw-submission-batch-present \
  --require-draw-run-records-increase --require-encode-draw-cpu-decrease
```

For state-width-only changes, do not require draw-run record growth; those
patches should compare per-present copy and append buckets instead.

The `[dxmt9-perf-encoder]` / `[dxmt9-perf-encoder-stream]` lines and
`commit_chunk_draw_run_*` plus `commit_chunk_*_cpu_ms` counters carry the churn
and replay attribution. The exact
per-experiment flags live in each leaf's `**Method.**` field. See
`agents/rules/environment_variables.rules.md` for env-var meanings and
`agents/rules/metal_debugging.rules.md` for the full workflow.

## Cross-references

- [const-upload](../const-upload/index.md) — constant-upload boundaries are the larger, separate
  draw-run break class (`2.88x` state-delta); crossing them needs const
  coalescing, not a stream/IB payload. The 4.64GB cbuf write bucket is measured
  in the same encoder-breakdown runs.
- [snapshot-cache](../snapshot-cache/index.md) — the D3D9 draw-state snapshot cache and binding-agnostic
  snapshot reuse address the same per-draw state-binding cost from the front end.
- [hidden-backend-storage](../hidden-backend-storage/index.md) — the GPU-side ~1.63GiB VS-write bucket these CPU
  wins do not touch; the label-join here is shared evidence for that domain.
- [index-cache-locality](../index-cache-locality/index.md) — the one accepted *GPU-side* win, which reduces VS
  invocations rather than CPU encode cost; auto-expand is a different indexed-path
  axis.
- [overview-3dmark05-gt1](../overview-3dmark05-gt1.md) — root map, priority DAG, and ceiling synthesis.

## Root 3DMark05 Map Detail Migration - 2026-07-08

Detail migrated from the former long-form root [3DMark05 overview](../overview-3dmark05-gt1.md) so that `state-churn-encode` owns its detailed synthesis while the root overview stays cross-domain only.

### From Central finding (read this first)

Almost every other hypothesis (visible varying width, shader temps,
render/raster state toggles, primitive reorder, const-upload size,
pixel-format views) was **rejected as "not the first-order owner."**
Several CPU-side reductions are real but orthogonal to the GPU limiter.
The current stream/IB branch is now a useful negative gate, not a GPU-side
denominator win: the preflight shows bounded stream0/stream1/IB tuple
alternation in frame60 hot rows ([state-churn-encode-stream.05](state-churn-encode-stream.05.md)), the
row-scoped staging A/B proves `60/2` can be made handle-stable without changing
draw/PSO/argbuf shape ([state-churn-encode-stream.08](state-churn-encode-stream.08.md)), and the Xcode
follow-up rejects handle identity as the first-order backend owner
([state-churn-encode-stream.09](state-churn-encode-stream.09.md)). Stream/IB handle churn remains relevant for
CPU batching/encode work, but not for the current GT1 hidden-backend GPU
limiter. The follow-up per-draw PSO gate also finds no stream/IB-handle-stable
run where PSO changes independently, so current PSO movement is not an Xcode
counter target either ([hidden-backend-storage-shape.18](../hidden-backend-storage/hidden-backend-storage-shape.18.md)). The current full
gate now carries that result as `pso-backend-isolation=reject-current`, so
unisolated PSO motion is blocked in the next-experiment queue as well
([hidden-backend-storage-shape.19](../hidden-backend-storage/hidden-backend-storage-shape.19.md)).


### From Current Gate Summary

Latest encode-state update: generation/lane fast-path and queued-submission
microfixes close the deep-compare and trivial-copy branches
(state-churn-encode-encode-phase.32 through
state-churn-encode-encode-phase.37). Shared shader bytecode plus sampler
and TSS/render-state flat-capacity compaction reduce copied state width
(state-churn-encode-encode-phase.38,
state-churn-encode-encode-phase.39,
state-churn-encode-encode-phase.40,
[state-churn-encode-encode-phase.42](state-churn-encode-encode-phase.42.md)). The resource-retention follow-up then
marks batch-front draw resources once while preserving per-draw binding
override/snapshot resource marking
([state-churn-encode-encode-phase.43](state-churn-encode-encode-phase.43.md)). The F1 N-1 state-copy elision was
first proven behind `DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1`
([state-churn-encode-encode-phase.44](state-churn-encode-encode-phase.44.md)), then the default-path attribution
showed `411,362` non-front materialized states / `4.209GB` discarded by batch
append ([state-churn-encode-encode-phase.47](state-churn-encode-encode-phase.47.md)). The safe subset is now
default: same-stamp continuations elide state copies while the queue keeps
normal compatibility grouping. The 120s scout reports
`submit_draw_run_batch_discarded_state_records 411,362 -> 0`,
`d3d9_snapshot_state_elided=412,180`, and
`d3d9_snapshot_state_copy_cpu_ms 261.001 -> 138.856`, with a normal output
frame and no pipeline skips/errors. GPU/completion remain flat/noisy, so this
is accepted as copy-policy cleanup rather than the FPS owner
([state-churn-encode-encode-phase.48](state-churn-encode-encode-phase.48.md)). The current follow-up accepts
queued-submission carrier construction as a bounded CPU target:
`commit_chunk_queue_draw_submission_emplace_cpu_ms=1,123.253`,
`13.99%` of queued-submission CPU, or `0.646ms/present`, so optional-state or
direct-construct work is plausible cleanup but not the whole FPS answer
([state-churn-encode-encode-phase.45](state-churn-encode-encode-phase.45.md)). Optional queued-submission carrier
storage then removes about half of that child:
`commit_chunk_queue_draw_submission_emplace_cpu_ms` `1,123.253 -> 573.056`
(`-48.98%`) and queued-submission CPU `8,031.316 -> 7,867.581` (`-2.04%`),
with a normal output frame ([state-churn-encode-encode-phase.46](state-churn-encode-encode-phase.46.md)). The
after-run queue residual closes further carrier work for now: queue submission
is `4.522ms/present`, but snapshot is `3.900ms/present`, emplace is
`0.329ms/present`, and the remainder is only `0.292ms/present`; snapshot cache
lookup remains `3.343ms/present`. The render-state entry-count probe
shows GT1 fits in `64` active render states (`max=62`, `gt64=0`), but the
default table already starts at `62`, so the implementation uses a conservative
`128`-slot priority active payload rather than a `64` cap
(state-churn-encode-encode-phase.41,
[state-churn-encode-encode-phase.42](state-churn-encode-encode-phase.42.md)). The latest structural sizes are
`FlatDrawStateRecord=7,984B`, `CanonicalDrawState=10,312B`, and
`DrawRunSubmission=21,008B`; the 120s scout stayed visually normal with
render-state overflow `0`. The follow-up VS indexed-float opportunity probe
keeps hash semantics unchanged and shows only a small safe tail remains:
batch-miss VS const hashing could avoid `20.720MB` (`272B` per indexed-float
call, `5.47%` of VS-constant hash bytes) by hashing the full float file but
prefix-bounding int/bool constants. These are CPU state-width/hash wins/proofs
only: do not spend Xcode budget on them without a new GPU-facing mechanism. The
shader-layout reuse follow-up accepts only the conservative reason-mask-safe
subset by default: broad post-build compatibility is `154,985 / 380,288`
(`40.75%`), but actual safe reuse is only `7,565 / 390,712` (`1.94%`) and cuts
shader-layout rebuild `0.3715 -> 0.3386ms/present`; treat it as a small CPU
cleanup, not the next FPS owner (snapshot-cache-snapshot.19). The
uniform payload append prereserve probe then rejects a simple reserve-check
cleanup: `append_uniform/present` stays flat (`0.474488 -> 0.474157ms`) and the
parent append bucket regresses, so future work on that child must split lookup
bucket walk, equality compare, payload copy, and lookup linking first
(state-churn-encode-encode-phase.51). That split now exists as attribution:
`draw_uniform_payload_append_copy_cpu_ms=813.196ms` dominates reserve
(`53.018ms`) and link (`62.443ms`), while lookup is `294.215ms` total
(`154.102ms` in bucket chains). Read phase52 as timer attribution, not an
optimization A/B; the next plausible uniform-append lever is fewer or narrower
owned payload copies across the queue boundary
([state-churn-encode-encode-phase.52](state-churn-encode-encode-phase.52.md)). The first narrow copy cleanup is now
accepted: in-place `DrawUniformPayloadRecord` construction cuts append-copy
`813.196 -> 602.274ms` and append-uniform parent `1299.014 -> 1128.212ms`, with
visual smoke normal but FPS flat/noisy. Remaining uniform-payload work requires
storage-shape changes, not another construction micro-optimization
(state-churn-encode-encode-phase.53). A follow-up prefetched-PSO resolve
cache is rejected-current: it found `152,261` same-handle hits, but the parent
`encode_draw_pipeline_lookup_cpu_ms` did not fall (`934.420 -> 950.626ms`), so
the experiment code was reverted. Treat pipeline lookup as secondary until a
split counter names a larger subchild; keep the next no-gputrace focus on the
larger encode/snapshot/replay buckets and completion-pacing
(state-churn-encode-encode-phase.54). Returning to that larger encode
child, the argbuf reopen split shows the legacy `argbuf_open` counter is a
reopen-block parent rather than actual Metal open work: `open_call=573.804ms`
versus `reopen_post=891.359ms`, with table bind, cached cbuf repoint, content
probe, and about `319ms` of still-unattributed post-open work. This is
attribution-only and keeps FPS/GPU/completion flat, but it redirects argbuf work
toward pre-open component identity or a further post-open split instead of a
single `openArgbuf()` micro-optimization
(state-churn-encode-encode-phase.55). The pre-open whole-table reuse check
then rejects that direction for GT1 (`961,473` candidates, `0` skips), and the
post-open residual split shows the phase55 `~319ms` residual is distributed
bookkeeping rather than a single hidden child: table probe `50.933ms`, byte
account `51.990ms`, cbuf cache/dirty scans `118.813ms`, and force-dirty
bookkeeping `104.757ms`. The split run adds hot-path timer overhead, so use it
as attribution only (state-churn-encode-encode-phase.56,
state-churn-encode-encode-phase.57). The binding-packet plan split then
rejects plan construction as the next primary FPS lever: the largest named
child is fragment texture/sampler planning at only `0.204682ms/present`, and
the child timers are default-off via `DXMT9_PERF_BINDING_PACKET_PLAN_SPLIT=1`
because they perturb the parent bucket (state-churn-encode-encode-phase.58).
The latest uniform-payload backend A/B then rejects slot-local dedup as a
required GT1 default: `DXMT9_DISABLE_DRAW_UNIFORM_PAYLOAD_DEDUP=1` removes
lookup CPU (`276.107 -> 0ms`) and cuts the targeted append-uniform parent
`1041.108 -> 799.528ms`, but extra appends rise `877,508 -> 930,994` and the
broader queue submission bucket is flat (`6813.183 -> 6817.526ms`). Keep the
knob as a diagnostic or repeat-run policy candidate; it does not change the
current FPS owner by itself (state-churn-encode-encode-phase.59).
The draw-issue split then closes another open CPU bucket as attribution:
`DXMT9_PERF_DRAW_ISSUE_SPLIT=1` shows every GT1 issued draw is indexed,
visibility/non-indexed/expanded/split children are `0`, and the Metal
`drawIndexedPrimitives` call itself accounts for `897.049ms` (`77.0%` of the
issue parent). Do not spend the next iteration on wrapper micro-optimization
around `encode_draw_issue_cpu_ms`; only fewer Metal draw calls or a different
submission model can move that bucket materially
(state-churn-encode-encode-phase.60).
The argbuf cbuf probe split then closes cached-repoint/content-probe as a
single-stage primary target: FFPPS repoint is large in bytes
(`899,453` calls / `345.390MB`) but only `137.306ms`, VS probe is `83.048ms`
with low hits (`143,728 / 931,743`), and dirty VS update remains larger at
`936.123ms`. Keep the next argbuf work on table reopen frequency/storage shape
or dirty VS upload frequency rather than skipping VS probe or micro-optimizing
FFPPS repoint (state-churn-encode-encode-phase.61). The later default
cleanup moves the cached-repoint/content-probe timers behind
`DXMT9_PERF_ARGBUF_CBUF_PROBE_SPLIT=1`, keeping calls/bytes/hit counters live
while removing another attribution-only timer layer from the normal profile
(state-churn-encode-encode-phase.86). The dirty VS identity
refresh closes the local dirty-mirror skip variant too: the original probe saw
`808,845` dirty VS probes with `0` hits, and the current post-compact recheck
still sees `862,747` probes with `0` hits and `992.154MB` of miss bytes. Treat
dirty VS updates as real current-model identity churn, not stale cache repeats
(state-churn-encode-encode-phase.62,
[state-churn-encode-encode-phase.123](state-churn-encode-encode-phase.123.md)). The completed-seq snapshot cleanup
then trims only the table-reserve child (`0.196 -> 0.158ms/present`) while
`argbuf_open`, `argbuf_setup`, encode, and replay fail to promote, so
completed-waterline plumbing is not the argbuf/FPS answer
(state-churn-encode-encode-phase.124). The follow-up
PSO-prefetch cleanup removes another legacy uniform scratch consumer:
`prefetchSlotPipelines()` no longer materializes `DrawUniformPayload` just to
build depth/tile/draw PSO keys, and the key-descriptor spec proves those keys
are uniform-value independent. The A/B cuts backend uniform materialization
`12.345MB/present -> 9.011MB/present`, materialization CPU
`0.449 -> 0.337ms/present`, and PSO-prefetch state-copy CPU
`0.150 -> 0.016ms/present`, but `encode_chunk`, `commit_chunk_replay`, and
no-enqueue completion wait are effectively flat, so this is a local compact-
consumer cleanup rather than an FPS owner ([state-churn-encode-encode-phase.125](state-churn-encode-encode-phase.125.md)).
The following site-attribution scout then shows the remaining backend
materialization is not hidden in framegraph or miscellaneous consumers:
draw-encoder command `36.81%`, queue observation `36.79%`, and draw-encoder
per-param `26.40%`. Queue observation is the cleanest next local target because
its full payload dependency is only the projected-texture compat input, not
actual Metal draw encoding ([state-churn-encode-encode-phase.126](state-churn-encode-encode-phase.126.md)).
That queue site is now gone: compact hot state carries
`nonIdentityTextureTransformStageMask`, so queue diagnostics report projected
compatibility with `0` queue-observation materialization
([state-churn-encode-encode-phase.127](state-churn-encode-encode-phase.127.md)). The adjacent lazy command-uniform
probe is rejected: delaying command materialization until the draw loop changes
`draw_uniform_payload_materialized_draw_encoder_command/present` only `323.548 -> 323.233`, so the
remaining command site is real draw consumption rather than pass-open observer
waste (state-churn-encode-encode-phase.128). The follow-up
payload-delta probe also rejects broad non-shader payload hash churn as the
argbuf reopen owner: all `931,917` changed-payload reopens are explained by
VS/PS constant hashes and `changed_nonconst_only=0`
(state-churn-encode-encode-phase.63). The scoped VS-cbuf plan-shape run
then rejects dirty-range width as the next large cbuf owner: frame60 averages
`0.702` dirty float regs/upload but `57.483` planned float regs/upload, with
`20.21%` indexed-float full-struct fallback. The follow-up exact bytecode
sparsity pass then lowers the generic packed-layout branch: frame60 has `0`
safe non-indexed packed bytes, and the whole theoretical gap is indexed VS
(`59` draws matching the `59` full/indexed uploads). All hot indexed rows use
static offsets `0;1;2` with relative sources `a0.x/a0.y`, so the remaining
packing proof is specifically a vertex BLENDINDICES dynamic-window problem,
not arbitrary sparse constants. Packed constants now require that hard
dynamic-window translator/ABI proof rather than a generic non-indexed layout.
The geometry follow-up then rejects that branch as a current broad target for
the hottest indexed VS sample: `12` payloads / `75,395` vertices include one
draw with `a0.x=0..255` and `a0.y=0..254`, so the required
`c[a0 + 0..2]` window reaches the full vertex constant range. The cbuf lane now
points at constant churn, segmented/persistent storage, or table-reopen
frequency, not another prefix trim or packed indexed constants
(state-churn-encode-encode-phase.64,
state-churn-encode-encode-phase.65,
state-churn-encode-encode-phase.66).
The later source-attribution probes sharpen that conclusion: wide VS deltas
are mostly contiguous full-prefix churn (`span / changed = 1.057x`,
full-prefix `78.46%` of changed regs), and `prefix_regs=256` source buckets
own `92.51%` of changed regs while the top two shader-pair buckets own
`54.63%` of full-prefix regs. The top owners include the known frame60 `60/1`
hidden-backend hot row and the known BLENDINDICES matrix-palette VS, so the
next cbuf gate is setter-range attribution for those hot sources or a Stage 2b
cbuf ABI, not small-delta slicing ([state-churn-encode-encode-phase.138](state-churn-encode-encode-phase.138.md),
[state-churn-encode-encode-phase.139](state-churn-encode-encode-phase.139.md)). The setter-range follow-up then
moves that gate upstream: app `SetVertexShaderConstantF` calls are mostly
small (`count=3/4` in the hot concrete rows), while the PE dirty-shadow flush
merges them into wide `SET_VS_CONST_F` spans such as `count=196/201/205`.
The existing sparse dirty-run splitter then proves the width mechanism by
making flush records exact, but rejects record splitting as the current
argbuf/FPS lever because VS cbuf update bytes and P4 stay flat while record
count rises. The surviving cbuf lane is Stage 2 cbuf ABI/storage or broader
P2/P3/P4 movement, not more PE const-record slicing
([state-churn-encode-encode-phase.140](state-churn-encode-encode-phase.140.md),
[state-churn-encode-encode-phase.141](state-churn-encode-encode-phase.141.md)). The follow-up Stage 2b opportunity
counter then confirms the remaining table churn is the constants-only cbuf
path, not resource-array mutation: `resource_array=0`,
`reopen_cbuf_only=1,004,713`, and `argbuf_table_bind_calls=1,004,713`
([state-churn-encode-encode-phase.142](state-churn-encode-encode-phase.142.md)). The Stage 2b ABI/runtime follow-up
then proves that table churn is mechanically removable: the default-off
`DXMT9_ARGBUF_DIRECT_CBUF=1` scout keeps the Stage 2 candidate shape
(`588,953` candidates, `0` resource-array candidates) while dropping
`argbuf_table_bind_calls`, `argbuf_open`, `argbuf_setup`, and argbuf cbuf
updates to `0` in a normal visual run. That does not promote FPS:
`sampled_avg_fps=16.864` and
`completion_wait_without_enqueue_ms_per_present=28.565`, so the current
average-FPS lane returns to P4/P2/P3 cadence and producer overlap rather than
more argbuf table microfixes ([state-churn-encode-encode-phase.143](state-churn-encode-encode-phase.143.md),
[state-churn-encode-encode-phase.144](state-churn-encode-encode-phase.144.md)). A follow-up legacy uniform scratch
cleanup removes redundant full-payload zero-fill before compact materialization
and nudges `encode_draw_cpu_ms_per_present` `8.580 -> 8.426` in a normal visual
run, but it is also FPS-flat (`16.865 -> 16.931`) and leaves no-enqueue
completion wait dominant (state-churn-encode-encode-phase.145).


### From What is settled vs open

- Several CPU reductions are real (dirty-range reset + FFP-VS slice reuse
  cut cbuf traffic 4.6 GB→~1 GB; binding-override cut encode CPU 10–30%) —
  but every one left GPU frame time flat. [const-upload](../const-upload/index.md), [state-churn-encode](index.md), [snapshot-cache](../snapshot-cache/index.md)

- Current stream/IB churn as a production claim. The hot rows are true handle
  churn, and the row-scoped staging A/B proves handle identity can be
  controlled. The Xcode follow-up then rejects handle identity as the
  first-order owner: `60/2` stream/IB handles go to zero while GPU time and VS
  buffer writes stay flat. [state-churn-encode-stream.04](state-churn-encode-stream.04.md),
  [state-churn-encode-stream.08](state-churn-encode-stream.08.md), [state-churn-encode-stream.09](state-churn-encode-stream.09.md)

- Remaining CPU tracks: pacing/completion wait, backend encode, commit_chunk
  replay, and residual snapshot rebuild. The current low-overhead scout
  `app-d3d9-3dmark05-current-lowoverhead-20260613` makes this the average-FPS
  owner lane: `completion_present_wait_ms=25.091ms/present`,
  `gpu_command_buffer_time_ms=3.113ms/present`,
  `encode_chunk_cpu_ms=11.112ms/present`,
  `commit_chunk_replay_cpu_ms=10.746ms/present`,
  `commit_chunk_queue_draw_submission_cpu_ms=4.596ms/present`,
  and `d3d9_snapshot_draw_submission_cpu_ms=3.748ms/present`. Immediate
  presents, `present_boundary_wait_ms=0`, and `completion_pending_depth_max=0`
  mean P4 is the observed wait bucket. The follow-up overlap scout
  `app-d3d9-3dmark05-pipeline-overlap-r1-20260613` makes the mechanism sharper:
  `completion_wait_with_enqueue_ms=0`,
  `completion_wait_without_enqueue_ms=44789.044`,
  `completion_enqueue_while_waiting=0`,
  `completion_enqueue_pending_depth_max=1`, and
  `completion_dequeue_age_p50/p95_ms=0.044/0.065`. The gap follow-up
  `app-d3d9-3dmark05-pipeline-gap-r1-20260613` then shows wait-end to next
  enqueue p50/p95/p99 `20.501/54.643/63.634ms`. The stage split
  `app-d3d9-3dmark05-pipeline-stage-r1-20260613` refines that edge:
  wait-end to `CommitPublish` p50/p95 `16.645/30.880ms`,
  `EncodeDequeue` `20.116/35.167ms`, Metal commit `36.470/55.470ms`, and
  pending enqueue `36.502/55.508ms`. The current wallclock owner is therefore
  hard under-pipelining at the P4 boundary plus P2/P3 CPU cadence that runs
  after the exposed wait instead of feeding a next command buffer during it.
  The direct boundary/latency A/B
  present-pacing-boundary-latency-ab.06 rejects dxmt9's explicit boundary
  wait as that missing producer-overlap lever: fresh baseline,
  `DXMT9_DISABLE_PRESENT_BOUNDARY=1`, and
  `DXMT9_MAX_FRAME_LATENCY=6 DXMT9_CAP_FRAME_LATENCY_TO_BACKBUFFERS=0` all keep
  `present_boundary_waits=0`, `completion_wait_without_enqueue_ms≈44s`, and
  sampled FPS p50 `17.8-18.0`; the disabled-boundary run proves env propagation
  with `present_boundary_skipped=1740`. The next localization must therefore
  timestamp before `CommitPublish` or outside dxmt9's explicit boundary wait
  instead of re-tuning `DXMT9_*PRESENT_BOUNDARY*` policy. The later completion
  signal perturbation also rejects dxmt9 completed-seq/waterline publication as
  the hidden dependency: `DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS=8` applies
  `13568ms` of delay, but `SetRenderTarget -> Clear`, record 1, first chunk,
  and next-enqueue p50 all remain flat.
  A later sub-command-buffer cap A/B also rejects simple mid-chunk cap
  retuning: cap=8 doubles sub-CBs and drops cap suppression, but tail FPS is
  flat/worse and wait-end -> next-enqueue p50 lengthens.
  present-pacing-subcb-cap.25
  That follow-up present-pacing-prepublish-stage.07 shows the app/Wine/PE
  side is not the long edge: wait-end to unix `commit_chunk` entry is only
  p50/p95 `1.040/2.668ms`, while wait-end to `CommitPublish` remains
  `15.894/29.912ms` and wait-end to Metal commit remains `22.276/54.146ms`.
  Current average-FPS work is therefore back inside dxmt9 commit/replay/submit
  and backend encode, with `commit_chunk_replay_cpu_ms=18981.064`, nested
  `commit_chunk_queue_draw_submission_cpu_ms=8154.509`, and nested
  `d3d9_snapshot_draw_submission_cpu_ms=6636.191` in the new scout. The
  same-sample stage-delta follow-up [present-pacing-stage-delta.08](../present-pacing/present-pacing-stage-delta.08.md) removes
  the percentile-subtraction ambiguity: current GT1 still has
  `completion_wait_with_enqueue_ms=0`, and the exposed path splits into
  `commit_chunk entry -> CommitPublish` p50/p95 `6.172/28.101ms`,
  `CommitPublish -> EncodeDequeue` `2.535/5.086ms`, and
  `EncodeDequeue -> commandBuffer.commit()` `11.384/22.232ms`. Queue wake is
  secondary; pre-publish replay/submit/snapshot and post-dequeue backend encode
  are the two load-bearing CPU stages.
  After the
  accepted cbuf identity, packet-cache, and snapshot hash
  work, `snapshot.09` was
  `completion_wait_ms=39978.924`, `encode_draw_cpu_ms=17711.215`, and
  `d3d9_snapshot_draw_submission_cpu_ms=7196.881` over `1740` presents. The
  latest commit_chunk stage split is similar end-to-end but corrects the
  attribution: `completion_wait_ms=38990.561`, `encode_draw_cpu_ms=17051.620`,
  `d3d9_snapshot_draw_submission_cpu_ms=7779.855`, and
  `bridge_commit_latency=22.473s`, of which `21.839s` is replay, not raw
  bridge ABI cost. The replay child split then names queued draw submission and
  snapshot as the first replay owner: `commit_chunk_replay_cpu_ms=22223.637`,
  `commit_chunk_queue_draw_submission_cpu_ms=9927.191`, nested
  `d3d9_snapshot_draw_submission_cpu_ms=7696.922`,
  `commit_chunk_draw_batch_submit_cpu_ms=3229.424`, and
  `commit_chunk_draw_run_submit_cpu_ms=2093.639`; draw-run scan, state apply,
  and const upload record dispatch are all sub-400ms. The broad bind-cache,
  broad setter-skip, bridge-ABI, and draw-run-scan guesses are rejected. The
  current low-overhead scout already reads the
  `submit_draw_run_batch_*_cpu_ms` child counters: append remains
  `1.068ms/present`, append-uniform `0.488ms/present`, append-state
  `0.297ms/present`, and compat scan is now only `0.027ms/present`, so the
  generation/lane fast path has closed deep-compare as the near-term owner.
  Static follow-up [state-churn-encode-encode-phase.27](state-churn-encode-encode-phase.27.md) points at the nested
  snapshot side as an equally important candidate: snapshot cache lookup is
  `6350.751ms`, cache miss is `5271.187ms`, uniform build calls are `928,656`,
  and queue-slot uniform payload dedup appends `874,477` payloads. The key
  unproven assumption is whether declared draw-packet non-binding deltas often
  repeat the current value; if so, invalidating from actual changed reason
  rather than declared delta mask could reduce snapshot misses. The
  `--probe-draw-packet-actual-change` no-gputrace scout
  state-churn-encode-encode-phase.28 rejects that branch for current GT1:
  `draw_packet_declared_nonbinding=419,990`,
  `draw_packet_actual_nonbinding=419,990`, and
  `draw_packet_redundant_nonbinding=0`. The follow-up
  snapshot-cache-snapshot.10 accepts the narrower shader-constant refresh
  fast path: snapshot submission drops `7622.807ms→6495.069ms`, uniform refresh
  drops `2014.263ms→814.507ms`, and user-observed muzzle flash / particles /
  fog remain correct, but sampled FPS is flat (`15.717→15.752`). The remaining
  no-gputrace work should split or reduce named buckets first: cbuf
  upload/probe/repoint residual, binding-packet plan/cache, index setup/source
  resolve, shader-stream binding diversity, snapshot miss hot-build/VS indexed
  fallback, and the two draw submit paths. The later draw-issue split closes
  `encode_draw_issue_cpu_ms` as normal indexed Metal draw-call cost rather than
  wrapper overhead. The first
  argbuf-open split shows
  slot-30 bind shadowing is not useful (`table_bind_skipped=0`), and transient
  arena fast append has already removed the simple reserve-scan cost
  (`reserve` -51.95%, `encode_draw` -3.87%). Dirty-category identity repoint
  was also rejected (`0` hits over `19,769` candidates). The stream-bind split
  then shows the parent is not one bind class: texture/sampler is largest
  (`1065ms`), followed by index (`670ms`), shader stream (`497ms`), and raster
  (`389ms`), while FFP stream is negligible (`6.845ms`). The texture/sampler
  child further narrows to fragment resolve (`575ms`) and fragment direct bind
  (`317ms`); resource-array, vertex texture, and LOD-bias lanes are zero for
  GT1. Sampler pre-handle skip then avoids `2.108M` skipped sampler lookups and
  cuts texture/sampler parent CPU `-18.84%` in a same-present run; sampler-state
  hash reuse follows with fragment direct `-68ms` and texture/sampler parent
  `-69.6ms` in the default perf profile. Texture pre-resolve source matching is
  rejected and removed from the hot path. The cbuf category split shows raw
  `setBuffer` (`114.568ms`) and transient upload (`276.019ms`) are not the main
  remaining cbuf owners; the residual split then rejects upload-plan
  (`43.287ms`, nested in build) and observer callbacks (`0`) as owners and
  names binding content hash as the dominant cbuf child (`570.070ms`, VS
  `489.627ms`). The content-hash removal then drops the default binding hash
  counter to `0` and cuts cbuf update `1.216 -> 0.875ms/present`, leaving
  build/upload, content-probe/cached-repoint, binding writeback, and residual
  dispatch/timer cost as the next cbuf targets. Prefix-preserving raw cbuf
  builders then reduce build from `0.333815 -> 0.175342ms/present` and cbuf
  update from `0.875284 -> 0.679652ms/present`, with normal visual smoke. The
  failed live-range-only prefix variant produced dark/black geometry, so cbuf
  builders must preserve the old full-builder byte prefix even when usage bounds
  choose the prefix size. A later full-cbuf diagnostic forced full VS/PS cbuf
  uploads and raised cbuf/transient traffic by about `+519%` without an obvious
  visual fix, so full upload is not a default workaround; same-input mini-replay
  remains the required visual proof path. The accepted visual fix is instead to
  keep VS/PS component hashes inside each per-draw `DrawUniformPayload` and use
  those hashes for argbuf cbuf identity, because draw submission batches can
  carry a current payload while base `hot` still has the first draw's constant
  hashes. The opt-in argbuf cbuf probe split then rejects cached
  repoint/content-probe as a single-stage primary target: FFPPS repoint is many
  calls and bytes (`899,453` / `345.390MB`) but only `137.306ms`, VS probe is
  `83.048ms` with low hits, and dirty VS update remains larger
  (`936.123ms`). Those cached-repoint/content-probe timers are now opt-in only
  in the default profile, while the non-timed sizing counters remain live. The
  dirty VS identity refresh then reports `0` cached hits in both the original
  probe and the post-compact recheck (`862,747` dirty VS candidates,
  `992.154MB` miss bytes), so a local dirty-mirror repoint/skip path is closed.
  The exact bytecode sparsity pass rejects generic non-indexed
  packed constants as that cheaper storage shape: safe non-indexed packed save
  is `0`, and the whole theoretical gap is indexed VS (`59` draws matching the
  `59` full/indexed uploads). The indexed shape is static offsets `0;1;2`
  with relative sources `a0.x/a0.y`, so the missing bound is the runtime vertex
  BLENDINDICES range. The geometry follow-up rejects that bound for the hottest
  indexed VS sample: one dumped draw observes `a0.x=0..255` and `a0.y=0..254`.
  Remaining argbuf targets are now fewer table reopens, upstream VS
  dirty-frequency reduction, or segmented/persistent cbuf storage that preserves
  full indexed access semantics.
  Snapshot work is still open, especially residual non-constant payload hashing
  and VS indexed constant fallback, but it is no longer the sole first-order CPU
  owner.
  baselines-frame50.05, state-churn-encode-encode-phase.02,
  state-churn-encode-encode-phase.03, state-churn-encode-encode-phase.04,
  state-churn-encode-encode-phase.05,
  state-churn-encode-encode-phase.06,
  state-churn-encode-encode-phase.07,
  state-churn-encode-encode-phase.08,
  state-churn-encode-encode-phase.09,
  state-churn-encode-encode-phase.10,
  state-churn-encode-encode-phase.11,
  state-churn-encode-encode-phase.12,
  state-churn-encode-encode-phase.13,
  state-churn-encode-encode-phase.14,
  state-churn-encode-encode-phase.15,
  state-churn-encode-encode-phase.16,
  state-churn-encode-encode-phase.17,
  [state-churn-encode-encode-phase.18](state-churn-encode-encode-phase.18.md),
  [state-churn-encode-encode-phase.19](state-churn-encode-encode-phase.19.md),
  [state-churn-encode-encode-phase.20](state-churn-encode-encode-phase.20.md),
  [state-churn-encode-encode-phase.21](state-churn-encode-encode-phase.21.md),
  [state-churn-encode-encode-phase.22](state-churn-encode-encode-phase.22.md),
  [state-churn-encode-encode-phase.23](state-churn-encode-encode-phase.23.md),
  state-churn-encode-encode-phase.24,
  [state-churn-encode-encode-phase.25](state-churn-encode-encode-phase.25.md),
  state-churn-encode-encode-phase.61,
  state-churn-encode-encode-phase.62,
  state-churn-encode-encode-phase.63,
  state-churn-encode-encode-phase.64,
  state-churn-encode-encode-phase.65,
  state-churn-encode-encode-phase.66,
  snapshot-cache-snapshot.04,
  snapshot-cache-snapshot.05,
  snapshot-cache-snapshot.06,
  snapshot-cache-snapshot.07,
  snapshot-cache-snapshot.08,
  snapshot-cache-snapshot.09,
  snapshot-cache-snapshot.10,
  [snapshot-cache](../snapshot-cache/index.md),
  [present-pacing](../present-pacing/index.md)


### From Domain index

| Domain | Role | Headline verdict |
|--------|------|------------------|
| [baselines](../baselines/index.md) | frame120 / frame50 / frame60 reference captures | shape stable across regimes |
| [hidden-backend-storage](../hidden-backend-storage/index.md) | TVB/parameter storage model, VS-write density, scaling | model ACCEPTED; dominant sub-component OPEN; visible `VSOut` gate rejected in [hidden-backend-storage-shape.08](../hidden-backend-storage/hidden-backend-storage-shape.08.md), stale live-vsout smoke closed in [hidden-backend-storage-shape.13](../hidden-backend-storage/hidden-backend-storage-shape.13.md), backend escape feasibility triaged in [hidden-backend-storage-shape.14](../hidden-backend-storage/hidden-backend-storage-shape.14.md), Tile-FFP hot-row coverage rejected in [hidden-backend-storage-shape.15](../hidden-backend-storage/hidden-backend-storage-shape.15.md), stream/IB handle identity rejected in [state-churn-encode-stream.09](state-churn-encode-stream.09.md), seq-range System Trace route attribution accepted in [hidden-backend-storage-shape.28](../hidden-backend-storage/hidden-backend-storage-shape.28.md), encoder-summary route sidecars enabled in [hidden-backend-storage-shape.29](../hidden-backend-storage/hidden-backend-storage-shape.29.md), GPU floor vs wall-clock owner split accepted in [hidden-backend-storage-shape.30](../hidden-backend-storage/hidden-backend-storage-shape.30.md), current System Trace refresh accepted in hidden-backend-storage-shape.31, current shader-dump liveness refresh keeps generic varying trim closed in [hidden-backend-storage-shape.35](../hidden-backend-storage/hidden-backend-storage-shape.35.md) |
| [tvb-mechanism-proof](../tvb-mechanism-proof/index.md) | VS-inv ↓ → TVB write ↓, row-local + full-frame | ACCEPTED (load-bearing) |
| [index-cache-locality](../index-cache-locality/index.md) | opaque-depth cache, screen-blend, min-gain, CPU cost | opaque-depth WIN with refreshed frame60 proof; gate-shape scout says hot-row CPU waste is valid candidate build/lookup, not failed-gate churn; screen-blend target movement passes but aggregate top-GPU proof fails by non-target timing drift |
| [index-reuse-measurement](../index-reuse-measurement/index.md) | index reuse, geometry signature/size, state-class | VS-inv tracks cache-miss estimate |
| [primitive-reorder-diagnostics](../primitive-reorder-diagnostics/index.md) | reverse/min-index/split reorder probes | order = frame-shape artifact, not stable owner |
| [mini-replay-bisection](../mini-replay-bisection/index.md) | row-local replay + encoder bisection | reproduced amplification; enabled the proof |
| [vsout-layout](../vsout-layout/index.md) | visible varying width attempts | all REJECTED as owner |
| [shader-codegen](../shader-codegen/index.md) | temp/scratch trim, offline Metal IR | REJECTED; owner below AIR |
| [backend-shape-classifiers](../backend-shape-classifiers/index.md) | alpha/depth/cull/scissor/fog/texture/expand | REJECTED/secondary; indexed path mandatory |
| [attachment-pixelformat](../attachment-pixelformat/index.md) | R32F / X8 PixelFormatView suppression | secondary (texture-write), not VS owner |
| [const-upload](../const-upload/index.md) | cbuf/argbuf class/volatility/dirty-range/sparse | CPU amplifier, GPU unmoved |
| [state-churn-encode](index.md) | stream/IB churn, draw-run, binding override | CPU wins, GPU flat; stream/IB handle-stable A/B accepted as diagnostic in [state-churn-encode-stream.08](state-churn-encode-stream.08.md), Xcode rejected handle identity in [state-churn-encode-stream.09](state-churn-encode-stream.09.md), argbuf broad-payload-hash reopen path rejected in state-churn-encode-encode-phase.63, dirty VS cbuf width attributed to usage-prefix/indexed fallback in state-churn-encode-encode-phase.64, generic non-indexed packed cbuf layout rejected in state-churn-encode-encode-phase.65, indexed BLENDINDICES window rejected for the hottest VS sample in state-churn-encode-encode-phase.66, Stage2 argbuf hybrid is CPU-negative in state-churn-encode-encode-phase.67, the low-overhead FPS gate rejects disabling Stage2 as the current average-FPS lever in state-churn-encode-encode-phase.68, adjacent full-uniform payload elision is rejected in [state-churn-encode-encode-phase.93](state-churn-encode-encode-phase.93.md), component generations clean up only the smaller PS half in [state-churn-encode-encode-phase.95](state-churn-encode-encode-phase.95.md), fixed-payload split storage narrows append bytes but not FPS/P2/P3 in [state-churn-encode-encode-phase.116](state-churn-encode-encode-phase.116.md), command-front uniform payload copies are removed but do not move FPS/P4 in [state-churn-encode-encode-phase.117](state-churn-encode-encode-phase.117.md), VS/PS stage split shrinks the payload record to `96B` but leaves aggregate append bytes dominated by VS constants in [state-churn-encode-encode-phase.118](state-churn-encode-encode-phase.118.md), stage constants are then compacted to the usage-live floor in [state-churn-encode-encode-phase.120](state-churn-encode-encode-phase.120.md) (`uniform_append_bytes_per_present=490,549.644`, stage amplification `0.971x`) without FPS/P4 promotion, [state-churn-encode-encode-phase.121](state-churn-encode-encode-phase.121.md) shows remaining backend legacy uniform scratch materialization is `17.542MB/present` but only `0.616ms/present`, state-churn-encode-encode-phase.122 cuts that scratch path to `12.345MB/present` / `0.449ms/present` by reusing command-front uniform scratch while P4 stays flat, [state-churn-encode-encode-phase.123](state-churn-encode-encode-phase.123.md) rejects dirty VS argbuf identity repoint (`862,747` probes, `0` hits), state-churn-encode-encode-phase.124 trims only argbuf table reserve (`0.196 -> 0.158ms/present`) without moving the parent, [state-churn-encode-encode-phase.125](state-churn-encode-encode-phase.125.md) removes PSO-prefetch legacy uniform materialization (`12.345 -> 9.011MB/present`, PSO state-copy `0.150 -> 0.016ms/present`) while encode/replay/P4 stay flat, [state-churn-encode-encode-phase.126](state-churn-encode-encode-phase.126.md) attributes the remaining materialization to draw-encoder command `36.81%`, queue observation `36.79%`, and draw-encoder param `26.40%`, [state-churn-encode-encode-phase.127](state-churn-encode-encode-phase.127.md) eliminates the queue-observation site (`0` bytes/cpu) via compact projected-texture state while P4 remains no-enqueue dominated, state-churn-encode-encode-phase.128 rejects lazy command materialization because command materialized draws stay flat at about `323/present`, [state-churn-encode-encode-phase.129](state-churn-encode-encode-phase.129.md) moves VS/FFPVS cbuf content-history scans behind `DXMT9_PERF_ENCODER_BREAKDOWN_CBUF_CONTENT=1` so default encoder breakdown does not self-report diagnostic observer CPU as a renderer owner, state-churn-encode-encode-phase.130 confirms current legacy uniform scratch is only `0.228ms/present`, state-churn-encode-encode-phase.131 refreshes the argbuf payload-delta probe with `changed_nonconst_only=0`, [state-churn-encode-encode-phase.132](state-churn-encode-encode-phase.132.md) rejects sharing a mutable argbuf table across changed cbuf pointers as last-write-wins, [state-churn-encode-encode-phase.133](state-churn-encode-encode-phase.133.md) classifies direct cbuf Stage 2 as a shader/PSO ABI project rather than a host-only microfix, [state-churn-encode-encode-phase.134](state-churn-encode-encode-phase.134.md) proves the remaining argbuf payload-delta churn is float-constant turnover (`changed_vs=843,136 == changed_vs_float`, int/bool `0`) rather than int/bool invalidation, [state-churn-encode-encode-phase.135](state-churn-encode-encode-phase.135.md) is superseded because the first width probe retained unsafe previous-payload scratch, [state-churn-encode-encode-phase.136](state-churn-encode-encode-phase.136.md) fixes ownership and shows a mixed width distribution (`76.86%` of VS-float-changed rows `<=16`, `21.48%` `>64`, VS upload/changed-byte ratio `1.13x`), [state-churn-encode-encode-phase.137](state-churn-encode-encode-phase.137.md) shows the `>64` VS tail owns `91.07%` of changed registers, [state-churn-encode-encode-phase.138](state-churn-encode-encode-phase.138.md) shows those wide changes are mostly contiguous full-prefix churn (`span / changed = 1.057x`, full-prefix `78.46%` of regs), [state-churn-encode-encode-phase.139](state-churn-encode-encode-phase.139.md) attributes the full-prefix churn to a few shader-pair buckets, and [state-churn-encode-encode-phase.140](state-churn-encode-encode-phase.140.md) shows PE dirty-span flush merging, not full-range app setters, creates many wide VS const records. Next local cbuf experiment is sparse VS constant dirty-run flushing; present-pacing-lowoverhead-serial.24 still requires future CPU wins to move P4 wait/overlap before claiming FPS ownership |
| [snapshot-cache](../snapshot-cache/index.md) | D3D9 draw-state snapshot rebuild | historical CPU owner; current P2/P3 residual after direct-cbuf. Stream/IB miss-reason counts are not a new binding-only owner because pure binding invalidation does not bump `drawStableStateGeneration_`; redundant shader constant no-op invalidation is fixed but GT1 still has zero adjacent uniform-generation reuse; the latest direct-cbuf scout leaves lookup `2.859ms/present` and batch miss `2.162ms/present`; batch-only miss reasons show texture in `75.006%` of batch misses, but tuple counters show mixed rows are dominated by `shader+FVF/VDecl`, so texture-only is a scoped candidate, not the whole fix; whole-payload reuse is only a small cleanup (`-4,752` batch uniform builds, lookup `2.850 -> 2.843ms/present`), so the next owner remains batch-miss count/co-churn, hot-state storage, compact/interner work, or P4 overlap; see snapshot-cache-snapshot.21, [snapshot-cache-snapshot.22](../snapshot-cache/snapshot-cache-snapshot.22.md), [snapshot-cache-snapshot.23](../snapshot-cache/snapshot-cache-snapshot.23.md), [snapshot-cache-snapshot.24](../snapshot-cache/snapshot-cache-snapshot.24.md), and [snapshot-cache-snapshot.25](../snapshot-cache/snapshot-cache-snapshot.25.md) |
| [render-pass-store](../render-pass-store/index.md) | RT/depth re-entry, store DontCare, pass-chain | re-entry real; dominant top rows are immediate role-pair A/B/A target reuse; coalescing OPEN |

Latest state-churn visual update: [state-churn-encode-encode-phase.172](state-churn-encode-encode-phase.172.md)
compares the current h199 black-foreground window against the `v0.0.3` h196
release capture at the same HUD time class. The same dark foreground /
silhouette class exists in the visual-safe tag, so this sampled window is not a
standalone correctness regression.

Latest state-churn CPU update: state-churn-encode-encode-phase.193
runs the owned chunk-end pending-submission carry implementation under the
standard 120s no-gputrace foreground gate. The carry mechanism is valid:
`99.84%` of stored records are adopted and the chunk-end flush bucket falls
`0.817 -> 0.045ms/present`. It is not a bottleneck fix: replay stays flat
(`8.497 -> 8.492ms/present`), ready depth remains `1.000`, enqueue-during-wait
does not appear, and the saved end-drain work shifts into wider
`commit_chunk_draw_batch_submit_cpu_ms` (`1.714 -> 1.983ms/present`). Keep
`DXMT9_ENABLE_CHUNK_END_CARRY=1` default-off and do not spend `.gputrace` on
this candidate. The next useful branch is either submit-cost attribution plus
real N-1 materialization elision, or a locality-preserving P4 overlap design
that creates overlap without breaking CB/pass/tile locality or the `v0.0.3`
visual gate. [state-churn-encode-encode-phase.194](state-churn-encode-encode-phase.194.md) adds the next attribution
counter family for that branch:
`commit_chunk_replay_pending_flush_forced_resource_marking_*`.

Latest queue-submit attribution:
[state-churn-encode-encode-phase.195](state-churn-encode-encode-phase.195.md) reframes the current "wall" question as
closed local copy/carrier branches versus the still-open P4/no-enqueue and
replay/encode owners. [state-churn-encode-encode-phase.196](state-churn-encode-encode-phase.196.md) then runs the
first discriminator and rejects queue mutex acquisition:
`submit_draw_run_batch_queue_lock_cpu_ms_per_present=0.018` while
`completion_wait_without_enqueue_ms_per_present=27.837`,
`commit_chunk_replay_cpu_ms_per_present=8.424`, and
`encode_chunk_cpu_ms_per_present=11.249` remain in the current-head class. The
H225 visual captures are coherent and the run has
`draw_skipped_no_pipeline=0` / `gpu_command_buffer_errors=0`. Do not treat queue
lock tuning as the next GT1 average-FPS lever. The follow-up summary-only
reanalysis in [state-churn-encode-encode-phase.197](state-churn-encode-encode-phase.197.md) rejects the broad
outer-submit-unknown branch as well: known child scopes explain `89.96%` of
`commit_chunk_draw_batch_submit_cpu_ms`, append is `76.63%` of that parent, and
append is mostly uniform (`51.51%`) plus state (`26.00%`). The remaining local
branch is append materialization width or the larger snapshot/cache branch.
[state-churn-encode-encode-phase.198](state-churn-encode-encode-phase.198.md) then narrows the uniform half: payload
lookup plus payload-record append storage explain only `38.02%` of uniform
append, leaving `0.411ms/present` in stage-level uniform append work. This keeps
N-1 materialization elision or stage-level uniform append reduction alive as
local CPU cleanup, but the FPS branch still requires a render-pass-safe
P4/run-ahead design. [state-churn-encode-encode-phase.199](state-churn-encode-encode-phase.199.md) adds the missing
fixed/VS/PS component find/append split for the next no-gputrace run; old H225
data predates those counters and intentionally reports the new rows as `n/a`.
[state-churn-encode-encode-phase.200](state-churn-encode-encode-phase.200.md) then implements the resulting
fixed-payload handle carry and cuts the targeted component
`0.229 -> 0.150ms/present`, but `uniform_append_parent_cpu_ms_per_present`
stays flat (`0.882 -> 0.880`) and the P4 class does not move, so it remains
local cleanup rather than a wall-breaking FPS lever.
[state-churn-encode-encode-phase.201](state-churn-encode-encode-phase.201.md) then puts a ceiling on the remaining
uniform append branch: known scopes plus component scopes explain `77.75%` of
the `0.880ms/present` parent, residual is `0.196ms/present`, and the largest
named child left is VS stage append at `0.116ms/present` with `661,640` VS
stage records. That makes further uniform append work optional local cleanup,
not an Xcode/gputrace candidate by itself. The FPS branch remains P4/no-enqueue
overlap or a larger replay/encode materialization change that moves serial rows.

## Direct-Cbuf Generality Gate — relocated from the root overview 2026-07-29

This block lived in `docs/perfomance/overview.md` under
"Direct-Cbuf Generality Gate". It is per-experiment promotion detail owned by
this domain, so it was moved here on 2026-07-29; the root overview keeps only
the one-paragraph default-policy statement. Text is unchanged:

> A 2026-07-20 same-build ABBA remeasurement validates
> `DXMT9_ARGBUF_DIRECT_CBUF=1` as a cross-workload constants-only Stage 2 CPU
> cleanup. Across GT1, GT2, GT3, and SFIV, draw encode falls `20.7-32.6%`, chunk
> encode falls `12.6-24.9%`, slot-30 argbuf setup/binds become zero, sampled FPS
> changes only `+0.32%` to `+1.15%`, and every run has zero GPU errors. The
> targeted GT3 `1:07.66` capture is visually normal. After a deterministic
> payload-source dirty-rebind regression closed the remaining correctness gate,
> the constants-only path was promoted default-on; explicit value `0` retains
> the rollback lane. Phase-sampled GPU p50 increases in GT3/SFIV, so this remains
> a CPU-path promotion rather than an FPS/GPU claim. Resource-array mode
> intentionally retains the mutable argbuf table. See the [cross-workload
> gate](state-churn-encode-encode-phase.202.md) and [correctness
> gate](state-churn-encode-encode-phase.203.md).

Evidence status, checked 2026-07-29. The correctness gate
([state-churn-encode-encode-phase.203](state-churn-encode-encode-phase.203.md))
keeps every cited artifact. The cross-workload gate
([state-churn-encode-encode-phase.202](state-churn-encode-encode-phase.202.md))
does not: its GT1/GT2/GT3 and SFIV `off`/`on` ABBA run directories and the GT1/
GT2/GT3 `direct-cbuf-vs-off-r1.md` comparison reports are gone from disk. Only
`experiments/output/app-d3d9-3dmark05-direct-cbuf-generality-gt3-visual-67s-retry-20260720`
and the SFIV `on` trace analysis survive. The leaf is unmarked because it is not
*wholly* evidence-missing, but the `20.7-32.6%` / `12.6-24.9%` figures above are
last measurements and cannot be re-derived from what remains. Treat the
default-on decision as standing and the percentages as historical.
