---
description: Wine runtime, prefix, app-local/builtin, PE/unix provider, and ABI debugging rules for dxmt9
paths:
  - "experiments/**"
  - "scripts/run_apps/**"
  - "scripts/wine/**"
  - "tests/module_boundary/**"
  - "tests/conformance/d3d9/**"
  - "tests/integration/wsi_present/**"
globs: "{experiments,scripts,tests}/**/*"
alwaysApply: false
---

# Wine Debugging Rules

Use this when debugging dxmt9 under Wine: runtime selection, prefix state,
app-local vs builtin loading, PE/unix provider discovery, ABI handshakes, and
Wine-hosted module-boundary or conformance failures.

## Runtime First, dxmt9 Second

Wine runtime drift can masquerade as bridge or renderer failure. Before changing
dxmt9 code, prove which Wine root, prefix, lane, and architecture produced the
evidence.

**Rules:**
- Record `wine_root` / manifest `wine_id`, prefix path, PE arch, lane
  (`app-local`, `builtin`, or provider-side), and `WINEDLLOVERRIDES` in every
  live result.
- Prefer manifest-resolved Wine roots for wild experiments; do not hardcode
  Heroic, CrossOver, or local build paths in runner defaults.
- Treat `Wine-*-DXMT`, VK/Proton-style builds, and CrossOver bottles as
  different hosts unless a spec or app rule explicitly accepts them.
- A failure without an ABI line, provider locator evidence, or staged artifact
  manifest is incomplete evidence, not a renderer conclusion.

## App-Local vs Builtin Is Evidence, Not A Toggle

App-local and builtin lanes answer different questions. App-local proves that
the staged `d3d9.dll` / `winemetal.dll` / `winemetal.so` set can attach without
polluting the Wine tree. Builtin proves Wine-tree installation and lookup.

**Rules:**
- Keep app-local and builtin artifacts separate in manifests and debug results.
- For app-local runs, record `DXMT9_WINEMETAL_SO`, staged file hashes, and
  whether runtime provider fallback was allowed.
- For builtin runs, record the Wine `lib/wine/*-windows` and `*-unix` source
  paths. Do not reuse app-local pass/fail status for builtin acceptance.
- Timeout after a successful PE probe is a wrapper/runtime lifecycle issue unless
  logs show an earlier module-boundary failure.

## Provider Locator And ABI Handshake

The PE bridge and unix provider must be built as a matching set. An ABI mismatch
or wrong provider path invalidates downstream D3D9/Metal interpretation.

**Rules:**
- Search logs for provider candidate selection before investigating renderer
  state.
- Search logs for `abi-hash handshake OK` or the mismatch diagnostic before
  treating a PE/unix call failure as a bug in the called function.
- If `winemetal.so` cannot load, classify it as provider locator / unix module
  load evidence (`wine-provider-locator`), not as public D3D9 conformance.
- If the ABI hash mismatches, classify it as `wine-abi-handshake` and rebuild
  every PE/unix artifact in lockstep.

## Result Paths

Use the machine-readable result paths first, ad-hoc log summaries second.

| Lane | Preferred runner | Required artifact |
|---|---|---|
| Module-boundary | `tests/module_boundary/run_module_boundary.py` | `dxmt9.debug.result.v1` plus stage/provider/ABI sidecars |
| WSI smoke | `tests/integration/wsi_present/run_wsi_present.py` | `diagnostics.wsi` with window identity and capture classification |
| Wild app | `scripts/run_apps/run_experiment.py` | `debug_result.json`, capture artifacts, Wine/prefix fields |
| D3D9 conformance | `tests/conformance/d3d9/MANIFEST.toml` evidence | current lane/arch result per case |

## Related

- `agents/rules/test_wild.rules.md` - wild app runtime selection.
- `agents/rules/environment_variables.rules.md` - `DXMT*`, `DXMT9*`, and Wine
  harness environment variables.
- `specs/tests/design.md` - module-boundary, conformance, WSI, and debug-result
  evidence contracts.
