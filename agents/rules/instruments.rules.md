---
description: Instruments measurement semantics, Processor Trace support, symbolication and interpretation limits
paths:
  - "agents/skills/cputrace/**/*"
  - "agents/skills/gputrace/**/*"
  - "scripts/tools/*xctrace*"
  - "docs/perfomance/**/*.md"
globs: "agents/skills/cputrace/**/*"
alwaysApply: false
---

# Instruments evidence

These rules own instrument selection and interpretation. Use
[xctrace rules](xctrace.rules.md) for CLI collection/export and
[Metal rules](metal_debugging.rules.md) for Xcode's separate Metal frame debugger.
Execution entrypoints are [cputrace](../skills/cputrace/SKILL.md) and
[gputrace](../skills/gputrace/SKILL.md).

## Measurement kinds

| Instrument/evidence | Supports | Does not establish by itself |
|---|---|---|
| Time Profiler | Sampled native stacks/weights in the observed window | Every invocation, exact call count, exclusive guest CPU or scheduler residency |
| Processor Trace | Reconstructed function flow, calls and reported instructions/cycles on supported targets | Unqualified guest-PC recovery, complete symbols or GPU cost |
| Thread State/System Trace | Recorded state transitions and scheduling evidence | Useful CPU work inferred solely from elapsed waits |
| Metal System Trace | Available GPU/driver intervals, process activity and signposts | Every shader invocation, occupancy or display presentation without the corresponding source |

Retain source units, self/inclusive definitions, process/thread scope and window.
Overlapping intervals must not be summed into elapsed wall time without an
explicit aggregation model. Missing rows, symbols or counter support stay unknown.

## Processor Trace capability

According to [Apple's Processor Trace documentation](https://developer.apple.com/documentation/xcode/analyzing-cpu-usage-with-processor-trace),
checked 2026-10-18:

- Mac recording requires M4 or later and macOS 15.4 or later. Device recording
  requires iPhone 16/16 Pro or later with iOS 18.4+, or M4+ iPad Pro with iPadOS 18.4+.
- Analysis requires Instruments 16.3 or later. A Mac that cannot record can
  still analyze a saved supported trace. Do not reject offline analysis on M1.
- Hardware trace captures branch/execution information; it is distinct from
  periodic stack sampling. Match debug symbols to the exact distributed build.
  Some generated branch-island addresses remain unnamed even with symbols.
- Apple's typically less-than-1% overhead statement concerns processor tracing;
  other Instruments activity can increase total overhead. It is not dxmt9/Wine
  observer-control evidence. Validate the actual workload before making claims.
- GUI support does not guarantee a particular CLI template, export schema or
  parser. Discover capabilities on the installed version. No live Processor
  Trace Wine/Rosetta guest-attribution qualification is claimed by these rules.

## Symbols, identity and analysis state

- Bind symbols/dSYMs and module maps to the recorded images, architecture and
  load generation. REA static VAs/RVAs need a proven loaded-base mapping.
- Existing Wine/Rosetta Time Profiler observations contain opaque translated PE
  PCs; a guest module map alone cannot recover them. Native dyld-loaded unix
  frames may still symbolize. See [PE observations](../skills/cputrace/references/wine-pe.md).
  Do not transfer that result into either support or rejection of Processor
  Trace without collector-specific evidence.
- Preserve the selected time range, thread/process filter and self/inclusive
  aggregation when comparing views. Charge-to-callers, prune and flatten alter
  attribution or visibility; record these settings alongside exported results.
- Keep native CPU, sample population, elapsed waits, GPU execution and Xcode
  replay timing separate. Matched observer controls and output correctness are
  required for performance claims; a populated UI is not qualification.
- A shared run ID is provenance, not clock calibration. Correlating Instruments
  with Perfetto requires proven endpoints/identity and clock mapping under
  [Perfetto rules](perfetto.rules.md).
