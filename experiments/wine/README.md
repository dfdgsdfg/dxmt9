# experiments/wine/

Operator workflow for the Wine root manifest used by wild experiments.

- **Manifest:** `manifest.toml` (committed). Lists every Wine root the harness may use.
- **Manifest schema:** `specs/experiments/assets/wine-manifest.schema.toml`.
- **Runtime spec:** `specs/experiments/runtime/{requirements,spec}.md`.
- **macdrv symbol-bridge spec:** `specs/winemetal/{requirements,spec}.md`. Authoritative for which Wine builds actually work and why.

## Recommended setup: Sikarugir pre-built Wine (path A)

Run once per machine:

```sh
scripts/run_python.sh scripts/wine/install_wine.py \
  --engine sikarugir-cx-24.0.7 \
  --target-id sikarugir-cx-24.0.7 \
  --register-in-manifest
```

The script:
1. Fetches `WS12WineCX24.0.7_<rev>.tar.xz` from [`Sikarugir-App/Engines`](https://github.com/Sikarugir-App/Engines) and extracts `wswine.bundle/` into `experiments/wine/<id>/`.
2. Fetches the matching `Template-*.tar.xz` from [`Sikarugir-App/Wrapper`](https://github.com/Sikarugir-App/Wrapper) and drops its `Frameworks/*.dylib` files plus their version-alias symlinks (FreeType, libinotify, etc.) and `.framework` bundles (GStreamer.framework, SikarugirSdk.framework) into `experiments/wine/vendor/`. A single shared `vendor/` is reused by every installed wine engine on this machine.
3. Renames `bin/wine` → `bin/wine.real` and `bin/wineserver` → `bin/wineserver.real`, then writes thin Bash shims that export `DYLD_FALLBACK_LIBRARY_PATH` and `DYLD_FALLBACK_FRAMEWORK_PATH` pointing at `vendor/` so Wine's `dlopen()` calls (FreeType etc.) find the co-located dylibs and framework bundles.
4. Audits the bundle's `winemac.so` for the `_macdrv_functions` symbol (refuses to register a stripped build).
5. Appends a `[[wine]]` entry to `manifest.toml` with
   `metal_surface_protocol="legacy-macdrv-symbols:<target-id>"` and its archival patch
   metadata.

## Directory layout

```
experiments/wine/
├── manifest.toml         (committed; Wine root registry)
├── README.md             (committed; this file)
├── vendor/               (gitignored; shared dylib + framework dir)
│   ├── libfreetype.6.dylib
│   ├── libfreetype.dylib -> libfreetype.6.dylib
│   ├── GStreamer.framework/
│   ├── SikarugirSdk.framework/
│   └── …  (≈94 dylibs + 55 version-alias symlinks)
└── <target-id>/          (gitignored; wswine.bundle contents)
    ├── bin/wine          (shim — exports DYLD_FALLBACK_*_PATH=…vendor/)
    ├── bin/wine.real     (CrossOver Perl wrapper, unused)
    ├── bin/wineserver    (shim)
    └── lib/wine/…
```

After install, the resolver picks it up automatically:

```sh
scripts/run_python.sh scripts/wine/resolve.py --list   # should show OK sikarugir-cx-24.0.7
```

CATALOGUE entries point at this manifest id via `wine_id = "sikarugir-cx-24.0.7"`.

## Which Wine roots work today

Audited 2026-05-11 on macOS Apple Silicon:

| Source | Works? | How to qualify |
|---|---|---|
| **`Sikarugir-App/Engines` pre-built** (`WS12WineCX24.0.7_*`, `WS12WineSikarugir10.0_*`) | ✅ | `install_wine.py` — fully automated. |
| **Self-built Wine** with `wine/patches/winemac-expose-symbols-<ver>.patch` applied | ✅ | Follow 3Shain's [DXMT Installation Guide for Geeks](https://github.com/3Shain/dxmt/wiki/DXMT-Installation-Guide-for-Geeks); also documented in `wine/patches/README.md`. |
| **[`3Shain/wine`](https://github.com/3Shain/wine) fork release `v9.9-mingw`** (the upstream-DXMT author's Wine, `wine-9.9-4-g496a727`) | ✅ | Manual: extract the release `wine.tar.gz` into `experiments/wine/3shain-v9.9/` and add the `manifest.toml` entry (`metal_surface_protocol = "legacy-macdrv-symbols:3shain-v9.9"`). Bootstrap any fresh/version-updated prefix once with `WINEDLLOVERRIDES="mscoree=;mshtml="` — the tarball bundles no Wine Mono/Gecko and the mono-install dialog hangs headless runs. Verified 2026-08-26: triangle x64+x86/wow64 and SFIV end-to-end; see R-WMB-6.2. |
| CodeWeavers CrossOver product (`~/Applications/CrossOver.app`) | ❌ blocked | Four issues audited on CrossOver 26 (2026-05-11): (1) `bin/wine` Perl wrapper demands a bottle — bypassable via `wineloader` or `cxbottle --create` + `wine --bottle <name>`; (2) wow64 missing `MemoryWineLoadUnixLibByName` (class 1002), **not bypassed by bottle context**; (3) `ntdll.so` hardcodes `/opt/cxoffice/lib/wine`, `WINEDLLDIR0`/`WINEDLLPATH` don't take effect; (4) `CrossOver.app/lib/wine/` is SIP/codesign-protected — `winemetal.so` cannot be staged. No dxmt9-side fix unblocks all four. Use Sikarugir-Engines. |
| Heroic `Wine-11.x`, `Wine-11.x-DXMT`, `Wine-Crossover-23.7.1-1` | ❌ | None. `winemac.so` is stripped. The `-DXMT` suffix bundles dxmt's pre-built D3D11 DLLs only; it does **not** patch `winemac.so` (md5-identical to vanilla). |

## Binary paths must be POSIX, not `D:\`

Sikarugir's (and any other macOS) Wine re-runs `wineboot` at every `wine` invocation when it detects a stale `.update-timestamp` or new macOS mount, and that wineboot rewrites `dosdevices/<letter>:` to whatever mount it auto-claims (in practice `D:` ends up pointing at a cryptex mount on this machine). dxmt9 therefore uses the **local POSIX path** to the binary inside `experiments/apps_3rd/<name>/`; Wine resolves that through its built-in `Z:` drive (= `/`). The `install_drive_letter` field in CATALOGUE stays for documentation but no longer routes the launch path.

## Manually-placed bundles

Drop a Heroic-style bundle (the `Contents/Resources/wine/` layout) into this
directory; it is gitignored. Reference it in `manifest.toml` with `source =
"manual"`, a `$REPO_ROOT/experiments/wine/...` path, and an exact
`metal_surface_protocol`. A new Wine build remains `unknown` until its WSI
protocol is qualified; symbol visibility alone does not qualify the legacy
path.

## What is and isn't committed

- Committed: `manifest.toml`, this README.
- Gitignored: every other file under `experiments/wine/` — bundle directories, the runtime dylibs `install_wine.py` deposits here, shim wrappers, downloaded tarballs.
