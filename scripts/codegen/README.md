# scripts/codegen

Build-time code generators consumed by Meson `custom_target`s. Outputs are
written into the build tree, not committed.

- `extract_device_c_schema.py` — parses `include/dxmt9/device_c.h` and emits
  the unix-side schema header used by the bridge generator.
- `gen_wine_bridge.py` — generates the `dxmt9_bridge_ops.generated.h`,
  client/server bridge `.cpp`, and Wine unix-call entry table from the
  device_c schema and the winemetal unix schema.

The generated ABI hash is owned by `gen_wine_bridge.py`: it includes bridge
operation declaration ordinals, generated argument-record fields, and the
pointer-width-independent field schema plus layout-affecting preprocessor
context (including packing directives and ABI macro values) of the POD records
in `device_c.h` and `winemetal_thunks.hpp`. This source schema is the
cross-target canonical layout contract; generated native `Args_*` and WoW64
`Args32_*` records retain separate target-local standard-layout and
trivially-copyable static assertions. Do not fold compiler-specific
`sizeof`/`offsetof` values into the shared hash, since PE32 and unix64 must
agree on it; never edit generated build outputs.
