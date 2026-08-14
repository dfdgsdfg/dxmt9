# tests/native/bridge

Wine PE/unix canonical wire-format and bridge specs. All run without a live Wine
runtime using pointer-free POD construction and deterministic dispatch.

| Spec | Covers (R-* anchor) |
|------|---------------------|
| `chunk_record_layout_spec.cpp` | Canonical POD layout / size / alignment and schema completeness (`R-BACK-2.54`) |
| `chunk_record_registry_spec.cpp` | Stable object identity, generation reuse, and canonical-only negotiation (`R-BACK-2.54`) |
| `chunk_record_validation_spec.cpp` | Canonical bounds, sections/handles, and side-effect-free malformed rejection (`R-BACK-2.54`–`R-BACK-2.55`) |
| `render_tape_spec.cpp` | Frame/sequence v2 closure, reducer invariants, and the 45-case production-bound capture/replay refinement checker for initial bytes, between-Present mutation, generation reuse, and stale rejection (`R-HARN-REPLAY-7.2`–`7.15`, `R-VERIF-6.6`) |
| `render_tape_projection_spec.cpp` | Pure one-command-event draw-range projection, canonical locator/object/blob conservation, and fail-closed frame/snapshot/closure selection (`R-HARN-REPLAY-7.16`/`7.17`) |
| `pe_chunk_record_value_spec.cpp` | Direct canonical producers, PE state staging, retention, and seal/preflight (`R-BACK-2.52`, `R-BACK-2.55`) |
| `pe_full_snapshot_equivalence_spec.cpp` | Delta/full-snapshot semantic equivalence using typed canonical barrier payloads (`R-BACK-2.55`) |
| `bridge_ops_spec.cpp` | Generated bridge opcode table parity + DOD chunk op placement (`R-BACK-2.10`, `R-VERIF-7.3`) |
| `bridge_marshalling_value_spec.cpp` | Native/WoW64 argument blocks and canonical blob marshalling (`R-VERIF-7.3`) |
| `wmt_setbytes_dispatch_spec.cpp` | `setVertexBytes` / `setFragmentBytes` discriminator via fake unix-call thunk (`R-BACK-12.3`) |

Retired pointer-bearing record/blob fixtures are not part of this suite.
Canonical specs keep their focused typed fixture builders local to the owning spec.

## Running

```sh
meson test -C build-x86_64-builtin dxmt9-chunk-record-validation-spec
```

## Conventions

- Spec files: `<area>_spec.cpp` (snake_case).
- Test target name: `dxmt9-<name>` (kebab — `bridge` prefix omitted because
  test names already begin with `chunk-record-` / `bridge-` / `wmt-`).
- New chunk-record specs construct typed canonical headers, sections, handles, and
  payloads; do not reintroduce retired envelopes as comparison fixtures.
- Tests must run without Metal; these specs exercise pure POD wire behavior.
