# winemetal Requirements — Wine macdrv Symbol Bridge

This spec governs how dxmt9 reaches the macOS-side Cocoa/Metal helpers
inside Wine's `winemac.drv` so the presenter can attach a `CAMetalLayer`
to the game's `NSView`. It is a sibling to `specs/d3d9/wsi/` (which
covers the D3D9-side WSI semantics) and to
`specs/experiments/runtime/` (which covers Wine root manifest mechanics).

dxmt9 calls into a handful of `winemac.drv` helpers (`get_win_data`,
`macdrv_view_create_metal_view`, `macdrv_view_get_metal_layer`, etc.).
Upstream Wine 11.7 removed the `visibility(default)` attribute from
those helpers — the implementations still exist inside `winemac.so`,
they are no longer dlsym-reachable, and Wine's `__wine_unix_call_funcs[]`
table for `winemac.drv` has only two slots (`unix_init`,
`unix_quit_result`). dxmt9 therefore cannot get a Metal layer attached
on a stock Wine 11.7 build; the game renders to an offscreen drawable
and the user sees a black window.

Upstream dxmt (3Shain) takes the same approach: it documents that the
operator must build Wine with those symbols re-exposed, and points at
CrossOver Wine 24+ as a known-good build. dxmt9 adopts the same model —
maintain a small Wine patch, document it, and detect missing exports at
runtime instead of failing silently.

---

## 1. Scope

**R-WMB-1.1** This spec covers the symbol-exposure contract between
dxmt9's unix-side bridge (`src/winemetal/unix/winemetal_private_api.mm`,
`src/dxmt9/dxmt9_presenter_macdrv.cpp`) and Wine's `winemac.so`. It
does not redefine the D3D9 WSI semantics in `specs/d3d9/wsi/`, only
how the underlying Cocoa/Metal handle is obtained.

**R-WMB-1.2** Out of scope:
- Bundling a Wine binary distribution.
- Implementing a `__wine_unix_call_funcs[]` consumer (upstream Wine
  does not provide Metal slots in that table; see the rationale at the
  top of this document).
- Switching the rendering path to Vulkan/MoltenVK. The user-facing
  output remains native Metal.
- Patching anything in Wine outside `dlls/winemac.drv/`.

**R-WMB-1.3** The bridge concerns macOS only. Linux, Windows-native,
and other hosts are unaffected.

---

## 2. Wine Symbol Surface

**R-WMB-2.1** The set of `winemac.drv` symbols dxmt9 currently relies
on is, at minimum:

- `get_win_data(HWND hwnd) -> macdrv_win_data*`
- `release_win_data(macdrv_win_data *data)`
- `macdrv_create_metal_device(void) -> macdrv_metal_device`
- `macdrv_release_metal_device(macdrv_metal_device)`
- `macdrv_view_create_metal_view(macdrv_view, macdrv_metal_device) -> macdrv_metal_view`
- `macdrv_view_get_metal_layer(macdrv_metal_view) -> macdrv_metal_layer`
- `macdrv_view_release_metal_view(macdrv_metal_view)`
- `macdrv_get_cocoa_window(HWND, BOOL require_on_screen) -> macdrv_window`

…plus the aggregate `macdrv_functions` struct when present.

The schema example
`specs/winemetal/assets/winemacdrv.h.example` carries the canonical C
signatures the patch must keep visible.

**R-WMB-2.2** The exact set is permitted to shrink as dxmt9's
presenter evolves; it must not grow without a spec update. Each added
symbol becomes part of the patch surface and the manifest's compatible
Wine matrix.

**R-WMB-2.3** Calling these symbols requires the unix-side calling
convention they were declared with. dxmt9's bridge mirrors the same
signatures via a vendored header (R-WMB-3.x).

---

## 3. Vendored Header

**R-WMB-3.1** dxmt9 ships an in-tree header
`include/winemetal/winemacdrv.h` that vendors a minimal copy of the
Wine `winemac.drv` C signatures dxmt9 calls. The header may be hand-
authored from Wine source rather than copied wholesale, but each
declaration must match the Wine source for the supported Wine version
matrix (R-WMB-6.x).

