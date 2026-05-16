---
description: Experiment app catalogue, fixture, launcher, and external-app layout rules
paths:
  - "experiments/CATALOGUE.toml"
  - "experiments/apps/**"
  - "experiments/apps_3rd/**"
  - "experiments/launchers/**"
  - "scripts/run_apps/**"
  - "scripts/run_suites/**"
globs: "experiments/{CATALOGUE.toml,apps/**,apps_3rd/**,launchers/**}"
alwaysApply: false
---

# Test App Rules — Catalogue, Fixtures, and Launchers

Use these rules when adding, removing, or changing experiment applications.
Wine runtime selection belongs in `agents/rules/test_wild.rules.md`; app
inventory and launcher shape belong here.

## App Storage Boundaries

| Path | Committed? | Rule |
|---|---:|---|
| `experiments/apps/` | yes | Small repo-local or license-compatible fixture apps only. |
| `experiments/apps_3rd/` | no | External commercial installs and extracted benchmark payloads. |
| `experiments/prefixs/` | no | Per-app Wine prefixes created by runtime tooling. |
| `experiments/output/` | partial | Run evidence; do not commit normal result payloads. |

Do not commit commercial app binaries, local Heroic installs, or absolute
machine paths into `experiments/apps/` or `CATALOGUE.toml`.

## Catalogue Rules

Every active experiment app needs one `[[app]]` entry in
`experiments/CATALOGUE.toml` with:

- stable kebab-case `name`;
- provenance fields: `source`, `license`, `source_kind`, `license_scope`;
- `binary`, `launcher`, `features`, `status`, `requires_wine`, and capture
  metadata when the runner consumes it;
- `build_script` when `python3 scripts/run_apps/run_experiment.py run <name>
  --build` should work.

For external apps, prefer `binary` paths under
`experiments/apps_3rd/<name>/...` and pair the entry with `wine_id` when it is
run through Wine. Avoid absolute `/Users/...` paths in committed catalogue
entries; they make the target non-reproducible for other machines.

## Launcher Rules

- Launcher filenames match `CATALOGUE.name`: `experiments/launchers/<name>.sh`.
- Shared launchers are allowed only for bundled fixture families; document the
  mapping in `experiments/launchers/AGENTS.md`.
- Each launcher sources `experiments/launchers/common.sh` before staging or
  running dxmt9.
- App-specific wrappers in `scripts/run_apps/` stay thin. Prefer the
  consolidated Python runner unless extraction, installer bootstrap, or a
  host-specific oracle lane requires shell glue.

## Folder Documentation

Use local `AGENTS.md` files for folder conventions. Keep generic folder-level
`README.md` files out of `experiments/`; per-app provenance notes are acceptable
only when they describe the fixture source, license, or rebuild steps.

## Related

- `agents/rules/test_wild.rules.md` - Wine runtime and failure triage.
- `specs/experiments/requirements.md` - experiment pass/fail contract.
- `specs/experiments/runtime/requirements.md` - Wine, prefix, and app install
  layout.
