# Wild Testing Rules — Wine Runtime Selection

Rules for running dxmt9 against real D3D9 binaries (catalogue experiments,
SFIV, Anno 1404, etc.) in `experiments/`. They cover the host Wine
runtime, not the dxmt9 build itself.

> **Spec:** the mechanics of the manifest, prefix bootstrap, and apps_3rd
> layout are defined in `specs/experiments/runtime/{requirements,spec}.md`.
> This rule covers operational guidance only.

## Rule: Default to the Sikarugir-Engines Wine runtime

**All wild-experiment runners MUST default to a Wine root whose
`winemac.so` re-exposes `_macdrv_functions`** so dxmt9 can attach a
`CAMetalLayer`. Without that symbol, the bridge silently no-ops and
the user sees a black window. See
`specs/winemetal/requirements.md` R-WMB-6.2 for the audited
compatibility matrix.

| Runtime | Use as default? | Why |
|---------|-----------------|-----|
| **`sikarugir-cx-24.0.7`** (Sikarugir-App/Engines pre-built) | **Yes** | Installed in one command via `python3 scripts/wine/install_wine.py --engine sikarugir-cx-24.0.7 --target-id sikarugir-cx-24.0.7 --register-in-manifest`. Pre-built `winemac.so` exposes `_macdrv_functions`; wow64 is compatible with dxmt9's `MemoryWineImageInfo` lookup. Verified SFIV end-to-end 2026-05-11 (status pass, 76 s benchmark). |
| Self-built Wine with `wine/patches/winemac-expose-symbols-<ver>.patch` | Yes (alternative) | The reproducible-from-source path; see `specs/winemetal/requirements.md` R-WMB-10.B. |
| `Wine-11.x` / `Wine-11.x-DXMT` (Heroic / Gcenx redistributed) | **No** | `winemac.so` is stripped (md5-identical between vanilla and `-DXMT`). The `-DXMT` suffix bundles pre-built dxmt D3D11 DLLs only and does **not** patch `winemac.so`. Runtime probe rejects them. |
| Heroic `Wine-Crossover-23.7.1-1` | **No** | Same stripping pattern; Heroic PR #5488 itself documents this build as a fallback "only when no other option works." |
| CodeWeavers CrossOver product (licensed) | **No** | Audited 2026-05-11: four independent blockers — Perl `bin/wine` wrapper (bottle-context required, also bypassable via `cxbottle` CLI but does not fix the next three), wow64 missing `MemoryWineLoadUnixLibByName` (class 1002) and **bottle context does not bypass it**, `ntdll.so` hardcodes `/opt/cxoffice/lib/wine` with no working `WINEDLLDIR` override, and `CrossOver.app/lib/wine/` is SIP/codesign-protected so `winemetal.so` cannot be staged. See `specs/winemetal/requirements.md` §6.2. |
| `Wine-*-VK` / Proton-style VK builds | No | Substitutes a different `d3d9.dll` and reroutes through Vulkan; the comparison is no longer "dxmt9 vs. Wine builtin." |

**Reason — concrete incident (2026-05-10):** SFIV under `Wine-11.6-DXMT`
appeared to flood `dxmt9.log` with `0xc0000005` access violations on
every `factory_*` bridge call, suggesting a 32-bit bridge regression.
Re-running the same 32-bit binary (`conf-d3d9-triangle-x86.exe`) under
vanilla `Wine-11.6` produced a clean `[winemetal-abi] info: abi-hash
handshake OK` log and passed. The `Wine-11.6-DXMT` runtime had drifted
from upstream Wine in a way that masked or simulated wow64 dispatch
differently — the dxmt9 bridge was fine, the runtime was the variable.

## Rule: one way to run a catalogue app

**Every wild app is run through `run_experiment.py`.** There is one invocation,
and it is the same for GT1/GT2/GT3, SFIV, Anno, and everything else:

```sh
python3 scripts/run_apps/run_experiment.py run <app-id>
```

Per-app shell wrappers under `scripts/run_apps/` are **legacy — do not add
new ones.** Three were removed on 2026-07-29 and each shows why:

| Removed | Why |
|---|---|
| `run_app-d3d9-sfiv-benchmark_experiment.sh` | Forwarded every argument unchanged. It added nothing except a second name for the same command, and its README description had drifted to describe behaviour it no longer had. |
| `run_app-d3d9-anno-1404_experiment.sh` | Hardcoded a Heroic `Wine-11.7` root and a prefix path, overriding the entry's `wine_id`. This is the anti-pattern below, shipped. |
| `run_suites/run_sfiv_benchmark_crossover_oracle.sh` | Passed `--host crossover`, which `run_experiment.py` does not accept, so it had been failing at argument parsing. CrossOver is a rejected runtime (table above). |