**R-WMB-3.2** The header carries license attribution to Wine
(LGPL-2.1-or-later) in its top comment block and is marked
"declaration-only — implementation lives in `winemac.so`." It contains
no Wine implementation code, only function pointer signatures and the
small `macdrv_win_data` view struct shape dxmt9 reads.

**R-WMB-3.3** The header is independent of the patch file
(R-WMB-4.x). dxmt9 can compile against the header on any host; the
patch is only needed at *runtime* (i.e. when an unpatched Wine root is
selected).

---

## 4. Wine Patch

**R-WMB-4.1** dxmt9 maintains a single textual patch
`wine/patches/winemac-expose-symbols.patch` that re-applies
`__attribute__((visibility("default")))` (or equivalent declaration
attribute) to every symbol in §2. The patch must:
1. Apply cleanly to one specific upstream Wine commit / tag, named in
   the patch header (e.g. `Applies to: wine-staging-11.7`).
2. Be minimal — only the visibility / linker attribute change. No
   functional or ABI-breaking edits.
3. Carry an SPDX-style header summarising why dxmt9 needs each symbol
   exposed.

Example shape: `specs/winemetal/assets/macdrv-expose-symbols.patch.example`.

**R-WMB-4.2** The patch is **not** auto-applied by dxmt9's build
system. The operator (or downstream packager such as Heroic's
DXMT-flavored Wine, or CodeWeavers CrossOver) is responsible for
producing a Wine bundle that contains it.

**R-WMB-4.3** The patch lives in-tree at `wine/patches/` with a README
that explains how to rebase against a newer Wine source tree. Each
supported Wine version gets exactly one patch file. When upstream
Wine churns the relevant code, a new patch file is added and the
older one is retained for archival reference until the manifest
matrix drops the older Wine.

---

## 5. Manifest Extension

**R-WMB-5.1** Each `[[wine]]` entry in
`experiments/wine/manifest.toml` (defined in
`specs/experiments/runtime/`) gains an optional boolean field:

```toml
requires_patch = true   # default false
```

- `false` (default) — the Wine root either already exposes the
  symbols natively (Wine ≤ 11.6, CrossOver-derived builds) or its
  exposure status is unknown / unverified.
- `true` — the Wine root needs the dxmt9 patch applied for the
  bridge to work. dxmt9's runtime probe (R-WMB-7) will fail fast if
  the symbols are missing.

**R-WMB-5.2** A second optional field:

```toml
patch_status = "applied"  # applied | unpatched | unknown
```

- `applied` — operator asserts the patch is in this Wine root.
- `unpatched` — operator asserts the patch is **not** applied.
  Harness emits a warning at run start.
- `unknown` — default. Harness behaves per `requires_patch`.

**R-WMB-5.3** Resolution behavior when the harness selects a wine
root for a wild experiment (in `scripts/run_apps/run_experiment.py`):
1. If `requires_patch = true` and `patch_status = "unpatched"` → hard
   error before launch, with a message naming the patch path and a
   pointer to this spec.
2. If `requires_patch = true` and `patch_status = "unknown"` → warn
   at run start. dxmt9's runtime probe (R-WMB-7) is the secondary
   gate.
3. If `requires_patch = false` → no extra check; trust the operator's
   declaration.

**R-WMB-5.4** A new utility script
`scripts/wine/check_patch.py <wine_root>` produces a one-line `applied
/ unpatched / unknown` result by `nm`-ing the Wine root's `winemac.so`
for the required symbols. Operators may run it ad-hoc to populate the
`patch_status` field; the harness does **not** call it automatically
on every run (cost).

---

## 6. Compatibility Matrix

**R-WMB-6.1** dxmt9 supports a discrete set of Wine versions per
release. The current set is recorded in this spec and mirrored in
`experiments/wine/manifest.toml`. A Wine root outside that set may
work but is not validated.

**R-WMB-6.2** The initial supported set, in order of preference:

