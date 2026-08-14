---
type: "Spec Gap"
title: "Experiments Gap"
description: "Implementation and evidence gaps for wild integration experiments."
tags: [specs, gap, experiments]
---

# Experiments Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Experiments Layer

✅ Structurally complete for the two-lane contract (R-WILD-1.2 as revised
2026-08-06): the runner, launcher harness, output layout, verified native-lane
sample entries, and the Wine-lane wild catalogue with its manifest-governed
runtime (`R-RT-*`) all exist. The former "partial because Wine, not native
injection" framing described drift against the pre-catalogue clause and is
retired with that revision.

| Area | Status | Spec |
|---|---|---|
| PE Render Tape capture boundary | ⚠️ bounded frame/sequence production capture, one-shot standard-Present output digest, and transactional publisher | The default-off device registry owns exact descriptors/payloads and supported CPU-written contents from creation, then serializes the actual PE shadow plus generation-qualified object and initial-mutation seeds at the arm Present. `DXMT9_RENDER_TAPE_PROFILE=sequence-tape` explicitly keeps two consecutive standard-Present intervals in one journal, preserving one complete digest-backed mutation between them; interval 1 is not sealed or published. `ObjectDefine` expected extent/count and pre-effect unique-subresource summed-byte closure reject incomplete checkpoints. Bounds-checked uncompressed and DXT1/DXT3/DXT5 2D/cube face+mip locks plus complete-buffer locks now close pre-command resource bytes; full locks establish row-tight seeds, while partial locks overlay only an existing exact-size seed and emit the resulting complete content. If an already-lockable 2D texture subresource is first touched by an unseeded partial writable lock, capture may take one exact-owner full CPU-visible lock after user unlock, strip pitch, and record one complete seed/current-content snapshot; it admits only the exact bytes returned by that successful full lock and never admits inferred or otherwise unproven allocation bytes or the partial seed. Full-lock/copy/unlock or identity/extent/generation proof failure rejects only the tape with a typed first diagnostic. Texture-derived 2D surface wrappers now take the same exact-owner fallback after their original surface unlock, after proving the owner texture identity and generation-qualified subresource; standalone, cube, volume, and user-memory surfaces remain surface-owned and do not use the fallback. This capture-only relock may repeat backend dirty/autogen work even though the bytes are read-only; that side-effect/performance debt is not a promotion claim. The exact Firefly Forest GT2 blocker was `incomplete_subresource_seed` for an A8R8G8B8 128x32 resource whose observed first write was only a 728-byte partial update; this lockable CPU-visible closure does not claim general GPU readback, indexed GT2 replay, identity-sidecar completion, or executable projection. For each captured canonical `PRESENT`, the PE owner drains prior replay and renderer work before reserving a one-shot existing-Presenter offscreen mirror; the normal drawable remains intact and the mirror uses the same present PSO/source/gamma in that command buffer. Finish drains the captured replay, flushes its renderer work before checking the ticket, reads tightly packed bytes, hashes SHA-256, and validates fixed result metadata before `PresentComplete`; cancellation/failure aborts only the tape without changing Present HRESULT or publishing. Volume locks, missing initial bytes, unavailable metadata, prior-backbuffer, PresentEx, and broader grammar remain open. |
| `experiments/CATALOGUE.toml` + launcher tree scaffolded | ✅ | R-WILD-5.1 |
| Wine-lane launcher harness (`run_experiment.py`, launcher scripts, manifest-selected runtime staging) | ✅ | R-WILD-1.2 (Wine lane), R-RT-* |
| Internal backbuffer frame dump + SSIM comparison + `result.json` output | ✅ | R-WILD-2.3, R-WILD-4.1 |
| Bootstrap verified entry: `conf-d3d9-wsi-present` on Heroic Wine 11.5 | ✅ | local workflow validation |
| Verified real application entries: `sample-d3d9-basic-hlsl`, `sample-d3d9-tutorial07`, `sample-d3d9-hdr-formats`, `sample-d3d9-dxut-simple`, `sample-d3d9-irrlicht-lights` | ✅ | Heroic Wine 11.5, direct capture, SSIM 1.0000 |
| Initial catalogue from R-WILD-3.1 staged and verified | ✅ | All five required feature groups covered |
| Reference screenshots for initial catalogue entries | ✅ | R-WILD-4.1 |
| Harness script evidence-production contracts (`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, `scripts/run_suites/`) | ⚠️ | `specs/experiments/harness/requirements.md`, `specs/experiments/harness/spec.md` |
| Harness `R-HARN-*` requirement enforcement | ❌ | Requirements in `specs/experiments/harness/requirements.md` are phrased as predicates but nothing evaluates them; no checker exists. Deliberate scope choice on 2026-07-27 for the docs-only specification round — means these documents can corrode exactly as the harness scripts they describe already have |
| `replay` domain harness (`scripts/tools/run_3dmark05_mini_replay.py`) | ✅ | Works as of 2026-07-28. All five defects `specs/experiments/harness/requirements.md` §2-§6 derive their rationale from are fixed, plus a sixth found while debugging them: defects 1 and 2 (direct-cbuf signature rewrite, sliced-stream-offset double-count) in `12348666`; defect 6 — every draw's PSO, depth state, and cull mode collapsed onto draw 0's state — in `07c39ecb`; defect 5 (`--force-fragment-color` returning a bare `float4` from a function declared `FSOut`/`FfpFsOut`) in `fe673fd5`; defect 3 (unrecognized `core::Format` silently becoming `RGBA8Unorm`) in `e2d3ed0e`; and defect 4 (black image, exit 0) in `36a41ad5`. Defect 4's root cause: the generated program bound nothing to fragment `buffer(5)`, while every dxmt9 fragment shader declares `constant FsVolatile& fsVolatile [[buffer(5)]]` and drives an alpha-test switch ending in `discard_fragment()` from its contents — the undefined read discarded every fragment. The vertex stage had always bound its own `DrawVolatile` at the same index; only the fragment volatile was omitted. Verified end-to-end: replaying the GT1 frame60 `enc1` manifest renders the recognisable "Return to Proxycon" interior, 12,231 distinct RGB values over 784,476 of 786,432 non-black pixels, untextured because the replay binds white dummy textures |
| Unified Render Tape `frame-tape` / `sequence-tape` profiles (`R-HARN-7.1`–`7.6`, `R-HARN-REPLAY-7.1`–`7.17`) | ⚠️ bounded refinement, production capture/identity, reduction, sequence identity, and structural projection | Production validation/provider replay retain unchanged canonical chunks. Frame admission covers full-surface `Clear` or one fixed-function textured `DrawPrimitiveUP`; sequence admission covers exactly two such intervals separated by one complete mutation. Native and captured Sikarugir evidence conserve exact frame and two-interval output digests; whole-command reduction/bisection preserves validated closure. The pure projection planner now conserves one `FULL_SNAPSHOT`-anchored contiguous Draw range, canonical locators, exact generations, and ordered blob references without provider effects. DXT1/DXT3/DXT5 2D/cube face+mip CPU-lock closure is captured structurally, but indexed GT2 replay, identity-sidecar completion, and executable projection remain unproven. Shaders/arbitrary declarations, VB/IB, PresentEx, controls/destruction in reducible input, prior-output loads, and broader interval counts remain open. |
| Render Tape v2 to executable GT2 draw-slice (`R-HARN-REPLAY-7.16`/`7.17`) | ⚠️ structural projection foundation only | `device_c_render_tape_projection.*`, `dxmt9-render-tape project`, and native/CLI specs prove fail-closed structural readiness without rewriting wire bytes or changing provider grammar. Actual GT2 projection/replay still lacks captured indexed-draw, VB/IB, shader/declaration, and render/depth attachment closure; no captured GT2 `events.bin` exists. Render Tape v2 also lacks authoritative frame/source/sequence/logical-pass mapping, so only canonical event ordinal plus record index is admitted today. |
| `replay` domain output-validity self-assertion | ✅ | R-HARN-REPLAY-3.1, R-HARN-REPLAY-3.6, instantiating R-HARN-3.1/3.2/3.3. Closed 2026-07-29 (defect 7). `run_3dmark05_mini_replay.py --run` now reads the written color output back, counts distinct RGB triples and non-background pixels, records both in `mini-replay-summary.json` under `validity`, and exits non-zero when the image carries a single distinct value — the exact shape of defect 4's four all-black lanes that printed `draws=229 repeat=1` and exited 0. The threshold is the named constant `MIN_DISTINCT_RGB_VALUES = 2`; no percentage-coverage gate was added because the degenerate case is the only failure shape with evidence behind it. `validity` is present on every invocation, stating explicitly when the assertion did not run. Verified end-to-end: the GT1 frame60 `enc1` manifest passes (12,231 distinct values over 784,476 of 786,432 non-background pixels); the same manifest sliced to its 42 depth-only draws exits 1 naming the degeneracy. The generated program applies the same threshold to the same image as of 2026-07-29 (R-HARN-REPLAY-3.7, row below), so the guarantee no longer depends on which entry point was used |
| `replay` domain replay binary self-asserts when invoked directly | ✅ | R-HARN-REPLAY-3.7, instantiating R-HARN-3.1/3.3. Closed 2026-07-29. The generated Objective-C++ program now counts distinct RGB triples in the same readback buffer `writePpm` wrote from, prints `distinct_rgb=<n>` on the existing `mini replay draws=<N> repeat=<R>` line, and returns `REPLAY_DEGENERATE_EXIT_STATUS` (3) — naming the output path, the count, and the threshold in the wrapper's own wording — when the count is below the threshold. The threshold is interpolated into the generated source from the single `MIN_DISTINCT_RGB_VALUES` constant the wrapper assertion uses, so the two cannot drift. The wrapper assertion stays and is not redundant: it is the only side that records `validity` and diagnoses a missing, truncated, or unreadable image, so the binary's degenerate status is a dedicated value that `run_binary()` carries through to that assertion instead of surfacing as an opaque subprocess error. Verified end-to-end: the 42-depth-only-draw slice invoked directly as `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH=... ./dxmt9-3dmark05-mini-replay` prints `distinct_rgb=1` and exits 3 where it previously printed `mini replay draws=42 repeat=1` and exited 0; the full `enc1` manifest prints `distinct_rgb=12231` and exits 0 |
| `replay` domain pixel results can be proven to execute the path under test | ✅ | R-HARN-REPLAY-3.4, R-HARN-REPLAY-3.5, R-HARN-REPLAY-3.8. Closed 2026-07-29 (defect 7). The two questions stay separate and are answered by separate mechanisms. **Containment** is `coverage`: `mini-replay-summary.json` names the manifest rows and encoders replayed, the draw and shader-variant counts, and the per-`vs_hash`/per-`ps_hash` draw counts (summing to the draw count), plus a `scope` string stating this is a single-encoder slice of one frame that does not establish any branch executed. **Execution** is the opt-in `--prove-executed 'REGEX=>REPLACEMENT'`: it prepares a second shader tree under `<output-dir>/execution-proof/` with `re.sub` applied to every generated `.metal` source, replays it, compares the two images, records `execution_proof` (pattern, files scanned/mutated, sites, differing pixels, verdict), and exits non-zero when they are identical, because an identical image proves the mutated construct was never executed. The two failure modes are separate named verdicts and separate messages: `not-present` (matched no site — the pattern is wrong or the construct is absent; **not** an execution verdict, and decided from the generated sources before anything is compiled) and `present-but-not-executed` (matched sites, byte-identical image — defect 7's shape). Verified against defect 7's own manifest: the literal `dxmt9_cdef<N> : ` pattern reports `not-present` (0 sites across 34 generated sources), because those dumps predate `d63f7a65`'s emission; reconstructing the DEF select over the inputs the dumps do carry (`cFloat[196] = ...` overlays plus `cFloat[clamp(a0.x + N, 0, 255)]` relative reads, in exactly eight vertex shaders) matches 48 sites in 8 of 34 sources and changes 0 of 786,432 pixels, while the unconditional-marker control over the same 48 sites changes 15,134 pixels — independently reproducing the original hand investigation's figure. A pixel-identical replay still must not be used as the correctness oracle for a shader-translator or codegen change unless `--prove-executed` (or equivalent instrumentation) establishes the changed path executes in the replayed window |
| `replay` domain resolved attachment formats recorded in `mini-replay-summary.json` | ✅ | R-HARN-REPLAY-2.3, instantiating R-HARN-2.4. Closed 2026-07-29. `prepare()` now writes an `attachment_formats` block carrying, for colour and depth, the resolved `MTLPixelFormat` name beside the `core::Format` ordinal it was resolved from, the attachment dimensions, whether depth carries stencil, and whether the value came from a declared manifest attachment or the legacy no-`attachments` default. `resolve_attachment_formats()` is the single resolution point: `render_source()` bakes the same record into the generated `.mm`, so the artifact cannot name a format the replay did not render with |
| `docs/perfomance/overview-sfiv.md` SFIV baseline figure | ✅ | Reproduced 2026-07-29. The `11.3` sampled fps that looked like a 4x regression was the `debug` experiment profile — validation layer on, debug logging, 1.0 GB of log. The same build under `perf` gives `43.02` sampled fps with a 22 MB log, inside the documented `44.668`/`45.416`/`42.684` family. The profile now defaults to `perf` (`d431dc1a`) and is recorded in the run output, so the trap that produced the false reading is closed |
| `scripts/tools/summarize_3dmark05_cleanup_candidates.py` citation counting | ❌ | Miscounts brace-expanded citations such as `...-r{1,2,3}-...`, classifying 84 referenced runs (4.5 GB) as unreferenced and eligible for cleanup when they are not |
| `docs/perfomance/` `source:` citation integrity | ❌ | 33 of the 55 distinct `.log` paths mentioned in `docs/perfomance/` are already missing from disk as of a 2026-07-27 scan (34 of 56 when the same scan also covers `docs/`, `agents/`, and `README.md` as a whole, not only `docs/perfomance/`); the citations are dangling |
| Harness domain map (`specs/experiments/harness/spec.md` §1) is a partial partition, not a complete one | ⚠️ | R-HARN-1.1. Of the 86 in-scope harness scripts (`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, `scripts/run_suites/`), mechanically applying the domain map's own `Owns` column (explicit names plus its `scripts/check/*` and `scripts/tools/compare_*` wildcard rows) assigns exactly 41. The remaining 45 are owned by no domain: 11 are named by `reduce/spec.md` §2.4 while it explicitly declines to assign them a domain (verified against that section's own listed 11 filenames); 5 more are named individually elsewhere without a domain assignment (`analyze_indexed_probe_classes.py`, `analyze_shader_dumps.py`, `analyze_xcode_dxmt_encoder_attribution.py` in `join/spec.md` and `join/requirements.md`; `run_with_timeout.py` in `probe/spec.md`; `shader_corpus_tool.py` in `audit/spec.md`); and 29 are not named in any of the sixteen tracked harness documents at all — including `run_dx9_present_policy_ab.py` (661 lines, a documented workflow in `agents/rules/metal_debugging.rules.md` §7), `run_d3d9_conformance.py` (521 lines, whose `:264` sets `DXMT9_PREWARM=disabled` into its launched subprocess with no domain to attach that contract-relevant variable to per parent `spec.md` §4 Rule 1), `analyze_pso_backend_churn.py` (585 lines), and 26 other `scripts/tools/` scripts (mostly `analyze_*`, plus `audit_backend_escape_surface.py`, `cleanup_dxmt9_temp_prefixes.py`, `gen_wine_d3d9_test_inventory.py`, `package_app_local.py`, `plan_backend_escape_reduced_ab.py`, `plan_effect_roi_forcewhite_probes.py`, `run_3dmark05_semantic_replay_gate.py`, `select_3dmark05_payload_window.py`, and `sync_corpus.sh`). No checker enforces domain assignment; this is a documentation-completeness gap, not a runtime one |
| `compare-gate → record` boundary (`specs/experiments/harness/spec.md` §2) cites no parent `R-HARN-*` requirement | ⚠️ | Extracting every `R-HARN-\d+\.\d+` token from each `###` boundary subsection in `spec.md` §2 returns a non-empty match set for seven of the eight boundaries and an empty set for exactly one, the terminal `compare-gate → record` section (`spec.md:254-274`). `specs/experiments/harness/audit/requirements.md` R-HARN-AUDIT-1.2 already discloses this accurately and extends parent principles to the `audit` domain's own `record`-stage requirements by stated analogy rather than inventing a citation; this row records the gap in the parent document without adding a parent requirement to close it, so as not to contradict what `audit`'s own documents already say about it |

