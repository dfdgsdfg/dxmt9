# scripts/codegen

Build-time code generators consumed by Meson `custom_target`s. Outputs are
written into the build tree, not committed.

- `extract_device_c_schema.py` — parses `include/dxmt9/device_c.h` and emits
  the unix-side schema header used by the bridge generator.
- `gen_wine_bridge.py` — generates the `dxmt9_bridge_ops.generated.h`,
  client/server bridge `.cpp`, and Wine unix-call entry table from the
  device_c schema and the winemetal unix schema.