| Wine source | Working out of box? | How to qualify | Notes |
|---|---|---|---|
| **`Sikarugir-App/Engines` pre-built Wine** (`WS12WineCX24.0.7_*.tar.xz`, `WS12WineSikarugir10.0_*.tar.xz`) | ✅ | Drop the `wswine.bundle` into `experiments/wine/<id>/`; co-locate the Sikarugir Wrapper Template's `Frameworks/*.dylib` files in `experiments/wine/` (the dyld `@rpath/bin/../..` search target); replace `bin/wine` and `bin/wineserver` with shims that export `DYLD_FALLBACK_LIBRARY_PATH=experiments/wine`. `scripts/wine/install_wine.py` automates all of the above. | **dxmt9's default recommended runtime.** Verified 2026-05-11 end-to-end on SFIV: handshake OK, 76 s full benchmark, status pass. The Sikarugir builds re-expose `macdrv_functions` and ship a compatible wow64 (the `MemoryWineImageInfo` info class 1002 issue documented for vanilla CrossOver 26 does not affect this build). |
| CodeWeavers CrossOver product (`~/Applications/CrossOver.app`, licensed) | ❌ blocked | Three independent issues, audited 2026-05-11 on CrossOver 26 (`wine-11.0-8709`): **(a)** `bin/wine` is a Perl wrapper that demands a "default" CrossOver bottle dxmt9 prefixes do not own — bypassable via direct `wineloader` invocation. **(b)** wow64 `NtQueryVirtualMemory` lacks the `MemoryWineLoadUnixLibByName` arm (class 1002) — `unsupported class 1002` FIXME every call, returns `STATUS_INVALID_INFO_CLASS`. Class 1000 (`MemoryWineLoadUnixLib`) **is** supported. **(c)** `ntdll.so` has `/opt/cxoffice/lib/wine` baked in as the unixlib search root; `WINEDLLDIR0` and `WINEDLLPATH` are referenced in strings but empirically did not override unixlib pairing in the PoC. Class 1000 retry under WINEDLLOVERRIDES=winemetal=b returned `STATUS_DLL_NOT_FOUND` even with both env vars pointing at our staged tree. | **Not supported.** No dxmt9-side code change can fix (a)+(b)+(c) without writing into the user's licensed install at `/opt/cxoffice` (sudo + system-dir modification) or patching the CrossOver wineloader (license-incompatible). Users who want CrossOver should wait for CodeWeavers to repair the wow64 thunk or use a separate licensed install pre-deployed at `/opt/cxoffice`. |
| Self-built Wine from WineHQ / CodeWeavers source with the dxmt9 macdrv patch applied (R-WMB-10) | ✅ | Confirmed by `scripts/wine/check_patch.py` after `make install`. | The reproducible / fully-open path for contributors who cannot license CrossOver and want to track upstream Wine more closely than Sikarugir releases. |
| Gcenx `macOS_Wine_builds` releases (Heroic redistributes these as `Wine-11.x`, `Wine-11.x-DXMT`) | ❌ | none. Both vanilla and `-DXMT` variants ship a stripped `winemac.so` with only `__wine_unix_call_funcs` / `__wine_unix_call_wow64_funcs` visible. | dxmt9 must refuse to run wild experiments on these. The "DXMT" suffix names a pre-bundled set of dxmt D3D11 DLLs, not a Wine patch — `winemac.so` is md5-identical to the vanilla bundle (`67afe1eb6fab5b958f47a6d58f4306b8`). Verified 2026-05-11 against Wine-11.0_1 / 11.6_1 / 11.7 / 11.7-DXMT. |
| `Heroic-Games-Launcher/wine-crossover` (`Wine-Crossover-23.7.1-1` mirror) | ❌ | none. Also stripped. | Heroic PR #5488 itself documents this as a fallback "only when no other option works." Listed for completeness. |
| `wine-staging-master` ≥ 11.8 from a source build | Depends | The dxmt9 patch must be rebased; rerun `check_patch.py`. | Until the rebase lands, not in the supported set. |

**R-WMB-6.3** Updating to a new Wine minor version is a deliberate
change: the patch is rebased, validated against the symbol set in
§2, and a new manifest entry is added. The previous version's
manifest entry remains valid for one more release for operators who
have not migrated.

