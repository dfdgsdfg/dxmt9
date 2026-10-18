# dxmt9 Environment Variables — Capture / Debug

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (frame capture, dump, validation, and mini-replay payload capture).
A flag is "set" when its value is a non-empty string that is not `0`, unless
documented otherwise. See the index for global notes.

## Capture / Debug

| Var | Purpose | Default | Where |
|---|---|---|---|
| `MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED` | Apple's Metal capture-layer env spellings seen in local workflows. Do **not** set these by default for 3DMark05 perf probes: current Wine/3DMark05 startup can black-screen with capture-layer env present. `run_3dmark05_perf_probe.sh` uses dxmt9's own `DXMT_METAL_CAPTURE_FRAME/PATH` trigger and only adds `MTL_CAPTURE_ENABLED=1` when `DXMT_3DMARK05_SET_MTL_CAPTURE_ENABLED=1` is deliberately set; either env satisfies the file-capture-layer preflight if a caller sets it explicitly | unset | Metal runtime |
| `DXMT9_SYNC_PSO_COMPILE` | Force draw/library PSO compilation to run inline instead of on the async compile queues. `MTL_CAPTURE_ENABLED=1` or `METAL_CAPTURE_ENABLED=1` also enables this path automatically because the capture layer has reproduced a hang while the encode thread waits on an async prefetched PSO future | `0` | `dxmt9_pipeline_cache.cpp` |
| `DXMT_METAL_CAPTURE_FRAME` | Capture this 1-based frame as `.gputrace` | unset | `dxmt9_capture.cpp` |
| `DXMT9_PER_DRAW_DEBUG_GROUPS` | Force the per-draw `Draw[seq=,prim=]` Metal debug groups on (`1`) or off (`0`). Unset means **on only when a capture is configured** (`DXMT_METAL_CAPTURE_FRAME` / `MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED`), because they are read only inside a capture and cost a string format, a bridged `WMT::String` allocation and a push/pop pair per draw — `2.7 ms/present` of encode-thread time on GT2 (`state-churn-encode-append-decomposition.16`). Per-render-pass, blit and present groups are unaffected | capture-gated | `dxmt9_capture.cpp` |
| `DXMT_METAL_CAPTURE_PATH` | Output path for the capture | tmp file | `dxmt9_capture.cpp` |
| `DXMT_METAL_CAPTURE_DESTINATION` | MTLCaptureManager destination: `gpuTraceDocument` / `gputrace` / `file` for file output, or `developerTools` / `xcode` for an attached-Xcode capture route | `gpuTraceDocument` | `dxmt9_capture.cpp` |
| `DXMT_3DMARK05_METAL_CAPTURE_DESTINATION` | 3DMark05 wrapper-only forwarder for `DXMT_METAL_CAPTURE_DESTINATION`; use `developerTools` only when testing Xcode-attached capture without defaulting to `MTL_CAPTURE_ENABLED=1`. Preflight that Xcode can attach to the real Wine child and choose a reachable target frame; this route proves `destination=1` start/stop log lines and does not directly write `frame<N>.gputrace` | unset | `run_3dmark05_perf_probe.sh` |
| `DXMT_3DMARK05_ALLOW_NO_FILE_CAPTURE_LAYER` | Disable the 3DMark05 wrapper's file-capture-layer preflight. Use only for deliberate late-failure diagnostics where `Capture layer is not inserted` at frame capture time is the expected evidence; normal file `.gputrace` runs should fail before Wine launch when the Wine child lacks `MetalCaptureEnabled` and no explicit `MTL_CAPTURE_ENABLED` / `METAL_CAPTURE_ENABLED` env is set | `0` | `run_3dmark05_perf_probe.sh` |
| `DXMT_DUMP_GPU_TEXTURE_HANDLE` | Dump a specific texture (BMP) post-readback | unset | `dxmt9_capture.cpp` |
| `DXMT_DUMP_GPU_TEXTURE_PATH` | Output path for texture BMP | unset | `dxmt9_capture.cpp` |
| `DXMT_DUMP_TEXTURE_HANDLE` / `DXMT_DUMP_TEXTURE_DIR` | CPU-shadow texture dump at upload. Writes the existing BMP view plus a full raw `.bin` for every selected 2D/cube/volume subresource; the raw filename records dimensions, row pitch, and slice pitch so signed/non-color formats and mip chains remain inspectable | unset | core/texture path |
| `DXMT_DUMP_SHADER_DIR` / `DXMT_DUMP_SHADER_BYTECODE_DIR` | Shader translation / bytecode dumps. `DXMT_DUMP_SHADER_DIR` also writes `compiled/<sha256>.metal` and per-attempt JSON at the actual `newLibraryWithSource:options:nil` call, recording the exact UTF-8 source digest, null options, success, architecture, OS and device. Regenerated translation dumps alone are not compilation evidence. Any nonempty directory, including literal `0`, is enabled and diagnostic-only | unset | shader transform; `winemetal_private_api.mm` |
| `DXMT9_PROBE_VERTEX_POSITION_DIR` | Bounded actual-draw clip-position observation using a temporary instrumented PSO and unused vertex slot 29. Writes atomic min/max bits and invocation counts only after the actual CB completes, plus original/instrumented source/PSO and source identity receipts. Requires all selectors below, effect trace + bindings + geometry exactly `1`, and a shader dump directory. Capture uses a frozen capture-source witness; replay requires an exact effect-trace sequence and separate validated source-plan mapping. Always diagnostic, including directory `0` and rejected configurations; not a passive proof of original GPU reads or a policy qualification | unset | `winemetal_vertex_position_probe.mm`, draw diagnostics |
| `DXMT9_PROBE_VERTEX_POSITION_SOURCE_SHA256` | Exact lowercase 64-hex actual compiled vertex-source digest. Only the supported generated entry with an unused slot 29 and a terminal return is instrumented; missing/ambiguous source lineage rejects before effects | required with probe | unix vertex probe |
| `DXMT9_PROBE_VERTEX_POSITION_MAX_DRAWS` | Process-lifetime attempt budget, decimal 1–32, with at most 64 retained original-PSO descriptor associations | required with probe | unix vertex probe |
| `DXMT9_PROBE_VERTEX_POSITION_MAX_VERTICES` | Output capacity / maximum admitted index + 1, decimal 1–8192. Every CPU-readable Shared IB index is bounded before effects | required with probe | unix vertex probe |
| `DXMT9_PROBE_VERTEX_POSITION_INDEX_COUNT` | Exact triangle-list index count, positive multiple of 3, at most 24576. One instance and zero base vertex/instance only; unsupported calls issue the original draw once without probing | required with probe | unix vertex probe |
| `DXMT9_PROBE_VERTEX_POSITION_DETAIL` | `position` (also empty) retains four clip-position words; `skinning-inputs` selects the closed two-bone shape and 23 four-word input/intermediate fields, at most 16 attempts / 2048 vertices. The latter hoists operands and can change arithmetic; baseline pixel calibration is mandatory. Unknown values disable probing | `position` | unix vertex probe |
| `DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256` | Diagnostic compiler intervention: comma-separated 1–8 distinct original lowercase SHA-256 values select bounded generated vertex entries with the position probe's source/terminal-return guard; prepend `#pragma clang fp contract(off)`. Duplicate/malformed/over-bound selections disable transformation. Actual compiled-source/PSO lineage records changed bytes; logs bind original/transformed digests. Unknown shape or preparation failure keeps original source before compilation. Any nonempty request, including `0`, is disqualified from public GPU ranking, even if unmatched; not a production math policy | unset | `MTLDevice_newLibraryFromSource`, vertex probe policy |
| `DXMT_FORCE_PRESENT_TEXTURE_HANDLE` | Force a specific texture as present source | unset | presenter |
| `DXMT_SKIP_TEXTURE_HANDLE` | Suppress texture by handle | unset | presenter / encoder |
| `DXMT_CAPTURE_FRAME` | Internal backbuffer capture frame; also primary frame for multi-frame runs | unset | core / experiment runner |
| `DXMT_CAPTURE_FRAMES` | Comma-separated internal backbuffer capture frame list | unset | core / experiment runner |
| `DXMT_CAPTURE_RANGE` | Inclusive internal backbuffer capture range `start:end:interval` | unset | core / experiment runner |
| `DXMT_EXPERIMENT_CAPTURE_PATH` | Single-frame experiment-harness capture path | unset | experiment runner |
| `DXMT_EXPERIMENT_CAPTURE_DIR` | Multi-frame internal capture output directory (`frameNNNNNN.bmp`) | unset | core / experiment runner |
| `DXMT9_RENDER_TAPE_CAPTURE` | Enable the production Render Tape producer. New captures arm authenticated identity v4 plus execution-topology v1 after seed/checkpoint completion and abort on identity failure without falling back to v2. Capture/output publication does not enable `DXMT9_CPU_READY_TAPE` or select a larger queue; storage follows the independently selected renderer provider. By default capture selects Exact attachment preservation from queue construction; `DXMT9_RENDER_TAPE_ACTION_PROFILE=normal` opts out of that override for capture-only diagnostics. The actual action history is authenticated in `action_profile.bin`; see `specs/experiments/gap.md` for missing Normal replay qualification. **PE fast-path side effect:** this flag disables the per-(texture, level) surface-handle cache, preserving unambiguous ObjectDestroy and alias identities. Capture is therefore not a valid sample of that getter's cost. | `0` | `d3d9_pe_device_tape.cpp`, `d3d9_pe_device_child_surface.cpp`, `dxmt9_device.cpp`, `dxmt9_command_queue.cpp` |
| `DXMT9_RENDER_TAPE_ACTION_PROFILE` | Explicit `normal` keeps ordinary attachment actions during capture; unset/empty or `tape-exact` preserves historical Exact capture. Unknown names disable PE capture without enabling Exact. No effect with capture off. Actual queue history is authenticated; Normal artifacts remain inspectable but unqualified for existing CPU/GPU policy replay. Capture drains/cache exclusion still perturb execution. See R-HARN-REPLAY-9.19 and the bridge environment table. | `tape-exact` | shared capture-policy predicate, PE gate, queue construction |
| `DXMT9_RENDER_TAPE_SKIP_PRESENTS` | Skip this many successful idle Present boundaries before arming the one-interval Render Tape capture. This is a capture-boundary selector only; it does not change command recording or production batching | `0` | `d3d9_pe_device.cpp` |
| `DXMT9_RENDER_TAPE_ARM_REQUEST_PATH` / `DXMT9_RENDER_TAPE_ARM_REQUEST_ID` | Capture-only one-shot external selector. Both required: new drive-qualified Windows file path and nonzero 32-lowercase-hex request ID. Wait for `request_ready`, then publish with `dxmt9-render-tape-arm-request.exe` in the same prefix. Validates PID/creation, exact 64-byte request and bounded expiry; no retry after consumption/rejection, including capture abort. Nonzero `SKIP_PRESENTS` conflicts. At most 600 s waiting; request/bootstrap lifetime at most 30 s. Does not prove engine/camera membership or qualify timing. See `specs/experiments/harness/replay/capture-arm-request.md` | unset | PE capture-only Present boundary |
| `DXMT_LEAK_STATEBLOCKS` | Skip stateblock destruction (leak-check triage) | unset | d3d9 |

