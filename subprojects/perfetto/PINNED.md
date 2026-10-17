# Private Perfetto SDK pin

Source: https://github.com/google/perfetto.git, tag `v49.0`, resolved commit
`33a4fd07897a9a648664926ea27769278a19ff13` (the tag object resolves to this
commit in the shallow checkout used to acquire the SDK).

The checked-in private SDK is the upstream amalgamation from `sdk/perfetto.h`
and `sdk/perfetto.cc`. SHA-256:

| File | SHA-256 |
|---|---|
| `sdk/perfetto.h` | `01f81730839c1d833c4988fc2830ca9c9b0231b0476a4f9f87b5d63f80d77e69` |
| `sdk/perfetto.cc` | `33a93a2a1f02f3442b8384214d5bb466177fbbe75ba6324f09656b140d4efd19` |
| `LICENSE` | `73f3b0eee3d8d064888f928c52211f33bdcc9145925a9e1340d07cd78ed62351` |

Perfetto is Apache-2.0 licensed. The unmodified upstream license is retained
in `LICENSE`. The SDK is private to the unix tracing target; do not expose its
types or symbols through PE or public bridge interfaces.