The cost of a second invocation path is not the wrapper; it is that the two
paths drift. SFIV's wrapper did not set `DXMT_EXPERIMENT_PROFILE`, so it
silently measured the `debug` profile — validation layer on, debug logging —
and produced `11.3` sampled fps against a real `43.02`. That was investigated
as a 4x renderer regression before the profile was found. The profile now
defaults to `perf` and is recorded in the run output, but the general lesson
is the rule above: one path.

Two supervised wrappers under `scripts/tools/` are **not** covered by this and
stay, because they add real supervision rather than renaming a command:
`run_3dmark05_perf_probe.sh` (timeouts, watchdog, capture preflight, proof
gates) and `run_app-d3d9-3dmark05-verify_direct.sh` (direct-prefix runs with a
process-group kill).

## How to Apply

In `experiments/CATALOGUE.toml` the per-app default is the manifest id, not a path:

```toml
# In CATALOGUE [[app]] for a wild experiment:
wine_id = "heroic-11.7"
```

On the command line, `--wine-id` overrides the CATALOGUE default:

```sh
python3 scripts/run_apps/run_experiment.py run <app-id> --wine-id heroic-11.7
```

What to avoid — never hardcode a Wine path or default to a non-vanilla variant. Both bypass the manifest:

```sh
wine_root_default=".../Wine-11.7-DXMT/.../wine"   # bypasses the manifest.
```

Always allow `--wine-root <path>` to override the default so a
maintainer can A/B-test against a patched runtime when intentional.

When choosing a host for a perf A/B run, ensure both `vanilla` and
`dxmt9` lanes share the **same** Wine root unless the comparison
explicitly is "vanilla-Wine builtin d3d9 vs. staged dxmt9" — in which
case both must still be unpatched Wine builds.

## Documented Exceptions

The list of apps that legitimately need a non-vanilla runtime is small;
adding one is a deliberate decision and must be justified inline:

| App | Required runtime | Reason |
|-----|------------------|--------|
| `app-d3d9-anno-1404` | `Wine-*-DXMT` (any current DXMT build) | Vanilla Wine trips `d3dx10_43` / `D3DX10SaveTextureToMemory` before the game reaches a usable baseline. Documented in `experiments/README.md`. |

If you find another app that genuinely requires a patched Wine build,
add a row here with the failure mode and a link to evidence. Do not
silently flip a runner's default — that hides what the patched runtime
is compensating for.

## Diagnostic Checklist When a "Wild" Run Fails

Before reporting a dxmt9 regression from a wild run, confirm:

1. **Wine root is vanilla.** Inspect `result.json:wine_root` — current
   vanilla Heroic build (e.g. `Wine-11.7`), not `Wine-*-DXMT` /
   `Wine-*-VK` / a CrossOver bottle.
2. **A small repro reproduces under the same runtime.** Run a fast
   sanity test (e.g. `conf-d3d9-triangle`) with the same `--wine-root`.
   If it passes, the bug is app-specific, not bridge-wide.
3. **Both x64 and x86 lanes are tested when the failing binary is
   32-bit.** dxmt9 stages both `build-win32-x64-builtin` and
   `build-win32-x86-builtin`; a stale `build-x86_64-builtin` /
   `build-win32-x86-builtin` mismatched against `build/` has bitten us
   before — when in doubt rebuild every staged directory.
4. **`abi-hash handshake OK` is in `<binary>_dxmt9.log`.** If it is
   absent the PE-side `winemetal.dll` either failed `DllMain` or never
   ran the handshake; further bridge errors are downstream of that.
5. **`winemetal.so` install_name deps are `@rpath/winemac.so` /
   `@rpath/ntdll.so`.** Bare-dep `.so` files (`winemac.so` /
   `ntdll.so` without the `@rpath/` prefix) silently break Wine's
   `NtQueryVirtualMemory(info=kMemoryWineLoadUnixLib=1000)` lookup —
   the bridge falls through to the unsupported `info=1002` path and
   `DllMain` rejects `winemetal.dll` with `abi-hash unix-call
   failed status=0xc0000003`. The
   `winemetal_unix_install_name_fixup` `custom_target` in
   `src/winemetal/unix/meson.build` is supposed to rewrite the deps
   to `@rpath/...` post-link, but it only runs when the canonical
   build target is invoked. A direct `ninja src/winemetal/unix/winemetal.so`
   skips the stamp and leaves the .so with bare deps. The audit
   `scripts/check/audit_winemetal_install_names.py`
   (`dxmt9-winemetal-install-name-audit` meson test) checks for this
   and reports the manual `install_name_tool -change` command to
   undo the regression.

## Related

- `agents/rules/environment_variables.rules.md` — `DXMT*` runtime knobs.
- `agents/rules/metal_debugging.rules.md` — capture / counter / signpost
  workflow that assumes a clean Wine baseline.
- `experiments/README.md` — per-app wild-run conventions and the
  catalogue of supported targets.