### Render Tape bootstrap exact closure (frame-tape capture only)

The frame-tape arm-boundary producer now materializes only the exact
generation-qualified
closure reachable from the validated bootstrap overlay, required Present
output, and recursive descriptor dependencies such as a texture-derived
Surface's parent texture/subresource. Unreferenced incomplete live objects are
pruned; referenced missing, stale-generation, incomplete, or dependency seeds
fail closed with typed attribution. A complete omitted pre-arm object is
materialized just in time before its first command/control reference. Its exact
seed expectation is independent of unrelated events; a missing or incomplete
seed fails closed at that identity's first use. Mutations and destroys of
unadmitted objects remain registry-only. Arbitrary deferred closure for opaque
producer-side references remains out of scope. Sequence-tape retains the
complete all-live arm snapshot because its second interval cannot admit
`ObjectDefine` events.

### GT2 frame-tape retry lifecycle (capture-only)

`PresentOutput` is a capture-owned single-holder role. The admission that names
a new holder hands the role back first, and an arm attempt that does not reach
an active interval hands it back immediately rather than at the next attempt.
A surface holder the admission itself registered and that still carries only
the admission's own wrapper reference is retired into the tombstone set, so the
existing monotone-generation and alias-replacement rules keep applying to it
unchanged; any other holder is demoted back to its exact displaced
initial-content state and stays registered. Retirement is scoped to that proven
swap-chain output handoff — a generic standalone or texture-derived alias the
capture merely re-roled is only ever demoted, so the policy never removes an
entry the alias rules still own. This closes the two r6 GT2 failures in
`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r6-20260814`:
`present_output_count count=2..8` across retries, and the terminal
`prior_not_retained_alias` registry invalidation that a recycled wire object id
produced when it met a stale holder in the logical-slot replacement scan.

