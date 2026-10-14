---
description: REA setup and evidence workflow for understanding target D3D9 applications
paths:
  - "experiments/**/*"
  - "docs/perfomance/**/*.md"
globs: "experiments/**/*"
alwaysApply: false
---

# REA Target Application Analysis

Use external [REA](https://github.com/morluto/rea) to investigate how a target
application uses D3D9: capability checks, resource creation, state changes,
query polling, presentation, and scene selection. Start with a concrete
behavioral question and follow only the relevant code paths.

## Global Installation

Install the official npm package `rea-agents` through pnpm; its CLI is `rea`.
The verified baseline is 6.4.0. Keep it outside dxmt9's build dependencies:

```sh
pnpm add -g rea-agents@6.4.0
command -v rea
rea --version
rea --help
```

REA 6.4.0 supports Node.js 22.19+ within 22.x, 24.11+ within 24.x, or 26+.
If the command is missing, check `pnpm bin -g` and the shell's `PATH`.
For upgrades use `pnpm add -g rea-agents@latest`, then recheck command help
and record the new version with the analysis. Do not mix global package
managers via `rea update`, or add a package.json/node_modules to dxmt9.

The CLI is sufficient for repository work; MCP registration is separate.
If MCP integration is wanted, follow upstream
[installation](https://github.com/morluto/rea/blob/main/docs/installation.md).
`rea setup` edits agent configuration and installs workflow instructions;
global CLI installation alone does not perform those steps.

## Native Provider Preflight

Native PE analysis requires a configured Hopper, Ghidra, or IDA provider.
Installing REA alone does not install a decompiler. Discover availability:

```sh
rea providers --json | jq '{providers: [.analysis_provider_candidates[] |
  {provider: .provider, availability: .availability}]}'
rea doctor --provider ghidra --json
```

For headless analysis, configure an existing Ghidra installation:

```sh
export GHIDRA_INSTALL_DIR=/absolute/path/to/ghidra_12.1.4_PUBLIC
export JAVA_HOME=/absolute/path/to/compatible-jdk
rea doctor --provider ghidra --json
```

REA 6.4.0 accepts Ghidra 12.1.x with a full JDK satisfying that release's
requirements (12.1 requires JDK 21+). Ghidra's native decompiler and JDK must
match the host architecture, even when analyzing a 32-bit x86 PE on Apple
Silicon. See upstream installation instructions for native component setup.

For existing Hopper, use `HOPPER_LAUNCHER_PATH` if discovery fails and run
`rea doctor --provider hopper --json`; complete its first-run UI before
unattended analysis. IDA uses `REA_IDA_MCP_CONFIG` as documented upstream.
Pass `--provider` explicitly to make evidence reproducible. A failed preflight
is an environment limitation, not evidence about the target's behavior.

### Homebrew Installation on This Host

Ghidra 12.1.4 is installed at `/opt/homebrew/opt/ghidra/libexec`. REA 6.4.0
does not discover this installation through Homebrew's `ghidraRun` wrapper;
set the installation root explicitly in the shell running REA:

```sh
export GHIDRA_INSTALL_DIR=/opt/homebrew/opt/ghidra/libexec
rea doctor --provider ghidra --json
```

This passes the scoped provider check with the host's arm64 decompiler and
Temurin JDK 25.0.4. Recheck after upgrades. The local
`~/workspaces/ghidra/eotfs/` directory contains an existing Ghidra project
(`.gpr`/`.rep`), not the installation root. REA's Ghidra provider creates a
temporary project from the original executable; it does not reuse that saved
project or its annotations.

## Select and Identify the Target

Read the app's `binary` entry in
[`experiments/CATALOGUE.toml`](../../experiments/CATALOGUE.toml). Select the
actual EXE or owning DLL, not its shell launcher, Wine loader, or dxmt9's DLL.
Resolve an absolute path and retain app version, architecture, and SHA-256.
For example, from the repository root with 3DMark05 installed:

```sh
rea_target="$PWD/experiments/prefixs/app-d3d9-3dmark05/drive_c/Program Files (x86)/Futuremark/3DMark05/3DMark05.exe"
rea_output="$PWD/experiments/output/rea/3dmark05-$(date -u +%Y%m%dT%H%M%SZ)"
mkdir -p "$rea_output"
file "$rea_target" > "$rea_output/target-format.txt"
shasum -a 256 "$rea_target" > "$rea_output/target-sha256.txt"
rea --version > "$rea_output/rea-version.txt"
rea doctor --provider ghidra --json > "$rea_output/provider.json"
```

Check command exit status and provider readiness before continuing. Keep raw
analysis under ignored `experiments/output/rea/` or the owning experiment's
output directory. Preserve the original executable bytes.

## Bounded Investigation

After provider preflight succeeds, save the overview and search evidence:

```sh
rea_snapshot="$rea_output/snapshot.json"
rea analyze "$rea_target" --provider ghidra --snapshot "$rea_snapshot" --json > "$rea_output/overview.json"
rea search "$rea_target" Direct3DCreate9 --kind procedures --provider ghidra --snapshot "$rea_snapshot" --json > "$rea_output/d3d9-search.json"
```

Use discovered symbols or addresses for follow-up commands. For example,
after setting `rea_address` to an actual address returned by analysis:

```sh
rea xrefs "$rea_target" "$rea_address" --provider ghidra --snapshot "$rea_snapshot" --json > "$rea_output/xrefs.json"
rea function "$rea_target" "$rea_address" --provider ghidra --snapshot "$rea_snapshot" --json > "$rea_output/function.json"
rea instructions "$rea_target" "$rea_address" --provider ghidra --snapshot "$rea_snapshot" --json > "$rea_output/instructions.json"
```

`rea decompile` returns pseudocode; `rea trace` follows a literal feature
query through analyzed references. Neither is a runtime execution trace.
Each CLI invocation is a separate session. Snapshots reuse eligible exact
queries; a new query may still start the provider and repeat binary analysis.
Keep calls sequential when sharing a snapshot. Use a persistent MCP session
for extensive investigations. Consult `rea <command> --help` for the installed
version rather than assuming that upstream main matches the package.

## D3D9 Interpretation and Validation

- D3D9 methods are usually indirect COM vtable calls. Missing method names in
  searches do not prove absence of `Present`, `GetData`, or a draw call. Resolve
  interface identity, slot, pointer width, calling convention, and arguments
  against the relevant D3D9 declaration before naming an indirect call.
- Record module identity, image base, RVA, and the address convention returned
  by the provider. Do not equate a static VA or file offset with an ASLR-adjusted
  address observed in Wine; establish the loaded module base first.
- Check consequential pseudocode against instructions, callers, constants,
  and HRESULT branches. Recovered types, signedness, and indirect targets can
  be incomplete. Keep observations, inferences, and unresolved paths distinct.
- Validate predicted behavior with the existing experiment runner and traces.
  Follow [test_wild.rules.md](test_wild.rules.md) and the relevant
  [3DMark](experiment_3dmark.rules.md) or [SFIV](experiment_sfiv.rules.md) rules.
  Static reachability does not prove that a branch ran, how often it ran, or
  its CPU/GPU cost; REA process capture does not replace the Wine harness.
- Turn behavioral findings into focused repros or conformance assertions.
  Rendering optimizations still require
  [rendering_correctness.rules.md](rendering_correctness.rules.md) evidence.
  Preserve the [license/reference policy](codebase_conventions.rules.md):
  target pseudocode is evidence, not MIT-compatible implementation source.

## Evidence Handoff

Retain the question, exact commands, REA/provider versions and settings,
target SHA-256/architecture, function addresses/RVAs, raw output paths,
limitations, and the runtime run ID supporting each conclusion. Save full
JSON to files and inspect bounded excerpts instead of flooding agent context.
Preserve provider failures and `cleanup_incomplete` reports; resolve retained
resources before retrying rather than launching duplicate analysis sessions.

Place durable performance conclusions in the owning `docs/perfomance/` domain
with concrete artifact links; compatibility gaps belong in the owning specs.
Follow [documentation.rules.md](documentation.rules.md). Do not promote a
static hypothesis to a measured result or commit target executables, provider
databases, or bulk decompiler output into source documentation.

External reference: [CLI and Evidence guide](https://github.com/morluto/rea/blob/main/docs/cli.md).
