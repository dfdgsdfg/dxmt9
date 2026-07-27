---
type: "Spec Plan"
title: "Harness Plan — Rollout and Legacy Migration"
description: "Harness domain rollout order, legacy envelope migration path, and open items."
tags: [specs, experiments, harness, plan]
---

# Harness Plan — Rollout and Legacy Migration

Implements `specs/experiments/harness/requirements.md` and the
stage/envelope model in `specs/experiments/harness/spec.md`. States
the order the seven domain subdirectories (`runner`, `probe`,
`replay`, `reduce`, `join`, `gate`, `audit`) are specified in, how
the legacy `result.json` / `3dmark05-trace-artifacts.json`
provenance migrates to the artifact envelope, and — most importantly
— what is currently known broken, so a reader is never misled into
treating the current harness state as the specified one.

---

## 1. Rollout Order

The seven domains (R-HARN-1.1) are specified in dependency order,
not alphabetical or file-tree order:

1. **`runner` and `probe` first.** Every other domain consumes an
   artifact one of these two produces — `run-capture` output, the
   `.gputrace` bundle, the geometry `.meta` sidecar — so their
   contracts must exist before a downstream domain's `spec.md` can
   cite them accurately. `probe`'s `dump-extract → offline-replay`
   boundary is the one `spec.md` §2 documents in the most detail,
   because it is the boundary that actually failed silently
   (R-HARN-4.3, R-HARN-4.4).
2. **`replay`, `reduce`, `join` next**, in that order. All three
   consume `probe` (and, for `reduce`, `runner`) artifacts and
   produce summary artifacts of their own; no artifact dependency
   forces an order among the three, but `replay` is written first
   because its contract is currently unmet (§3) and stating the
   intended contract precisely is the most urgent of the three.
3. **`gate` and `audit` last.** `gate` consumes `reduce` and `join`
   output at the `log-reduce → compare-gate` and
   `external-join → compare-gate` boundaries, so it cannot be
   specified accurately before those two domains exist. `audit`
   consumes `gate`'s verdicts at the terminal
   `compare-gate → record` boundary and is therefore specified
   last.

This is a writing/rollout sequence for
`specs/experiments/harness/<domain>/{requirements,spec}.md`. It is
not an implementation or enforcement sequence — no enforcement of
any `R-HARN-*` requirement exists yet, for any domain (§3).

## 2. Legacy Envelope Migration

Today's provenance is spread across two ad-hoc, per-app-family
shapes with no shared schema:

- **`result.json`**, written by the `runner` domain
  (`scripts/run_apps/run_experiment.py`), mixes the counter payload
  three independent readers depend on — `run_experiment.py`'s own
  `expected_counters` L3 gate, `compare_3dmark05_perf_counters.py`,
  and `docs/perfomance/` `source:` citations — with provenance
  fields such as `wine_root` that were added over time without a
  declared schema (`agents/rules/test_wild.rules.md`'s diagnostic
  checklist already depends on `result.json:wine_root` informally).
- **`3dmark05-trace-artifacts.json`**, written by the `probe` domain
  (`scripts/tools/run_3dmark05_perf_probe.sh` and
  `scripts/tools/finalize_3dmark05_perf_probe.sh`) as a manifest of
  capture and analysis artifact paths for one probe run, in a shape
  unrelated to `result.json`'s provenance fields.

The migration this plan anticipates, but has not started, is three
steps:

1. **Add.** The envelope's seven fields (`schema`, `producer`,
   `stage`, `domain`, `inputs`, `env_snapshot`, `validity`; `spec.md`
   §3) are added alongside the existing ad-hoc fields in both files
   — either as a wrapper around the existing shape or as a sibling
   sidecar file that references it.
2. **Move.** Consumers that today read the ad-hoc provenance fields
   are moved to read the corresponding envelope fields instead.
3. **Remove.** Once no consumer reads an ad-hoc provenance field,
   that field is removed from `result.json` and
   `3dmark05-trace-artifacts.json`.

**The counter payload is not provenance and is not migrated.**
`result.json`'s counter fields — the values `expected_counters`,
`compare_3dmark05_perf_counters.py`, and `docs/perfomance/`
`source:` citations actually read as measurements, as opposed to the
`wine_root`-style provenance fields above — are untouched at every
step of this migration. Step 3 removes only the ad-hoc provenance
fields the envelope replaces; it does not touch, rename, or
restructure the counter payload those three readers consume.

**No migration step has been performed.** As of this writing,
neither `result.json` nor `3dmark05-trace-artifacts.json` carries
any envelope field. This plan states the intended migration path; it
does not claim any part of it is done. A reader who finds an
`env_snapshot` or `validity` field in a live `result.json` should
treat that as evidence the migration has since started elsewhere,
not as something this document produced.

## 3. Open Items

These are not aspirational gaps in the usual `gap.md` sense — they
are the specific reasons a reader must not treat the requirements
and spec just written as a description of the harness scripts as
they exist today.

- **Enforcement is unbuilt.** Every `R-HARN-*` requirement in
  `specs/experiments/harness/requirements.md` is phrased as a
  predicate so an enforcement checker could evaluate it, but no such
  checker exists. This is deliberate — the docs-only scope for this
  specification round was chosen on 2026-07-27 — and it means these
  documents can corrode exactly as the harness scripts they describe
  already have: a future harness change can silently violate any of
  R-HARN-2.1 through R-HARN-6.3 and nothing will fail a build or a
  test run to catch it.
- **The `replay` harness does not currently work.** Of the five
  defects `requirements.md` §2-§6 derive their rationale from,
  defects 1, 3, 4, and 5 are unfixed. Defect 2 — the
  `dump-extract → offline-replay` sliced-stream-offset double-count
  (R-HARN-4.3) — was fixed in commit `12348666`
  ("fix(mini-replay): repair direct-cbuf transform and sliced stream
  offset"). The cause of the resulting black replay output is
  unknown: constants, scissor, cull, depth input, and draw issue
  were all eliminated as candidates during the 2026-07-25/27
  vertex-remap experiment, and `--force-fragment-color` — the
  diagnostic flag that would bisect the failure between the geometry
  and fragment stages (R-HARN-6.3) — is itself broken, so the one
  tool that would narrow the search is unusable exactly when it is
  needed.
- **`scripts/tools/summarize_3dmark05_cleanup_candidates.py`
  miscounts brace-expanded citations.** A citation of the shape
  `...-r{1,2,3}-...` is not expanded before the reference count is
  taken, so 84 referenced runs (4.5 GB) are classified as
  unreferenced and eligible for cleanup when they are not.
- **34 of 56 `docs/perfomance/` `source:` citations are dangling.**
  An audit on 2026-07-27 found 34 of the 56 log/output paths cited
  as evidence in `docs/perfomance/` already missing from disk. Per
  `spec.md` §3's "second use of `inputs` digests", none of these 34
  citations carries a recorded digest, so today there is no way to
  distinguish "never produced" from "produced, then cleaned up" from
  "a different file now sits at that path" for any of them.

## 4. Non-Goals

This plan does not change harness code, add a new harness, build an
enforcement checker, or perform the artifact migration §2 describes.
It records the order the remaining domain documents are written in
and the state a reader must not mistake for the specified one.
