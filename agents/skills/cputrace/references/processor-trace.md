# Processor Trace function-flow analysis

Use this route for short-call frequency, execution order and instruction/cycle
questions that sampled stacks cannot answer. Read the hardware, symbol and
interpretation constraints in [Instruments rules](../../../rules/trace_instruments.rules.md).

## Select capture or offline analysis

For an existing saved trace, open it in compatible Instruments and inspect its
recorded device, process, symbols and window. A recording-incapable Mac can still
analyze it; do not start a new workload just to inspect the file.

For new Mac recordings, verify M4+ hardware, macOS 15.4+ and Instruments 16.3+.
Retain the actual device/OS/tool versions. Use read-only preflight:

```sh
system_profiler SPHardwareDataType
sw_vers
xcrun xctrace version
xcrun xctrace list templates
xcrun xctrace list instruments
xcrun xctrace help record
```

An M1/M2/M3 cannot record this route under Apple's stated support. If another
collector can answer the question, explicitly select it as a different evidence
source; do not call sampled stacks Processor Trace.

## Capture and inspect

1. In Xcode, use Product > Profile and select Processor Trace for a supported
   native target. For an existing process, select the actual target through
   Instruments' available target controls. Preserve Wine catalogue supervision
   and verify the real child; guest attribution is not qualified by this guide.
2. Record a bounded interval containing the activity, then stop and save the
   `.trace` under `traces/<app-runid>/`. Wait for processing to complete.
3. Supply exact-build symbols through File > Symbols > Add Symbols > Add Folder
   of dSYMs when needed. Record remaining raw addresses; branch islands may
   remain unnamed even with the correct dSYMs.
4. Select the relevant process/thread and inspection range. Use Call Tree for
   inclusive/self relationships, Summary: Function Calls for aggregate counts
   and reported active duration/instructions, and Function Calls for individual
   invocations. Inspect the Processor Trace and Thread State tracks together.
5. Record IPC, instructions and cycles with the selected scope and units. Save
   available exports and the unmodified trace. For UI-only views, retain the
   view settings and supporting screenshots without claiming a machine-checked
   export. Use available computer-use tooling for native UI operation.

If installed xctrace exposes a usable Processor Trace template/instrument,
apply the [bounded CLI recording/export workflow](instruments.md) with that
discovered configuration. Check installed help for recording options. Do not
invent a schema or pass Processor Trace exports to the Time Profiler summarizer.
When CLI export cannot expose the required view, use Instruments and report
that limitation.

## Qualification limits

Check actual events, symbol coverage, recording diagnostics and the selected
window before claiming call completeness. Treat Wine/Rosetta guest function
reconstruction as a separate unqualified capability: hardware support alone
does not establish the mapping to the original PE binary or REA RVAs.

Before comparing traces, preserve or reset Charge to Callers, Prune and Flatten
settings and use the same range selection policy. Distinguish active CPU work
from wall duration and thread waits. Apple's tracing-overhead estimate does not
replace matched controls for this application.

Source: [Analyzing CPU usage with the Processor Trace instrument](https://developer.apple.com/documentation/xcode/analyzing-cpu-usage-with-processor-trace),
checked 2026-10-18. This workflow records support requirements, not a new live
Processor Trace acceptance result.