## Mini-replay / payload capture

`DXMT9_RENDER_TAPE_OUTPUT_ROOT` is consumed by the **PE** publisher. Under
Wine, pass a drive-qualified absolute path such as `Z:\\Users\\...` after
checking the prefix's drive mapping; a host POSIX `/Users/...` path is not a
Windows absolute path. Confirm `publisher=1` and a published bundle, not just
the benchmark runner's `status=pass`. The path safety/atomic publication
contract is `R-HARN-REPLAY-7.12` in
`specs/experiments/harness/replay/requirements.md`.

`DXMT9_EFFECT_DRAW_TRACE=1` logs selected draw state under its existing
sequence/encoder/material filters. The cold trace includes run-level state
hashes and the actual selected immutable uniform payload's hashes and counts.
`DXMT9_EFFECT_DRAW_TRACE_PAYLOAD_BYTES=1` additionally emits active float/int/bool
bytes (empty text for a zero count); leave it off for smaller binding-only
discriminators. `pass_seq` plus
`encoder` identifies the existing physical pass owner without forcing a pass
break. Compare boundaries in aligned draw order, not numeric IDs across
processes. Run-level hashes alone are not per-draw byte identity
when a DrawRun changes uniform payloads. `DXMT9_EFFECT_DRAW_TRACE_SEQ_MIN/MAX`
bound log volume; confirm that the captured identity sidecar's entire source
interval is covered. `DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA=1` and
`DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED=1` are needed for a whole-frame
state comparison. This diagnostic is default-off, can generate large logs,
and is not a low-observer timing configuration.

