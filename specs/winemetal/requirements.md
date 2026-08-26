---
type: "Spec Requirements"
title: "winemetal Requirements — Wine Metal Surface Bridge"
description: "ExtEscape and legacy macdrv compatibility contracts for Wine Metal surfaces."
tags: [specs, winemetal, requirements]
---

# winemetal Requirements — Wine Metal Surface Bridge

This spec governs how dxmt9 obtains a Wine-owned `CAMetalLayer` for a Win32
`HWND`. D3D9-visible swap-chain behavior remains owned by `specs/d3d9/wsi/`;
artifact discovery remains owned by `specs/deploy/`.

The legacy macdrv symbol-bridge requirements retain their stable IDs except for
the retired `R-WMB-7.x` generic-probe contract. That legacy contract applies
only to exact Wine builds qualified as
`legacy-macdrv-symbols:<runtime-id>`; it is not the
forward compatibility strategy for current Wine 11.x. Sections 11-18 define
the primary `ExtEscape` surface protocol proposed by [Wine MR !11058](https://gitlab.winehq.org/wine/wine/-/merge_requests/11058)
and consumed by [upstream DXMT PR #166](https://github.com/3Shain/dxmt/pull/166).
Numeric values and wire layout remain revision-pinned until Wine accepts and
merges a stable interface. Where a legacy requirement conflicts with
`R-WMB-11`-`R-WMB-17`, the newer requirement controls all non-legacy runtimes.

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
  legacy bridge to work. WSI selection and failure follow `R-WMB-15.1` and
  `R-WMB-16.2`.

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
   at run start. Exact legacy-runtime qualification under `R-WMB-14.1` is the
   secondary gate.
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
| **`3Shain/wine` fork release `v9.9-mingw`** (the upstream-DXMT author's Wine, `wine-9.9-4-g496a727`) | ✅ | Extract `wine.tar.gz` into `experiments/wine/3shain-v9.9/` and register the manifest entry (`metal_surface_protocol = "legacy-macdrv-symbols:3shain-v9.9"`). **Bootstrap any fresh/updated prefix once with `WINEDLLOVERRIDES="mscoree=;mshtml="`** — the tarball bundles no Wine Mono/Gecko, and the interactive mono-install dialog otherwise hangs a headless run forever (observed twice on 2026-08-26 before diagnosis). | **Accepted alternative runtime**, verified 2026-08-26: `winemac.so` exports `_macdrv_functions` with the exact 10-slot layout dxmt9's `macdrv_functions_t` mirror expects (table dump symbol-mapped slot-by-slot; `macdrv_win_data.client_cocoa_view` confirmed at offset `0x18`), plus the flat-symbol fallback set. conf-d3d9-triangle passed on both x64 and x86/wow64 lanes (abi-hash handshake OK in experimental wow64 mode — the CrossOver class-1002 blocker is absent), and SFIV rendered end-to-end (benchmark scene, 50.73 avg fps at capture) — all reusing the sikarugir-linked `winemetal_dxmt9.so` without rebuild. Ecosystem benefit: the same Wine root serves upstream DXMT's D3D10/11 DLLs, so one runtime covers D3D9–11. |
| CodeWeavers CrossOver product (`~/Applications/CrossOver.app`, licensed) | ❌ blocked | Three independent issues, audited 2026-05-11 on CrossOver 26 (`wine-11.0-8709`): **(a)** `bin/wine` is a Perl wrapper that demands a "default" CrossOver bottle dxmt9 prefixes do not own — bypassable via direct `wineloader` invocation **or** by creating a bottle through `cxbottle --create --bottle <name> --template <winxp\|win10\|...>` and running `wine --bottle <name>` (verified CLI works). **(b)** wow64 `NtQueryVirtualMemory` lacks the `MemoryWineLoadUnixLibByName` arm (class 1002) — `unsupported class 1002` FIXME every call, returns `STATUS_INVALID_INFO_CLASS`. **Bottle context does NOT bypass this** — confirmed in a follow-up PoC where the same failure reproduces inside a `cxbottle`-created bottle. Class 1000 (`MemoryWineLoadUnixLib`) **is** supported by wow64. **(c)** `ntdll.so` has `/opt/cxoffice/lib/wine` baked in as the unixlib search root; `WINEDLLDIR0` and `WINEDLLPATH` are referenced in strings but empirically did not override unixlib pairing in the PoC. Class 1000 retry under WINEDLLOVERRIDES=winemetal=b returned `STATUS_DLL_NOT_FOUND` even with both env vars pointing at our staged tree. **(d)** `~/Applications/CrossOver.app/Contents/SharedSupport/CrossOver/lib/wine/` is **SIP/codesign-protected** — `winemetal.so` cannot be staged there (`Operation not permitted`), foreclosing the builtin-lane fallback. | **Not supported.** No dxmt9-side code change — including the bottle CLI route — fixes (b) or works around (c)+(d) without writing into the user's licensed install at `/opt/cxoffice` (sudo + system-dir modification + SIP entitlements) or patching the CrossOver wineloader (license-incompatible). Users who want CrossOver should wait for CodeWeavers to repair the wow64 thunk. |
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
DLL set, not to a patched `winemac.so`. WSI selection under `R-WMB-15.1`
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
`patch_status = "applied"` and whose exact runtime identity qualifies the
`macdrv_functions` path under `R-WMB-14.1` and `R-WMB-15.5`.

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

---

## 11. ExtEscape Scope

**R-WMB-11.1** This spec covers the control-plane path from a PE-side `HWND` to a
Wine-owned client-surface token and `CAMetalLayer` token, their transfer to the
unix presenter, and their ordered release.

**R-WMB-11.2** This spec does not authorize PE code to call Metal or Objective-C,
does not put host pointers in recorded command chunks, and does not make dxmt9
responsible for implementing the Wine-side escape handler in an upstream Wine
tree.

**R-WMB-11.3** The protocol is macOS-only. Other hosts must reject the macOS WSI
path without changing their D3D9 loader behavior.

## 12. Primary `ExtEscape` Protocol

**R-WMB-12.1** The PE D3D9 WSI control path must query
`QUERYESCSUPPORT(MACDRV_ESCAPE_GET_SURFACE)` on an HDC obtained for the target
`HWND` before issuing `MACDRV_ESCAPE_GET_SURFACE`.

**R-WMB-12.2** A successful get-surface response must contain a fixed-width POD
payload with exactly two opaque 64-bit values:

- a Wine client-surface token retained by `winemac.drv`;
- a borrowed `CAMetalLayer` token whose validity is pinned by that surface.

The PE side must treat both values as opaque integers. It must not dereference,
retain, release, message, or otherwise interpret either host object.

**R-WMB-12.3** The layer token may cross the PE/unix boundary only in a dedicated,
cold WSI bootstrap or rebind call. It must not appear in `CommandChunk`, draw,
state, resource, query, or shader records and must never be persisted as part of
a schema-stable hot-path packet.

**R-WMB-12.4** The unix presenter may convert the nonzero layer token to a
borrowed `CAMetalLayer` reference only after PE observes a positive escape
result and validates both returned tokens. Wine retains ownership. dxmt9 must
not release the layer independently of the associated Wine surface.

**R-WMB-12.5** `MACDRV_ESCAPE_RELEASE_SURFACE` must be issued exactly once for
every successfully acquired surface token. Before release, the unix presenter
must stop drawable acquisition and drain or fence every command buffer that can
reference the layer. PE retains the acquisition HDC as cold state, releases the
surface through that same HDC/`ExtEscape` path after the unix teardown or
rebind call returns, and balances the HDC with `ReleaseDC` after the release
attempt. No HDC crosses the PE/unix wire or enters a `CommandChunk`.

**R-WMB-12.6** A reset or additional-swap-chain rebind must not expose a half-
updated pair. The new surface is acquired and validated first; the unix
presenter atomically adopts it or reports failure; only then may PE release the
old surface. Failure preserves the old valid binding when D3D9 reset semantics
permit it.

## 13. Protocol Schema and Provenance

**R-WMB-13.1** dxmt9 must keep the escape names (currently values 6790 and 6791),
payload size, field widths, and release semantics in one declaration-only
compatibility header. PE and native tests must consume that header rather than
duplicating magic escape numbers.

**R-WMB-13.2** The compatibility header must identify the Wine commit or merged
Wine release that defines the protocol and carry Wine LGPL attribution when it
derives declarations from Wine source. It must contain no Wine implementation
code.

**R-WMB-13.3** Each supported Wine runtime entry must record an exact Metal
surface protocol value:

```toml
metal_surface_protocol = "extescape-v1" # extescape-v1 | legacy-macdrv-symbols:<runtime-id> | unsupported | unknown
```

`extescape-v1` requires a successful runtime `QUERYESCSUPPORT`; a manifest
claim alone must never bypass the probe.

Legacy qualification must include the exact runtime identity in the value, for
example `legacy-macdrv-symbols:sikarugir-cx-24.0.7-<hash>`. An unqualified
`legacy-macdrv-symbols` declaration is not sufficient for selection. The
runtime's unixlib-loader capabilities are recorded separately under
`R-DEPLOY-3.11` and must not be inferred from this field.

## 14. Legacy macdrv Symbol Fallback

**R-WMB-14.1** When `MACDRV_ESCAPE_GET_SURFACE` is not supported, dxmt9 may use
the existing aggregate `macdrv_functions` path only for a Wine build that
is explicitly pinned as `legacy-macdrv-symbols:<runtime-id>` and has end-to-end
WSI evidence.

**R-WMB-14.2** Symbol visibility alone is not sufficient qualification for Wine
11.x. A runtime is incompatible with the legacy path if unixlibs are loaded
`RTLD_LOCAL`, if `macdrv_win_data` differs from the layout expected by the
consumer, or if no persistent client view exists when the swap chain is created.

**R-WMB-14.3** The historical `visibility("default")` patch and
`requires_patch` / `patch_status` manifest fields are legacy-only compatibility
surfaces. They must not be documented as the strategic fix for current Gcenx or
upstream Wine 11.x builds. Version-specific patch files may remain as archival
and reproducibility material for already-qualified legacy runtimes.

## 15. Runtime Selection and Compatibility

**R-WMB-15.1** WSI acquisition order is fixed:

1. probe and use `extescape-v1`;
2. use the legacy symbol path only for an explicitly qualified runtime;
3. fail with `D3DERR_NOTAVAILABLE` and one diagnostic.

There is no silent offscreen-present fallback for a windowed device that
requires presentation.

**R-WMB-15.2** The compatibility manifest and run result must record the exact
declared protocol, the observed acquisition path, and the independently
observed unixlib-loader capability set. The observed acquisition value is one
of `extescape-v1`, `legacy-macdrv-symbols`, or `unavailable`.

**R-WMB-15.3** As of the contract baseline, these runtime classes are interpreted
as follows:

| Wine runtime | Protocol status | Qualification |
|---|---|---|
| Wine revision containing the accepted `MACDRV_ESCAPE_*_SURFACE` interface | target | Runtime query, x64/WoW64 smoke, resize, teardown, and present evidence required |
| Stock upstream Wine or Gcenx/Heroic Wine without that Wine change | unsupported | A working unixlib loader does not provide a Metal surface; the probe must fail cleanly, and a distribution suffix alone is not qualification |
| Exact audited Wine build with a validated `macdrv_functions` ABI | required legacy fallback | Must use the legacy path only for a manifest entry qualified as `legacy-macdrv-symbols:<runtime-id>` |
| Self-built Wine carrying only the old visibility patch | unknown | Must not be accepted until loader visibility, layout, and client-view lifetime are all proven |

**R-WMB-15.4** A new Wine minor version is unsupported until its protocol probe,
payload layout, surface lifetime, and both PE architectures are revalidated. A
matching filename or successful `nm` symbol audit is not sufficient evidence.

**R-WMB-15.5** The existing `macdrv_functions` acquisition path is a required
legacy fallback for every exact Wine manifest entry qualified as
`legacy-macdrv-symbols:<runtime-id>`. A failed `QUERYESCSUPPORT` probe on such
an entry must continue into that fallback; it must not be treated as terminal
incompatibility. The `winemetal_dxmt9.*` rename and the addition of ExtEscape
must not change this Wine-facing fallback behavior. Individual Wine
distributions are evidence fixtures, not part of the protocol name or contract.

**R-WMB-15.6** WSI protocol qualification and unixlib-loader qualification are
orthogonal. A windowed deployment is supported only when the selected deploy
mode satisfies `R-DEPLOY-3.11` and this section selects either a runtime-probed
`extescape-v1` path or an exact `legacy-macdrv-symbols:<runtime-id>` path. A
successful provider ABI handshake alone must not enable presentation.

## 16. Failure and Diagnostics

**R-WMB-16.1** A failed `GetDC`, `QUERYESCSUPPORT`, get-surface escape, fixed
`cbOutput` call-shape check, zero output token, unix adoption, or release must
produce one scoped diagnostic containing the stage, `HWND`, Wine version/root
when known, and Wine
or GDI status.

**R-WMB-16.2** Unsupported acquisition must fail device or swap-chain creation
with `D3DERR_NOTAVAILABLE` (or the closest already-established creation error).
It must not report success while presenting to an offscreen drawable.

**R-WMB-16.3** Release errors must not cause double release. After a release
attempt, the PE token state is cleared and the failure is logged; unix objects
must already be quiescent.

**R-WMB-16.4** dxmt9 must emit one machine-readable line per acquired binding:

```text
[dxmt9-wsi] layer_acquisition=<extescape-v1|legacy-macdrv-symbols|unavailable> hwnd=0x<hex>
```

The experiment harness must preserve this value in `result.json`.

## 17. Verification

**R-WMB-17.1** Native protocol tests must cover payload size/width, zero tokens,
unsupported `QUERYESCSUPPORT`, get failure, unix-adoption rollback, and exactly-
once release without loading Wine or Metal.

**R-WMB-17.2** Wine integration evidence must cover:

- successful creation/present/destruction on x64;
- the same flow from a 32-bit PE process under WoW64;
- reset/rebind with an in-flight frame;
- additional swap chains on distinct `HWND`s;
- unsupported-Wine failure without a crash or black-window success;
- repeated create/destroy proving balanced surface acquisition and release.

**R-WMB-17.3** A coexistence run with upstream DXMT installed must prove that
DXMT's D3D11 WSI and dxmt9's D3D9 WSI can both initialize without sharing bridge
module identity. Artifact-name ownership is specified by `R-DEPLOY-2.11`.

**R-WMB-17.4** Every ExtEscape or bridge-module rename change must pass builtin
x64 and WoW64 WSI smoke through the existing `macdrv_functions` fallback on at
least one exact audited `legacy-macdrv-symbols:<runtime-id>` runtime before
rollout. Evidence
must show the suffix-qualified dxmt9 modules, an ABI-hash handshake,
`layer_acquisition=legacy-macdrv-symbols`, successful device creation, and at
least one successful present. This gate does not claim app-local support on a
runtime that lacks `MemoryWineLoadUnixLibByName`; app-local support remains
governed by `R-DEPLOY-3.2`-`R-DEPLOY-3.12` and
`R-DEPLOY-6.2`-`R-DEPLOY-6.10`.

**R-WMB-17.5** Integration evidence must cover the composed negative case in
which the unix provider loads and passes its ABI handshake but no qualified
Metal-surface protocol is available. The observed loader disposition must
remain successful, `layer_acquisition` must be `unavailable`, and creation must
fail according to `R-WMB-16.2` without a black-window success.

## 18. Non-Goals

- No Vulkan/MoltenVK presentation fallback.
- No dependence on Wine private struct offsets in the primary path.
- No automatic acceptance of an unmerged Wine MR as a stable ABI.
- No bundling of a patched Wine runtime under the MIT-only dxmt9 artifact set;
  any separately distributed Wine derivative remains LGPL-compliant and
  version-pinned.
