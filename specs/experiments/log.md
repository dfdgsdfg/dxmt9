---
type: "Spec Log"
title: "Experiments Log"
description: "Domain-level maintenance history for Experiments specs."
tags: [specs, log, experiments]
---

# Experiments Log

Domain-level maintenance history for `experiments/` specs. Keep current implementation and evidence status in [gap](gap.md), and use this log for structural edits, migrations, and older detail that should not stay in the current overview.

## 2026-07-29

- Retired the exploratory commercial catalogue entry that was the only app
  requiring a non-vanilla Wine root. Its `[[app]]` block, launcher, gap rows,
  and `experiments/README.md` section were removed; it had not been re-run
  since the Wine manifest landed, and its runtime was stated inconsistently
  across `CATALOGUE.toml`, `agents/rules/test_wild.rules.md`, and its
  since-removed shell wrapper — a disagreement no run had ever resolved.
- **R-RT-7.3 retired.** `specs/experiments/runtime/requirements.md` §7 staged
  that app's migration and pinned its Wine-DXMT exception. With the app gone
  the requirement has no subject. Precedent for retiring an ID did not exist
  in this repository, so the convention chosen is: leave the ID in place,
  prefix the body with `*(Retired <date>. ID retained, not reused.)*`, state
  what it used to require and which general mechanisms survive it, and link
  here. Neighbouring IDs are not renumbered — `documentation_spec.rules.md`
  requires IDs to stay stable, and a tombstone is the only way to keep a
  removed contract's number from being silently reused.
- Consequently `agents/rules/test_wild.rules.md`'s **Documented Exceptions**
  section now records that there are none, and states the procedure for
  adding one, instead of carrying a table with a dead row. The generic
  exception machinery — `wine_alternatives` (R-RT-5.3) and the run-start
  non-vanilla warning (R-RT-6.3) — is unchanged and still normative.

## 2026-07-08

- Added this domain log as the maintenance history companion for the domain specs.