`DXMT9_EFFECT_DRAW_TRACE_BINDINGS=1` adds cold `dxmt9-effect-fs-volatile`
rows with the actual post-override fragment-slot-5 bytes and raw alpha override,
and `dxmt9-effect-ffp-vs` rows at successful slot-3 upload/cache-binding points
with actual host half-pixel/viewport/clip bytes. The latter are binding events,
not recomputed per-draw snapshots: carry them only within the same physical
pass owner and report a missing first binding as unknown. These rows honor the
sequence and encoder filters, not material filters; join to issued effect-draw
rows by source/ordinal/command/subdraw. They do not inspect GPU memory and do
not prove a skipped draw issued or every other FFP field matched. Both new
subflags are default-off and bindings also requires the master trace flag.
Bindings also adds `dxmt9-effect-vs-volatile` rows for the actual vertex-slot-5
80-byte push. Effect-draw rows include `bound_cbuf_mode` and the tracked
VS/PS/FFPVS/FFPPS backing slices' byte lengths, readability and SHA-256; with
`PAYLOAD_BYTES=1` they include exact raw bytes. Direct-slot bindings are tracked
in a cold thread-local shadow reset by the actual render-encoder open event;
argument-table bindings use their existing slice cache. A missing binding,
invalid extent or non-shared Metal storage is unknown, never equal. These are
CPU-visible backing bytes, not proof that the GPU read the same address/value.
Serial encoding is the supported scope; do not infer child-worker coverage.

