# Wild Testing Rules — Wine Runtime Selection

Rules for running dxmt9 against real D3D9 binaries (catalogue experiments,
SFIV, Anno 1404, etc.) in `experiments/`. They cover the host Wine
runtime, not the dxmt9 build itself.

> **Spec:** the mechanics of the manifest, prefix bootstrap, and apps_3rd
> layout are defined in `specs/experiments/runtime/{requirements,design}.md`.
> This rule covers operational guidance only.

## Rule: Default to Vanilla Wine

**All experiment runners MUST default to a pristine, unpatched Wine build.**

| Runtime | Use as default? | Why |
|---------|-----------------|-----|
| `Wine-11.7` (vanilla Heroic) | **Yes** | Reference baseline. Same `d3d9.dll` shim, `wow64` dispatcher, and `ntdll` thunks regardless of who runs the test. Update this row when Heroic publishes a newer minor (`Wine-11.8`, etc.) — the rule is "current vanilla Heroic Wine," not a frozen version. |
| `Wine-*-DXMT` | No (exception below) | Carries DXMT-author patches to `d3d9` / `dxgi` / wow64. Hides dxmt9 bridge regressions and produces inconsistent baselines across machines. |
| `Wine-*-VK` / Proton-style VK builds | No | Substitutes a different `d3d9.dll` and reroutes through Vulkan; the comparison is no longer "dxmt9 vs. Wine builtin." |
| CrossOver Wine | No (unless explicitly testing CrossOver host) | Forks Wine; results don't generalize. |

**Reason — concrete incident (2026-05-10):** SFIV under `Wine-11.6-DXMT`
appeared to flood `dxmt9.log` with `0xc0000005` access violations on
every `factory_*` bridge call, suggesting a 32-bit bridge regression.
Re-running the same 32-bit binary (`d9vk-d3d9-triangle-x86.exe`) under
vanilla `Wine-11.6` produced a clean `[winemetal-abi] info: abi-hash
handshake OK` log and passed. The `Wine-11.6-DXMT` runtime had drifted
from upstream Wine in a way that masked or simulated wow64 dispatch
differently — the dxmt9 bridge was fine, the runtime was the variable.

## How to Apply

When adding or editing a runner script under `scripts/run_apps/`,
`scripts/run_suites/`, or `scripts/tools/`:

```sh
# Good — vanilla Wine baseline. The wine_id resolves through the
# manifest at experiments/wine/manifest.toml; never hardcode paths.
wine_id = "heroic-11.7"          # in CATALOGUE [[app]]
# Or on the command line:
bash scripts/run_apps/run_<name>_experiment.sh --wine-id heroic-11.7

# Avoid — hardcoding a path or pointing at a non-vanilla variant.
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
| `anno-1404-gold` | `Wine-*-DXMT` (any current DXMT build) | Vanilla Wine trips `d3dx10_43` / `D3DX10SaveTextureToMemory` before the game reaches a usable baseline. Documented in `experiments/README.md`. |

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
   sanity test (e.g. `d9vk-d3d9-triangle`) with the same `--wine-root`.
   If it passes, the bug is app-specific, not bridge-wide.
3. **Both x64 and x86 lanes are tested when the failing binary is
   32-bit.** dxmt9 stages both `build-win32-x64-builtin` and
   `build-win32-x86-builtin`; a stale `build-x86_64-builtin` /
   `build-win32-x86-builtin` mismatched against `build/` has bitten us
   before — when in doubt rebuild every staged directory.
4. **`abi-hash handshake OK` is in `<binary>_dxmt9.log`.** If it is
   absent the PE-side `winemetal.dll` either failed `DllMain` or never
   ran the handshake; further bridge errors are downstream of that.

## Related

- `agents/rules/environment_variables.rules.md` — `DXMT*` runtime knobs.
- `agents/rules/metal_debugging.rules.md` — capture / counter / signpost
  workflow that assumes a clean Wine baseline.
- `experiments/README.md` — per-app wild-run conventions and the
  catalogue of supported targets.
