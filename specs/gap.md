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
- `IDirect3D9Ex` and `IDirect3DDevice9Ex` are implemented, including adapter
  Ex queries, `CheckDeviceState()`, `ResetEx()`, `PresentEx()`, latency control,
  vblank wait, residency, GPU priority, and the Ex surface helpers.

## Verification

- TLC is currently run locally through `bash scripts/verify_tla.sh` and the
  Meson `dxmt9-verify-tla` target.
- Remote CI is not configured yet; when it is added, it should call the same
  local script.

## Next Priority

No core Ex gaps remain. Remaining work is backend-specific and tracked in
`specs/backend`.
