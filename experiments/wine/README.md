# experiments/wine/

Operator workflow for the Wine root manifest used by wild experiments.

- **Manifest:** `manifest.toml` (committed). Lists every Wine root the harness may use.
- **Schema reference:** `specs/experiments/assets/wine-manifest.schema.toml`.
- **Spec:** `specs/experiments/runtime/{requirements,design}.md`.

## Adding a Wine root

1. Install the Wine build via its native channel (Heroic, brew, GPTK installer, CrossOver, Sikarugir) or drop a Heroic-style bundle into this directory.
2. Append a `[[wine]]` entry to `manifest.toml` with:
   - `id` (unique short string)
   - `source` (`heroic` / `brew` / `gptk` / `crossover` / `sikarugir` / `manual`)
   - `variant` (`vanilla` / `dxmt` / `vk` / `kegworks` / `patched`)
   - `path` (absolute, may use `$HOME` or `$REPO_ROOT`)
3. Run `python3 scripts/wine/resolve.py --list` to verify the entry validates (note: resolve.py lands in a later task; the `--list` check is forward-looking).

## Manually-placed bundles

Drop a Heroic-style bundle (the `Contents/Resources/wine/` layout) into this directory; it is gitignored. Reference it in `manifest.toml` with `source = "manual"` and a `$REPO_ROOT/experiments/wine/...` path.

## What is and isn't committed

- Committed: `manifest.toml`, this README.
- Gitignored: every other file under `experiments/wine/` (including bundle directories).
