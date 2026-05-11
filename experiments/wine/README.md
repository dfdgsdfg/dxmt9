# experiments/wine/

Operator workflow for the Wine root manifest used by wild experiments.

- **Manifest:** `manifest.toml` (committed). Lists every Wine root the harness may use.
- **Manifest schema reference:** `specs/experiments/assets/wine-manifest.schema.toml`.
- **Runtime spec:** `specs/experiments/runtime/{requirements,design}.md`.
- **macdrv symbol-bridge spec:** `specs/winemetal/{requirements,design}.md`. **Read this first** before adding a Wine root — most Heroic / Gcenx redistributed builds ship with `winemac.so` symbols stripped and cannot drive dxmt9. The spec's §6.2 compatibility matrix is authoritative.

## Which Wine roots work today

Audited 2026-05-11 on macOS Apple Silicon (Gcenx tags 11.0_1, 11.6_1, 11.7, 11.8; Heroic Wine-Crossover-23.7.1-1; CrossOver 26):

| Source | Works? | How to qualify |
|---|---|---|
| **CodeWeavers CrossOver product** (`~/Applications/CrossOver.app`) | ✅ | already exposes `_macdrv_functions`; symlink it into `experiments/wine/crossover-<ver>/`. |
| **Self-built Wine** with `wine/patches/winemac-expose-symbols-<ver>.patch` applied | ✅ | follow 3Shain's [DXMT Installation Guide for Geeks](https://github.com/3Shain/dxmt/wiki/DXMT-Installation-Guide-for-Geeks) (also referenced from `specs/winemetal/requirements.md` R-WMB-10). |
| Heroic `Wine-11.x`, `Wine-11.x-DXMT`, `Wine-Crossover-23.7.1-1` | ❌ | none. `winemac.so` is stripped. The `-DXMT` suffix bundles dxmt's pre-built D3D11 DLLs only; it does **not** patch `winemac.so`. |
| `Wine-Staging-macOS` / older Gcenx 11.x | ❌ | Heroic's UI may suggest this for DXMT; that advice is outdated. dxmt9's runtime probe will reject it. |

## Adding a Wine root

1. Install the Wine source per the matrix above. Either:
   - Path A — copy/symlink CrossOver's bundled wine tree into `experiments/wine/<id>/`, or
   - Path B — clone Wine source, apply the dxmt9 macdrv patch, build, install into `experiments/wine/<id>/` (see `wine/patches/README.md`).
2. Run `python3 scripts/wine/check_patch.py $(pwd)/experiments/wine/<id>` to confirm `applied`.
3. Append a `[[wine]]` entry to `manifest.toml` with:
   - `id` (unique short string)
   - `source` (`heroic` / `brew` / `gptk` / `crossover` / `sikarugir` / `manual`)
   - `variant` (`vanilla` / `dxmt` / `vk` / `kegworks` / `patched`)
   - `path` (absolute, may use `$HOME` or `$REPO_ROOT`)
   - `requires_patch = true` and `patch_status = "applied"` for source-built or CrossOver-imported roots (see `specs/winemetal/assets/wine-manifest-requires-patch.toml.example`).
4. Run `python3 scripts/wine/resolve.py --list` to verify the entry parses (note: `resolve.py` lands in a later round; the `--list` check is forward-looking until then).

## Manually-placed bundles

Drop a Heroic-style bundle (the `Contents/Resources/wine/` layout) into this directory; it is gitignored. Reference it in `manifest.toml` with `source = "manual"` and a `$REPO_ROOT/experiments/wine/...` path. Always run `check_patch.py` after placing the bundle — bundles whose `winemac.so` is stripped will fail at runtime regardless of the source label.

## What is and isn't committed

- Committed: `manifest.toml`, this README.
- Gitignored: every other file under `experiments/wine/` (including bundle directories and CrossOver symlinks).
