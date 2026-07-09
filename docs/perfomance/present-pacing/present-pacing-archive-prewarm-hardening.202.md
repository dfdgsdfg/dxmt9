---
domain: present-pacing
workload: 3DMark05 GT1
subcategory: method
order: 202
title: Archive Prewarm Hardening Closes The Startup-Flake Class
date: 2026-07-09
type: no-gputrace
status: accepted-reliability-fix
source: experiments/output/app-d3d9-3dmark05-prewarm-hardening-r1-20260709/result.json; docs/perfomance/present-pacing/present-pacing-inline-const-delta.201.md; specs/backend/requirements.md
related: docs/perfomance/present-pacing/index.md
---

# Present-Pacing H215 - Archive prewarm hardening (R-BACK-3.9..3.11)

## Question

H214 root-caused the startup-flake class: a shader archive bloated to `125MB`
by probe campaigns was full-prewarmed synchronously inside `CreateDevice`
(twice per launch), and 3DMark05 self-aborted deterministically. Does the
hardening (`30bee79b`) close the class?

## Change

Per the new contracts: async full prewarm with compile fallback and a
queued-replay backfill for racing archive writes, `DXMT9_ARCHIVE_MAX_PREWARM_MB`
size guard (default `512`), a bounded mid-session milestone save under the
(newly actually-implemented) write-side `LOCK_EX`, a debug-env pollution guard
that skips saves for classifier sessions, and a process mutex around
`MTLBinaryArchive` add/serialize (pre-existing latent hazard).

## Regression-fixture verdict

The exact failing condition reproduced with the hardened build — the
preserved `125MB` archive restored, same probe recipe:

| Gate | Result |
|---|---|
| Startup (was 2/2 deterministic self-abort) | **`status=pass`, `2,220` presents** — healthy-population center, `gpu_err=0` |
| Non-blocking load | `prewarm_async_completion` `1.67s` in the background; `CreateDevice` unblocked |
| Persistence without clean shutdown | `prewarm_milestone_save_count=1`; archive mtime advanced after a timeout-killed run |
| FPS tax of the compile-fallback window | none observable (`2,220` vs `2,220-2,297` population) |

Side effects for the measurement methodology: probe FPS baselines no longer
depend on hidden archive state (cold/warm variance closed), and classifier
probes stop bloating the shared archive. `prewarm_entries_loaded=2` on a
`125MB` archive suggests that counter counts archive objects rather than
PSO entries — cosmetic, noted for a later counter-semantics pass.
