# Gap Tracker

This document tracks the remaining delta between the current implementation
and the spec set.

## Completed

- Encoder merging now uses a Bloom-filter hazard check to split encoders on
  possible read/write conflicts.
- D3DBC shader translation now decodes SM2/SM3 bytecode into instructions and
  lowers them into Metal source.
- The backend carries `RingSafety`, `EncodeSafety`, `WineCommit`, and
  `NoUseAfterFree` debug annotations.
- The shader thunk path is wired through the opaque `winemetal` bridge.

## Verification

- TLC is currently run locally through `bash scripts/verify_tla.sh` and the
  Meson `dxmt9-verify-tla` target.
- Remote CI is not configured yet; when it is added, it should call the same
  local script.

## Next Priority

| Priority | Work | Spec anchor |
|---|---|---|
| 1 | Keep TLC local-only for now; remote CI hookup is deferred | R-VERIF-6.1 |