**R-WMB-6.4** Heroic's wine downloader (`Wine-Staging-macOS`,
`Wine-11.x`, `Wine-11.x-DXMT`, `Wine-Crossover-*`) is **not** a
supported source for dxmt9 today. Heroic's UI may still advertise
"DXMT compatibility" — that label refers to the pre-bundled D3D11
DLL set, not to a patched `winemac.so`. The runtime probe (R-WMB-7)
will reject any of these Wine roots once dxmt9 attempts to attach a
Metal layer. Operators must use Sikarugir-Engines pre-builds, the
licensed CodeWeavers CrossOver product (with the info-class
follow-up), or a self-built Wine (R-WMB-10).

**R-WMB-6.5** Wine roots re-execute `wineboot` at every `wine`
invocation when the prefix's `.update-timestamp` is stale or when
the runtime detects new macOS volumes. That wineboot rewrites
`<prefix>/dosdevices/<letter>:` for any letter Wine auto-claims (in
practice `D:` on this machine, which gets re-bound to the first
`/Volumes/` or cryptex mount). dxmt9 must therefore not depend on a
`dosdevices`-routed binary path. The harness's binary path for a
wild experiment is the **local POSIX path** to the executable inside
`experiments/apps_3rd/<name>/`; Wine resolves that via its built-in
`Z:` drive (mapped to `/`). The `install_drive_letter` field stays
in CATALOGUE for documentation but is no longer load-bearing for the
launch path.

---

## 7. Runtime Probe and Failure Mode

**R-WMB-7.1** At first call into the bridge
(`src/winemetal/unix/winemetal_private_api.mm`'s
`CreateMetalViewFromHWND` and friends), dxmt9 must:
1. Attempt `dlsym(RTLD_DEFAULT, "macdrv_functions")` first.
2. Fall back to direct `dlsym(RTLD_DEFAULT, "macdrv_view_create_metal_view")` etc.
3. If neither yields a non-NULL function pointer for the symbols in §2:
   - Emit **one** `[Error]` line via the dxmt9 logger naming this
     spec, the missing symbol(s), the active Wine root, and a
     remediation hint.
   - Propagate `D3DERR_NOTAVAILABLE` (or equivalent) from the next
     `CreateDevice` / present-time call that triggers the lookup.
   - Do **not** silently return success and render to an offscreen
     drawable.

**R-WMB-7.2** The error is emitted at most once per process to avoid
log spam. A repeat lookup on the same path uses the cached "missing"
state.

**R-WMB-7.3** The remediation hint must list, in order:
1. "Use a Wine build that exposes macdrv symbols (CrossOver Wine 24+, or a Heroic build patched per `wine/patches/winemac-expose-symbols-<ver>.patch`)."
2. "If you have a custom Wine, run `scripts/wine/check_patch.py <root>` to confirm the symbols are visible."
3. A link to this spec (`specs/winemetal/{requirements,design}.md`).

---

## 8. Documentation

**R-WMB-8.1** `agents/rules/test_wild.rules.md` must reference this
spec from its "Documented Exceptions" / "Diagnostic Checklist"
sections. Specifically:
- The diagnostic checklist gains a step: "Is the Wine root patched
  (run `scripts/wine/check_patch.py`)?"
- The exceptions table notes that any Wine root with
  `requires_patch = true` is treated as a runtime requirement, not a
  bug-class workaround.

**R-WMB-8.2** `experiments/wine/README.md` must document the
`requires_patch` and `patch_status` fields with an inline example.

**R-WMB-8.3** A new `wine/patches/README.md` explains the patch
workflow: how to rebase, how to run `check_patch.py` after rebase,
how to add a new Wine version row to R-WMB-6.2.

---

## 9. Non-Goals

- No automatic Wine source fetching, building, or patching from
  dxmt9. The patch is a text file in-tree; the operator owns the
  Wine build.
- No fallback to a Vulkan/MoltenVK WSI path. dxmt9 stays native
  Metal end-to-end. If the user wants the Vulkan route, it is a
  future and explicit spec, not a hidden fallback.
