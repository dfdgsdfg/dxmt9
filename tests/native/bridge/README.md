# tests/native/bridge

Wine PE/unix V2 wire-format and bridge specs. All run without a live Wine
runtime using pointer-free POD construction and deterministic dispatch.

| Spec | Covers (R-* anchor) |
|------|---------------------|
| `chunk_record_v2_layout_spec.cpp` | V2 POD layout / size / alignment and schema completeness (`R-BACK-2.54`) |
| `chunk_record_v2_registry_spec.cpp` | Stable object identity, generation reuse, and V2-only negotiation (`R-BACK-2.54`) |
| `chunk_record_v2_validation_spec.cpp` | V2 bounds, canonical sections/handles, and side-effect-free malformed rejection (`R-BACK-2.54`–`R-BACK-2.55`) |
| `pe_chunk_record_v2_value_spec.cpp` | Direct V2 producers, PE state staging, retention, and seal/preflight (`R-BACK-2.52`, `R-BACK-2.55`) |
| `pe_full_snapshot_equivalence_spec.cpp` | Delta/full-snapshot semantic equivalence using typed V2 barrier payloads (`R-BACK-2.55`) |
| `bridge_ops_spec.cpp` | Generated bridge opcode table parity + DOD chunk op placement (`R-BACK-2.10`, `R-VERIF-7.3`) |
| `bridge_marshalling_value_spec.cpp` | Native/WoW64 argument blocks and V2 blob marshalling (`R-VERIF-7.3`) |
| `wmt_setbytes_dispatch_spec.cpp` | `setVertexBytes` / `setFragmentBytes` discriminator via fake unix-call thunk (`R-BACK-12.3`) |

Retired V1 record/blob fixtures are not part of this suite. V2 specs keep
their focused typed fixture builders local to the owning spec.

## Running

```sh
meson test -C build-x86_64-builtin dxmt9-chunk-record-v2-validation-spec
```

## Conventions

- Spec files: `<area>_spec.cpp` (snake_case).
- Test target name: `dxmt9-<name>` (kebab — `bridge` prefix omitted because
  test names already begin with `chunk-record-` / `bridge-` / `wmt-`).
- New chunk-record specs construct typed V2 headers, sections, handles, and
  payloads; do not reintroduce V1 envelopes as comparison fixtures.
- Tests must run without Metal; these specs exercise pure POD wire behavior.
