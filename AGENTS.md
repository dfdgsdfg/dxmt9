# dxmt9 Agent Init

This file is the lightweight repository entry point for agents. Keep it short:
durable project rules live in `agents/rules/*.rules.md`, and subsystem behavior
belongs in `specs/`.

## Reference Workspaces

Use these adjacent checkouts as references when the task calls for comparison,
but do not treat them as writable unless the user explicitly asks.

| Path | Role |
|---|---|
| `~/workspaces/dxmt` | Upstream / sibling DXMT reference |
| `~/workspaces/d9vk` | D9VK, DXVK's legacy reference |
| `~/workspaces/dxvk` | DXVK reference |
| `~/workspaces/wine` | Wine source reference |
| `~/workspaces/wine-build` | Wine build tree |
| `~/workspaces/wine-build-wow64` | Wine wow64 build tree |

## Rule Inventory

Read the matching rule before making changes in that area. This table is only an
inventory; the files below are the source of truth.

| Rule file | Use when |
|---|---|
| `agents/rules/codebase_conventions.rules.md` | Editing `include/`, `src/`, `tests/`, Meson files, bridge code, Wine/PE/unix boundaries, Metal ownership, or deterministic tests. |
| `agents/rules/documentation.rules.md` | Editing `AGENTS.md`, nested `AGENTS.md` files, agent docs, or general repository documentation. |
| `agents/rules/documentation_memory.rules.md` | Adding or updating `agents/rules/*.rules.md` as project memory. |
| `agents/rules/documentation_spec.rules.md` | Editing `specs/**/*.md`, requirement IDs, design docs, gap tracking, verification mapping, or local implementation plans. |
| `agents/rules/environment_variables.rules.md` | Looking up or documenting `DXMT*`, `DXMT9*`, Wine experiment, or Apple/Metal debug environment variables. |
| `agents/rules/experiments_apps_3dmark05.rules.md` | Checking or updating 3DMark05 experiment status, launcher constraints, output evidence, or Wine/vkd3d-shader comparison notes. |
| `agents/rules/experiments_apps_street_fighter_iv_benchmark.rules.md` | Checking or updating Street Fighter IV Benchmark setup, binary discovery, host lanes, D3DX9 workaround notes, or oracle capture status. |
| `agents/rules/metal_debugging.rules.md` | Debugging Metal captures, Xcode `.gputrace`, Instruments, validation layer, GPU counters, signposts, labels, or command-buffer failures. |
| `agents/rules/test_wild.rules.md` | Running or editing real-app Wine experiments under `experiments/`, choosing Wine runtimes, diagnosing wild-run failures, or touching app runners. |

## Documentation Routing

Use this routing before writing durable notes:

| Information type | Put it in |
|---|---|
| Local folder/app status | The nearest `AGENTS.md` |
| Reusable pitfall or convention | `agents/rules/*.rules.md` |
| Durable behavior, architecture, compatibility, or verification contract | `specs/` |
| Missing implementation or missing evidence | `specs/gap.md` |
| Short-lived execution staging | ignored `plan.md` / scratchpad |