`DXMT9_EFFECT_DRAW_TRACE_CAPTURE_SOURCES_ONLY=1` limits these effect/binding
rows to the exact source selected at `encodeChunk` from the shared pre-Ready
capture registry (pool owner, seq and full source/storage identity). It works
without enabling color readbacks. The master trace flag is still required;
only empty/`0`/`1` are valid and malformed values disable the trace, never widen
it. Effect rows include capture owner/ordinal/raw-ordinal/generations; join them
to the published Tape sidecar to exclude failed attempts. Bounded registry
collisions and unsupported sources mean missing rows. Sequence filters still
intersect this scope; they do not define capture authority. Bind tracking may
carry state from an earlier source of the same physical encoder, but emits no
rows for uncaptured sources. The observer remains diagnostic-only.

`DXMT9_DUMP_DRAW_TEXTURE_CAPTURE_SOURCES_ONLY=1` applies the same exact
owner/source/storage witness to draw-texture collection **before** charging
the dump budget. It requires `REPEAT_PER_PASS=1`, valid `SEQ_MIN/MAX` and
`MAX_DUMPS` in `[1,256]`. Only empty/`0`/`1` are accepted. It works without
effect logging or color readbacks; the output directory and texture selectors
remain required. Sidecar JSON carries `captureSourceOwner`,
`captureSourceOrdinal`, `captureRawOrdinal`, `captureSourceGeneration` and
`captureStorageGeneration`. Join these to the published identity sidecar,
not attempt-local registration. Missing/stale provenance or cold witness
storage failure omits the observation. Normal rendering and the public
strict GPU gate remain unchanged; this is still a timing-invalid diagnostic.

