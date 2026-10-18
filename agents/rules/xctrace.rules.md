---
description: xctrace collection, target identity, bounded lifecycle and export constraints
paths:
  - "scripts/tools/*xctrace*"
  - "scripts/tools/*system_trace*"
  - "agents/skills/cputrace/**/*"
  - "agents/skills/gputrace/**/*"
globs: "scripts/tools/*xctrace*"
alwaysApply: false
---

# xctrace collection and export

`xcrun xctrace` is the Instruments CLI. These rules own transport and lifecycle;
[Instruments rules](instruments.rules.md) own measurement meaning and device
support. Workflows live in [cputrace](../skills/cputrace/SKILL.md) and
[gputrace](../skills/gputrace/SKILL.md).

## Preflight and target identity

- Check the selected full Xcode installation, `xctrace version`, available
  templates/instruments and installed command help. Command Line Tools alone
  are not sufficient for this workflow. Do not switch global Xcode settings
  merely to analyze an existing artifact.
- A listed template does not prove the target hardware supports recording it.
  Check the selected instrument's device/OS requirements separately.
- Retain the catalogue Wine runtime and supervisor. The shell, Wine launcher
  and actual application child may have different PIDs/names. Bind the observed
  target to the run and loaded images; a process-name substring is not proof.
- Prefer the existing guarded 3DMark System Trace sidecar for that workload.
  It checks session lock state and runs the probe dry-run before collection.
  Follow its disk guard; the recorded 10 s system-wide lane failed at trim with
  about 2.4 GiB free, so its workflow requires at least 4 GiB.

## Bounded recording and failure handling

- Use an explicit target mode, duration and unique output path. Environment
  injection applies to launched targets, not an already-running attached process.
  System-wide recording is appropriate only when needed for the question;
  retain competing-process data and filter the intended target explicitly.
- `--no-prompt` does not grant missing permissions. Preserve denied access and
  collector errors instead of treating an empty trace as no activity.
- Capture timeout and workload supervision are separate. Stop only collectors
  and children owned by the run; preserve normal save/finalization where possible.
  A killed recorder or interrupted trim cannot qualify a complete capture.
- Keep the original `.trace`, stderr, tool/template versions and target/window
  provenance. Do not overwrite or delete evidence as routine cleanup. A failed
  attempt and a retry remain distinct artifacts.

## Export contract

- Export the TOC first, select the actual run number and available schema, then
  export only the required tables. Do not assume `run=1` or a schema exists on
  every Xcode release. Preserve units, XML `id`/`ref` relationships and symbols.
- A successful export with zero expected rows does not establish coverage.
  Distinguish unavailable schemas, no observed activity, parse failures and loss.
- Generic XML parsing is inspection, not qualification. Use the repository's
  CPU or Metal summaries for their supported schemas. Do not feed Processor
  Trace tables to a Time Profiler parser or invent an export schema name.
- `.trace` is an Instruments recording. Xcode/MTLCaptureManager `.gputrace`
  and the engine's `.pftrace` have their own collectors and validation paths.
- External `xtrace` wrappers such as
  [Kr1sso/xtrace-skill](https://github.com/Kr1sso/xtrace-skill) are optional
  references, not installed repository dependencies. Record any wrapper version
  and collector changes; automatic `sample` fallback is a separate observation.
