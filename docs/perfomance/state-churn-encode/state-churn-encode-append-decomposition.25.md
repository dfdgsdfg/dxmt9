---
domain: state-churn-encode
workload: 3DMark05 GT2
subcategory: append-decomposition
order: 25
title: Getter-Cache + Warm-Epoch Harvest — Bridge −2.21 ms/present Converts To +7.2% GT2
date: 2026-08-20
type: experiment-run
status: accepted-win
source: experiments/output/app-d3d9-3dmark05-bridge-opcodes-r2; experiments/output/app-d3d9-3dmark05-harvest-{a1,a2,a3,b1r,b2r}
related: docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.24.md; docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.23.md
---

# Getter-Cache + Warm-Epoch Harvest — Bridge −2.21 ms/present Converts To +7.2% GT2

**What shipped.** The two [.24](state-churn-encode-append-decomposition.24.md)
candidates, merged as `6faeb559`: PE-side caching of the three stable getters
(`f9dc7e75` — per-(container,level) surface handle borrow, per-index swap-chain
handle, constructor-resolved adapter count) and recorder retention pins kept
warm across chunk boundaries (`c7b3b141` — `endEpoch()` with `kWarmEpochs=1`
replaces clear-at-reset, so the steady working set stops re-crossing for
addref/release every chunk).

**Conformance gate first.** The full-suite gate initially looked like 14
regressions whose names matched the merge's surface; a three-point full-suite
matrix plus singleton reruns proved all of them chunk-poisoning artifacts and
the merge clean — the audit, the pre-existing decl-group SEH teardown flake it
uncovered, and the real (also pre-existing) `CreateCubeTexture` validation bug
fixed in `88fcdc76` are recorded in `specs/d3d9/gap.md` ("Conformance-gate
audit", 2026-08-20). Post-fix suite: 234/235.

**Mechanism proof** (`bridge-opcodes-r2` vs [.24]'s r1, per-present, presents
1,653 → 1,771 in the same 120 s window):

| lane | r1 ms/p | r2 ms/p | delta |
|---|---|---|---|
| `texture_get_surface_level` | 1.118 | **0.000** | −1.118 |
| `device_get_swap_chain` | 0.641 | **0.000** | −0.641 |
| `factory_adapter_count` | 0.060 | **0.000** | −0.060 |
| buffer/texture/shader addref+release | 0.632 | 0.309 | −0.373 (crossings −68%) |
| buffer lock+unlock, surface_lock_rect, commit_chunk | 4.87 | 4.72 | ≈ flat (as expected) |
| **bridge total** | **7.34** | **5.13** | **−2.21** |

The getter lanes vanish outright; refcount churn drops to the eviction
residual; the lock/commit lanes the change does not touch stay flat. This is
the [.24] ledger delivered almost exactly (predicted ~1.8 + ~0.7).

**FPS conversion** (matched-pair ABBA, `--build-root` prebuilt worktree at
merge parent `840d95c1`, same-machine back-to-back, `--frame-sampling`
steady-body extraction anchored on `[dxmt9-perf-frame `, parity proven by
byte-identical `winemetal.so` and zero buildoption diffs, staged-build sha
verified per run, zero GPU errors everywhere):

| run | mean fps | median | p10 | p90 | frames |
|---|---|---|---|---|---|
| A1/A2/A3 (head) | 28.28 / 28.37 / 28.34 | 29.91 / 30.01 / 29.93 | ~22.3 | ~36.5 | 1773–1778 |
| B1/B2 (base) | 26.40 / 26.49 | 27.92 / 27.94 | 20.9 | 33.9 | 1656–1661 |

**A vs B: mean +7.2%, median +7.3%, uniform +6.7–7.3% across p10–p90**, with
within-arm spread of only 0.3% — non-overlapping by more than 20× the spread.
Present counts (+7.1%) independently corroborate. The uniform shift across the
whole distribution says this is a straight producer-wall reduction, not a
hitch-tail artifact ([.19]'s trap does not apply). Measured −2.21 ms on a
~35–37 ms producer-paced frame predicts +6.0–6.5%; the extra ~1% is plausibly
reduced wineserver round-trip contention riding the same change.

**Verdict.** Accepted win, defaults already on (the cache and warm epochs are
unconditional; `DXMT9_RENDER_TAPE_CAPTURE` still opts the level cache out for
capture identity). GT2 producer ledger after this: game logic ~60%, locks
2.65 ms, commit_chunk 1.49 ms, surface_lock_rect 0.58 ms remain the reducible
bridge candidates ([.24]'s "hard" share), plus `notePeDeviceCallAfterPresent`
(~0.3 ms, [.23]) inside the recorder.
