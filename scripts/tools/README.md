# scripts/tools

General-purpose developer tooling that does not belong to a single suite or
build. `shader_corpus_tool.py` is also imported by the Meson tests under
`tests/meson.build` for shader-corpus listing and gap reporting.

- `shader_corpus_tool.py` — manage the shader corpus: list passing files,
  drift against upstream, gap reporting (used by the Meson test wiring).
- `package_app_local.py` — assemble a self-contained `dxmt9-app-local`
  distribution from PE and unix builds.
- `cleanup_dxmt9_temp_prefixes.py` — list/prune temporary Wine prefixes
  created by experiment runs.
- `sync_corpus.sh` — sync a vkd3d upstream corpus into this tree via
  `shader_corpus_tool.py sync`.
- `run_dx9_fast_sanity_suite.sh` — fast-sanity suite runner that drives the
  fast-sanity bundle through `run_experiment.py`.