With `DXMT9_EFFECT_DRAW_TRACE_GEOMETRY=1`, a separate
`dxmt9-effect-geometry-bytes` row hashes the bounded original index range and
the referenced stream-0 min/max span after base-vertex/offset/stride. Setting
`DXMT9_EFFECT_DRAW_TRACE_GEOMETRY_MAX_REFS=0` retains that summary without the
decoded-vertex sample loop. This is a CPU-visible geometry discriminator, not
an acceptance oracle: other streams, GPU-written contents, intervening unused
vertices, padding and hash collisions require independent localization.
Bounds/readability failures are not matching geometry evidence. It neither
waits for the GPU nor closes a render pass.

With both `BINDINGS=1` and `GEOMETRY=1`,
`dxmt9-effect-issued-geometry` separately observes the actual indexed Metal
call and the last successful stream-0/80-byte DrawVolatile binding on that
encoder. It records final index offset/count/type, native/volatile base vertex,
stream offset/stride and bounded CPU-Shared range hashes (64 MiB per range).
The cold shadow resets on actual encoder creation; rejected draw filters clear
selection before returning. Fake/suppressed calls emit no issued witness.
Require `index_readable`, `stream0_readable` and `volatile_known`, plus matching
source/draw identity, before comparing: empty-span hashes are not equality
evidence. Missing/invalid bindings, zero stride, instanced stream-0, negative or
overflowing effective vertices and private storage remain unknown. Different
physical offsets can be valid staging rebases with equal bytes. This does not
cover non-indexed/expanded draws, parallel children or actual
GPU-read/later-mutation behavior; it remains a timing-invalid diagnostic.

The same flags add `dxmt9-effect-issued-stream` for declared programmable
vertex streams 1–15 on indexed draws. Actual `setVertexBuffer` calls provide
the buffer and offset at Metal slots 6–20; `setBytes` invalidates that buffer
witness. Require `binding_known=1` and `readable=1`. The range uses the issued
index/base-vertex values or DrawVolatile instance divisor and native
base-instance/count. `layout_scope=expected-declaration-stride` explicitly
means the stride comes from the expected declaration, not reflection of the
selected compiled PSO. Declared-but-unused inputs, padding, private/unreadable
storage and subsequent GPU/CPU mutation remain outside this byte-hash proof.
The three-draw visible native fixture uses stream 1 for POSITION at a nonzero
binding offset; changing only that region changes pixels on both replay routes.

The same combined flags emit `dxmt9-effect-issued-raster` on both indexed and
non-indexed real draw calls. Per-encoder cold state records actual viewport,
scissor, rasterizer and blend/stencil-reference calls, exact scalar bytes and
independent known flags. A missing blend-color call is unknown, not a zero
color. PSO labels are diagnostic text only: `encodeDraw` rewrites them from the
desired shader-variant hash, so they cannot authenticate the actual compiled
source or pipeline descriptor. Capture-source and draw filters still apply.

With `DXMT_DUMP_SHADER_DIR` also set before process start, conventional PSO
creation now follows the actual source-library → named-function → PSO object
chain. `pipeline_creation_known=1` on an issued raster row names immutable
`pipelines/<pipeline_creation_sha256>.json` plus full VS/FS source SHA-256.
The JSON serializes the actual descriptor's color/blend, depth/stencil format,
sample/raster, topology/tessellation and buffer-mutability fields. Authenticate
that file and its `compiled/<source_sha256>.metal` inputs; neither desired state
nor the label is used for this association. Binary-only libraries, specialized
functions, tile/mesh pipelines and missing/publication-failed/conflicting
lineage are unknown. Cold object associations are capped at 16,384 total and
die with the actual object; the observer does not retain Metal resources.
This proves construction inputs, not driver-internal binary choices or GPU
execution. It does not change the strict original-pixel policy gate.

