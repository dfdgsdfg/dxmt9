# tests/native/bridge

Wine PE/unix wire-format and replay specs. All exercise the chunk
record + bridge ABI without a live Wine runtime — POD construction +
deterministic dispatch only.

| Spec | Covers (R-* anchor) |
|------|---------------------|
| `chunk_record_spec.cpp` | POD layout / size / alignment of `CommandRecord*` types (`R-ARCH-3.*`) |
| `chunk_record_validation_spec.cpp` | Wire chunk parse, bounds, schema status, handle extraction (`R-CORE-11.*`) |
| `chunk_record_replay_spec.cpp` | Record categorization + draw-run scan + dispatch by `ImportedRecordReplayCategory` (`R-CORE-11.*`) |
| `chunk_record_hazard_spec.cpp` | Hazard FSM + ordering decisions + resource hazard derivation (`R-BACK-2.14-2.27`) |
| `chunk_record_import_spec.cpp` | End-to-end import + truncated tail + multi-record iteration + run-param conversion (`R-VERIF-7.1`) |
| `bridge_ops_spec.cpp` | Generated bridge opcode table parity + DOD chunk op placement (`R-BACK-2.10`, `R-VERIF-7.3`) |
| `wmt_setbytes_dispatch_spec.cpp` | `setVertexBytes` / `setFragmentBytes` discriminator via fake unix-call thunk (`R-BACK-12.3`) |

Shared fixture: `chunk_record_import_spec_fixtures.hpp` (harness, record
builders, byte helpers, hazard fixtures).

## Running

```sh
meson test -C build-x86_64-builtin dxmt9-chunk-record-import-spec
```

## Conventions

- Spec files: `<area>_spec.cpp` (snake_case).
- Test target name: `dxmt9-<name>` (kebab — `bridge` prefix omitted because
  test names already begin with `chunk-record-` / `bridge-` / `wmt-`).
- New chunk-record specs include `chunk_record_import_spec_fixtures.hpp` and
  follow the existing `RecordingBackend` / `makeXxxRecord` helper API.
- Tests must run without Metal — the bridge layer is PE/unix-only and these
  specs exercise pure POD wire validation.
