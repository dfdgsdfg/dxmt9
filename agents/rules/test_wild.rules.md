# Wild Testing Rules — Wine Runtime Selection

Rules for running dxmt9 against real D3D9 binaries (catalogue experiments,
SFIV, 3DMark05, and 3DMark06) in `experiments/`. They cover the host Wine
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
| CodeWeavers CrossOver product (licensed) | **No** | Audited 2026-05-11: four independent blockers — Perl `bin/wine` wrapper (bottle-context required, also bypassable via `cxbottle` CLI but does not fix the next three), wow64 missing `MemoryWineLoadUnixLibByName` (class 1002) and **bottle context does not bypass it**, `ntdll.so` hardcodes `/opt/cxoffice/lib/wine` with no working `WINEDLLDIR` override, and `CrossOver.app/lib/wine/` is SIP/codesign-protected so `winemetal_dxmt9.so` cannot be staged. See `specs/winemetal/requirements.md` §6.2. |
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
and it is the same for GT1/GT2/GT3, SFIV, and everything else:

```sh
python3 scripts/run_apps/run_experiment.py run <app-id>
```

Per-app shell wrappers under `scripts/run_apps/` are **legacy — do not add
new ones.** Three were removed on 2026-07-29; two of them show why (the third
belonged to an app that has since left the catalogue, and its lesson — a
hardcoded Wine root overriding the entry's `wine_id` — is the anti-pattern
spelled out under [How to Apply](#how-to-apply) below):

| Removed | Why |
|---|---|
| `run_app-d3d9-sfiv-benchmark_experiment.sh` | Forwarded every argument unchanged. It added nothing except a second name for the same command, and its README description had drifted to describe behaviour it no longer had. |
| `run_suites/run_sfiv_benchmark_crossover_oracle.sh` | Passed `--host crossover`, which `run_experiment.py` does not accept, so it had been failing at argument parsing. CrossOver is a rejected runtime (table above). |

The cost of a second invocation path is not the wrapper; it is that the two
paths drift. SFIV's wrapper did not set `DXMT_EXPERIMENT_PROFILE`, so it
silently measured the `debug` profile — validation layer on, debug logging —
and produced `11.3` fps against a real `43.02`. That was investigated
as a 4x renderer regression before the profile was found. The profile now
defaults to `perf` and is recorded in the run output, but the general lesson
is the rule above: one path.

**Those two figures are different metrics, so `4x` is the wrong size** (noted
2026-07-31 while re-checking the same trap in
`docs/perfomance/shader-codegen/shader-codegen-defselect.03.md`). `11.3` is a
median of the steady frame body; `43.02` is `sampled_avg_fps`, an average that
includes the hitch tail. Matched against the same SFIV `perf`-profile run, debug
costs **`3.2x` on the average** (`13.5` vs `43.0`) and **`5.3x` on the median**
(`11.3` vs `59.7`). Debug is more expensive than the old line claimed, not less.
The trap it warns about recurred in that leaf two months later, so also treat a
directory named `perf` as no evidence of the profile: check
`result.json:profile`, or for runs older than 2026-07-29, `dxmt9.log` size —
debug is two orders of magnitude larger.

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

**There are currently none.** Every catalogue app runs on a Wine root
accepted by the table above. The one entry that had a standing exception —
a commercial title that needed a `-DXMT` build because vanilla Wine tripped
its `d3dx10_43` path — was removed from `experiments/CATALOGUE.toml` on
2026-07-29 along with the exception itself; it was never re-run after the
manifest landed, so the exception had no live evidence behind it.

If you find an app that genuinely requires a patched Wine build, add a
table here (App / Required runtime / Reason) with the failure mode and a
link to evidence, and set the app's `wine_id` to the patched manifest entry
so the runtime warning in `specs/experiments/runtime/requirements.md`
R-RT-6.3 is a pre-approved exception rather than a surprise. Do not
silently flip a runner's default — that hides what the patched runtime is
compensating for.

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
   absent the PE-side `winemetal_dxmt9.dll` either failed `DllMain` or never
   ran the handshake; further bridge errors are downstream of that.
5. **`winemetal_dxmt9.so` install_name deps are `@rpath/winemac.so` /
   `@rpath/ntdll.so`.** Bare-dep `.so` files (`winemac.so` /
   `ntdll.so` without the `@rpath/` prefix) silently break Wine's
   `NtQueryVirtualMemory(info=kMemoryWineLoadUnixLib=1000)` lookup —
   the bridge falls through to the unsupported `info=1002` path and
   `DllMain` rejects `winemetal_dxmt9.dll` with `abi-hash unix-call
   failed status=0xc0000003`. The
   `winemetal_unix_install_name_fixup` `custom_target` in
   `src/winemetal/unix/meson.build` is supposed to rewrite the deps
   to `@rpath/...` post-link, but it only runs when the canonical
   build target is invoked. A direct `ninja src/winemetal/unix/winemetal_dxmt9.so`
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
