# 3DMark05 GT1 Performance — Investigation Map

> Root node of the `docs/perfomance/` knowledge graph. This is the
> authoritative cross-referenced set of domain overviews and
> one-experiment-per-file leaf nodes. The old append-only
> `specs/perfomance.plan.md` journal has been deleted/retired and must not be
> maintained as a performance source.

## The root question

**Why is 3DMark05 GT1 GPU-bound under dxmt9 (a D3D9→Metal translation
layer on Apple Silicon), and what owns the cost?**

Target: `app-d3d9-3dmark05`, GT1 path under `DXMT_EXPERIMENT_PROFILE=perf`.

## Central finding (read this first)

The top-3 render encoders dominate every captured frame (~98% of GPU
time) and write a large **"VS Buffer Device Memory Bytes Written"**
bucket (~1.6 GiB at frame60, ~1.0–2.2 GiB depending on capture). That
bucket is **not** explained by:

- dxmt CPU-side writers (argbuf/transient/cbuf ≈ 0.4 MiB), nor
- visible MSL `VSOut` width (184 B), nor
- AIR-visible shader scratch (128 B).

The settled part is the **scaling law**: this is hidden Apple GPU
vertex-stage / tiler / parameter-buffer (TVB/PB) backend storage that scales
with **VS invocation count × hidden per-invocation backend storage**. The
numerator is now actionable; the denominator is still open. Visible `VSOut`
width is only a source-visible lower bound, not the measured storage width. See
[[hidden-backend-storage]] for the model and [[tvb-mechanism-proof]] for the
accepted invocation-reduction proof.

That distinction matters for the hardware ceiling. At the current shape,
`~1.6 GiB/frame` of VS writes at `~22 fps` is already about `37 GB/s` before
texture reads, depth/color traffic, tile stores/loads, and CPU/GPU coherence.
On a base M1-class `~68 GB/s` memory system, VS write alone can consume roughly
55% of peak bandwidth, so this can be a true bandwidth limiter. On M1
Pro/Max/Ultra-class memory systems (`~200-400 GB/s`), the same measured byte
rate is less likely to be the whole limiter; the bottleneck class is SKU-
dependent.

**The one accepted production win** is opaque-depth **index-cache
locality** ([[index-cache-locality]]): reordering indices for opaque
depth-writing triangles improves the post-transform vertex cache, which
lowers VS invocations, which linearly lowers TVB write. Historical target rows
proved GPU `-18.4%`, VS invocations `-14.1%`, VS write `-16.8%`; the refreshed
frame60 proof reattaches the same mechanism to current rows `60/0+60/1`
(`-10.64%` target GPU, `-14.12%` VS invocations, `-16.77%` VS write).
The current fast-measure implementation passes the strong Xcode proof gates,
but remains an opt-in rather than a shared `perf` default: the non-diagnostic
smoke still adds about `+216ms` of total encode-draw CPU / `+301ms` of
index-setup CPU over a 1440-present run, and the narrow source-resolve counter
shows the owner is cache/candidate/draw-path work rather than base
index-buffer lookup.
The strongest historical measured path combines that production-shaped subset
with screen-blend locality (top GPU -11.89%), but screen-blend is only an
explicit exact/`lsb1` policy artifact, not a broad depth-read rule. The current
screen-blend proof attempt now has semantic input and target-row Xcode movement,
but it still does not promote: `60/2` improves while the aggregate top-GPU gate
fails. Follow-up row telemetry shows the path only applies to `60/2`; the
`60/0+60/1` regression is GPU-time-only replay variance rather than
reordered-cache mutation on non-target rows.
Rifle muzzle fire is no longer best described as "not submitted" or "globally
absent." The public `01:05` oracle shows the expected effect as simple
weapon-attached circular white/yellow bloom discs; the user-captured reference
frame has several infantry rifle shots rendered as compact round bloom discs
rather than long tracer strips, impact sparks, or broad haze. Current local
artifacts now have matching positive samples in the wide infantry window, so the
remaining bug should be scoped as frame/timing/ROI-specific final-writer loss
rather than global rifle-fire absence. The latest local reruns reclassify the
old absence as a dynamic-buffer
backing correctness issue: when a
queued draw kept only the logical `BufferHandle`, a later dynamic
`D3DLOCK_DISCARD` rename could make encode bind a newer active backing. The
current implementation records the concrete Metal backing in a separate
per-draw `DrawBindingSnapshot` payload and lets DEFAULT+DYNAMIC DISCARD rotate
again, while snapshot-bearing draws bind the recorded Metal buffer instead of
resolving the mutable active handle at encode time. `DrawBindingOverride`
remains a compact logical stream/IB delta; snapshot storage is separate so the
old draw-run coalescing path does not pay snapshot bytes on every override-only
draw. This remains a correctness/performance gate, not a cosmetic side issue:
visual parity now needs to be rechecked under the optimized snapshot path, and
FPS should be interpreted only with the visual-coupling counters for skipped
draws, Metal errors, fallback/overflow, render-pass churn, and completion
waits.
Because previous large white bloom mistakes moved performance materially, and
recent glow/bloom correctness fixes appear to bring small timing gains, the next
proof gate is visual parity / final-color writer isolation before more paired
Xcode performance budget. Treat a visual-fix timing gain as actionable only
after the perf summary's `Correctness / Visual-Coupling Counters` shows whether
the change also reduced skipped draws, Metal errors, hazard/probe churn,
fallback/overflow counters, render-pass churn, or completion waits. See
[[backend-shape-classifiers-alpha.04]]. The current visual-coupling frame60
smoke narrows the obvious wrong-path branch: skipped draws, Metal command-buffer
errors, tracked frame60 overflows, map-buffer GPU waits, and queue-sequence waits
are zero. All bloom hazards are false positives and `render_split_hazard=0`, so
the bloom prefilter is noisy but not a false render-split owner. Render-pass
preservation remains high, and the actual split reasons are RT/depth changes,
clears, and presents, so the correctness/perf coupling is not closed. A follow-up
ROI final-color comparison on the close-up captures shows `0x80`/`0x82`
force-white candidates affect broad overlay/tint/beam color, but still do not
create a local rifle muzzle sprite in that older close-up branch. The follow-up
ROI geometry join makes the shape clearer:
`0x80`/`0x82` are screen-space/fullscreen glow quads that cover the muzzle ROI by
construction, while `0x7f` fire-atlas projected bboxes overlap comparable ROIs
only in the later rifle-window probe and remain bbox-level evidence, not
final-color proof for the close-up missing muzzle flash. A regenerated close-up
`0x77` geometry run (`seq 477..560`) with command-index logging found only `9`
muzzle ROI overlaps with max coverage `5.586%`, plus a draw-local force-white
queue. The rank-1 command-index force-white replay still reported
`encode_draw_pso_prefetch_bypass_probe=0`, so independent-run ordinal/command
slot selectors are not yet a final-writer oracle. A follow-up replay without
the command-index gate but with an ROI scissor did apply
`encode_draw_pso_prefetch_bypass_probe=11`, proving the row/texture/draw
selector can hit; its image deltas were still dominated by independent-run
frame drift, not a clean local muzzle delta. This further classifies `0x77` as
thin tracer/impact geometry rather than the missing radial rifle muzzle bloom,
and moves the next proof toward same-run final-writer instrumentation or direct
gputrace draw inspection. A later `0x80` component-local force-white attempt
(`app-d3d9-3dmark05-rifle-frame1033-tex80-local-r03-frame1036-component1-tex80-s1036-e2-d1-ci337`)
must be treated as invalid evidence: the component/geometry gate promoted
`frame1036-component1` (`0x80`, bbox coverage `96.133%`) as a plausible
round-bloom candidate, but the replay summary reports
`probe_force_texture_white_draws=0` and only `21` draws in `seq=1036/enc=2`,
so the scoped selector did not actually hit the intended command. The resulting
image sequence drifted into a different close-up/bloom moment and cannot be
used as an A/B proof. The follow-up same-run payload/mini-replay gate
refines that again: six `517/2` `0x77` payload draws render `577` nonzero
orange/white pixels in standalone Metal with the real texture sidecar, the
force-fragment-color replay has the same footprint, and `depth_clear=0.0`
rejects all pixels. Therefore `0x77` is not a blank or globally skipped draw
class. A same-run depth sidecar for `517/2` then narrows the depth branch:
captured depth preserves `542 / 577` pixels and the force-color replay has the
same captured-depth footprint, so depth is relevant but not the full reason this
candidate disappears from the final close-up frame. A same-run pass-end color
sidecar for a regenerated `517/2` payload run then captured the same `0x77`
draw window (`11` `0x77` probe rows, payload draws `267..272`) and found no
bright final-color trace: the mini-replay bbox max was `[206,199,174]`, the
muzzle ROI max was `[170,164,140]`, and the full `X8R8G8B8` color attachment
had `0` pixels with any channel above `220`. So `0x77` is submitted and
renderable in isolation, but it is not currently proven to be the pass-end
final writer for the missing rifle muzzle bloom. The first draw-boundary color
history probe strengthens that: when gated by `seq=517/enc=2` and
`texture0=0x200000100000077`, the selected draw writes bright pixels immediately
after draw (`bbox` max `[255,255,252]`, `27` bright pixels, `7` white pixels),
but the prior pass-end sidecar has no channel above `220`. A current-build
follow-up made the split effect explicit: a texture-list after-draw history
requested `0x77` and `0x80`, matched only `0x77`, and wrote `10` bright
after-draw sidecars at command index `306`; the last sidecar still had
`[255,255,250]` in the mini bbox and `273` pixels with any channel above `220`.
A split-free current-build pass-end dump for `seq=517/enc=2` stayed dark
(`mini bbox max [207,199,175]`, full attachment bright pixels `0`). So the
stronger statement is not just "a later draw overwrites it": the after-draw
diagnostic split can materialize bright intermediate pixels, while the normal
render pass final store does not preserve the expected local muzzle contribution.
A command-attributed rerun then found `0x77` split sidecars at commands
`319/320` with only non-local full-frame bright pixels, and `0x80` at command
`322` with cyan/white full-frame glow pixels but still zero bright pixels in the
mini/muzzle ROIs. A non-mutating `0x80` geometry census confirms why ROI overlap
alone was misleading: the `0x80` class is a fullscreen screen-space post/glow
quad in the close-up window, not a local muzzle sprite. Draw/blend/depth order,
tile load/store/preservation behavior, the diagnostic split changing pass shape,
animation-time mismatch, run-local selector drift, or a separate unidentified
muzzle draw are now stronger suspects than "not submitted" or "depth alone
rejects it."

A later correction lowers the earlier `seq=517` evidence: visual inspection now
shows that frame is not the foreground close-up rifle muzzle frame, so the
`517/2` artifacts are selector/pass-shape evidence rather than close-up final
writer proof. The close-up s820 rerun is the current anchor, but the oracle is
`analysis/captures_png/frame000820.png`, not the run-level `actual.png`:
`actual.png` is a later HUD frame (`984`) and contains a separate working
machine-gun muzzle flash. With the corrected rifle muzzle ROI
`620,200..770,330`, the s820 backbuffer pass-end has no local muzzle result
(`max [94,102,99]`, bright `0`, white `0`, warm `0`). The wider forward ROI
`600,180..800,360` is also warm/white zero. The effect census has `602` indexed
triangle rows with no point-sprite candidates, and the previously suspected
fire-atlas family `0x75/0x76/0x77/0x7f` is absent in that sample. The visible
s820 candidates are material/post/shadow classes: `0x5a` DXT1 material,
`0x8b` blue glyph/mask, `0x8d` R32F shadow/depth, and `0x8c/0x8e` scene/post
textures. The earlier s820 after-draw history was scoped to right-shifted ROIs
and produced only tiny weapon/post highlights, not a radial orange/white rifle
muzzle flash. A corrected after-draw ROI rerun (`ci0..260`, `579` rows per ROI,
commands `1..225`) has `warm_rows=0`, `white_rows=0`, and `max_warm=0` for both
the corrected muzzle and wider forward ROIs; its `bright_rows=243` are cyan/post
false positives (`max=(128,255,255)`, `warm=0`). Use the normal
YouTube/demo/working-machinegun flash as a visual shape oracle, then validate
with corrected final-color/warm ROIs. Public YouTube GT1/demo footage is only a
shape/event oracle; local promotion still requires a same-frame final-color
writer for the weapon-attached muzzle pixels. The YouTube demo shows infantry
rifle flashes around `00:01:00.6..00:01:05` as simple circular white/yellow
bloom discs attached to the muzzle, smaller than the machine-gun plume but with
the same saturated-core/post-bloom behavior. The user-captured `01:05` frame
from `JbKmFz6v9uk` is the clearest public reference: several rifle shots render
as round bright discs, not as long tracer strips and not as tiny isolated
pixels. The similar crouched close-up around `00:01:18` has no muzzle bloom, so
that frame remains a negative timing sample. A clipped YouTube analysis window
(`00:01:00..00:01:05`) recorded positive oracle frames at about `60.6s`,
`61.5s`, `61.9s`, and `63.3s`; with the `01:05` screenshot added, the expected
shape should be treated as a weapon-attached circular white/yellow bloom disc,
not merely any warm final pixel.
The DISCARD-wait scout aligned with that oracle in the firing window:
`app-d3d9-3dmark05-rifle-muzzle-oracle-0105-r1` at HUD `Time 0:59.56` shows
live rifle/impact flashes, while `app-d3d9-3dmark05-rifle-muzzle-oracle-0105-r2`
at HUD `Time 1:05.70` is already after the local flash event. The targeted
`app-d3d9-3dmark05-rifle-muzzle-oracle-0105-r3` capture at HUD `Time 1:04.10`
shows strong weapon/effect bloom in the same public-oracle scene. Its
`texture0=0x200000100000080` draw at `seq=964/enc=2/cmd=387/draw=399` is a
valid two-triangle sprite (`primitive_count=2`, `vertex_count=6`,
`screen_min=(879.263,332.626)`, `screen_max=(893.206,346.57)`) using the
working flash shader pair `VS 0xcc8eea2d38e22c96` /
`PS 0x6eac62f18235c99a`. This lowers the old "final writer unidentified"
hypothesis for the current build: the more likely owner is the dynamic
DEFAULT-vertex-buffer DISCARD/rename path, where queued draws previously kept
only the logical `BufferHandle` and could resolve a newer active backing at
encode time. The follow-up implementation now stores per-draw concrete stream/IB
backing snapshots in a separate `DrawBindingSnapshot` payload, marks the
selected rename ring entry's `lastUsedSeqId`, and makes encoder stream/index
binding prefer the snapshot Metal handle and contents bytes. The logical
`DrawBindingOverride` payload stays compact and only carries stream/IB deltas.
The first no-gputrace optimized-path scout below confirms the public round-bloom
visual shape; the remaining proof is whether it reduces the old DISCARD
serialization cost rather than merely moving work into pacing/completion waits.
The local positive machine-gun run (`seq=984`) confirms the same warm oracle:
warm starts at `cmd=182/enc=504` on the `0x7f` fire-atlas source and is then
carried through the post chain (`0x8c/0x8e/0x8a/0x8b`), reaching `36,548` warm
pixels / `25,201` white pixels in the wide ROI. The corrected s820 history
passes through post textures but never sees the `0x7f` warm source in the rifle
ROI, so the current issue is better framed as missing/wrong source-sprite
selection, animation timing, coordinates/state, or a separate unidentified
draw, not a globally broken bloom post chain. A follow-up normal-capture scout
over `900..1180`, then a dense `1080..1120` wide-scene window, found active
tracers/glare but no clean YouTube-style local infantry muzzle bloom. The
`seq=1092` after-draw ROI summary reached commands `1..260` across four
small-muzzle/glare ROIs and contained zero `0x7f` rows in that sampled
diagnostic; warm hits were dominated by `0x8d`/material/tracer/glare classes.
A denser local capture (`1086..1098` step 1) still classifies top warm/white
final pixels as tracer/impact/glare contamination rather than the external
public-oracle rifle muzzle bloom.

