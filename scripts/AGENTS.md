# scripts

Helper scripts grouped by purpose. Implementation-side conventions live in
`agents/rules/codebase_conventions.rules.md`; generated bridge sources are
produced from `codegen/` and consumed by Meson `custom_target`s.

| Directory     | Role                                                       | Meson-wired |
|---------------|------------------------------------------------------------|-------------|
| `codegen/`    | Build-time codegen consumed by Meson `custom_target`s      | yes         |
| `check/`      | Validation and audit scripts run as Meson tests            | yes         |
| `build_apps/` | D3D9 sample-app build helpers                              | no          |
| `run_apps/`   | Per-app experiment runners                                 | no          |
| `run_suites/` | Multi-app benchmark and regression suites                  | no          |
| `install/`    | Wine and Heroic prefix setup                               | no          |
| `tools/`      | General-purpose dev tooling (corpus, packaging, cleanup)   | partial     |

`tools/` is marked partial because `shader_corpus_tool.py` is invoked by the
`dxmt9-shader-corpus-gaps` Meson test even though most other entries in
`tools/` are not.

See the per-directory `README.md` for the file inventory and one-line
descriptions of each script.

## Harness conventions

Use the directory boundary to decide how a harness should behave:

| Directory | Harness kind | Result / gate convention |
|---|---|---|
| `scripts/check/` | Deterministic validation and audits | Exit nonzero on failure; direct or indirect Meson coverage required. |
| `scripts/run_apps/` | Single-app experiments | Write `experiments/output/<name>/result.json`; may use screenshots, SSIM, timings, and logs. |
| `scripts/run_suites/` | Multi-app experiment or benchmark aggregation | Keep going across cases when useful, but final exit code must reflect any failed required case. |
| `scripts/tools/` | Developer tooling and shared helpers | Prefer importable Python helpers when logic is reused by tests or suites. |
| `tests/module_boundary/` | Deterministic built-artifact boundary tests | Write schema-versioned JSON with lane, arch, artifact hashes, command, env snapshot, exit code, checks, and fixed failure category. |

Shell harnesses must stay thin: `#!/usr/bin/env bash`, `set -euo pipefail`,
`script_dir`, `repo_root`, array-based command construction, and explicit final
status propagation when continuing after a failed case.

Python harnesses should use `argparse`, `Path`, checked-in default paths relative
to `REPO_ROOT`, and `json.dumps(..., indent=2, sort_keys=True) + "\n"` for
machine-readable result files. If a script can be imported by a Meson test or
another harness, keep side effects behind `main()` and
`if __name__ == "__main__"`.

Do not use experiment pass criteria for deterministic tests. Module-boundary
and conformance smoke tests classify loader, ABI, provider, and public API
failures directly; screenshots, SSIM, frame timing, and wild app behaviour
belong only to experiment or benchmark suites.
