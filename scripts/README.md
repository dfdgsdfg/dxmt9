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
