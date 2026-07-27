---
type: "Spec Gap"
title: "Experiments Gap"
description: "Implementation and evidence gaps for wild integration experiments."
tags: [specs, gap, experiments]
---

# Experiments Gap

Domain-owned implementation and evidence gap tracker. Use the [root gap index](../gap.md) for cross-domain rollup.

## Experiments Layer

⚠️ Partial. The runner, launcher harness, output layout, one verified local
bootstrap entry, and the full initial real-application catalogue from
R-WILD-3.1 exist. The layer remains partial because the current implementation
depends on the Wine builtin path rather than the native macOS injection path
described by R-WILD-1.2.

| Area | Status | Spec |
|---|---|---|
| `experiments/CATALOGUE.toml` + launcher tree scaffolded | ✅ | R-WILD-5.1 |
| Wine launcher injection harness (`run_experiment.py`, launcher scripts, Heroic staging) | ⚠️ | R-WILD-1.2 |
| Internal backbuffer frame dump + SSIM comparison + `result.json` output | ✅ | R-WILD-2.3, R-WILD-4.1 |
| Bootstrap verified entry: `conf-d3d9-wsi-present` on Heroic Wine 11.5 | ✅ | local workflow validation |
| Verified real application entries: `sample-d3d9-basic-hlsl`, `sample-d3d9-tutorial07`, `sample-d3d9-hdr-formats`, `sample-d3d9-dxut-simple`, `sample-d3d9-irrlicht-lights` | ✅ | Heroic Wine 11.5, direct capture, SSIM 1.0000 |
| Exploratory commercial entry: `app-d3d9-anno-1404` | ⚠️ | supported on Heroic `Wine-11.6-DXMT`; plain `Wine-11.6` is research-only due to Wine `d3dx10_43` aborts |
| Initial catalogue from R-WILD-3.1 staged and verified | ✅ | All five required feature groups covered |
| Reference screenshots for initial catalogue entries | ✅ | R-WILD-4.1 |
| Harness script evidence-production contracts (`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, `scripts/run_suites/`) | ⚠️ | `specs/experiments/harness/requirements.md`, `specs/experiments/harness/spec.md` |
| Harness `R-HARN-*` requirement enforcement | ❌ | Requirements in `specs/experiments/harness/requirements.md` are phrased as predicates but nothing evaluates them; no checker exists. Deliberate scope choice on 2026-07-27 for the docs-only specification round — means these documents can corrode exactly as the harness scripts they describe already have |
| `replay` domain harness (`scripts/tools/run_3dmark05_mini_replay.py`) | ❌ | Does not currently work. Of the five defects `specs/experiments/harness/requirements.md` §2-§6 derive their rationale from, defects 1, 3, 4, and 5 are unfixed; defect 2 (sliced-stream-offset double-count) was fixed in commit `12348666`. The cause of the resulting black replay output is unknown — constants, scissor, cull, depth input, and draw issue were all eliminated as candidates — and `--force-fragment-color`, the diagnostic flag that would bisect the failure between geometry and fragment stages, is itself broken |
| `scripts/tools/summarize_3dmark05_cleanup_candidates.py` citation counting | ❌ | Miscounts brace-expanded citations such as `...-r{1,2,3}-...`, classifying 84 referenced runs (4.5 GB) as unreferenced and eligible for cleanup when they are not |
| `docs/perfomance/` `source:` citation integrity | ❌ | 33 of the 55 distinct `.log` paths mentioned in `docs/perfomance/` are already missing from disk as of a 2026-07-27 scan (34 of 56 when the same scan also covers `docs/`, `agents/`, and `README.md` as a whole, not only `docs/perfomance/`); the citations are dangling |
| Harness domain map (`specs/experiments/harness/spec.md` §1) is a partial partition, not a complete one | ⚠️ | R-HARN-1.1. Of the 86 in-scope harness scripts (`scripts/tools/`, `scripts/run_apps/`, `scripts/check/`, `scripts/run_suites/`), mechanically applying the domain map's own `Owns` column (explicit names plus its `scripts/check/*` and `scripts/tools/compare_*` wildcard rows) assigns exactly 41. The remaining 45 are owned by no domain: 11 are named by `reduce/spec.md` §2.4 while it explicitly declines to assign them a domain (verified against that section's own listed 11 filenames); 5 more are named individually elsewhere without a domain assignment (`analyze_indexed_probe_classes.py`, `analyze_shader_dumps.py`, `analyze_xcode_dxmt_encoder_attribution.py` in `join/spec.md` and `join/requirements.md`; `run_with_timeout.py` in `probe/spec.md`; `shader_corpus_tool.py` in `audit/spec.md`); and 29 are not named in any of the sixteen tracked harness documents at all — including `run_dx9_present_policy_ab.py` (661 lines, a documented workflow in `agents/rules/metal_debugging.rules.md` §7), `run_d3d9_conformance.py` (521 lines, whose `:264` sets `DXMT9_PREWARM=disabled` into its launched subprocess with no domain to attach that contract-relevant variable to per parent `spec.md` §4 Rule 1), `analyze_pso_backend_churn.py` (585 lines), and 26 other `scripts/tools/` scripts (mostly `analyze_*`, plus `audit_backend_escape_surface.py`, `cleanup_dxmt9_temp_prefixes.py`, `gen_wine_d3d9_test_inventory.py`, `package_app_local.py`, `plan_backend_escape_reduced_ab.py`, `plan_effect_roi_forcewhite_probes.py`, `run_3dmark05_semantic_replay_gate.py`, `select_3dmark05_payload_window.py`, and `sync_corpus.sh`). No checker enforces domain assignment; this is a documentation-completeness gap, not a runtime one |
| `compare-gate → record` boundary (`specs/experiments/harness/spec.md` §2) cites no parent `R-HARN-*` requirement | ⚠️ | Extracting every `R-HARN-\d+\.\d+` token from each `###` boundary subsection in `spec.md` §2 returns a non-empty match set for seven of the eight boundaries and an empty set for exactly one, the terminal `compare-gate → record` section (`spec.md:254-274`). `specs/experiments/harness/audit/requirements.md` R-HARN-AUDIT-1.2 already discloses this accurately and extends parent principles to the `audit` domain's own `record`-stage requirements by stated analogy rather than inventing a citation; this row records the gap in the parent document without adding a parent requirement to close it, so as not to contradict what `audit`'s own documents already say about it |

---