The binding+geometry observer also emits `dxmt9-effect-issued-sampling` rows.
Decode `json_utf8` as UTF-8 JSON: direct fragment texture/view descriptors and
sampler creation fields come from actual Metal calls, not expected Pool state.
Actual slot-30 buffer/offset and argument-encoder writes are observed separately;
`argument_table_known` requires a bounded Shared table's current SHA-256 to
match the tracked writes. Reopening, setBytes, cache eviction and unobserved raw
mutation invalidate old entries. The table flag only authenticates the tracked
byte envelope; missing individual entries remain unknown. A subsequent observed write cannot restore
unobserved entries. Missing sampler creation remains unknown. Storage is capped
at 16 direct stages, 32 argument indices, 4 KiB per table and 64 slices per
backing buffer; associations contain value snapshots, not retained Metal
resources. This is fragment issued-descriptor evidence, not selected shader
ABI, cross-process resource identity, texture content, residency or GPU-read
proof. Capture/draw filters still gate emitted rows; collection is diagnostic.

Actual texture descriptors additionally carry a process-local `object_token`;
selected-view texture readbacks carry `metalTextureObservationId`. Nonzero
equality joins the same live Metal object within that process, not a Pool handle
or a reused address. Object associations contain monotonic numeric tokens and
do not retain textures. Disabled/failed observation returns zero (unknown).
Cross-process joins still require canonical Tape identity, source/pass/draw
provenance, complete subresource extents and an explicit no-writer interval
between sampling and readback; token equality alone is not content or GPU-read
proof.

The same binding+geometry gate emits `dxmt9-effect-issued-depth` rows. Its
`json_utf8` snapshots the successful actual Metal depth/stencil creation and
bind, including depth compare/write, both stencil faces, and actual stencil
reference updates. Fresh encoders, missing creation and conflicting creation
remain unobserved/unknown/ambiguous rather than adopting desired runtime state.
Null is a defensive observation state, not permission to bind nil under Metal
validation. Associations hold value snapshots only; output is bounded to 4 KiB.
This does not prove attachment contents or which GPU depth/stencil tests passed.

On direct-uniform routes the same gate adds
`bound_{vs,ps,ffpvs,ffpps}_issued_binding` to selected effect-draw rows. This
compares independently observed successful Metal buffer/offset calls at
vertex/fragment slots 0/3 against the high-level tracked slice using non-reused
process-local object tokens and offsets. setBytes invalidates a buffer witness.
`unobserved`, `explicit-null`, `inline-bytes`, `unknown`, `mismatch` and
`untracked` must not be treated as `match`. Argument-buffer routes omit these
fields. Associations retain numeric/value snapshots only, never Metal buffers.
This detects a skipped direct rebind even when tracked bytes look correct;
it does not prove later GPU-read bytes, resource lifetime or old original binds.

`DXMT9_DUMP_COLOR_ATTACHMENT_WIDTH` / `HEIGHT` optionally select exact positive
uint32 subresource extents before GPU readback and budget admission. They
intersect all existing selectors, including source authority; they do not
identify a resource or imply initialized/live pixels. Zero, malformed or
overflowing values disable collection. Use an extent to exclude unrelated
large float attachments from a bounded scene-color diagnostic; keep exact
source/store/channel joins and never infer a passing original frame.

Diagnostic-only knobs for dumping replayable per-draw payloads and for the
`scripts/tools/run_3dmark05_mini_replay.py` standalone replay harness. The dump
vars reuse the reverse-indexed-triangle row/class/span filters and the indexed
triangle encoder draw range; they read geometry/cbuf bytes on the draw path, so
use only in paired diagnostic probes.

Color-pass JSON records `storeActionKnown`, `effectiveStoreAction`,
`sampleCount` and `storedAttachmentContents` from the resolved attachment
ledger. A stored endpoint still needs initialized/live-content and canonical
identity proof before semantic comparison. Unknown/DontCare/resolve-only or
unsupported multisample bytes, including old metadata with no store witness,
are diagnostic-only and must not identify the first semantic divergence. The
observer does not force Store or change pass cuts.