The **initial** r6 abort remains open. Its first arm reached an active interval
and then aborted with `first_abort reason=block_resource_mutation`, with no
attribution for which branch failed. Only diagnosis, not a fix, is claimed
here, and the fail-closed behaviour is unchanged. The append can be rejected by
exactly three things — the PE registry shape, blob registration, and the
mutation event's own validation — so the owner now drives the blob-register and
mutation steps separately (the same pair `resourceMutationBytes` performs) and
emits one bounded `mutation_reject` line naming the failing step
(`registry_entry_missing`, `subresource_out_of_range`, `blob_register`,
`mutation_event`), the status, the capture state, identity/subresource/bytes,
whether the identity is still live in the tape, and each counter against its
limit (`event_count`, `buffered_bytes`, `owned_blob_bytes`,
`owned_blob_entries`). That makes the mutation event's fused `InvalidInput`
decidable rather than inferred. At that point no capacity was raised and no
predicate relaxed: no correctness-preserving layout or registry-only treatment was
provable from the existing artifact, so none was applied, and the next GT2 run
must name the failing predicate before any further capacity change is considered.

The subsequent GT2 r7 exact-closure capture attempt log identifies the failing
predicate: line 63 records `detail=blob_register status=CapacityExceeded` with
`owned_blob_bytes=66847615/67108864`, `incoming=524288`, and
`overage=263039`, so that run established a required owned-byte lower bound of
67,371,903. GT2 r8 then proved that the former 68 MiB value was only a prefix
lower bound, not a sufficient total-bundle bound. The capture-only policy is
now a 256 MiB default, an optional decimal-byte
`DXMT9_RENDER_TAPE_MAX_BLOB_BYTES` override, and a hard 1 GiB ceiling;
unset/invalid/zero values use the default and valid over-ceiling values clamp
to 1 GiB. Digest, descriptor, generation, event, and replay validation remain
unchanged. A complete GT2 bundle fitting this policy is still unproven; the r7
capture attempt log is not a bundle artifact, and r8 is not evidence that the
default is sufficient for the whole bundle. PresentOutput retry ownership is a
separate fixed capture correctness issue and is not used to justify this
capacity policy.