- No support for Wine roots that have *partially* exposed symbols
  (e.g. only `get_win_data` visible). The probe treats partial
  exposure as missing, since the path is unusable without the full
  set.
- No support for hot-reloading the symbol set within a running
  process. Detection is one-shot at first use.

---

## 10. Setup Workflows

Two recipes are supported. Both result in a `[[wine]]` entry whose
`patch_status = "applied"` and whose probe (R-WMB-7) finds
`macdrv_functions`.

### 10.A — Sikarugir pre-built Wine (recommended)

`Sikarugir-App/Engines` releases ship Wine bundles that already
re-expose the macdrv symbols dxmt9 needs. The Wrapper Template (`Sikarugir-App/Wrapper`) ships the macOS runtime dylibs
(`libfreetype`, `libinotify`, GStreamer.framework, etc.) the Wine
binary links against. dxmt9 places them side-by-side.

**R-WMB-10.A.1** `scripts/wine/install_wine.py` (operator-run,
single command) MUST automate:
1. Downloading `WS12WineCX24.0.7_<rev>.tar.xz` or
   `WS12WineSikarugir10.0_<rev>.tar.xz` from
   `https://github.com/Sikarugir-App/Engines/releases/download/v1.0/`.
2. Extracting the contained `wswine.bundle` to
   `experiments/wine/<id>/`.
3. Downloading the matching `Sikarugir-App/Wrapper` Template
   (`Template-<rev>.tar.xz`).
4. Extracting `Template-*.app/Contents/Frameworks/*.dylib` to
   `experiments/wine/` (one level above `<id>/`, where the bundle's
   own `@rpath/bin/../..` resolves at runtime).
5. Renaming `experiments/wine/<id>/bin/wine` and
   `experiments/wine/<id>/bin/wineserver` to `*.real`, then writing
   thin Bash shims that export
   `DYLD_FALLBACK_LIBRARY_PATH=<experiments/wine>` and exec the
   real binary. These let Wine's `dlopen()` calls (e.g. FreeType)
   find the co-located dylibs.
6. Verifying `winemac.so` exposes `_macdrv_functions` (the same
   check `scripts/wine/check_patch.py` performs).
7. Optionally appending a manifest entry — gated behind a CLI flag
   so an existing entry is never overwritten silently.

**R-WMB-10.A.2** The list of supported Sikarugir engine tags is
sourced from `Sikarugir-App/Engines/EngineList.txt` at script
runtime. The script must refuse to install an engine not listed
there (defensive; reduces accidental drift).

**R-WMB-10.A.3** `install_wine.py` MUST be idempotent. Re-running
with the same target id either no-ops (if the bundle is intact and
the symbol audit passes) or replaces the bundle cleanly.

### 10.B — Wine source build (reproducible alternative)

3Shain dxmt's geek-guide path. Use this when Sikarugir's releases
do not cover the desired Wine version, or when the operator wants a
fully-from-source pipeline.

**R-WMB-10.B.1** dxmt9 ships a documentation entry at
`wine/patches/README.md` that walks the operator through:
1. Cloning a tagged Wine source tree (WineHQ or the CodeWeavers
   open-source CrossOver Wine tarball linked from
   https://www.codeweavers.com/about/wine).
2. Applying `wine/patches/winemac-expose-symbols-<wine-version>.patch`.
3. Configuring + building on macOS (Apple Silicon native or
   x86_64 via Rosetta 2 — the dxmt-community default per
   3Shain/dxmt #141).
4. Installing into `experiments/wine/<id>/`.
5. Running `scripts/wine/check_patch.py <root>` to confirm
   `applied`.
6. Adding the matching `[[wine]]` entry to
   `experiments/wine/manifest.toml`.

**R-WMB-10.B.2** dxmt9 does **not** automate the source-build
steps. Build toolchain (Homebrew, LLVM, GStreamer, Wine
prerequisites) varies per macOS version and is the operator's
responsibility.

**R-WMB-10.B.3** Cross-reference: the 3Shain dxmt geek guide lives at
https://github.com/3Shain/dxmt/wiki/DXMT-Installation-Guide-for-Geeks.
That guide is normative for the source-build workflow on macOS.