After the concrete rename-snapshot implementation, a no-gputrace visual scout
`app-d3d9-3dmark05-rename-snapshot-rifle-oracle-range1086-1098-r1` captured
`frame001086..001098` under the optimized path. The run still timed out at the
wrapper watchdog and has no Xcode proof, but the image series is visually
useful: large machine-gun bloom is present at `frame001086..001096`, and
`frame001098` (`Time 1:03.09`, HUD frame `1090`) shows multiple small circular
white/yellow rifle muzzle bloom discs on the right-side soldiers, matching the
public `01:05` oracle shape. A shaped warm/white component pass over the same
frame records the clearest right-side rifle disc as `component 11`, bbox
`776,343..796,364`, aspect `1.05`, fill `82.14%`, with a saturated white core.
That makes the local positive visual reproducible by artifact rather than only
manual inspection. Counters stayed quiet for the correctness/error branch
(`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`,
`render_split_hazard=0`, `map_buffer_wait_ms=0.000`); `queue_sequence_wait_ms`
was `333.901ms` in this capture-range run, so completion/pacing remains a
separate perf axis, not a buffer-map serialization regression.
A current same-run rerun with effect geometry,
`app-d3d9-3dmark05-rifle-oracle-positive-effect-geometry-r1`, keeps that visual
shape reproducible under the split-payload model and ties the cleanest local
component to a concrete source row. The shaped scanner finds
`frame001094/component1` in the right-rifle oracle window, bbox
`767,344..790,368`, aspect `1.04`, fill `79.71%`, `warm=440`,
`white=254`, max `[255,255,255]`. The seq-matched geometry join promotes
`texture0=0x200000100000080` at `seq=1094/enc=2/cmd=319`, primitive count `2`,
with `roi_coverage=100%` and `bbox_coverage=19.241%`; `0x7f/0x75` remain
`blocked-local-non-source` for the same components. This makes `0x80` the
current local rifle-bloom source family, later confirmed by after-draw color
history, while the old fire-atlas interpretation should stay scoped to broad
frame-wide overlap unless it survives the same local bbox gate. The run still
has no gputrace/Xcode proof, but its
visual-coupling counters are clean enough for a no-gputrace gate:
`present_encoded=1680`, `draw_skipped_no_pipeline=0`,
`gpu_command_buffer_errors=0`, `render_split_hazard=0`,
`map_buffer_wait_ms=0.000`, `queue_sequence_wait_ms=334.736`, and split-payload
binding override traffic `84,983,920` bytes. The remaining perf question is not
"is rifle bloom globally absent"; it is whether this correctness path changes
the hot GPU rows or only fixes a small alpha/effect sprite while the main
bandwidth/TVB and render-pass-store owners remain elsewhere.

A direct force-white replay of the top row
(`app-d3d9-3dmark05-rifle-oracle-positive-tex80-local-r01-frame1094-component1-tex80-s1094-e2-d1-ci319`)
did not promote that candidate into proof. The run timeout-finalized and
captured frames, but its perf summary shows `probe_force_texture_white_draws=0`
and `encode_draw_pso_prefetch_bypass_probe=0`; the indexed-probe CSV has only
the header, and `seq=1094/enc=2` reports only `20` draw calls with
`alpha_blend_textured_draws=0`. Its `frame001094` capture also drifted from the
wide infantry scene into the close-up machine-gun scene. So the
`1094/2/cmd319` command queue remains a same-run geometry target, not an
independent A/B image proof.

A follow-up same-run after-draw color-history probe confirms the local writer:

- `app-d3d9-3dmark05-rifle-oracle-tex80-afterdraw-color-noenc-r1`
- artifacts:
  `traces/app-d3d9-3dmark05-rifle-oracle-tex80-afterdraw-color-noenc-r1/analysis/color-history-summary.md`,
  `traces/app-d3d9-3dmark05-rifle-oracle-tex80-afterdraw-color-noenc-r1/analysis/tex80-afterdraw-crops.png`
- first `0x80` draw: `seq=1094/enc=2/draw=297/cmd=319`, but
  `round_bloom_candidate` has `bright=0`, `white=0`, `warm=0`
- second `0x80` draw: forced split moves it to
  `seq=1094/enc=3/draw=0/cmd=320`; `round_bloom_candidate` records
  max `[255,254,252]`, `bright=706`, `white=196`, `warm=909`

This makes the two-triangle `0x80` sprite the after-draw writer for the
public-oracle-shaped rifle bloom. The after-draw crop visually matches the
user-provided `01:05` oracle: a compact circular white/yellow bloom disc at the
barrel tip, not a flame mesh, tracer strip, or broad haze. The earlier
`enc=2/cmd319` color-dump miss was an instrumentation trap: the first matching
after-draw dump split the pass, so the adjacent small bloom draw moved to the
next encoder. This confirms the visual-correctness source, but it is still a
diagnostic split, not an Xcode GPU-counter proof for the GT1 hot rows. The
performance interpretation after this proof is narrower: the confirmed muzzle
source is a tiny local sprite, so it does not by itself explain the current
8-22fps envelope. It remains a required visual parity gate because wrong
pass/order/blend/store behavior can hide submitted pixels while still paying
draw and preservation cost; the primary residual perf owners still point at
hidden TVB/PB writes, render-pass store/re-entry traffic, and completion/present
pacing.

A follow-up split-payload scout
`app-d3d9-3dmark05-rename-snapshot-splitpayload-scout-r1` kept the optimized
path but separated concrete backing snapshots into `DrawBindingSnapshot` instead
of bloating every `DrawBindingOverride`. This run also timeout-finalized with
partial logs, and the single captured `frame001098` drifted to the machine-gun
close-up rather than the public rifle-oracle infantry scene, so it is not new
rifle-proof. It is useful for structure/perf sanity: binding-override payload
traffic dropped from the previous snapshot scout's `252,327,296` bytes to
`84,775,040` bytes for roughly the same 302k override records, while
`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`,
`render_split_hazard=0`, `map_buffer_wait_ms=0.000`, and
`queue_sequence_wait_ms=48.773ms`. The dominant wait remains
`completion_wait_ms=37,520.868`, so the next performance owner is still
completion/present pacing and GPU-side execution, not the old map-buffer wait.

A `frame60` `.gputrace` attempt remains blocked by capture-layer mechanics, not
by a dxmt9 draw/pipeline failure. File and `developerTools` destinations both
reported `startCapture failed` / `Capture layer is not inserted`; simply leaving
Xcode at the welcome screen did not insert the capture layer. A deliberate
`MTL_CAPTURE_ENABLED=1` smoke
(`app-d3d9-3dmark05-splitpayload-frame60-mtlcapture-r1`) produced a black
`actual.png`, no `result.json`, zero encoder rows, and no draw/present counters,
so that mode is not a valid performance sample for this app. Do not use it to
compare FPS or GPU time; either attach/launch through a real Xcode capture-layer
path, or keep using no-gputrace visual/perf scouts until a `.gputrace` can be
generated without changing the visual path.
A current-head phase43 recheck reproduced the same split after the latest
draw-state/resource-retention CPU work. The file destination
(`app-d3d9-3dmark05-phase43-frame60-gputrace-r1-20260613`) rendered normally
with `status=pass`, `present_encoded=1680`, and no timeout, but failed capture
with `destination=2 destination_supported=0` / `Capture layer is not inserted`.
The Xcode-open `developerTools` rerun
(`app-d3d9-3dmark05-phase43-frame60-xcode-devtools-r1-20260613`) also rendered
normally (`present_encoded=1740`) but failed with
`destination=1 destination_supported=0`. Therefore this branch still has no new
Xcode encoder-counter proof; treat both runs as normal-rendering counter samples
and capture-workflow negatives, not `.gputrace` evidence.
A sidecar Instruments capture does work without changing the visual path:
`app-d3d9-3dmark05-phase43-xctrace-system-r1-20260613` ran the supervised
no-gputrace wrapper and then recorded a 15s `Metal System Trace` with
`xctrace --all-processes`. Exported `metal-gpu-intervals` joined `3590/3590`
dxmt encoder rows by `RenderPass[seq=...,enc=...]`, covering
`seq=1394..1593`. The captured stage sum was `9303.143ms`, split
`8495.658ms` vertex and `807.485ms` fragment (`91.32%` vertex share). The top
rows were all `/11` large-geometry encoders (`16.299..21.459ms` stage sum) with
1.5M-1.86M vertices each, which reinforces the vertex-heavy bottleneck shape.
This still is not Xcode replay-counter proof: exported counter/shader-profiler
schemas were empty for the needed fields, so `VS Buffer Device Memory Bytes
Written` remains unavailable until a real `.gputrace` replay/export succeeds.
An in-place embedded-plist retry
(`app-d3d9-3dmark05-captureplist-frame60-gputrace-r1`) patched
`MetalCaptureEnabled=true` into copied `wine.capture.real` and
`wine.capture.real-preloader` binaries, then launched the normal Wine tree
without `MTL_CAPTURE_ENABLED=1`. This avoided the black-screen startup path and
rendered normally: `actual.png` at HUD `Time 0:58.77` / `Frame 1025` contains
visible circular white/yellow rifle/effect bloom in the wide infantry scene, and
the partial summary reports `present_encoded=1680`, `draw_calls=1234243`,
`draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`,
`map_buffer_wait_ms=0.000`, and `queue_sequence_wait_ms=0.000`. However,
`MTLCaptureManager` still logged `Capture layer is not inserted` at frame60 and
no `frame60.gputrace` was produced. Treat this as a cleaner negative capture
sample: embedded plist copies can be visually/runtime-safe, but they still do
not prove that the Wine temp child process has an inserted Metal capture layer.
The follow-up original-name replacement wrapper
(`scripts/tools/run_with_wine_metal_capture_layer.sh`) did prove that the temp
Wine launcher can receive `MetalCaptureEnabled`: the same mechanism wrote a
valid synthetic `perf-d3d9-present-loop` `.gputrace`, and the 3DMark05 temp
`/var/folders/.../winetemp.../wine.real` showed `Info.plist entries=13` with
`MetalCaptureEnabled`. 3DMark05 nevertheless black-screened before D3D9
draw/present (`bridge_draw=0`, `bridge_present=0`) and wrote no
`frame60.gputrace`. Treat capture-layer-inserted 3DMark05 startup as an
invalid evidence path; use no-gputrace counters, xctrace Metal System Trace,
DAG dumps, or a future attach-after-normal-start Xcode route instead. For new
Metal System Trace sidecars, prefer
`scripts/tools/run_3dmark05_system_trace_sidecar.sh -- ...`; it runs the probe
dry-run first, refuses locked sessions before starting xctrace, and then
exports/summarizes `metal-gpu-intervals` against the dxmt encoder CSVs.
The usable System Trace sidecar was repeated after the compact draw-state
submission work. `app-d3d9-3dmark05-post-compact-state-r1-20260613` joined
`2482/2482` encoder rows over `seq=1154..1389`; its stage sum was
`6346.966ms`, split `5701.647ms` vertex and `645.319ms` fragment (`89.83%`
vertex share). Its top rows are still large indexed `rt_change` encoders:
`1155/1` is `20.002ms` with `1,149,930` vertices, and the top 12 rows stay in
the `15.927..20.002ms` range with only sub-ms fragment time. This is not a
strict A/B against the `phase43` trace because the seq ranges and scene phases
differ, but it keeps the residual owner in the hidden TVB / primitive-binning /
backend-route lane rather than moving it to fragment, texture, or attachment
traffic. The regenerated sidecar aggregate sharpens that: `opaque-depth-indexed`
owns `68.25%` of stage time and `rt_change` owns `79.52%`, both with vertex
share above `92%`. Route verdicts are still unavailable for these artifacts
because their indexed probe draw CSVs are header-only; the next System Trace
sidecar must include indexed draw telemetry and must fail if those rows do not
join the traced encoder rows before selecting depth-only, textured, or color
backend routes. [[hidden-backend-storage-shape.27]]
The same artifact was run through the capture ROI/component gate. A tight
right-effect window found compact warm/white candidates, led by bbox
`1300,556..1370,602` with `warm=528`, `white=136`, and max `[255,255,255]`.
Relaxing the search shows that the surrounding effect can merge into a much
larger spark/glow component, so this frame is a visual-positive sample but not a
draw-owner-ready target. The visual-target gate currently reports
`blocked-components-no-local-writer` until same-run effect geometry ties the
component to a local `0x7f`/`0x75` source row.