### GT2 r9 surface identity closure (capture-only)

GT2 r9 removed the 256 MiB blob-capacity blocker: the capture attempt no longer
fails at `blob_register` capacity. Its next typed failure is
`unmaterialized_pre_arm_object kind=SURFACE`, which identifies a missing
generation-qualified Render Tape registry identity rather than another blob
capacity shortfall. Standalone wrappers from the cached swap-chain
`GetBackBuffer` path and device `GetRenderTarget`/
`GetDepthStencilSurface` now register their exact surface identities before
chunk admission; texture-derived aliases continue to register through their
parent texture dependency. `PresentOutput` is admitted against the same stable
cached backbuffer identity used by commands, with a raw wire-identity check
against the cached PE identity to detect cache drift. Exact-generation
fail-close remains unchanged, and duplicate wrapper references are not added
for explicit `Create*` calls or aliases. Native bridge coverage exercises
standalone registration routing, two wrapper references and balanced release,
alias non-regression, and stale-generation rejection.

### GT2 buffer partial-seed closure (capture-only)

The remaining GT2 seed gap for indexed buffer inputs is now narrowed to one
bounded PE fallback: when the generation-qualified registry reports that the
first writable buffer lock is partial and unseeded, the owner may relock the
same `D9CBuffer` over offset zero and exactly `D9CBufferDesc::size` bytes with
`D3DLOCK_READONLY` after the application unlock succeeds. Only the exact bytes
returned by that lock become the complete zero-offset seed. Identity,
descriptor-extent, generation, lock, copy, or unlock proof failures reject the
tape with a typed diagnostic while preserving the application HRESULT; the
capture-off path performs no relock or copy. The read-lock's possible backend
synchronization/dirty side effects remain explicit capture-only debt, and
`D3DLOCK_DISCARD` is not promoted. This does not widen provider indexed-draw
grammar or claim that GT2 replay is complete.

