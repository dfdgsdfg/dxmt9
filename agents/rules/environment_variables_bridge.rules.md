# dxmt9 Environment Variables — PE bridge / recorder

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (PE-side chunk recorder diagnostics). A flag is "set" when its value is a
non-empty string that is not `0`, unless documented otherwise. See the index
for global notes.

## PE bridge / recorder

| Var | Purpose | Default |
|---|---|---|
| `DXMT9_PE_RECORDER_STATS` | Emit PE recorder aggregate stats | `0` |
| `DXMT9_PE_RECORDER_CHUNK_LOG` | Emit PE recorder chunk boundary logs | `0` |
| `DXMT9_PE_DRAW_FULL_SNAPSHOT` | Force every draw packet to ride a full PE state snapshot instead of the default delta. Applied in `src/d3d9/d3d9_pe_device.cpp::buildDrawPrimitivePacket` (lines ~497-583 under `if (dxmt9PeFullSnapshotEnabled())`). Trade-off: wire size grows ~10x (typical packet ~100 B → ~1 KB), the importer's run-coalescer (`packetHasNoStateDelta`) sees no empty packets so every draw breaks the coalesced run, but each packet is replayable in isolation — debug / stress / out-of-order-replay only. Equivalence with the default delta path is regression-guarded by `tests/native/bridge/pe_full_snapshot_equivalence_spec.cpp`. | `0` |
| `DXMT9_PE_CHUNK_MAX_RECORDS` | Override max pending PE chunk records before commit | `64` |
| `DXMT9_PE_CHUNK_MAX_BYTES` | Override max pending PE chunk bytes before commit | `262144` |