That positive visual proof still does not identify the final writer for the
oracle's circular weapon-attached bloom. The first dense-candidate owner probe
(`seq=1097`, `ci0..260`) also contained zero `0x7f` rows in the sampled ROI
history; the suspicious right-soldier ROI topped out at `max_warm=53` on
`0x8d`, not the working fire-atlas source. A later non-mutating effect census
lowers the earlier source-absence interpretation: `0x7f` is present
frame-wide at `1086..1093` (`seq=1092 cmd=202`, four small screen-blend draws
with the working flash shader pair), while `0x75`/post-effect candidates appear
in `1094..1098`. The wide-scene bug is therefore not simply "fire atlas is
globally absent"; the missing proof is whether any submitted fire/effect draw is
the final-color writer for the YouTube-style local rifle muzzle bloom. Same-run
final-writer capture or direct Xcode draw inspection is the next gate. A first
same-run `seq=1092` capture/effect-trace probe confirmed four live `0x7f` draws
again, but its captured HUD was `Frame 1084` / `Time 1:02.89` in the large-gun
close-up rather than the intended wide infantry ROI scout. That makes frame/HUD
visual verification a required precondition before promoting any future ROI or
draw-owner result. A follow-up same-frame after-draw final-writer probe for
`texture0=0x7f,0x75` matched only `0x75`; it produced warm/white pixels in the
center and glare-control ROIs (`max_warm=1562` / `1714`), while the
right-soldier muzzle ROI stayed weak (`max_warm=47`, `max_white=0`). The
captured frame (`Time 1:03.21`, HUD frame `1084`) shows horizontal tracers/glare
rather than a local weapon-attached muzzle bloom, so this keeps `0x75` in the
beam/glare class for that earlier scout; the later same-run `0x80` after-draw
probe below resolves the current wide-scene local writer.
The external shape oracle is now anchored to James Mackenzie's `3DMark05 Demo
(4K 2160p)` page / YouTube video `JbKmFz6v9uk`: the useful evidence is the
GT1/demo `00:01:00..00:01:05` infantry window, especially the user-captured
`01:05` frame, where the expected rifle effect is a weapon-attached circular
white/yellow bloom disc with a saturated core and warm halo.
Treat the `01:05` frame as a compact round bloom oracle, not a flame-mesh
oracle: a correct rifle shot can be just a bright circular post-bloom disc at
the muzzle, similar in class to the machine-gun muzzle flash but smaller and
less plume-shaped.
This external oracle is a shape filter, not a pixel oracle: reject long tracer
lines, impact sparks, cyan beam/engine lights, broad haze, and warm background
panels unless the component is local to the barrel tip and short-lived across the
firing event. The local dense
component-to-geometry join reinforces the negative gate. Unfiltered
`1086..1098` component ROIs overlap many `0x7f` fire-atlas rows, but those are
huge projected bboxes with near-zero bbox coverage. After applying
`--min-bbox-coverage-pct 1`, only `0x5a` material and `0x8d` shadow/depth rows
survive; no `0x7f` or `0x75` local bloom writer remains in the current capture
set. The same local gate now feeds force-white queue planning: filtering for
the expected `0x20000010000007f`/`0x200000100000075` source textures with
`roi_coverage>=75%` and `bbox_coverage>=1%` yields an empty queue, while the
non-texture-filtered audit yields only four `0x5a` material candidates. This
current capture set should not be escalated to another Xcode/gputrace replay as
the rifle muzzle target. The combined visual-target gate now emits
`visual-target-gputrace=blocked-local-non-source`: `156` component rows are
present, but local overlap collapses to `0x5a:4` and `0x8d:1`, with `0` source
queue rows for `0x7f`/`0x75`. The corrected round-bloom component pass
(`aspect<=2.5`, `fill>=15%`) broadens the scanner to the public `01:05` oracle
and finds `221` compact components, but the seq-matched local effect join still
leaves only `9` non-source overlaps (`0x7b`, `0x01`, `0x17`, `0x5a`, `0x08`)
and `0` local `0x7f`/`0x75` source overlaps.

Almost every other hypothesis (visible varying width, shader temps,
render/raster state toggles, primitive reorder, const-upload size,
pixel-format views) was **rejected as "not the first-order owner."**
Several CPU-side reductions are real but orthogonal to the GPU limiter.
The current stream/IB branch is now a useful negative gate, not a GPU-side
denominator win: the preflight shows bounded stream0/stream1/IB tuple
alternation in frame60 hot rows ([[state-churn-encode-stream.05]]), the
row-scoped staging A/B proves `60/2` can be made handle-stable without changing
draw/PSO/argbuf shape ([[state-churn-encode-stream.08]]), and the Xcode
follow-up rejects handle identity as the first-order backend owner
([[state-churn-encode-stream.09]]). Stream/IB handle churn remains relevant for
CPU batching/encode work, but not for the current GT1 hidden-backend GPU
limiter. The follow-up per-draw PSO gate also finds no stream/IB-handle-stable
run where PSO changes independently, so current PSO movement is not an Xcode
counter target either ([[hidden-backend-storage-shape.18]]). The current full
gate now carries that result as `pso-backend-isolation=reject-current`, so
unisolated PSO motion is blocked in the next-experiment queue as well
([[hidden-backend-storage-shape.19]]).

## Domain map

```mermaid
flowchart TD
  Root["GT1 perf run\n~1260 presents / 913714 draws\nframe120 33.611ms GPU, top-3 = 98.4%"]

  Root --> Base["[[baselines]]<br/>frame120 / frame50 / frame60 reference captures"]

  %% GPU side
  Base --> GPU{{"GPU limiter:\ntop-3 encoders, memory/write bound\n(not ALU / texture-read)"}}
  GPU --> HBS["[[hidden-backend-storage]]<br/>TVB / parameter scaling model (ACCEPTED)"]
  HBS --> HBD["hidden denominator<br/>stage-out vs binning/PB vs spill (OPEN)"]
  HBS --> TVB["[[tvb-mechanism-proof]]<br/>VS-inv reduction -> TVB write reduction (ACCEPTED)"]

  %% rejected GPU ownership hunts
  HBS -.rejected owner.-> VSO["[[vsout-layout]]<br/>visible varying width"]
  HBS -.rejected owner.-> SCG["[[shader-codegen]]<br/>temp/scratch/offline IR"]
  HBS -.rejected/secondary.-> BSC["[[backend-shape-classifiers]]<br/>alpha/depth/cull/scissor/fog/texture/expand"]
  HBS -.secondary.-> APF["[[attachment-pixelformat]]<br/>R32F / X8 PixelFormatView"]

  %% measurement → reorder → the win
  HBS --> IRM["[[index-reuse-measurement]]<br/>VS-inv tracks post-transform cache miss"]
  IRM --> PRD["[[primitive-reorder-diagnostics]]<br/>reorder owns order? (frame-shape artifacts)"]
  IRM --> MRB["[[mini-replay-bisection]]<br/>row-local reproduction + bisection"]
  MRB --> TVB
  PRD --> ICL["[[index-cache-locality]]<br/>opaque-depth cache (THE WIN)"]
  TVB --> ICL
  IRM --> ICL

  %% Open backend mechanisms not yet proved by current probes
  HBD --> PBIN["Apple position/binning pass<br/>not tested by visible position-only VSOut"]
  HBD --> MESH["Metal 3 mesh/object path<br/>untried GT1 backend escape hatch"]
  HBD --> PSPILL["PSO/state churn backend spill<br/>current per-draw gate not isolated"]

  %% P1 GPU memory
  GPU --> RPS["[[render-pass-store]]<br/>same RT/depth re-entry, store DontCare (P1)"]
  GPU --> TFFP["DXMT9_TILE_FFP<br/>implemented but narrow/default-off FFP tile path"]

  %% CPU side
  Base --> CPU{{"CPU / pacing cost\ncurrent low-overhead: completion_wait 44.8s,\ncommit_chunk replay 19.5s,\nencode_draw 16.5s,\nsnapshot 6.8s\n(hard under-pipelined P4)"}}
  CPU --> SNAP["[[snapshot-cache]]<br/>D3D9 draw-state rebuild\n(historical owner, now residual)"]
  CPU --> SCE["[[state-churn-encode]]<br/>stream/IB churn and commit_chunk replay"]
  CPU --> CU["[[const-upload]]<br/>cbuf/argbuf traffic (CPU amplifier)"]
  CPU --> PP["[[present-pacing]]<br/>completion_wait dominated by present completion<br/>current direct path already immediate<br/>no next-CB enqueue during wait<br/>BeginScene immediate<br/>SetRT/Clear share higher app frame 0x88760<br/>command dispatcher 0x4886E0 gates Clear dispatch<br/>dxmt9 completion-signal delay does not move Clear/first chunk"]
  SCE --> SNAP
  PP --> SCE

  classDef win fill:#d6f5d6,stroke:#2b7a2b,color:#063
  classDef open fill:#fff3cd,stroke:#a80,color:#640
  classDef rej fill:#f8d7da,stroke:#a33,color:#600
  classDef base fill:#e8eefc,stroke:#3559a8,color:#0b2239
  class TVB,ICL win
  class HBS,HBD,IRM,MRB,RPS,PBIN,MESH,PSPILL,TFFP open
  class VSO,SCG,BSC,APF rej
  class Base,SNAP,SCE,CU base
```

## Priority DAG

The remaining work is organized into priority levels. P0/P1 are active
GPU-frame targets; P2/P3 are CPU encode/submit cadence tracks; P4 is the
wallclock/present-completion wait bucket that must move when P2/P3 work becomes
small enough. Keep this average-FPS lane separate from the hot-frame
hidden-backend GPU limiter.

```mermaid
flowchart LR
  Start["Current evidence\nframe Counters + perf log"] --> P0["P0: GPU memory / write pressure"]
  Start --> P1["P1: pass split / store traffic"]
  Start --> P2["P2: recover draw-run / reduce per-draw encode"]
  Start --> P3["P3: reduce transient / const payload"]
  Start --> P4["P4: present pacing / wallclock sync"]

  P0 --> P0r["→ [[hidden-backend-storage]] → [[tvb-mechanism-proof]]\n→ [[index-cache-locality]] (accepted numerator lever)\n→ hidden denominator mechanisms still open"]
  P1 --> P1r["→ [[render-pass-store]] (re-entry real; A/B/A immediate target reuse; coalescing open)"]
  P2 --> P2r["→ [[state-churn-encode]] + [[snapshot-cache]] (CPU wins, replay split, GPU flat)"]
  P3 --> P3r["→ [[const-upload]] (CPU bytes ↓ 4.6GB→1GB, GPU flat)"]
  P4 --> P4r["completion_wait is present-completion paced\ncurrent direct path already immediate\nwatcher backlog rejected\nno next-CB enqueue during wait\nPE early calls immediate\n3DMark05 command dispatcher gates Clear dispatch\nSetRT return → Clear p50 17.4ms\ndxmt9 completed-seq/waterline dependency rejected"]

  classDef p0 fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  classDef p1 fill:#fff0d6,stroke:#b26b00,color:#2b1900
  classDef act fill:#e8ffe8,stroke:#3c8f3c,color:#0d2b0d
  class P0,P1 p0
  class P2,P3,P4 p1
  class P0r,P1r,P2r,P3r,P4r act