### GT2 r11 pending alias replacement (capture-only)

GT2 r11
(`experiments/output/app-d3d9-3dmark05-gt2-frame-tape-exact-closure-r11-20260814`)
retained surface identity `kind=1 generation=1 object_id=4294967602` for a
pending command (`object_destroy deferred ... pending=1`) but then rejected the
same exact handle as `unmaterialized_pre_arm_object`. The registry lifetime
truth table classified a texture-derived alias with zero wrapper references as
`RetainedAlias` before considering its pending chunk reference, so a later
wrapper for the same parent/subresource logical slot could replace the registry
entry before `captureCommittedRenderTapeChunk` materialized the old handle.

Pending chunk ownership now takes precedence over retained-alias ownership.
Logical-slot replacement classifies that state as
`PendingChunkRequiresFlush`; the capture-enabled registry synchronously commits,
captures, and drains the existing builder with the child flush reason, then
restarts registration once. This preserves the required order of old exact
generation materialization, old command, pending drain and old destroy before
the new generation definition. Flush failure or an unresolved second attempt
invalidates and aborts only capture, leaving the application call and the
bridge-failure retry contract unchanged. Native coverage binds the production
classification to the bounded old-command-to-new-definition sequence; GT2 r12
wild evidence and cross-build validation remain pending.

