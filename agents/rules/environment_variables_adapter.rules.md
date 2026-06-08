# dxmt9 Environment Variables — Adapter spoofing / Compatibility

Part of the [`environment_variables.rules.md`](environment_variables.rules.md)
index (D3D9 driver ID overrides and opt-out / opt-in flags for known-issue
surfaces). A flag is "set" when its value is a non-empty string that is not `0`,
unless documented otherwise. See the index for global notes.

## Adapter spoofing

| Var | Purpose | Default |
|---|---|---|
| `DXMT_ADAPTER_NAME` | Override `Description` | system |
| `DXMT_ADAPTER_VENDOR_ID` | Override vendor-id (numeric) | system |
| `DXMT_ADAPTER_DEVICE_ID` | Override device-id (numeric) | system |
| `DXMT_ADAPTER_DRIVER` | Override driver string | system |

## Compatibility

| Var | Purpose | Default |
|---|---|---|
| `DXMT_COMPAT_HUD` | Enable Compat HUD overlay | `0` |
