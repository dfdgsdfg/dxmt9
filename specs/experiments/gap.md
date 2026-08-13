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
| PE Render Tape capture boundary | ⚠️ production device-owned bootstrap producer with fail-closed supported-content scope and transactional publisher | The default-off device registry owns exact descriptors/payloads and supported CPU-written contents from creation, then serializes the actual PE shadow plus generation-qualified object and initial-mutation seeds at the arm Present. `ObjectDefine` expected extent/count and pre-effect unique-subresource summed-byte closure reject incomplete checkpoints. Full uncompressed 2D and complete-buffer seeds are supported; partial/BC layouts, cube/volume locks, missing initial bytes, and unavailable metadata abort. With `DXMT9_RENDER_TAPE_OUTPUT_ROOT` set to a safe PE-visible absolute root, the internal publisher writes one unique v2 frame directory using same-filesystem staging and atomic rename; unset or unsafe roots leave capture inert. One bounded production capture has established structural wild identity, and its provider replay now passes; the prior-backbuffer, PresentEx, and broader-grammar limits remain open. |
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
| Unified Render Tape `frame-tape` / `sequence-tape` profiles (`R-HARN-7.1`–`7.6`, `R-HARN-REPLAY-7.1`–`7.11`) | ⚠️ bounded production-provider identity slice, artifact host, bundle command, exact native Metal Clear oracle, and one captured-bundle replay complete | `device_c_render_tape.*` defines the pointer-free tape and production PE capture boundary. `device_c_render_tape_provider.*` now preflights the complete tape and actual blob catalogue before effects, accepts either the exact explicit RT0 identity or the production implicit default RT0, reconstructs replay-owned generation-qualified surface/buffer/2D-texture wrappers, applies complete seeds through production lock/unlock paths with checked texture extents, and sends unchanged canonical chunks through the existing importer, `DeviceReplaySink`, retention ledger, queue, and completion paths. A typed `OffscreenPresentOutput` replaces only drawable acquisition/scheduling while sharing the production present pass and readback. `dxmt9-render-tape-provider` plus `run_dxmt9_render_tape.py provider-replay` validate components, hash actual blobs, construct the preflight-matched device, and report validity/coverage/conservation/output-oracle scope. The accepted grammar is intentionally one uncompressed single-sample colour output and exactly full-surface `Clear` → identity `Present`; unsupported records, descriptors, partial state/content, stale generations, and ordinal mismatches fail closed. Native tests cover the four-event implicit-output capture shape, explicit/wrong/null binding classification, exact digest comparison, checked texture extent, routing, cleanup, and real Metal offscreen readback. One bounded captured `perf-d3d9-present-loop` bundle now replays through the production provider; `sequence-tape`, broader frame grammar, reducer/bisect tooling, and tape-to-draw-slice projection remain open. |
| `replay` domain output-validity self-assertion | ✅ | R-HARN-REPLAY-3.1, R-HARN-REPLAY-3.6, instantiating R-HARN-3.1/3.2/3.3. Closed 2026-07-29 (defect 7). `run_3dmark05_mini_replay.py --run` now reads the written color output back, counts distinct RGB triples and non-background pixels, records both in `mini-replay-summary.json` under `validity`, and exits non-zero when the image carries a single distinct value — the exact shape of defect 4's four all-black lanes that printed `draws=229 repeat=1` and exited 0. The threshold is the named constant `MIN_DISTINCT_RGB_VALUES = 2`; no percentage-coverage gate was added because the degenerate case is the only failure shape with evidence behind it. `validity` is present on every invocation, stating explicitly when the assertion did not run. Verified end-to-end: the GT1 frame60 `enc1` manifest passes (12,231 distinct values over 784,476 of 786,432 non-background pixels); the same manifest sliced to its 42 depth-only draws exits 1 naming the degeneracy. The generated program applies the same threshold to the same image as of 2026-07-29 (R-HARN-REPLAY-3.7, row below), so the guarantee no longer depends on which entry point was used |
| `replay` domain replay binary self-asserts when invoked directly | ✅ | R-HARN-REPLAY-3.7, instantiating R-HARN-3.1/3.3. Closed 2026-07-29. The generated Objective-C++ program now counts distinct RGB triples in the same readback buffer `writePpm` wrote from, prints `distinct_rgb=<n>` on the existing `mini replay draws=<N> repeat=<R>` line, and returns `REPLAY_DEGENERATE_EXIT_STATUS` (3) — naming the output path, the count, and the threshold in the wrapper's own wording — when the count is below the threshold. The threshold is interpolated into the generated source from the single `MIN_DISTINCT_RGB_VALUES` constant the wrapper assertion uses, so the two cannot drift. The wrapper assertion stays and is not redundant: it is the only side that records `validity` and diagnoses a missing, truncated, or unreadable image, so the binary's degenerate status is a dedicated value that `run_binary()` carries through to that assertion instead of surfacing as an opaque subprocess error. Verified end-to-end: the 42-depth-only-draw slice invoked directly as `DXMT9_MINI_REPLAY_COLOR_OUTPUT_PATH=... ./dxmt9-3dmark05-mini-replay` prints `distinct_rgb=1` and exits 3 where it previously printed `mini replay draws=42 repeat=1` and exited 0; the full `enc1` manifest prints `distinct_rgb=12231` and exits 0 |
| `replay` domain pixel results can be proven to execute the path under test | ✅ | R-HARN-REPLAY-3.4, R-HARN-REPLAY-3.5, R-HARN-REPLAY-3.8. Closed 2026-07-29 (defect 7). The two questions stay separate and are answered by separate mechanisms. **Containment** is `coverage`: `mini-replay-summary.json` names the manifest rows and encoders replayed, the draw and shader-variant counts, and the per-`vs_hash`/per-`ps_hash` draw counts (summing to the draw count), plus a `scope` string stating this is a single-encoder slice of one frame that does not establish any branch executed. **Execution** is the opt-in `--prove-executed 'REGEX=>REPLACEMENT'`: it prepares a second shader tree under `<output-dir>/execution-proof/` with `re.sub` applied to every generated `.metal` source, replays it, compares the two images, records `execution_proof` (pattern, files scanned/mutated, sites, differing pixels, verdict), and exits non-zero when they are identical, because an identical image proves the mutated construct was never executed. The two failure modes are separate named verdicts and separate messages: `not-present` (matched no site — the pattern is wrong or the construct is absent; **not** an execution verdict, and decided from the generated sources before anything is compiled) and `present-but-not-executed` (matched sites, byte-identical image — defect 7's shape). Verified against defect 7's own manifest: the literal `dxmt9_cdef<N> : ` pattern reports `not-present` (0 sites across 34 generated sources), because those dumps predate `d63f7a65`'s emission; reconstructing the DEF select over the inputs the dumps do carry (`cFloat[196] = ...` overlays plus `cFloat[clamp(a0.x + N, 0, 255)]` relative reads, in exactly eight vertex shaders) matches 48 sites in 8 of 34 sources and changes 0 of 786,432 pixels, while the unconditional-marker control over the same 48 sites changes 15,134 pixels — independently reproducing the original hand investigation's figure. A pixel-identical replay still must not be used as the correctness oracle for a shader-translator or codegen change unless `--prove-executed` (or equivalent instrumentation) establishes the changed path executes in the replayed window |
| `replay` domain resolved attachment formats recorded in `mini-replay-summary.json` | ✅ | R-HARN-REPLAY-2.3, instantiating R-HARN-2.4. Closed 2026-07-29. `prepare()` now writes an `attachment_formats` block carrying, for colour and depth, the resolved `MTLPixelFormat` name beside the `core::Format` ordinal it was resolved from, the attachment dimensions, whether depth carries stencil, and whether the value came from a declared manifest attachment or the legacy no-`attachments` default. `resolve_attachment_formats()` is the single resolution point: `render_source()` bakes the same record into the generated `.mm`, so the artifact cannot name a format the replay did not render with |
| `docs/perfomance/overview-sfiv.md` SFIV baseline figure | ✅ | Reproduced 2026-07-29. The `11.3` sampled fps that looked like a 4x regression was the `debug` experiment profile — validation layer on, debug logging, 1.0 GB of log. The same build under `perf` gives `43.02` sampled fps with a 22 MB log, inside the documented `44.668`/`45.416`/`42.684` family. The profile now defaults to `perf` (`d431dc1a`) and is recorded in the run output, so the trap that produced the false reading is closed |
| `scripts/tools/summarize_3dmark05_cleanup_candidates.py` citation counting | ❌ | Miscounts brace-expanded citations such as `...-r{1,2,3}-...`, classifying 84 referenced runs (4.5 GB) as unreferenced and eligible for cleanup when they are not |
| `docs/perfomance/` `source:` citation integrity | ❌ | 33 of the 55 distinct `.log` paths mentioned in `docs/perfomance/` are already missing from disk as of a 2026-07-27 scan (34 of 56 when the same scan also covers `docs/`, `agents/`, and `README.md` as a whole, not only `docs/perfomance/`); the citations are dangling |
| Harness domain map (`specs/experiments/harness/spec.md` §1) is a partial partition, not a complete one | ⚠️ | R-HARN-1.1. Of the 86 in-scope harness scripts (`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, `scripts/run_suites/`), mechanically applying the domain map's own `Owns` column (explicit names plus its `scripts/check/*` and `scripts/tools/compare_*` wildcard rows) assigns exactly 41. The remaining 45 are owned by no domain: 11 are named by `reduce/spec.md` §2.4 while it explicitly declines to assign them a domain (verified against that section's own listed 11 filenames); 5 more are named individually elsewhere without a domain assignment (`analyze_indexed_probe_classes.py`, `analyze_shader_dumps.py`, `analyze_xcode_dxmt_encoder_attribution.py` in `join/spec.md` and `join/requirements.md`; `run_with_timeout.py` in `probe/spec.md`; `shader_corpus_tool.py` in `audit/spec.md`); and 29 are not named in any of the sixteen tracked harness documents at all — including `run_dx9_present_policy_ab.py` (661 lines, a documented workflow in `agents/rules/metal_debugging.rules.md` §7), `run_d3d9_conformance.py` (521 lines, whose `:264` sets `DXMT9_PREWARM=disabled` into its launched subprocess with no domain to attach that contract-relevant variable to per parent `spec.md` §4 Rule 1), `analyze_pso_backend_churn.py` (585 lines), and 26 other `scripts/tools/` scripts (mostly `analyze_*`, plus `audit_backend_escape_surface.py`, `cleanup_dxmt9_temp_prefixes.py`, `gen_wine_d3d9_test_inventory.py`, `package_app_local.py`, `plan_backend_escape_reduced_ab.py`, `plan_effect_roi_forcewhite_probes.py`, `run_3dmark05_semantic_replay_gate.py`, `select_3dmark05_payload_window.py`, and `sync_corpus.sh`). No checker enforces domain assignment; this is a documentation-completeness gap, not a runtime one |
| `compare-gate → record` boundary (`specs/experiments/harness/spec.md` §2) cites no parent `R-HARN-*` requirement | ⚠️ | Extracting every `R-HARN-\d+\.\d+` token from each `###` boundary subsection in `spec.md` §2 returns a non-empty match set for seven of the eight boundaries and an empty set for exactly one, the terminal `compare-gate → record` section (`spec.md:254-274`). `specs/experiments/harness/audit/requirements.md` R-HARN-AUDIT-1.2 already discloses this accurately and extends parent principles to the `audit` domain's own `record`-stage requirements by stated analogy rather than inventing a citation; this row records the gap in the parent document without adding a parent requirement to close it, so as not to contradict what `audit`'s own documents already say about it |

## Render Tape bounded wild evidence

The canonical `perf-d3d9-present-loop` experiment was run with
`PRESENT_LOOP_ITERATIONS=2`, the coordinator-provided Sikarugir runtime and
manifest, and a fresh run-local PE-visible Wine root (`Z:\\...`). The app
exited with code 0. The runner's optional screenshot helper missed the window;
that independent failure is not capture or bundle validation evidence.

For this run, the runtime was
`/Users/dididi/workspaces/dxmt9/experiments/wine/sikarugir-cx-24.0.7` and the
manifest was `/Users/dididi/workspaces/dxmt9/experiments/wine/manifest.toml`.
The captured bundle was
`/tmp/dxmt9-render-tape-final.1TVZ4h/frame-90260231960200-1`; its
`events.bin` is 2992 bytes with SHA-256
`e2641223b27e357f19b04d57b76522c6225f8c76c9ca2804e5bab24e6d6017bf`.
It contains four events, one object definition, one bootstrap chunk, one
command chunk with `Clear` + `Present`, and zero blobs or mutations. The v2
CLI `validate` and `inspect` commands passed. The `provider-replay` command
returned status `complete` with exit code 0; the result was
`production_capture=true` and `production_provider_replay=true`.

The provider requirements were 256×256, format 21. Readback was 262144 bytes
with SHA-256
`49843e277c6ce8246d199c69c77aba0e7791c50522ab16c6a926f1528bd7474c`, and
object conservation was 1 created / 1 released. The capture has no expected
digest, so `output_oracle=false`, `expected_digest_captured=false`, and
`expected_digest_matched=false`. Its uniform clear makes
`output_non_degenerate=false`; that is a limitation of the evidence, not a
failure.

The staged binary SHA-256 values were x64 `d3d9.dll`
`9069c164bc52c3231426a3c952a2e92efcfc29307dfbf623093e03c8c30a18ff`, x64
`winemetal.dll`
`e9f3e5b1e0a8c00342b66c1495443e2ae57802e3b12fd54e396cea85d3e92c34`, x86
`d3d9.dll`
`d82c16729efc94e728046223b4fdcdd39ad09fb9c1049e1a29f0d1879bd471bb`, x86
`winemetal.dll`
`5b75ed7bc24695f6bc7eeba41a68adba13a839c1b1a945bdebefdf1bbd542403`, and
Unix `winemetal.so`
`d11194d43520eeb9f990f23f0bf0314dad168866e6c65b1868772e014674cd0c`.

The production implicit backbuffer is represented as a `PresentOutput` surface
with `initial-content-not-required`; proof for intervals that load prior
backbuffer contents remains open. Direct `PresentEx`/direct-control calls do
not currently emit the canonical `PRESENT` event and therefore fail closed at
`presentChunkSeen`; PresentEx event support is a separate documented gap.

---