## Render Tape bounded wild evidence

The canonical `perf-d3d9-present-loop` experiment was run with
`PRESENT_LOOP_ITERATIONS=2`, the Sikarugir runtime, an existing manifest-owned
prefix, and a fresh PE-visible output root (`Z:\\...`). The app and runner
both exited successfully.

For this run, the runtime was
`/Users/dididi/workspaces/dxmt9/experiments/wine/sikarugir-cx-24.0.7` and the
manifest was `/Users/dididi/workspaces/dxmt9/experiments/wine/manifest.toml`.
The captured bundle was
`experiments/output/render-tape-oracle-final.0ZFP3y/frame-95919862787500-1`;
its
`events.bin` is 2992 bytes with SHA-256
`ed6bb63659a72c60066c0653d4934669dd7f7081e7389f6a110f13a94eb5c7be`.
It contains four events, one object definition, one bootstrap chunk, one
command chunk with `Clear` + `Present`, and zero blobs or mutations. The v2
CLI `validate` and `inspect` commands passed. The `provider-replay` command
returned status `complete` with exit code 0; the result was
`production_capture=true`, `production_provider_replay=true`, and
`output_oracle=true`.

The provider requirements were 256×256, format 21. Readback was 262144 bytes
with SHA-256
`49843e277c6ce8246d199c69c77aba0e7791c50522ab16c6a926f1528bd7474c`, and
object conservation was 1 created / 1 released. Capture and replay both
produced that digest, so
`expected_digest_captured=true` and `expected_digest_matched=true`. Its uniform
clear makes
`output_non_degenerate=false`; that is a limitation of the evidence, not a
failure.

The staged binary SHA-256 values were x64 `d3d9.dll`
`f3b62e8c9d2886a99134f6c00d9027fa9230a2d949cba8ce04b9924e432abadc`, x64
`winemetal.dll`
`9ed9c128a5e21e60b6bc0c887fe860da8eef463940f70791564324ea6c069912`, x86
`d3d9.dll`
`d5160032acb1926988153835a31b8832da0316aa733ad80df20e13b3fd08552c`, x86
`winemetal.dll`
`98cdeacb700bfd159274d526aae0d3b9fc2ccdef84258eacae4be27f3c423d92`, and
Unix `winemetal.so`
`062191018118ba5b617614cb08ae0daa1bc050dd22b3111727bbadb4e5f549fb`.

The production implicit backbuffer is represented as a `PresentOutput` surface
with `initial-content-not-required`; proof for intervals that load prior
backbuffer contents remains open. Direct `PresentEx`/direct-control calls do
not currently emit the canonical `PRESENT` event and therefore fail closed at
`presentChunkSeen`; PresentEx event support is a separate documented gap.

---