Completed tight BGRA8/RGBA8 color readbacks also record `rawChannelOrder` and
four `channelSha256` values over the unconverted storage-channel bytes. Other
formats/extents report null channel witnesses. Use these only to distinguish
RGB from alpha differences; an X8 alpha mismatch is not automatically a visible
RGB mismatch. Channel hashes do not replace the original full-byte oracle,
prove defined contents or make a diagnostic run eligible for policy ranking.

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_DUMP_INDEXED_GEOMETRY_DIR` | Directory for dumping per-draw indexed triangle geometry payloads (index + stream0/stream1 bytes). Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry`. Filtered by the reverse-indexed row/class/span filters and the encoder draw range | unset |
| `DXMT9_DUMP_INDEXED_GEOMETRY_CBUFS` | Also dump real per-draw uniform (cbuf) payloads beside the geometry dumps. Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry-cbufs` | `0` |
| `DXMT9_DUMP_INDEXED_GEOMETRY_MAX_DRAWS` | Cap on the number of indexed draw geometry payloads dumped. Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry-max-draws` | `16` |
| `DXMT9_DUMP_INDEXED_GEOMETRY_VS` / `DXMT9_DUMP_INDEXED_GEOMETRY_PS` | Filter the geometry dump by VS/PS shader hash (decimal or `0x`-prefixed, matching `3dmark05-perf-indexed-probe-draws.csv`). Exposed by `run_3dmark05_perf_probe.sh --dump-indexed-geometry-vs/-ps` | unset |
| `DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE` / `_PATH` / `_SEQ` / `_ENC` | Dump a render pass's raw D24X8 depth attachment sidecar for mini-replay `--depth-input`. `_HANDLE` selects the depth texture handle and `_PATH` the output file (both required to enable); `_SEQ`/`_ENC` optionally scope to one render-pass seq/encoder. Exposed by `run_3dmark05_perf_probe.sh --dump-depth-attachment-handle/-path/-seq/-enc` | unset |
| `DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW` / `_PATH` / `_DIR` / `_HANDLE` / `_INDEX` / `_SEQ` / `_ENC` / `_DRAW(S)` / `_COMMAND_INDEX(_MIN/_MAX)` / `_COMMAND_DRAW_INDEX` / `_TEXTURE0(S)` | Dump a raw color attachment at a selected pass end or, with `AFTER_DRAW=1`, immediately after a selected replay draw. `_COMMAND_DRAW_INDEX` addresses an exact subdraw within an expanded DrawRun command, avoiding ambiguity when all subdraws share one encoder draw ordinal. `_PATH` captures the first match; `_DIR` preserves distinct matching identities | unset |
| `DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH` / `_ROIS` | Append selected color-attachment RGB statistics to CSV. ROIs use `left,top,right,bottom:name` separated by semicolons; names should not contain commas. With no raw `_PATH`/`_DIR`, readback covers only the clipped union bounding box. Requires an attachment/pass/draw selector; `_HANDLE` selects the surface, not its alias texture. With `AFTER_DRAW=0`, a blit is added at existing pass ends without requesting a draw split. One-pixel ROIs give exact RGB values for the supported 8-bit BGRA diagnostic, but omit alpha and do not observe intra-pass writes. Every nonempty summary path, including literal `0`, is diagnostic-only and cannot qualify GPU policy ranking | unset |
| `DXMT9_DUMP_COLOR_ATTACHMENT_FRAME` / `_MAX_DUMPS` | Select one positive 1-based encoded-Present interval on a dedicated queue encode thread. `FRAME` requires explicit `MAX_DUMPS` in 1–256 for raw dumps (1–16384 for `HASH_ONLY=1`); malformed/zero/out-of-bound selectors disable the observer. Source changes do not advance the frame; an encoded Present advances once. A changed pool owner or decreasing source sequence resets the cold cursor. This ordinal is not PE capture authority, successful presentation, or GPU completion: independently join every retained seq/enc to the captured identity sidecar. Metadata records `encodedFrame`; missing coverage or budget exhaustion is not a matching pass. Existing path/attachment selectors and diagnostic-only policy rejection still apply | unset |
| `DXMT9_DUMP_COLOR_ATTACHMENT_HASH_ONLY` | Exact `0`/`1` selector. With `1`, retain only `.bin.json` metadata and SHA-256 of the completed raw readback, never a `.bin` image. Requires a path/directory, an attachment/pass selector, and explicit `MAX_DUMPS` in 1–16384. Hashes cover tight raw bytes without channel conversion or alpha normalization; raw mode also emits the hash. Existing raw bytes at a hash-only destination reject that write. This bounds disk output, not GPU readback volume, allocations, or timing perturbation; no pass split is requested with `AFTER_DRAW=0`. Hash-only runs remain diagnostic and cannot qualify policies | unset |
| `DXMT9_DUMP_COLOR_ATTACHMENT_DEPTH_PAIR` | Exact `0`/`1`. Add native float32 depth `.depth.bin` plus `.depth.json` alongside each selected color pass-end hash. Requires bounded hash-only color observation, `AFTER_DRAW=0`, and `MAX_DUMPS` in 1–256. Reuses color selection/source provenance and budget; only stored, same-extent single-level 2D depth without logical stencil/MSAA is supported: native Depth32Float (252), or depth-plane extraction from logical D24X8 backed by Depth32Float_Stencil8 (260). Missing/discarded/unsupported depth emits no witness. No forced Store, numeric conversion or pass split; up to width×height×4 additional disk bytes per selected pass. Diagnostic-only, not a final oracle | unset |
| `DXMT9_DUMP_COLOR_ATTACHMENT_CAPTURE_SOURCES_ONLY` | Exact `0`/`1`, requires bounded hash-only mode. Observe only sources whose valid Arena capture identity was registered under the queue lock before Ready consumption. The cold 1024-entry table matches pool owner, exact seq and full source/storage generation-qualified identity against the actual encoder source. Queue teardown forgets that owner; collision/unsupported route/missing identity yields missing observation, never a match. Metadata retains publication ordinals/generations. This includes failed capture attempts: join the retained rows to the independently validated **published** Tape sidecar before comparing. It is not capture-success authority and does not support inferring coverage from an encoded-frame guess | unset |
| `DXMT9_DUMP_DRAW_TEXTURE_HANDLE(S)` / `_TEXTURE0_ANY` / `_TEXTURE0_WIDTH` / `_TEXTURE0_HEIGHT` / `_TEXTURE0_FORMAT` / `_SEQ` / `_SEQ_MIN` / `_SEQ_MAX` / `_ENC` / `_DIR` | Read back a shader-visible texture after the existing pass ends for resource-state diagnostics. `_DIR` plus a handle, texture-0 classifier, PS or slot selector enables the dump. `_SEQ` selects one exact source sequence; `_SEQ_MIN/MAX` bound the inclusive sequence interval; `_ENC` may further restrict the encoder. Default behavior dumps each handle/view/stage only once per process | unset |
| `DXMT9_DUMP_DRAW_TEXTURE_PS` / `_SLOTS` | Restrict to a pixel-shader hash (decimal or `0x`) and logical texture indices (fragment 0–15, vertex 16–19). Selectors intersect existing filters. Malformed selectors or out-of-range slots disable the observer with a diagnostic, never widen selection | unset |
| `DXMT9_DUMP_DRAW_TEXTURE_REPEAT_PER_PASS` / `_MAX_DUMPS` | Opt in to repeated readback keyed by exact handle/view/stage/sequence/encoder. Requires explicit finite `SEQ_MIN`, `SEQ_MAX` and `MAX_DUMPS` in 1–256; invalid or inverted bounds disable the observer. The cap counts distinct scheduled readbacks, not frames. Existing pass boundaries are preserved; readbacks and cold bookkeeping still perturb execution and cannot qualify a policy measurement | `0` / unset |
| `DXMT9_MINI_REPLAY_CAPTURE_PATH` | `run_3dmark05_mini_replay.py` `.gputrace` capture output path | unset |
| `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH` | `run_3dmark05_mini_replay.py` replay color output image path | unset |
| `DXMT9_MINI_REPLAY_REPEAT` | `run_3dmark05_mini_replay.py` replay repeat count | `1` |
| `DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB` | Free-space guard (MiB) before a mini-replay capture; overridable with `--min-capture-free-mb` | `2048` |