```

## Current Gate Summary

The latest gate report narrows the next Xcode budget. Residual proxy bytes alone
are not enough to schedule a capture; the candidate must either be an accepted
locality path, carry an explicit semantic policy, or prove a new non-reorder
backend mechanism before replay. The refreshed gate also closes the stale
`60/0 live-vsout` shader-smoke queue after its matching Xcode rejection
([[hidden-backend-storage-shape.13]]). The current gate also keeps the semantic
blocker explicit without requiring a selector-sweep artifact:
`final-color-proof-gap=blocked-proof-gap` and
`final-color-occlusion-predicate=blocked-semantic-proof-gap`
([[hidden-backend-storage-shape.16]]), and the joined visibility-positive gate
now emits `visibility-positive-oracle=reject-positive-oracle`
([[hidden-backend-storage-shape.17]]). Historical opaque-depth and screen-blend
proofs are kept as mechanism evidence. The refreshed opaque-depth proof now
reattaches Xcode movement to current frame60 rows `60/0+60/1`; screen-blend
now has a current same-input mini-replay `lsb1` semantic input and target-row
Xcode movement, but the full proof is demoted because top GPU does not decrease.
The row-level follow-up shows this is target-only movement plus non-target
replay timing drift, not direct mutation of `60/0+60/1`
([[index-cache-locality-opaque.08]], [[index-cache-locality-screenblend.08]],
[[index-cache-locality-screenblend.07]], [[index-cache-locality-screenblend.06]],
[[index-cache-locality-proofinput.01]]). The per-draw PSO gate now adds the
same budget guard for backend-spill guesses: `60/2` has `47` PSO changes but
`160` handle-tuple changes and `0` PSO-isolated stable-tuple runs, so PSO
remains a future controlled A/B rather than a current Xcode replay
([[hidden-backend-storage-shape.18]]). The automated full gate now records that
as `pso-backend-isolation=reject-current` and adds a `pso-backend-spill =
blocked-current-telemetry` implementation track
([[hidden-backend-storage-shape.19]]). The same full gate now carries the
locality spend threshold as `locality-semantic-ceiling=oracle-required`:
color-exact/zero-sample locality is too small for another Xcode capture, while
sample-visible locality is large enough only if a final-color/final-writer
oracle makes it safe ([[index-cache-locality-screenblend.10]]). The full gate
now also accepts the current real-texture mini-replay summaries directly as
`final-writer-replay-oracle=blocked-final-writer-hazard`: rank1 is a real
final-writer fail, rank2-4 are owner-masked color-exact rows, and owner-safe
LRU32 is `0` ([[hidden-backend-storage-shape.20]]). The backend escape audit is
now attached to the same full gate as
`backend-escape-surface=reduced-ab-required`, so mesh/object bridge-only,
position/binning visible-probe-only, and Tile-FFP no-coverage results also
block direct GT1 Xcode spend ([[hidden-backend-storage-shape.22]]). The reduced
A/B planner then makes the blocker actionable as `blocked-before-reduced-ab`:
mesh/object lacks a dxmt9 route/emitter, position/binning is not a real route
below visible `VSOut`, and Tile-FFP lacks hot-row coverage
([[hidden-backend-storage-shape.23]]). The Tile-FFP expansion split then shows
that this is not a minor FFP selector problem: `60/2` and `60/1` are `100%`
not-FFP fallback, `60/0` is `100%` unsupported-state fallback, and the run-top
rows require a programmable/textured tile or mesh-style route
([[hidden-backend-storage-shape.24]]). The programmable route split then
separates this into a smaller first reduced A/B and harder follow-ups: `60/0`
is a depth-only candidate, `60/1` needs programmable color, and `60/2` needs
the full programmable textured route ([[hidden-backend-storage-shape.25]]).
The first implementation smoke for that branch now reaches the entire `60/0`
depth-only row through a fragmentless, position-only Metal render PSO
(`42` draws / `97,294` primitives / `291,882` vertices, no reject/no-pipeline
logs; r5 also logs `2` accepted / `0` rejected fragmentless PSO variants), but
it is only route reachability until depth/color equality and Xcode counter
movement are attached. A user-observed sharper r5 frame, with subtle
texture-over haze/blur and bloom-like coverage disappearing, keeps semantic
equivalence open rather than promoting the route
([[hidden-backend-storage-shape.26]]).

| Track | Status | Evidence | Decision |
|---|---|---|---|
| Opaque-depth index locality | accepted opt-in; refreshed frame60 proof passed | Fast-measure proof: top GPU `-9.50%`; target `50/0+50/1` GPU `-18.39%`, VS invocations `-14.12%`, VS write `-16.79%`. Current post-stream/IB proof: target `60/0+60/1` GPU `13.800ms→12.331ms` (`-10.64%`), VS invocations `536,583→460,839` (`-14.12%`), VS write `646.173MiB→537.842MiB` (`-16.77%`), top-3 GPU `33.614ms→32.501ms` (`-3.31%`). CPU smoke still has index setup `+309ms` and source-resolve is flat. | Production-shaped path remains the safe GPU win and current proof demonstrates why the ongoing experiment matters. It is still not a shared `perf` default until CPU side-effect is lower or a broader runtime gate proves net positive. [[index-cache-locality-opaque.08]], [[index-cache-locality-proofinput.01]], [[index-cache-locality]] |
| Screen-blend index locality | historical explicit-tolerance proof; current target movement pass, aggregate proof fail | Historical combined run GPU `-11.89%`; `lsb1` image gate `739/786,432`, max delta `1`, SSIM `1.000000`. Current rank-1 mini-replay has LRU32 `52,865->38,272` (`-27.60%`) and `lsb1` image gate `33/786,432`, max delta `1`, SSIM `1.000000`. Full proof target `60/2` improves GPU `-3.55%`, VS invocations `-10.76%`, VS write `-10.84%`, but top GPU fails `+0.97%`; row follow-up shows reordered-cache applied only to `60/2` and non-target rows have GPU-time-only drift. | Keep as mechanism ceiling/proof artifact only. Target-row movement is not enough for promotion; do not generalize to broad depth-read. [[index-cache-locality-screenblend.09]], [[index-cache-locality-screenblend.08]], [[index-cache-locality-screenblend.07]], [[index-cache-locality-screenblend.06]], [[index-cache-locality-proofinput.01]], [[index-cache-locality-screenblend.04]] |
| Screen-blend / depth-read locality ceiling | blocked by semantic proof gap; automated as `oracle-required` | Current screen-blend target movement calibrates `-87,076` LRU32 to `-0.682ms` on `60/2`. Rank2-4 color-exact owner-masked windows sum to only `-9,113` LRU32 (estimated `-0.071ms`), and rank1-4 still include a visible fail while only reaching `-23,706` LRU32 (estimated `-0.186ms`). Zero-sample visibility rows are only `-2,016` LRU32; positive-sample rows are large (`-180,840`, estimated `-1.416ms`) but are not final-color proof. The semantic visibility join makes this concrete: rank2 has `39,835` samples but `0` final-color pixels, and rank1/rank3 are both sample-positive but split fail/pass. The current full gate emits `locality-semantic-ceiling=oracle-required`. | No more locality gputrace/Xcode spend without a final-color/final-writer oracle, a runtime selector that keeps at least `~41k` additional safe LRU32 delta, or a non-reorder backend-denominator mechanism. [[index-cache-locality-screenblend.10]], [[index-cache-locality-screenblend.09]], [[mini-replay-bisection-texture.10]], [[mini-replay-bisection-texture.11]] |
| Broad depth-read reorder | reject | Visible exact gain exists (`-8446` LRU32), but visible-fail hazard remains (`-1407` LRU32) | Requires final-color/final-writer proof; the Metal visibility scout can triage no-sample draws but positive samples are not final-color proof. [[mini-replay-bisection-semantic.01]], [[mini-replay-bisection-texture.08]], [[mini-replay-bisection-texture.09]], [[mini-replay-bisection-texture.11]] |
| Scoped depth-read/no-blend locality | mixed; not production-safe; current replay oracle blocked | Rank 1 keeps replay LRU32 `52,865 -> 38,272` (`-27.6%`) but real-texture replay changes `2 / 786,432` pixels and `7` final-writer pixels; rank 2 keeps final color exact with LRU32 `19,131 -> 13,194` (`-31.0%`) but `809` canonical owner pixels change; rank 3 keeps final color exact with LRU32 `11,398 -> 8,946` (`-21.5%`) but `52` owner pixels change; rank 4 keeps final color exact with LRU32 `4,237 -> 3,513` (`-17.1%`) but `17` owner pixels change; Metal visibility scout shows the old rank-1 `36..37` window is sample-visible, not no-sample; cache join shows zero rows are only `-2,016` of `-182,856` LRU32 delta; semantic visibility join shows all rank1-4 windows are sample-positive, including rank2 with no final color; current automated gate reports `final-color-proof-gap=blocked-proof-gap`, `visibility-positive-oracle=reject-positive-oracle`, and `final-writer-replay-oracle=blocked-final-writer-hazard` with owner-safe LRU32 `0` | Same state class contains both visible failure and color-exact owner-masked windows. The current real-texture replay set is not the oracle; another Xcode locality spend needs a different final-color/final-writer proof that keeps enough sample-visible gain, a stricter runtime-visible selector, or a non-reorder backend mechanism. Metal visibility remains useful for no-sample triage, but current positive rows are not a safe production selector. [[mini-replay-bisection-semantic.02]], [[mini-replay-bisection-texture.02]], [[mini-replay-bisection-texture.04]], [[mini-replay-bisection-texture.05]], [[mini-replay-bisection-texture.06]], [[mini-replay-bisection-texture.07]], [[mini-replay-bisection-texture.08]], [[mini-replay-bisection-texture.09]], [[mini-replay-bisection-texture.10]], [[mini-replay-bisection-texture.11]], [[hidden-backend-storage-shape.16]], [[hidden-backend-storage-shape.17]], [[hidden-backend-storage-shape.20]] |
| Primitive-conflict / occlusion selector | rejected-current | Rank1 fail vs rank2-4 pass scout: owner pixels `7` vs `3..641`, max depth `3.468` vs `0.0149..201.571`, max UV0 `544.169` vs `1.056..26497.059`; only color metrics separate. Existing D3D9 occlusion query resolves primitive count; diagnostic Metal visibility now reports per-draw sample counts but not final color; visibility-positive semantic join proves positive samples do not split final-color-empty, visible-exact, and visible-fail rows | Do not use owner-count/depth/UV/texcoord thresholds, the current D3D9 query path, or positive Metal visibility as a production selector. Use Metal visibility only as no-sample triage unless paired with final-color/final-writer proof. [[mini-replay-bisection-texture.07]], [[mini-replay-bisection-texture.08]], [[mini-replay-bisection-texture.09]], [[mini-replay-bisection-texture.11]] |
| Runtime final-color selector | blocked | Pass draws `3,5,6,7` and fail draw `4` share all `43` runtime-visible fields | Do not use full uniform payload identity as a production selector. [[mini-replay-bisection-semantic.01]] |
| Non-reorder backend mechanism | below-AIR gate only; current escape surface blocked before reduced A/B | Half-VSOut bytes/inv `-1.94%`, but GPU `+3.40%`; scoped `60/0` live-vsout changed expected VSOut `184 B -> 68 B` while Xcode VS buffer stayed flat (`224.947 MiB -> 224.990 MiB`, `1542.722 -> 1543.013 B/VS invocation`); refreshed gate reports backend-shape `reject`, VS-write attribution `backend-rejected`, and `shader-variant-backend-smoke=closed-by-xcode-gate`; strongest remaining state clue is correctness-invalid `large4096+alpha` blend-off (`VS write -52.86%`, B/inv `-43.56%`), but current `60/2` large alpha rows fail static blend-off equivalence (`InvDestColor+One` screen and varying-alpha standard blend); current PSO churn is stream/IB-dominant, the per-draw gate finds `0` PSO-isolated stable-tuple runs, and the full gate emits `pso-backend-isolation=reject-current`; backend escape audit reports `mesh-object=bridge-only-reduced-ab-required`, `position-binning=visible-vsout-probe-only`, and `tile-ffp=rejected-current-coverage`, now carried as `backend-escape-surface=reduced-ab-required`; the reduced A/B plan reports `blocked-before-reduced-ab`; Tile-FFP expansion refines that to `tile-ffp=blocked-hot-row-coverage/needs-programmable-tile-route`; programmable feasibility identifies `60/0` as a depth-only route candidate, while `60/2` remains the hard textured route | Do not spend more Xcode budget on visible-width retries, blend-off-as-fix, current no-sample locality, positive-visibility locality without final-color proof, unisolated PSO churn, stale shader-output smokes, current Tile-FFP widening, or bridge-only backend escape guesses. Next backend-route work should start with a reduced `60/0` depth-only equality/counter A/B before attempting the larger `60/2` textured route. [[hidden-backend-storage-shape.02]], [[hidden-backend-storage-shape.05]], [[hidden-backend-storage-shape.06]], [[hidden-backend-storage-shape.07]], [[hidden-backend-storage-shape.08]], [[hidden-backend-storage-shape.09]], [[hidden-backend-storage-shape.10]], [[hidden-backend-storage-shape.11]], [[hidden-backend-storage-shape.13]], [[hidden-backend-storage-shape.14]], [[hidden-backend-storage-shape.16]], [[hidden-backend-storage-shape.18]], [[hidden-backend-storage-shape.19]], [[hidden-backend-storage-shape.21]], [[hidden-backend-storage-shape.22]], [[hidden-backend-storage-shape.23]], [[hidden-backend-storage-shape.24]], [[hidden-backend-storage-shape.25]], [[mini-replay-bisection-texture.09]], [[mini-replay-bisection-texture.10]], [[mini-replay-bisection-texture.11]] |
| Apple position/binning path | visible-probe-only; real route missing | Existing `DXMT9_PROBE_POSITION_ONLY_VSOUT` only changes the source-visible `VSOut`/fragment diagnostic shape; the backend escape audit finds no separate position/binning route token in dxmt9, and the reduced A/B plan reports `blocked-real-route-missing`. It does not prove that Apple's hidden `[[position]]` binning output or a tile vertex/fragment split was avoided | Do not cite visible position-only VSOut as closure. A future probe must implement/force a real position-only binning/depth/mesh route and measure bytes/invocation in reduced A/B before GT1. [[vsout-layout]], [[hidden-backend-storage]], [[hidden-backend-storage-shape.21]], [[hidden-backend-storage-shape.23]] |
| Metal 3 mesh/object path | bridge-only; blocked before reduced A/B | Mesh/object command replay and PSO descriptors exist below winemetal, but the audit reports dxmt9 route `missing` and shader emitter `missing`; GT1's D3D9 path is not currently routed through a mesh/object backend, and the reduced A/B plan reports `blocked-missing-dxmt9-route` | Track as an exploratory backend escape hatch. The next evidence is not direct GT1 Xcode; it is a dxmt9 route/emitter or out-of-GT1 synthetic/replay A/B that passes equality and counter gates. [[hidden-backend-storage-shape.14]], [[hidden-backend-storage-shape.21]], [[hidden-backend-storage-shape.23]] |
| Tile-FFP path | rejected-current GT1 hot-row lever; programmable route required | `DXMT9_TILE_FFP=off` is still the default, but the current code has the selector, base-colour render PSO, tile PSO, tile constants, and per-draw tile post-pass. The coverage gate reports frame60 `60/0..2` eligible primitives `0`, and the partial run has only `98,469 / 1,900,371,413` eligible primitives (`0.005%`); the reduced A/B plan reports `blocked-hot-row-coverage`. The expansion split shows `60/2` and `60/1` are `100%` not-FFP fallback and `60/0` is `100%` unsupported-state fallback; full gate evidence is now `tile-ffp=blocked-hot-row-coverage/needs-programmable-tile-route`. | Keep current Tile-FFP as a narrow correctness/architecture lever. Do not spend GT1 Xcode budget on it and do not chase selector widening as the fix. A future Tile-FFP-class experiment must first define a programmable/textured tile or mesh-style route, then pass portable-vs-route equality and reduced counter gates. [[hidden-backend-storage-shape.14]], [[hidden-backend-storage-shape.15]], [[hidden-backend-storage-shape.23]], [[hidden-backend-storage-shape.24]] |
| Depth-only programmable route | runtime smoke passed; equality failed, Xcode blocked | `60/0` has `97,294` primitives, `100%` depth-only candidate coverage, `color_write=0`, depth write on, no alpha blend/test, and one PS shape. Texture mask is nonzero, but with color writes off and no alpha test the fragment output was initially expected not to own color correctness. The fragmentless-depth-only smoke covers all `42` row draws / `97,294` primitives / `291,882` vertices, reports position-only VSOut key `0x0`, and logs `2` accepted / `0` rejected fragmentless PSO variants. The same-input equality gate now blocks promotion: final frame color changes `170,328 / 786,432` pixels (`21.658325%`, max delta `252`), `60/0` pass-end color is exact (`0` changed bytes), but the `D24X8` depth sidecar changes `1,252,096 / 3,145,728` bytes (`39.803060%`, max delta `255`). The gate reports `blocked-equality-fail` / `blocked-equality` | Route reachability is proven, but this is no longer an Xcode/gputrace counter candidate. Debug the depth semantic difference first, or move to another route. Do not generalize this fragmentless shortcut to `60/2`, and do not treat sharper/haze-free visuals as a performance win until equality is restored. [[hidden-backend-storage-shape.25]], [[hidden-backend-storage-shape.26]] |
| Post-compact System Trace sidecar | accepted shape evidence; route verdict missing in that artifact | `post-compact-state-r1` joins `2482/2482` System Trace encoder rows and remains `89.83%` vertex-stage dominated (`5701.647ms` vertex / `645.319ms` fragment). The top row is `1155/1`, `20.002ms`, `1,149,930` vertices, and the top 12 rows are all large indexed `rt_change` encoders with sub-ms fragment time. Aggregate attribution puts `68.25%` of stage time in `opaque-depth-indexed` rows and `79.52%` in `rt_change` rows; both are >`92%` vertex-stage dominated. The earlier `phase43` sidecar is also vertex-dominated (`91.32%`) but captures a different seq/scene phase, so the comparison is ownership evidence rather than a strict A/B. Route verdicts are `route-unavailable` for this artifact because its indexed probe draw CSV is header-only. | Keep this as vertex-stage ownership evidence only. Route selection should use the seq-range sidecar below, not the header-only post-compact artifact. Keep CPU submit/snapshot compaction separate in no-gputrace counter lanes. Do not retry 3DMark05 `.gputrace` until capture can avoid startup mutation. [[hidden-backend-storage-shape.27]], [[baselines-gputrace-capture.01]] |
| Seq-range System Trace route sidecar | accepted route-attributed timing evidence | `systemtrace-indexed-r6-range` joins `215/215` System Trace rows, captures seq `1005..1024`, and proves indexed telemetry range gating (`29,549` indexed probe rows, seq `1000..1035`, out-of-range rows `0`). The trace is still vertex dominated (`650.195ms` stage, `577.770ms` vertex, `72.425ms` fragment, `88.86%` vertex share). Route attribution now splits the residual: programmable color `45.65%`, programmable textured `40.49%`, mixed programmable `12.02%`, and depth-only only `1.85%`. The first failed range attempt was not a renderer regression: stale `winemetal.so` meant the new seq-range env filter was absent from the active Wine unix provider, causing logs from `seq=2`; `install_heroic_wine.sh` now builds the unix provider target before copying it. | Use this as the normal-rendering fallback while `.gputrace` remains blocked. The next GPU-facing route work must account for programmable color/textured rows, not only depth-only. The evidence is timing/label/route attribution; Xcode replay counters are still required for `VS Buffer Device Memory Bytes Written` proof. [[hidden-backend-storage-shape.28]], [[baselines-gputrace-capture.01]] |
| Encoder-summary route sidecar tooling | accepted instrumentation and verified sidecar path | Encoder breakdown now emits `route_depth_only_*`, `route_programmable_textured_*`, `route_programmable_color_*`, `route_alpha_blend_primitives`, and `route_alpha_test_primitives` directly per encoder. The no-indexed `systemtrace-route-summary-r1` rerun joins `1633/1633` System Trace rows with `route_source=encoder-summary`, `0` indexed probe draw lines, `91.90%` vertex-stage share, and route split dominated by programmable color (`57.04%`) plus programmable textured (`29.22%`). | Use encoder-summary route sidecars as the default low-overhead route-timing fallback while `.gputrace` remains blocked. Indexed per-draw telemetry is now optional detail for exact draw/index selectors, not a prerequisite for route verdicts. [[hidden-backend-storage-shape.29]], [[baselines-frame60.04]] |
| GPU floor vs wall-clock owner | accepted gate split | `replay.03` proves the hot GPU path is not at a hardware floor: original 113-draw replay and sorted-row control have nearly identical VS invocations (`668,929` vs `667,944`) but VS write density drops `1710.0 -> 442.6 B/inv`. At the same time, the current low-overhead no-gputrace scout shows average wall-clock FPS is not GPU-execution-time-bound: `gpu_command_buffer_time_ms=3.113ms/present` versus `completion_present_wait_ms=25.091ms/present`, with frame wall p50/p95 `55.242/84.648ms`. | Keep two proof lanes separate. Primitive/PB locality work can still reduce hot-frame Xcode GPU cost, but it is not a broad average-FPS claim unless pacing/CPU also moves. Conversely, pacing-bound average FPS does not mean the GPU hot-frame path is efficient. [[hidden-backend-storage-shape.30]], [[mini-replay-bisection-replay.03]], [[present-pacing-current-fps-owner.04]], [[present-pacing]] |
| PE Present timing vs completion wait | rejected as hidden wait owner | `present-pe-timing-info-r1` logs PE `Present total_ms` p50/p95/max `2.580/5.077/22.659ms`, almost all in `flushPendingCommandChunk(Present)`, while `completion_wait_ms` p50/p95/max is `28.419/39.576/52.217ms`. The next unix chunk still crosses shortly after completion (`wait_to_commit_chunk_entry` p50/p95 `0.888/3.025ms`), but that is not a PE API-call timestamp. | Do not describe the no-enqueue gap as "app blocked inside Present". [[present-pacing-pe-present-timing.09]], [[present-pacing-stage-delta.08]] |
| PE next-call cadence after Present | rejected as app/Wine call-wait owner; accepts PE-local next-frame start | `present-pe-cadence-r1` logs `1703` first-call rows after PE `Present`; `1702` are `BeginScene`, with `entry_delta_ms` p50/p95/p99 `0.310/0.436/1.811ms`. `completion_wait_with_enqueue_ms=0` still holds, and post-wait stage counters remain large (`commit_entry_to_publish` p50 `18.475ms`, `encode_dequeue_to_command_buffer_commit` p50 `16.564ms`). | The app starts next-frame D3D9 work immediately in PE. The missing overlap is therefore PE-recorder/unix submission boundary or later: earlier chunk flush/publish architecture, pre-publish replay/snapshot reduction, and backend encode reduction are the relevant lanes. [[present-pacing-pe-call-cadence.10]], [[present-pacing]] |
| PE chunk cadence after Present | accepted attribution; rejects immediate unix crossing after first PE call | `present-pe-chunk-cadence-r1` keeps first PE call fast (`0.308ms` p50), but first non-empty PE chunk after `Present` is always `capacity_post`, always `64` records, and crosses into unix at steady p50/p95 `19.908/34.810ms`; first bridge cost itself is only `0.504/0.617ms` p50/p95. | The first boundary is PE-local chunk fill, not app API absence or bridge latency. Earlier useful publish is now a real architecture candidate, but must beat extra bridge/replay/render-pass costs and preserve D3D9 ordering/resource lifetime. [[present-pacing-pe-chunk-cadence.11]], [[present-pacing]] |
| PE chunk-size A/B | rejected as a simple run-ahead knob | `DXMT9_PE_CHUNK_MAX_RECORDS=32` reaches the recorder (`recordCount=32`) and slightly lowers first-chunk p50 (`19.908 -> 19.034ms`) plus no-enqueue wait-to-commit-entry p50 (`0.917 -> 0.471ms`), but `completion_wait_with_enqueue_ms` stays `0.000`; the run timed out, so aggregate wallclock is not a fps proof. | The first unix-visible N+1 boundary is capacity-driven, but simply halving capacity does not recover overlap. Do not blame steady-state first-call `GetBackBuffer`, `Query::GetData`, or lock dependency: first calls remain almost entirely `BeginScene`. [[present-pacing-pe-chunk-size-ab.12]], [[present-pacing]] |
| PE record milestones after Present | accepted attribution; refines chunk-cadence owner | `present-pe-record-milestones-r1` shows `BeginScene` p50 `0.306ms`, but record 1 is delayed to p50 `18.061ms` and is always `apply_state`; record 4 is already `draw_indexed` in every steady sample; record 64 reaches p50 `19.683ms` and the first chunk follows at `19.706ms`. | First-chunk delay is not mainly 64-record fill time. The front gate is the first record-producing state/draw boundary; once that boundary is hit, the recorder fills and flushes quickly. Early publish before the first draw is only useful if it can safely move dirty-state replay, not because empty `BeginScene` itself has Metal work. [[present-pacing-pe-record-milestones.13]], [[present-pacing]] |
| PE call sequence after Present | superseded coverage gap | `present-pe-call-sequence-r1` showed calls `1..4` after `Present` are immediate early RT setup, but it omitted `Clear` and `EndScene` from call milestones and therefore misidentified call 5 as `SetVertexShaderConstantF`. | Keep as the historical reason H20 added return/milestone coverage for `Clear` and `EndScene`. [[present-pacing-pe-call-sequence.14]], [[present-pacing]] |
| PE Clear gate after Present | accepted attribution; current front-gap owner | `present-pe-call-return-r2` shows `SetRenderTarget` return p50 `0.581ms` with only `0.015ms` duration, then `Clear` entry p50 `18.408ms`; the p50 gap from `SetRenderTarget` return to `Clear` entry is `17.635ms`. Record 1 is `apply_state` inside `Clear` at p50 `18.554ms`, and the first `capacity_post` chunk follows at p50 `20.400ms`. | Do not blame early RT setup, `GetBackBuffer`, `Query::GetData`, lock, or `SetRenderTarget` internals. The next pacing probe should explain the app/Wine cadence between `SetRenderTarget` return and `Clear` entry. [[present-pacing-pe-clear-gate.15]], [[present-pacing]] |
| PE Clear gate without frame sampling | rejected sampling artifact; accepted gate stability | `present-pe-clear-no-frame-sampling-r1` removes all `dxmt9-perf-frame` lines (`1,726 -> 0`) while preserving `SetRenderTarget` return -> `Clear` entry p50 (`17.651 -> 17.646ms`) and first-chunk p50 (`20.402 -> 20.386ms`). | Treat frame summary logs as correlation only. Continue investigating the owner before `Clear` entry: app timer/message cadence, Wine/macdrv processing, or an uncovered D3D9-adjacent path. [[present-pacing-pe-clear-nosampling.16]], [[present-pacing]] |
| PE wide call coverage before Clear | rejected child getter stall; accepted wider attribution | `present-pe-child-return-r1` reveals the narrow sequence skipped immediate child calls: `Surface::GetDesc`, `Texture::GetSurfaceLevel`, another `GetRenderTarget`, `SetRenderTarget`, and another `Surface::GetDesc`. The last `Surface::GetDesc` returns at p50 `0.674ms`, but `Clear` still enters at p50 `18.421ms`; last-return -> `Clear` p50 is `17.656ms`. | Do not spend another run on descriptor/subresource getter coverage. The remaining owner is below or outside meaningful PE D3D9 entry points: app timer/message cadence, Wine/macdrv event processing, or COM housekeeping that does not produce records. [[present-pacing-pe-wide-call-coverage.17]], [[present-pacing]] |
| Xcode System Trace CPU thread state | rejects broad runloop sleep; exact PE PC still open | Re-exported `phase43` `Metal System Trace` CPU tables show `time-profile`, `time-sample`, `runloop-events`, CoreAnimation present, and Metal submission/completion rows are available. The D3D/Wine producer thread `3DMark05.exe (0x3b1b5c)` has `15,354ms` Running sample weight across a `15.563s` trace, while `runloop-events` has only two `3DMark05.exe` rows on a different main thread. CA present request cadence remains p50 `74.398ms`, request -> presented handler p50 `33.637ms`, and submit -> completion p50 `14.397ms`. | Do not frame the remaining `SetRenderTarget` -> `Clear` gap as a broad app/Wine sleep without stronger evidence. Next proof should map raw PE/app frames on the producer thread or align a new System Trace with PE milestone logs. [[present-pacing-xctrace-threadstate.18]], [[present-pacing]] |
| PE caller PC/module for Clear gate | rejects hidden dxmt9 API wait; accepts wrapper-level gap | `present-pe-caller-module-r1` resolves caller PCs to PE modules. The steady sequence is identical in `1,716 / 1,716` ordinals: `SetRenderTarget` returns from 3DMark05.exe wrapper RVA `0x2AF4F` at p50 `0.730ms`, the nested getter resolves to `d3d9.dll!0x13EE9` with duration p50 `0.020ms`, then `Clear` enters from 3DMark05.exe wrapper RVA `0x2B061` at p50 `18.373ms`. `SetRenderTarget` return -> `Clear` remains p50 `17.484ms`, while `Clear` duration is only p50 `0.252ms`. | The remaining P4 front gate is above the wrapper stubs, not inside `SetRenderTarget`, `Clear`, descriptor/getter, lock, or query. The caller-stack follow-up supersedes using `0x2AF4F`/`0x2B061` as higher render-loop owners. [[present-pacing-pe-caller-pc.19]], [[present-pacing]] |
| PE caller stack for Clear gate | accepts 3DMark05 command-dispatch cadence | `present-pe-caller-stack-r1` shows milestones 2..8 share higher frame `3DMark05.exe+0x88760` in `1,707 / 1,707` matching ordinals. Disassembly identifies `0x88760` as the return site of command-object dispatcher `0x4886E0`; the virtual `call *0x18(%eax)` executes D3D wrapper commands such as `SetRenderTarget` and `Clear`. `SetRenderTarget` return -> `Clear` remains p50 `17.429ms`, with `completion_wait_with_enqueue_ms=0.0`. | P4's front gate is when 3DMark05 dispatches the record-producing `Clear` command object. This is external app command cadence unless a stronger raw-stack/static proof finds a dxmt9-controlled sleeper inside the dispatcher path. Continue average-FPS work on P2/P3 copy/replay/encode reductions, and treat earlier PE publish before `Clear` as speculative/order-sensitive. [[present-pacing-pe-caller-stack.20]], [[present-pacing]] |
| Completion-signal delay perturbation | rejects dxmt9 completed-seq/waterline dependency | `DXMT9_PERF_COMPLETION_SIGNAL_DELAY_MS=8` applies `1696` sleeps / `13568ms` after `waitUntilCompleted()` and before completed-seq publication. PE cadence and next enqueue remain flat: `SetRenderTarget -> Clear` p50 `17.631 -> 17.550ms`, record 1 p50 `19.025 -> 19.011ms`, first chunk p50 `20.802 -> 20.827ms`, and `completion_no_enqueue_wait_to_next_enqueue_p50_ms` `21.558 -> 20.274`. | Do not treat the `Clear` front gate or next Metal enqueue as waiting on dxmt9's completion signal. A lower actual Metal/CA completion dependency is still a separate experiment because this perturbation happens after Metal completion. [[present-pacing-completion-signal-delay.21]], [[present-pacing]] |
| Programmable textured route | required for largest residual row; hard | `60/2` has `389,376` primitives, `100%` programmable textured coverage, `14` unique PS, texture masks `0x7f`, `0x3f`, `0x1f`; this is the largest row but requires texture sampling or preserving the existing fragment path | Keep as the long path after depth-only/color reduced A/B. It is not a near-term selector tweak. [[hidden-backend-storage-shape.25]] |
| PSO/state churn backend spill | rejected-current Xcode candidate; isolated A/B still open | Current frame60 preflight shows hot rows are stream/IB-dominant: `60/2` has `47` PSO changes, but `271` stream-handle and `160` IB-handle changes; the per-draw join then shows `60/2` has `160` handle-tuple changes, max stable tuple run `6`, and `0` PSO-isolated stable-tuple runs; the automated full gate emits `pso-backend-isolation=reject-current`; `60/1` and `60/0` show the same no-isolated-run pattern | Keep CPU and GPU claims separate. Do not spend Xcode on PSO churn from the current rows; add a PSO-stable/PSO-churn A/B only if geometry, stream/IB bindings, visible state, and invocation count can be isolated. [[state-churn-encode]], [[hidden-backend-storage-shape.11]], [[hidden-backend-storage-shape.18]], [[hidden-backend-storage-shape.19]] |
| Stream/IB backend handle-stable A/B | rejected as first-order GPU owner | Baseline `60/2` is handle-churn-dominant (`stream_handle_changes=271`, `ib_handle_changes=160`, tuple changes `160/187`). Staged `60/2` keeps `187` draws, PSO `48 -> 48`, argbuf table `5056 -> 5056`, cbuf `96424 -> 96424`, and drops stream/IB handle changes to `0`, but adds `7.38 MiB` staged copy and turns the diversity into offset churn. Xcode then shows target GPU `19.184 -> 19.278 ms`, VS write `981.159 -> 981.166 MiB`, and unchanged VS invocations. | Do not spend more Xcode budget on stream/IB handle identity as a GPU-denominator hypothesis. Keep stream/IB work in the CPU/draw-run lane unless a new mechanism also changes VS invocations, primitive/binning shape, or a below-visible-VSOut backend path. [[state-churn-encode-stream.08]], [[state-churn-encode-stream.09]], [[hidden-backend-storage-shape.12]] |
| Index-cache CPU reduction | reject current attempts | Fixed cap cuts slots but not CPU; heap lazy frontier cuts scored work `-80.97%` but select CPU regresses `+21.40%`; bucketed select cuts scored work `-72.61%` but select CPU regresses `+32.46%`; unique upper-bound gate rejects `76` candidates but candidate CPU regresses `+8.50%`; persistent rejected verdicts are already implemented (`401,681` rejected hits / `143` cold misses); non-scope draw-shape prefiltering already happens before lookup; strict LRU builder normalization worsens candidate miss32 by `+46` and total encode CPU by `+36.930ms` | Do not spend more Xcode budget on these CPU-only variants. Next CPU work needs cheaper cold-miss candidate construction, a telemetry-proven eligible-subclass exclusion, or broader semantic-safe GPU payoff before no-gputrace promotion. [[index-cache-locality-cpucost.11]], [[index-cache-locality-cpucost.12]], [[index-cache-locality-cpucost.13]], [[index-cache-locality-cpucost.14]], [[index-cache-locality-cpucost.15]], [[index-cache-locality-cpucost.16]], [[index-cache-locality-cpucost.17]] |
| Current no-gputrace baseline | accepted as counter sample | Capture-delay-aware 120s scout: `present_encoded=1680`, standard `result.json` preserved, `draw_calls` `-0.02%`, GPU CB `+0.50%`, completion wait `+2.38%`, encode CPU flat, snapshot CPU `-0.11%` vs [[snapshot-cache-snapshot.10]]. A pre-fix 120s attempt produced only `partial-log` because the wrapper watchdog omitted the 70s capture delay. | Use as the current supervised timeout shape; it does not justify new Xcode budget by itself. [[baselines-frame50.05]] |
| Current visual-coupling frame60 scout | accepted as counter sample; not Xcode proof | Frame60 seq breakdown no-gputrace: `present_encoded=1680`, `draw_skipped_no_pipeline=0`, `gpu_command_buffer_errors=0`, tracked frame60 overflows `0`, `map_buffer_wait_ms=0`, `queue_sequence_wait_ms=0`; hazard bloom is entirely false-positive (`104,004 / 104,004`) but `render_split_hazard=0`; split reasons are RT/depth `13,169`, clear `4,906`, present `1,673`; render-pass preservation remains `120.10MiB/present`. A post-`01:05` oracle refresh (`current-residual-perf-after-oracle-r1`) stayed flat: `draw_calls -0.02%`, `render_pass_begin -0.09%`, `render_split_rt_change=13,163`, clear `4,895`, present `1,673`, `tile_preservation +0.03%`, `gpu_command_buffer_time_ms -0.26%`, `completion_wait_ms -2.44%`, sampled average `15.753fps`, late steady frames `~23fps`. Same-run wide-scene after-draw color history confirms two `0x80` sidecars; the second (`seq=1094/enc=3/draw=0/cmd=320`) writes the round candidate ROI (`bright=706`, `white=196`, `warm=909`) | Use for visual-fix before/after wrong-path gates. It narrows skipped/error/overflow/hazard-split, depth-alone rejection, and blank/unsent effect draws as the obvious explanations. The `0x80` sidecar proves the wide-scene rifle bloom writer, but remains diagnostic split evidence rather than a production perf sample. The current refresh confirms the residual performance owner did not move toward skipped/error handling after the muzzle writer was identified; continue with TVB/PB, RT/depth re-entry, and completion/present pacing. [[baselines-frame60.03]], [[backend-shape-classifiers-alpha.04]] |
| Low-overhead FPS recovery scout | accepted counter sample; rejects current FPS-zero interpretation and names the average-FPS lane | `current-lowoverhead-20260613` ran `--no-gputrace --no-encoder-breakdown --frame-sampling --timeout 120` and produced `1,807` frame samples with p50/p95/max FPS `18.102`/`26.630`/`30.351`, last sample `24.798fps`, `completion_present_wait_ms=25.091ms/present`, `gpu_command_buffer_time_ms=3.113ms/present`, `encode_chunk_cpu_ms=11.112ms/present`, `commit_chunk_replay_cpu_ms=10.746ms/present`, `present_boundary_wait_ms=0.000`, and no encoder/indexed probe rows. The same code state was visually normal by observation. Compared with the seq-range sidecar, the normal FPS envelope is similar but the sidecar's second-scale p99 encode/stream-bind/present-boundary tails disappear. | Treat the observed FPS-zero / one-draw-per-several-seconds state as a heavy instrumentation or transient-stall artifact unless a low-overhead scout reproduces it. Continue using System Trace sidecars for route attribution, not baseline FPS. Average-FPS work should now target P2/P3 CPU cadence and require P4/sample-FPS movement as the proof gate. [[baselines-frame60.04]], [[hidden-backend-storage-shape.28]], [[present-pacing-current-fps-owner.04]] |
| Encode CPU attribution | CPU wins accepted, fps proof still open | No-gputrace attribution has narrowed broad encode guesses into named CPU-only children: cbuf identity, packet-cache, snapshot, argbuf-open, sampler, and transient fast-append work all moved CPU but not GPU. Cbuf residual split named binding content hash as a dominant child (`570.070ms`, VS `489.627ms`), then the default path removed that byte scan (`binding_hash=0`) and cut cbuf update `1.216 -> 0.875ms/present`; prefix-preserving cbuf builders then cut cbuf build `0.333815 -> 0.175342ms/present`; binding-packet sampler key-hash reuse cut packet plan `0.666122 -> 0.599724ms/present`; uniform-refresh component reuse cuts refresh `2014.263ms→814.507ms` and snapshot submission `7622.807ms→6495.069ms` with FPS flat. A full-cbuf visual bisection knob rejects full upload as a default workaround (`argbuf_hybrid_bytes_per_encoder` +519.59%, no obvious visual normalization); the later visual fix is per-draw payload component hashes for argbuf cbuf identity, not full cbuf upload. The commit_chunk stage split rejects raw bridge/ABI overhead as the owner (`bridge_commit_latency=22.473s`, replay `21.839s`), and the child split names queued draw submission/snapshot as the first replay owner (`replay=22.224s`, queue submission `9.927s`, nested snapshot `7.697s`, draw-batch submit `3.229s`, draw-run submit `2.094s`). Queue append attribution rejects raw payload copy (`payload=65ms` despite `232.5MB`) and names state/uniform append; removing extra `CanonicalDrawState` value hops cuts batch append `2708→2116ms` and state append `958→720ms` with GPU flat. The remaining state child is now split far enough to reject PSO subview/invariant construction as the owner: SoA `appendDrawState()` storage is `707ms` (`80.47%` of state), while PSO is `50ms` and invariant is `22ms`; the split run itself adds timer overhead and should be read as attribution only. The latest resource-retention cleanup marks batch-front draw resources once and keeps per-draw binding override/snapshot marking, cutting `submit_draw_run_batch_resource_mark_cpu_ms` `27.146→24.739` with GPU flat. The F1 N-1 state-copy elision first proved the mechanism behind `DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1` (`400,838` elisions, `4.10GB` state bytes elided, state-copy CPU `258.969→139.672ms`) but stayed FPS-flat. The safe subset is now default: same-stamp continuations elide state while normal compatibility grouping remains, removing the measured default waste (`submit_draw_run_batch_discarded_state_records 411,362→0`, `d3d9_snapshot_state_elided=412,180`, state-copy CPU `261.001→138.856ms`, queue submission `7,463.771→7,023.458ms`) with a normal output frame; GPU/completion remain flat/noisy, so it is a copy-policy cleanup, not the average-FPS owner. The queued-submission carrier split accepted a bounded target (`emplace=1,123.253ms`, `13.99%` of queued-submission CPU); optional carrier storage then cut that target to `573.056ms` (`-48.98%`) and queue submission to `7,867.581ms` (`-2.04%`) with a normal output frame. Snapshot residual now points at queued batch miss: `batch_hit+miss=5,692.001ms` vs lookup `5,892.464ms`, then batch miss splits into uniform build `2,083.529ms`, hot build `1,774.774ms`, and shader layout `607.942ms`; the batch-uniform split then names hash as the local owner (`1,413.471ms`, `64.97%` of batch uniform build), led by non-constant hash `677.447ms` and VS constant hash `490.107ms` with indexed-float full fallback. Generation-gated non-constant hash reuse then accepts that local CPU target: reuse hits `376,949 / 418,143` (`90.15%`), non-constant hash drops `677.447→73.490ms`, batch uniform build drops `2,175.433→1,591.208ms`, and the output frame remains normal, but sampled FPS/GPU/completion wait stay flat/noisy. The follow-up hot-build split names repeated render-state `FlatStateSet` materialization as the local owner (`1,202.861ms`), ahead of key build (`485.840ms`), sampler (`213.765ms`), and TSS (`204.392ms`); generation-gated flat-state reuse then accepts that target with render/TSS/sampler hit rates `90.12%` / `99.36%` / `79.93%`, cutting hot-build `1.444→0.684ms/present` and snapshot submission `4.255→3.469ms/present`, with FPS/GPU/completion still flat/noisy. Adjacent uniform-payload elision is rejected for GT1: state elision fires but `d3d9_snapshot_uniform_elided=0`, so the same-state groups still change `uniformGeneration`. The VS indexed-float opportunity probe then rejects that fallback as a next large target: the safe int/bool tail reduction is only `20.720MB`, `272B` per call, and `5.47%` of batch VS-constant hash bytes. The binding-packet 2-way cache probe then rejects simple associativity: misses/collisions fall `189k→133k`, but cache CPU regresses `706.875→816.355ms`, parent packet CPU regresses `1,835.316→1,939.451ms`, and the code was reverted. The indexed default fast path gates diagnostic/reorder index-byte preparation when no diagnostic or staging path is active, cutting `encode_draw_index_setup_cpu_ms` `636.514→342.602` and index phase `775.311→480.350`, but total encode moves only `-0.73%` and FPS/GPU/completion stay flat/noisy. The argbuf reopen split rejects the idea that `argbuf_open` is mostly actual Metal open work (`open_call=573.804ms`, `reopen_post=891.359ms`), the follow-up pre-open identity skip rejects whole-table reuse for GT1 (`961,473` candidates, `0` skips, VS misses `812,520`, identity-check CPU `956.102ms`), and the post-open residual split shows the old `~319ms` residual is distributed bookkeeping rather than one hidden child. | No Xcode spend from these CPU results alone. Continue no-gputrace work on cbuf upload/probe/repoint residual, binding-packet stronger identity/plan reuse that reduces packet/probe width, shader-stream diversity, direct-construct/interned-state work, compact/interned draw-state storage, and commit_chunk submit-path internals; revisit index setup/source resolve only if a new counter names a non-diagnostic owner. Do not chase broad D3D9 setter no-op guards, slot-30 bind shadowing, dirty-category identity repoint, FFP stream binding, resource-array binding, vertex texture binding, LOD-bias upload, sampler lookup/rehash skip, texture pre-resolve source matching, raw cbuf `setBuffer`, cbuf upload-plan, observer callbacks, default cbuf content hashing, live-range-only cbuf prefix zeroing, full VS/PS cbuf fallback, another sampler `FlatStateSet` rehash removal, binding-packet cache associativity, default diagnostic index-byte preparation, bridge ABI tuning, raw payload-arena byte copy, PSO subview micro-optimization, run-invariant micro-optimization, repeated per-submission draw resource marking, draw-run scan heuristics, shader-layout micro-optimization, uniform copy/FFP build micro-optimization, batch non-constant uniform hashing, adjacent uniform snapshot elision, standalone VS indexed-float partial hashing, whole-table argbuf reuse, or further queued-submission carrier refactors unless cheap instrumentation first proves a new non-zero opportunity. Require same-input visual proof plus repeated no-gputrace counters before promoting stamp-only grouping or any binding/cbuf semantic shortcut to default. [[state-churn-encode-encode-phase.02]], [[state-churn-encode-encode-phase.03]], [[state-churn-encode-encode-phase.04]], [[state-churn-encode-encode-phase.05]], [[state-churn-encode-encode-phase.06]], [[state-churn-encode-encode-phase.07]], [[state-churn-encode-encode-phase.08]], [[state-churn-encode-encode-phase.09]], [[state-churn-encode-encode-phase.10]], [[state-churn-encode-encode-phase.11]], [[state-churn-encode-encode-phase.12]], [[state-churn-encode-encode-phase.13]], [[state-churn-encode-encode-phase.14]], [[state-churn-encode-encode-phase.15]], [[state-churn-encode-encode-phase.16]], [[state-churn-encode-encode-phase.17]], [[state-churn-encode-encode-phase.18]], [[state-churn-encode-encode-phase.19]], [[state-churn-encode-encode-phase.20]], [[state-churn-encode-encode-phase.21]], [[state-churn-encode-encode-phase.22]], [[state-churn-encode-encode-phase.23]], [[state-churn-encode-encode-phase.24]], [[state-churn-encode-encode-phase.25]], [[state-churn-encode-encode-phase.29]], [[state-churn-encode-encode-phase.30]], [[state-churn-encode-encode-phase.31]], [[state-churn-encode-encode-phase.43]], [[state-churn-encode-encode-phase.44]], [[state-churn-encode-encode-phase.45]], [[state-churn-encode-encode-phase.46]], [[state-churn-encode-encode-phase.47]], [[state-churn-encode-encode-phase.48]], [[state-churn-encode-encode-phase.49]], [[state-churn-encode-encode-phase.50]], [[state-churn-encode-encode-phase.55]], [[state-churn-encode-encode-phase.56]], [[state-churn-encode-encode-phase.57]], [[snapshot-cache-snapshot.04]], [[snapshot-cache-snapshot.05]], [[snapshot-cache-snapshot.06]], [[snapshot-cache-snapshot.07]], [[snapshot-cache-snapshot.08]], [[snapshot-cache-snapshot.09]], [[snapshot-cache-snapshot.10]], [[snapshot-cache-snapshot.11]], [[snapshot-cache-snapshot.12]], [[snapshot-cache-snapshot.13]], [[snapshot-cache-snapshot.14]], [[snapshot-cache-snapshot.15]], [[snapshot-cache-snapshot.16]], [[snapshot-cache-snapshot.17]], [[snapshot-cache-snapshot.18]] |

Latest snapshot-cache update: [[snapshot-cache-snapshot.20]] adds
`d3d9_snapshot_uniform_adjacent_same_generation*` counters to test whether
adjacent uniform snapshot elision was merely blocked by the same-state/lane
safety gate. The 120s no-gputrace run reports `0` same-`uniformGeneration`
adjacent submissions, including `0` in the different-state/lane opportunity
bucket, while `d3d9_snapshot_uniform_elided` remains `0`. This closes the
cross-batch uniform-handle carry idea for current GT1 unless a future run first
produces a non-zero adjacent-same-generation sample.

Latest encode-state update: generation/lane fast-path and queued-submission
microfixes close the deep-compare and trivial-copy branches
([[state-churn-encode-encode-phase.32]] through
[[state-churn-encode-encode-phase.37]]). Shared shader bytecode plus sampler
and TSS/render-state flat-capacity compaction reduce copied state width
([[state-churn-encode-encode-phase.38]],
[[state-churn-encode-encode-phase.39]],
[[state-churn-encode-encode-phase.40]],
[[state-churn-encode-encode-phase.42]]). The resource-retention follow-up then
marks batch-front draw resources once while preserving per-draw binding
override/snapshot resource marking
([[state-churn-encode-encode-phase.43]]). The F1 N-1 state-copy elision was
first proven behind `DXMT9_DRAWRUN_GROUP_BY_GEN_LANE=1`
([[state-churn-encode-encode-phase.44]]), then the default-path attribution
showed `411,362` non-front materialized states / `4.209GB` discarded by batch
append ([[state-churn-encode-encode-phase.47]]). The safe subset is now
default: same-stamp continuations elide state copies while the queue keeps
normal compatibility grouping. The 120s scout reports
`submit_draw_run_batch_discarded_state_records 411,362 -> 0`,
`d3d9_snapshot_state_elided=412,180`, and
`d3d9_snapshot_state_copy_cpu_ms 261.001 -> 138.856`, with a normal output
frame and no pipeline skips/errors. GPU/completion remain flat/noisy, so this
is accepted as copy-policy cleanup rather than the FPS owner
([[state-churn-encode-encode-phase.48]]). The current follow-up accepts
queued-submission carrier construction as a bounded CPU target:
`commit_chunk_queue_draw_submission_emplace_cpu_ms=1,123.253`,
`13.99%` of queued-submission CPU, or `0.646ms/present`, so optional-state or
direct-construct work is plausible cleanup but not the whole FPS answer
([[state-churn-encode-encode-phase.45]]). Optional queued-submission carrier
storage then removes about half of that child:
`commit_chunk_queue_draw_submission_emplace_cpu_ms` `1,123.253 -> 573.056`
(`-48.98%`) and queued-submission CPU `8,031.316 -> 7,867.581` (`-2.04%`),
with a normal output frame ([[state-churn-encode-encode-phase.46]]). The
after-run queue residual closes further carrier work for now: queue submission
is `4.522ms/present`, but snapshot is `3.900ms/present`, emplace is
`0.329ms/present`, and the remainder is only `0.292ms/present`; snapshot cache
lookup remains `3.343ms/present`. The render-state entry-count probe
shows GT1 fits in `64` active render states (`max=62`, `gt64=0`), but the
default table already starts at `62`, so the implementation uses a conservative
`128`-slot priority active payload rather than a `64` cap
([[state-churn-encode-encode-phase.41]],
[[state-churn-encode-encode-phase.42]]). The latest structural sizes are
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
cleanup, not the next FPS owner ([[snapshot-cache-snapshot.19]]). The
uniform payload append prereserve probe then rejects a simple reserve-check
cleanup: `append_uniform/present` stays flat (`0.474488 -> 0.474157ms`) and the
parent append bucket regresses, so future work on that child must split lookup
bucket walk, equality compare, payload copy, and lookup linking first
([[state-churn-encode-encode-phase.51]]). That split now exists as attribution:
`draw_uniform_payload_append_copy_cpu_ms=813.196ms` dominates reserve
(`53.018ms`) and link (`62.443ms`), while lookup is `294.215ms` total
(`154.102ms` in bucket chains). Read phase52 as timer attribution, not an
optimization A/B; the next plausible uniform-append lever is fewer or narrower
owned payload copies across the queue boundary
([[state-churn-encode-encode-phase.52]]). The first narrow copy cleanup is now
accepted: in-place `DrawUniformPayloadRecord` construction cuts append-copy
`813.196 -> 602.274ms` and append-uniform parent `1299.014 -> 1128.212ms`, with
visual smoke normal but FPS flat/noisy. Remaining uniform-payload work requires
storage-shape changes, not another construction micro-optimization
([[state-churn-encode-encode-phase.53]]). A follow-up prefetched-PSO resolve
cache is rejected-current: it found `152,261` same-handle hits, but the parent
`encode_draw_pipeline_lookup_cpu_ms` did not fall (`934.420 -> 950.626ms`), so
the experiment code was reverted. Treat pipeline lookup as secondary until a
split counter names a larger subchild; keep the next no-gputrace focus on the
larger encode/snapshot/replay buckets and completion-pacing
([[state-churn-encode-encode-phase.54]]). Returning to that larger encode
child, the argbuf reopen split shows the legacy `argbuf_open` counter is a
reopen-block parent rather than actual Metal open work: `open_call=573.804ms`
versus `reopen_post=891.359ms`, with table bind, cached cbuf repoint, content
probe, and about `319ms` of still-unattributed post-open work. This is
attribution-only and keeps FPS/GPU/completion flat, but it redirects argbuf work
toward pre-open component identity or a further post-open split instead of a
single `openArgbuf()` micro-optimization
([[state-churn-encode-encode-phase.55]]). The pre-open whole-table reuse check
then rejects that direction for GT1 (`961,473` candidates, `0` skips), and the
post-open residual split shows the phase55 `~319ms` residual is distributed
bookkeeping rather than a single hidden child: table probe `50.933ms`, byte
account `51.990ms`, cbuf cache/dirty scans `118.813ms`, and force-dirty
bookkeeping `104.757ms`. The split run adds hot-path timer overhead, so use it
as attribution only ([[state-churn-encode-encode-phase.56]],
[[state-churn-encode-encode-phase.57]]). The binding-packet plan split then
rejects plan construction as the next primary FPS lever: the largest named
child is fragment texture/sampler planning at only `0.204682ms/present`, and
the child timers are default-off via `DXMT9_PERF_BINDING_PACKET_PLAN_SPLIT=1`
because they perturb the parent bucket ([[state-churn-encode-encode-phase.58]]).
The latest uniform-payload backend A/B then rejects slot-local dedup as a
required GT1 default: `DXMT9_DISABLE_DRAW_UNIFORM_PAYLOAD_DEDUP=1` removes
lookup CPU (`276.107 -> 0ms`) and cuts the targeted append-uniform parent
`1041.108 -> 799.528ms`, but extra appends rise `877,508 -> 930,994` and the
broader queue submission bucket is flat (`6813.183 -> 6817.526ms`). Keep the
knob as a diagnostic or repeat-run policy candidate; it does not change the
current FPS owner by itself ([[state-churn-encode-encode-phase.59]]).
The draw-issue split then closes another open CPU bucket as attribution:
`DXMT9_PERF_DRAW_ISSUE_SPLIT=1` shows every GT1 issued draw is indexed,
visibility/non-indexed/expanded/split children are `0`, and the Metal
`drawIndexedPrimitives` call itself accounts for `897.049ms` (`77.0%` of the
issue parent). Do not spend the next iteration on wrapper micro-optimization
around `encode_draw_issue_cpu_ms`; only fewer Metal draw calls or a different
submission model can move that bucket materially
([[state-churn-encode-encode-phase.60]]).

```mermaid
flowchart TD
  Start["candidate for next GT1 Xcode budget"] --> Opaque{"opaque depth-write\ntriangle locality?"}
  Opaque -- "Yes" --> Keep["keep production-shaped\nopaque index-cache opt-in\nnot perf default yet"]
  Opaque -- "No" --> Screen{"strict screen-blend\nwith exact/lsb1 image policy?"}
  Screen -- "Yes" --> Explicit["allow explicit-tolerance artifact"]
  Screen -- "No" --> Broad{"changes primitive order\nin depth-read rows?"}
  Broad -- "Yes" --> Oracle{"final-color / final-writer\nruntime oracle?"}
  Oracle -- "No" --> RejectBroad["reject broad depth-read\nno Xcode spend"]
  Oracle -- "Yes" --> FutureOracle["future semantic proof family"]
  Broad -- "selected no-blend windows" --> ScopedDepth["scoped 60/2 depth-read/no-blend\nrank1 visible fail\nrank2/3/4 color-exact owner-masked"]
  ScopedDepth --> ConflictSelector["primitive-conflict selector\nnon-color metrics overlap\nfinal-color oracle required"]
  ConflictSelector --> ExistingOcc["existing occlusion query\nprimitive-count only"]
  ExistingOcc --> VisScout["Metal visibility scout\nsample counts only\nnot final color"]
  VisScout --> VisJoin["visibility-positive semantic join\npositive samples != final color"]
  Broad -- "No" --> Backend{"non-reorder backend-shape\nbytes/inv preflight clears?"}
  Backend -- "No" --> RejectBackend["reject current backend-shape family"]
  Backend -- "60/0 scoped live-vsout" --> ScopedVsout["Xcode gate rejected\nVS write unchanged"]
  Backend -- "Yes, broader mechanism" --> Spend["worth a new capture"]
  Backend -- "position/binning, mesh/object,\nor isolated PSO-spill" --> NewBackend["new denominator candidate\nmust isolate backend storage\nnot visible VSOut width"]
  Start --> Cpu{"generic CPU frontier\nonly?"}
  Cpu -- "Yes" --> CpuProbe{"has no-gputrace\nphase attribution?"}
  CpuProbe -- "No" --> CpuReject["no Xcode spend\nadd counters first"]
  CpuProbe -- "Yes" --> CpuNarrow["cbuf + packet + snapshot CPU wins\nargbuf fast append accepted CPU win\nstream split names texture/index/shader/raster\ntexture split names fragment resolve/direct\nsampler pre-handle + hash reuse accepted\ntexture pre-resolve + dirty identity rejected\ncbuf hash + build reduced\ncommit_chunk replay split rejects raw bridge owner\nreplay child split names queued submission/snapshot\nissue split = Metal indexed draw call\nnext: cbuf repoint/upload/probe / packet / index+stream\nplus submit-path internals and residual snapshot"]
  CpuNarrow --> Packet21["packet sampler key-hash reuse\naccepted CPU cleanup\nplan -9.97%/present"]
  CpuNarrow --> FullCbufDiag["full cbuf fallback diagnostic\nbytes +519%\nnot default workaround"]

  classDef good fill:#e8f5e8,stroke:#4d8b4d,color:#102a10
  classDef warn fill:#fff3d6,stroke:#b98222,color:#2a1b00
  classDef bad fill:#ffe8e8,stroke:#b64242,color:#2b0d0d
  class Keep,Explicit,FutureOracle,Spend,ScopedDepth good
  class Start,Opaque,Screen,Broad,Oracle,Backend,Cpu,CpuProbe warn
  class NewBackend warn
  class CpuNarrow good
  class RejectBroad,RejectBackend,CpuReject,ConflictSelector,ExistingOcc,ScopedVsout bad
