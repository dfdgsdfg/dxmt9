# experiments/wine/

Operator workflow for the Wine root manifest used by wild experiments.

- **Manifest:** `manifest.toml` (committed). Lists every Wine root the harness may use.
- **Manifest schema:** `specs/experiments/assets/wine-manifest.schema.toml`.
- **Runtime spec:** `specs/experiments/runtime/{requirements,design}.md`.
- **macdrv symbol-bridge spec:** `specs/winemetal/{requirements,design}.md`. Authoritative for which Wine builds actually work and why.

## Recommended setup: Sikarugir pre-built Wine (path A)

Run once per machine:

```sh
python3 scripts/wine/install_wine.py \
  --engine sikarugir-cx-24.0.7 \
  --target-id sikarugir-cx-24.0.7 \
  --register-in-manifest
```

The script:
1. Fetches `WS12WineCX24.0.7_<rev>.tar.xz` from [`Sikarugir-App/Engines`](https://github.com/Sikarugir-App/Engines) and extracts `wswine.bundle/` into `experiments/wine/<id>/`.
2. Fetches the matching `Template-*.tar.xz` from [`Sikarugir-App/Wrapper`](https://github.com/Sikarugir-App/Wrapper) and drops its `Frameworks/*.dylib` files (FreeType, libinotify, GStreamer.framework, ICU, etc.) into `experiments/wine/` (one level above the bundle — this is where dyld's `@rpath/bin/../..` resolves).
3. Renames `bin/wine` → `bin/wine.real` and `bin/wineserver` → `bin/wineserver.real`, then writes thin Bash shims that export `DYLD_FALLBACK_LIBRARY_PATH=experiments/wine` so Wine's `dlopen()` calls (FreeType etc.) find the co-located dylibs.
4. Audits the bundle's `winemac.so` for the `_macdrv_functions` symbol (refuses to register a stripped build).
5. Appends a `[[wine]]` entry to `manifest.toml` with `requires_patch=true, patch_status="applied"`.

After install, the resolver picks it up automatically:

```sh
python3 scripts/wine/resolve.py --list   # should show OK sikarugir-cx-24.0.7
```

CATALOGUE entries point at this manifest id via `wine_id = "sikarugir-cx-24.0.7"`.

## Which Wine roots work today

Audited 2026-05-11 on macOS Apple Silicon:

| Source | Works? | How to qualify |
|---|---|---|
| **`Sikarugir-App/Engines` pre-built** (`WS12WineCX24.0.7_*`, `WS12WineSikarugir10.0_*`) | ✅ | `install_wine.py` — fully automated. |
| **Self-built Wine** with `wine/patches/winemac-expose-symbols-<ver>.patch` applied | ✅ | Follow 3Shain's [DXMT Installation Guide for Geeks](https://github.com/3Shain/dxmt/wiki/DXMT-Installation-Guide-for-Geeks); also documented in `wine/patches/README.md`. |
| CodeWeavers CrossOver product (`~/Applications/CrossOver.app`) | ⚠️ partial | Exposes the symbols but its wow64 lacks `NtQueryVirtualMemory(MemoryWineImageInfo)`; dxmt9's bridge bails with `0xc0000003`. Pending follow-up. |
| Heroic `Wine-11.x`, `Wine-11.x-DXMT`, `Wine-Crossover-23.7.1-1` | ❌ | None. `winemac.so` is stripped. The `-DXMT` suffix bundles dxmt's pre-built D3D11 DLLs only; it does **not** patch `winemac.so` (md5-identical to vanilla). |

## Binary paths must be POSIX, not `D:\`

Sikarugir's (and any other macOS) Wine re-runs `wineboot` at every `wine` invocation when it detects a stale `.update-timestamp` or new macOS mount, and that wineboot rewrites `dosdevices/<letter>:` to whatever mount it auto-claims (in practice `D:` ends up pointing at a cryptex mount on this machine). dxmt9 therefore uses the **local POSIX path** to the binary inside `experiments/apps_3rd/<name>/`; Wine resolves that through its built-in `Z:` drive (= `/`). The `install_drive_letter` field in CATALOGUE stays for documentation but no longer routes the launch path.

## Manually-placed bundles

Drop a Heroic-style bundle (the `Contents/Resources/wine/` layout) into this directory; it is gitignored. Reference it in `manifest.toml` with `source = "manual"` and a `$REPO_ROOT/experiments/wine/...` path. Always run `scripts/wine/check_patch.py` (when it lands) after placing the bundle — bundles whose `winemac.so` is stripped will fail at runtime regardless of the source label.

## What is and isn't committed

- Committed: `manifest.toml`, this README.
- Gitignored: every other file under `experiments/wine/` — bundle directories, the runtime dylibs `install_wine.py` deposits here, shim wrappers, downloaded tarballs.