```

## Frame shape

Frame120 (historical bottleneck-shape capture, [[baselines-frame120.01]]):
total **33.611 ms** GPU, top-3 encoders **33.075 ms / 98.4%**; the same
RT/depth pair returns after another pass and accounts for **24.643 ms /
73.3%**. Passes are LLC/MMU/buffer-write limited, **not** ALU- or
texture-read-bound. Run-level: ~14673 passes preserve **167.73 GB** of
tile contents; draw-run submits = **580** against 913714 draws (≈99.94%
fail to batch), broken by const-upload (659938) and stream/IB state
deltas (793059 / 750041).

The current canonical A/B baseline is frame50 normal-source
([[baselines-frame50.01]]): **35.024 ms**, top-3 98.19%, rows 50/2
(56.9%) / 50/1 (24.5%) / 50/0 (16.8%), hidden backend estimate
≈1597.6 MiB. Mid-investigation probes A/B against frame60
([[baselines-frame60.01]]). The current post-visualfix frame60 refresh
([[baselines-frame60.02]]) keeps the same owner after the latest visual/cbuf
identity path: **33.614 ms**, top-3 **32.984 ms / 98.12%**, VS write
**1627.332 MiB**, hidden backend **1597.755 MiB**. Its no-mutate class proxy
([[hidden-backend-storage-shape.04]]) splits residual `60/2` into depth-read,
screen-blend, and standard-alpha classes with `~111-128 MiB` proxy hidden
backend each and `~25-28%` candidate LRU32 reduction. A scoped follow-up
([[mini-replay-bisection-semantic.02]]) proved one `60/2 depth-read +
no-alpha-blend` two-draw candidate exact under standalone same-input replay
(`0` changed pixels, replay LRU32 `-27.6%`) with clear depth and captured D24X8
depth. The rank-1 real-texture replay ([[mini-replay-bisection-texture.02]])
then supplied the missing sampled inputs and rejected exact promotion:
`2 / 786,432` pixels changed, max delta `5`, so the candidate fails both exact
and `lsb1`. A canonical primitive-id replay shows `7` final-writer pixels
changed, confirming a depth-read/depth-write-off order hazard rather than
same-primitive texture noise. The rank-2 follow-up
([[mini-replay-bisection-texture.04]]) keeps final color exact while cutting
LRU32 `19,131 -> 13,194` (`-31.0%`), but canonical primitive ownership still
changes at `809` pixels. Rank 3 ([[mini-replay-bisection-texture.05]]) repeats
the color-exact owner-masked shape with LRU32 `11,398 -> 8,946` (`-21.5%`) and
`52` owner pixels changed. Rank 4 ([[mini-replay-bisection-texture.06]]) also
keeps final color exact with LRU32 `4,237 -> 3,513` (`-17.1%`) and `17` owner
pixels changed. This keeps a stricter selector or Metal visibility scout path
alive for triage, while ruling out a broad same-state-class promotion. The
primitive-conflict selector
scout ([[mini-replay-bisection-texture.07]]) then checks the cheap threshold
family directly: owner-count, depth, UV, and projected-texcoord ranges all
overlap between the visible rank-1 failure and the rank2-4 exact passes. Only
final-color metrics separate the rows, so a real final-color/final-writer policy
is required before any further reorder promotion; Metal visibility can only
triage no-sample cases unless paired with that policy.
The follow-up occlusion feasibility audit
([[mini-replay-bisection-texture.08]]) rejects the current implementation as
that oracle: D3D9 occlusion query resolution is
primitive-count based. The diagnostic Metal visibility scout
([[mini-replay-bisection-texture.09]]) is now wired into dxmt9 draw encoding and
can export per-Metal-draw sample counts after GPU completion, but its first
`60/2` pass shows the old rank-1 `36..37` window and all `large4096` buckets are
sample-visible. The follow-up cache join ([[mini-replay-bisection-texture.10]])
shows zero-sample rows are small `596`-primitive buckets and account for only
`-2,016` of `-182,856` LRU32 delta. That makes visibility useful for no-sample
triage, not a final-color oracle or the hot hidden-backend owner. The semantic
visibility join ([[mini-replay-bisection-texture.11]]) closes the positive side:
rank2 is sample-positive (`39,835` samples) but has no final color, and rank1
and rank3 are both sample-positive but split visible fail versus visible
exact-pass. The current
post-rank4 perf gate
([[hidden-backend-storage-shape.05]]) folds this into the Xcode spend policy:
depth-read reorder is blocked by oracle requirements, while non-reorder
backend-shape work needs a bytes-per-invocation preflight. The
first offline shader preflight ([[hidden-backend-storage-shape.06]]) says the
top `60/2` and `60/1` rows are not promising visible-VSOut-width retries:
`live-vsout` cuts their IR return to `36 B`, but leaves `128 B` scratch. The
rank3 `60/0` row is the only hot row where `live-vsout` also removes visible
scratch, so it became the narrow primitive-order-preserving smoke candidate.
The scoped Xcode follow-up ([[hidden-backend-storage-shape.08]]) rejects that
candidate as a bottleneck fix: `60/0` expected VSOut falls `184 B -> 68 B`, but
VS buffer write remains `224.947 MiB -> 224.990 MiB` and bytes/invocation
remains `1542.722 -> 1543.013`. This closes visible `VSOut` width as the
non-reorder denominator lever. [[hidden-backend-storage-shape.09]] makes the next
budget rule explicit: no more visible-width Xcode retries; spend only on a legal
below-AIR state/parameter-shape hypothesis, final-color/final-writer proof for
sample-visible locality, or a real backend escape path. The
automated gate refresh ([[hidden-backend-storage-shape.13]]) then closes the
stale `live-vsout` shader-smoke queue as `closed-by-xcode-gate`, so the
automation no longer schedules another visible-output smoke after the matching
Xcode rejection. The follow-up alpha static-equivalence gate
([[hidden-backend-storage-shape.10]]) rejects the naive legal shortcut:
current `60/2` large alpha rows are `15` draws / `154,761` primitives split
across screen and standard-alpha blend classes, and none can disable blending
with static color equivalence. The PSO/state churn preflight
([[hidden-backend-storage-shape.11]]) then rejects the current hot rows as an
isolated Xcode candidate: `60/2` has `47` PSO changes, but the same row has
`271` stream-handle changes and `160` IB-handle changes, so current evidence
points at stream/IB and geometry locality before PSO/backend spill. The
stream/IB preflight ([[state-churn-encode-stream.04]]) confirms that this
state-motion signal is handle-dominant (`60/2` combined handle changes/draw
`2.305`, binding tuple changes `160/187`, stream1 extra changes `111`) and not
offset/stride noise. The row-scoped staging A/B
([[state-churn-encode-stream.08]]) keeps the geometry and high-level encoder
shape fixed while dropping `60/2` stream/IB handle changes to `0`; it does not
yet prove a win because it adds explicit copy traffic and leaves offset churn.

Bandwidth framing: frame60's `1627.332 MiB` VS write is already the dominant
single visible counter in the GPU ledger. At `~22 fps`, that is roughly
`37 GB/s` of VS write traffic alone. The base-M1 implication is different from
M1 Pro/Max/Ultra: the former can saturate once read/sampling/depth/color/store
traffic is included, while the latter needs a narrower backend or CPU/pacing
explanation for the same scene shape. This bandwidth framing is a hot-frame GPU
efficiency question, not an average-FPS ownership claim: [[hidden-backend-storage-shape.30]]
separates the two, using `replay.03` as the `3.86x` hidden-write-density
headroom proof and no-gputrace completion counters as the wall-clock pacing
proof.

## What is settled vs open

**Accepted**
- The GPU limiter is hidden vertex/tiler/parameter (TVB) backend storage,
  scaling with VS invocations × hidden per-invocation backend storage. The
  invocation numerator is proved actionable; the actual per-invocation storage
  denominator remains open. [[hidden-backend-storage]], [[tvb-mechanism-proof]]
- Opaque-depth index-cache locality is a real, semantic-safe GPU win, but
  stays opt-in until the remaining index-setup CPU side-effect is reduced or
  amortized by a broader runtime gate. [[index-cache-locality]]
- Several CPU reductions are real (dirty-range reset + FFP-VS slice reuse
  cut cbuf traffic 4.6 GB→~1 GB; binding-override cut encode CPU 10–30%) —
  but every one left GPU frame time flat. [[const-upload]], [[state-churn-encode]], [[snapshot-cache]]

**Rejected as first-order GPU owner**
- Visible `VSOut`/varying width, point-size, half-precision varyings. [[vsout-layout]]
- Translated-shader temp/scratch sizing; owner is below AIR. [[shader-codegen]]
- Current primitive-order-preserving backend-shape probes: half-VSOut moves
  bytes/inv only `-1.94%` and regresses GPU, so it fails the non-reorder gate.
  Offline `live-vsout` also left `60/2`/`60/1` scratch unchanged, and the
  scoped `60/0` Xcode counter gate then rejected the remaining visible-width
  candidate outright. [[hidden-backend-storage]]
- Render/raster state toggles (depth-write, depth-func, cull, scissor) and
  alpha-test; cull moves only the small named-tiled counters (~30 MiB). Large
  alpha blend-off remains a diagnostic backend-state clue, not a legal fix:
  current screen/standard-alpha rows fail static blend-off equivalence.
  [[backend-shape-classifiers]], [[hidden-backend-storage-shape.10]]
- Current PSO/state churn as an Xcode candidate. The hot rows are
  stream/IB-dominant rather than isolated PSO churn, and the per-draw join
  finds no stable stream/IB tuple run where PSO changes independently. PSO /
  backend spill still needs a deliberately controlled A/B before expensive
  counters. [[hidden-backend-storage-shape.11]],
  [[hidden-backend-storage-shape.18]], [[hidden-backend-storage-shape.19]]
- Current stream/IB churn as a production claim. The hot rows are true handle
  churn, and the row-scoped staging A/B proves handle identity can be
  controlled. The Xcode follow-up then rejects handle identity as the
  first-order owner: `60/2` stream/IB handles go to zero while GPU time and VS
  buffer writes stay flat. [[state-churn-encode-stream.04]],
  [[state-churn-encode-stream.08]], [[state-churn-encode-stream.09]]
- Primitive/triangle reorder as a *stable* lever — apparent wins were
  frame-shape/tile-coverage artifacts that did not reproduce on HEAD. [[primitive-reorder-diagnostics]]
- Const-upload payload size, R32F/X8 PixelFormatView suppression. [[const-upload]], [[attachment-pixelformat]]

**Open**
- Which sub-component of the hidden backend dominates (stage-out vs binning
  parameter storage vs compiler spill). [[hidden-backend-storage]]
- Whether a correctness-preserving alpha/backend-state A/B exists after the
  static blend-off gate. The known `large4096+alpha` clue is strong but still
  diagnostic-only. Rifle muzzle fire now has a concrete rename-snapshot
  implementation, so the open performance question is no longer "is the source
  draw globally missing?" but "does the optimized snapshot path preserve the
  round-bloom oracle while removing DISCARD completion waits?" Current FPS should
  remain diagnostic until the snapshot implementation is compared against the
  wait-based correctness baseline and the `Correctness / Visual-Coupling
  Counters` block stays quiet for skipped draws, Metal errors,
  fallback/overflow, render-pass churn, and completion waits. The latest frame60
  smoke has no skipped/error/overflow/hazard-split evidence, so the next cheap
  branch is final-color isolation plus RT/depth/clear/present pass-churn
  comparison, not broad error handling.
  [[hidden-backend-storage-shape.10]],
  [[backend-shape-classifiers-alpha.04]]
- Whether an actual Apple position-only/binning path can avoid hidden
  `[[position]]`/parameter storage. The existing position-only VSOut probe is a
  correctness-invalid visible-output diagnostic, not proof that this backend
  path was enabled or impossible. [[vsout-layout]], [[shader-codegen]]
- Whether Metal 3 mesh/object shaders can move GT1-style FFP/fixed-function
  geometry off the current vertex/tiler storage path. This has not been tried.
- Tile-FFP as a current GT1 FPS lever. The two-stage base-colour draw + tile
  post-pass code exists, but the coverage gate shows zero eligible primitives
  in frame60 hot rows and only `0.005%` eligible primitive share in the partial
  run. It remains a narrow correctness/architecture lever, not a measured GT1
  performance path. [[hidden-backend-storage-shape.15]]
- Whether a deliberately isolated PSO/state-churn A/B changes hidden backend
  layout/spill storage. The current rows do not isolate it: stream/IB handle
  churn dominates PSO changes in the hot encoders, and per-draw stable tuple
  runs contain no independent PSO changes. The present data rejects another
  PSO-churn Xcode spend but does not close the mechanism forever.
  [[hidden-backend-storage-shape.11]], [[hidden-backend-storage-shape.18]],
  [[hidden-backend-storage-shape.19]]
- Whether enough sample-visible locality can be made final-color/final-writer
  safe. The automated ceiling gate now rejects current color-exact/zero-sample
  buckets as too small for another Xcode capture, while leaving the large
  sample-visible bucket open only behind an oracle.
  [[index-cache-locality-screenblend.10]]
- Residual row `50/2` / refreshed `60/2` locality: useful under explicit
  exact/`lsb1` semantic policy for screen-blend, with current rank-1 semantic
  input and target-row Xcode movement prepared but aggregate top-GPU proof
  failed; row follow-up shows the failure is not non-target reordered-cache
  mutation. Class proxy now shows depth-read/screen/alpha `60/2` classes all
  have real `~25-28%` LRU32 ceilings;
  selected depth-read/no-blend two-draw windows have real `-27.6%`, `-31.0%`,
  `-21.5%`, and `-17.1%` LRU32 miss reductions. Rank 1 rejects exact/`lsb1`
  promotion after real textures, while ranks 2-4 are color-exact but
  owner-masked. A primitive-conflict selector scout rejects simple non-color
  thresholds, and the existing D3D9 occlusion query path is primitive-count only.
  dxmt9 now has a diagnostic Metal visibility scout for per-draw sample counts;
  zero counts can triage no-sample work, but positive counts still do not prove
  final color. The `60/2` cache join shows current zero rows are too small to
  own the hot LRU/hidden-backend traffic, and the current semantic ceiling
  projection shows rank2-4 exact-color windows are too small to justify another
  Xcode pass by themselves (`-9,113` LRU32, estimated `-0.071ms`). The path
  remains blocked by final-color/final-writer proof, a stricter runtime-visible
  selector that separates visible failures from masked windows, or a
  non-reorder backend mechanism.
  [[index-cache-locality]], [[hidden-backend-storage-shape.04]],
  [[mini-replay-bisection-semantic.02]], [[mini-replay-bisection-texture.02]],
  [[mini-replay-bisection-texture.04]], [[mini-replay-bisection-texture.05]],
  [[mini-replay-bisection-texture.08]], [[mini-replay-bisection-texture.09]],
  [[mini-replay-bisection-texture.10]]
- Dependency-aware pass coalescing for same RT/depth re-entry (P1). The current
  render-pass-store proof rejects direct texture reads, present/clear/helper
  ownership, and distant live-out for the top rows: dominant `A -> B -> A`
  patterns immediately touch color/depth targets again at distance `1`. The
  encoder-role join narrows the selector to textured/screen-blend depth-read
  passes alternating with opaque untextured depth-write passes; exact handle
  identity rotates and is not the useful selector. The pass-action follow-up
  confirms the stable shape: the depth-read side is color/depth `Load+Store`,
  while the opaque depth-write side is color/depth `Clear+Store`. This rejects a
  simple redundant-store-before-clear interpretation for the dominant pairs; the
  next proof has to preserve D3D9 ordering while moving or coalescing
  `Load+Store` depth-read work around `Clear+Store` opaque work.
  [[render-pass-store]]
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
  [[present-pacing-boundary-latency-ab.06]] rejects dxmt9's explicit boundary
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
  That follow-up [[present-pacing-prepublish-stage.07]] shows the app/Wine/PE
  side is not the long edge: wait-end to unix `commit_chunk` entry is only
  p50/p95 `1.040/2.668ms`, while wait-end to `CommitPublish` remains
  `15.894/29.912ms` and wait-end to Metal commit remains `22.276/54.146ms`.
  Current average-FPS work is therefore back inside dxmt9 commit/replay/submit
  and backend encode, with `commit_chunk_replay_cpu_ms=18981.064`, nested
  `commit_chunk_queue_draw_submission_cpu_ms=8154.509`, and nested
  `d3d9_snapshot_draw_submission_cpu_ms=6636.191` in the new scout. The
  same-sample stage-delta follow-up [[present-pacing-stage-delta.08]] removes
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
  Static follow-up [[state-churn-encode-encode-phase.27]] points at the nested
  snapshot side as an equally important candidate: snapshot cache lookup is
  `6350.751ms`, cache miss is `5271.187ms`, uniform build calls are `928,656`,
  and queue-slot uniform payload dedup appends `874,477` payloads. The key
  unproven assumption is whether declared draw-packet non-binding deltas often
  repeat the current value; if so, invalidating from actual changed reason
  rather than declared delta mask could reduce snapshot misses. The
  `--probe-draw-packet-actual-change` no-gputrace scout
  [[state-churn-encode-encode-phase.28]] rejects that branch for current GT1:
  `draw_packet_declared_nonbinding=419,990`,
  `draw_packet_actual_nonbinding=419,990`, and
  `draw_packet_redundant_nonbinding=0`. The follow-up
  [[snapshot-cache-snapshot.10]] accepts the narrower shader-constant refresh
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
  hashes. Remaining cbuf targets are now cached repoint, upload/setBuffer,
  content probe, and residual timer/dispatch cost.
  Snapshot work is still open, especially residual non-constant payload hashing
  and VS indexed constant fallback, but it is no longer the sole first-order CPU
  owner.
  [[baselines-frame50.05]], [[state-churn-encode-encode-phase.02]],
  [[state-churn-encode-encode-phase.03]], [[state-churn-encode-encode-phase.04]],
  [[state-churn-encode-encode-phase.05]],
  [[state-churn-encode-encode-phase.06]],
  [[state-churn-encode-encode-phase.07]],
  [[state-churn-encode-encode-phase.08]],
  [[state-churn-encode-encode-phase.09]],
  [[state-churn-encode-encode-phase.10]],
  [[state-churn-encode-encode-phase.11]],
  [[state-churn-encode-encode-phase.12]],
  [[state-churn-encode-encode-phase.13]],
  [[state-churn-encode-encode-phase.14]],
  [[state-churn-encode-encode-phase.15]],
  [[state-churn-encode-encode-phase.16]],
  [[state-churn-encode-encode-phase.17]],
  [[state-churn-encode-encode-phase.18]],
  [[state-churn-encode-encode-phase.19]],
  [[state-churn-encode-encode-phase.20]],
  [[state-churn-encode-encode-phase.21]],
  [[state-churn-encode-encode-phase.22]],
  [[state-churn-encode-encode-phase.23]],
  [[state-churn-encode-encode-phase.24]],
  [[state-churn-encode-encode-phase.25]],
  [[snapshot-cache-snapshot.04]],
  [[snapshot-cache-snapshot.05]],
  [[snapshot-cache-snapshot.06]],
  [[snapshot-cache-snapshot.07]],
  [[snapshot-cache-snapshot.08]],
  [[snapshot-cache-snapshot.09]],
  [[snapshot-cache-snapshot.10]],
  [[snapshot-cache]],
  [[present-pacing]]

## Domain index

| Domain | Role | Headline verdict |
|--------|------|------------------|
| [[baselines]] | frame120 / frame50 / frame60 reference captures | shape stable across regimes |
| [[hidden-backend-storage]] | TVB/parameter storage model, VS-write density, scaling | model ACCEPTED; dominant sub-component OPEN; visible `VSOut` gate rejected in [[hidden-backend-storage-shape.08]], stale live-vsout smoke closed in [[hidden-backend-storage-shape.13]], backend escape feasibility triaged in [[hidden-backend-storage-shape.14]], Tile-FFP hot-row coverage rejected in [[hidden-backend-storage-shape.15]], stream/IB handle identity rejected in [[state-churn-encode-stream.09]], seq-range System Trace route attribution accepted in [[hidden-backend-storage-shape.28]], encoder-summary route sidecars enabled in [[hidden-backend-storage-shape.29]], GPU floor vs wall-clock owner split accepted in [[hidden-backend-storage-shape.30]] |
| [[tvb-mechanism-proof]] | VS-inv ↓ → TVB write ↓, row-local + full-frame | ACCEPTED (load-bearing) |
| [[index-cache-locality]] | opaque-depth cache, screen-blend, min-gain, CPU cost | opaque-depth WIN with refreshed frame60 proof; screen-blend target movement passes but aggregate top-GPU proof fails by non-target timing drift |
| [[index-reuse-measurement]] | index reuse, geometry signature/size, state-class | VS-inv tracks cache-miss estimate |
| [[primitive-reorder-diagnostics]] | reverse/min-index/split reorder probes | order = frame-shape artifact, not stable owner |
| [[mini-replay-bisection]] | row-local replay + encoder bisection | reproduced amplification; enabled the proof |
| [[vsout-layout]] | visible varying width attempts | all REJECTED as owner |
| [[shader-codegen]] | temp/scratch trim, offline Metal IR | REJECTED; owner below AIR |
| [[backend-shape-classifiers]] | alpha/depth/cull/scissor/fog/texture/expand | REJECTED/secondary; indexed path mandatory |
| [[attachment-pixelformat]] | R32F / X8 PixelFormatView suppression | secondary (texture-write), not VS owner |
| [[const-upload]] | cbuf/argbuf class/volatility/dirty-range/sparse | CPU amplifier, GPU unmoved |
| [[state-churn-encode]] | stream/IB churn, draw-run, binding override | CPU wins, GPU flat; stream/IB handle-stable A/B accepted as diagnostic in [[state-churn-encode-stream.08]], Xcode rejected handle identity in [[state-churn-encode-stream.09]] |
| [[snapshot-cache]] | D3D9 draw-state snapshot rebuild | historical CPU owner; recovered to residual |
| [[render-pass-store]] | RT/depth re-entry, store DontCare, pass-chain | re-entry real; dominant top rows are immediate role-pair A/B/A target reuse; coalescing OPEN |

Related CPU-side counter design doc: [[overview]].

## How to read this graph

- **Domain overview** = `<domain>.md` (e.g. `index-cache-locality.md`). Each
  has a scope, a hypotheses/verdicts table, a mermaid dependency graph, and a
  synthesis.
Layout: the top level of `docs/perfomance/` holds only the root and the
domain overviews; every experiment lives under its domain's subdirectory.

```
docs/perfomance/
  overview.md                          # this file
  <domain>.md                          # 15 domain overviews
  <domain>/<domain>-<subcat>.<NN>.md   # leaf nodes (one experiment each)
```

- **Leaf node** = `<domain>/<domain>-<subcategory>.<NN>.md`, one experiment per
  file, numbered by execution order within its subcategory. Frontmatter carries
  `workload: 3DMark05 GT1`, `status`
  (accepted/rejected/inconclusive/model/tooling), and `source:` provenance. New
  entries should point at the actual `experiments/output/...` result, `traces/...`
  analysis, exported Xcode counters, or other concrete artifact rather than the
  deleted/retired spec journal. Every experiment is a 3DMark05 GT1 run.
- Links use the wiki-link form, e.g. `[[index-cache-locality]]` (basename
  without `.md`, resolved across subdirectories).
  Follow them like a wiki.
