---
domain: present-pacing
workload: STALKER CoP Day Normal-action capture diagnostic
title: "Authenticated Normal history input and first-access parser corrections"
type: evidence
status: capture-authenticated-replay-unqualified
updated: 2026-10-11
source: traces/stkcop-normal-finish-h481/{run.py,receipt.json,loaded-images.txt,progress.jsonl,restore/restored.json,inspection.json,summary.json,surface-inventory.json,normal-rejection.json,first_access.cpp,first-access.tsv,first-access.stderr}; traces/stkcop-normal-finish-h481/verification/{native-testlog.txt,x86_64-testlog.txt}
related: present-pacing-normal-history.480.md; present-pacing-root-game.477.md; ../../../specs/experiments/gap.md
---

# Normal history input is available; replay qualification remains closed

H481 publishes a genuine Normal bundle on its first arm using `229ed0a0`.
The bounded supervisor stops only the owned engine after manifest publication,
checking PID and process birth identity, and restores all five shared Wine
images. Catalogue return 1 reflects intentional termination, not a successful
benchmark. H480's finish failures do not reproduce; this is **not** evidence
that their causes have been fixed.

## Input and authentication

The diagnostic retains Sikarugir Day/high/1280x720, two game threads, 16 audio
voices, Metal occlusion OFF and draw limit zero. Counters, frame sampling and
validation are OFF. Capture uses Normal from construction, skip 1,400, a 1 GiB
blob limit, 65,536 events and 16,384 blob entries. It is not a default-path
performance measurement. The supervisor bounds engine time and arm count;
the screenshot delay is outside that bounded diagnostic interval.

Bundle: `traces/stkcop-normal-finish-h481/tape/frame-10699344711800-1`.
Manifest SHA-256:
`ab9b808f08c071cef1f4dd64d498ec87333109f9cec9a236e84536c0854f3e85`.

| Authenticated field | Value |
|---|---|
| Construction / arm / freeze | Normal, revisions zero |
| Token / epoch / queue | 1 / 1 / 2 |
| Completed queue / initializer baseline | 3,562 / 1 |
| Published sources | 3,563–3,565 |
| Events / definitions / mutations | 6,053 / 779 / 5,179 |
| Chunks / wire records | 92 / 4,278 |
| History rows / touched rows | 21 / 17 |
| Defined surface generations with exact history | 16 / 16 |
| Blobs / bytes | 4,926 / 443,986,082 |

History reports post-Present discard and two current-backbuffer wrappers:
surface 4294967297 generation 1 and surface 4294968987 generation 4.
History content authority is explicitly `unknown`; `replay_qualified` is false.
Stable-Exact CPU authentication rejects this Normal bundle before child launch.

Original source/output readbacks each contain 3,686,400 bytes and have SHA-256
`f99e419861d3c4127e96d8ab56dca62d56421919a5962084ae4ecd95ee42ce5a`.
Format 22 is decoded as BGRX despite the `.rgba` filename. The preview shows
a courtyard; equality is original Present evidence, not replay equality,
live-screen correctness or defined initial-content authority.

## First-access inspection

An offline probe uses production tape/chunk validation and
`renderTapeFirstAccessObserve` on this immutable input. It exposed two parser
defects in `device_c_render_tape_first_access_locator.cpp`:

- Constant sections include an eight-byte range header; treating their extent
  as a plain array rejected valid bootstrap constants. The locator now uses
  shared section-schema rules and checks the range extent/count.
- A valid binding with the canonical null handle means unbind, not a malformed
  resource reference. RT/DS bindings now clear tracked state; nonnull invalid
  handles still reject, including texture references.

Regression tests cover all six constant families, malformed range/extent/kind,
binding preservation and explicit-null RT/DS/texture handling. Native focused
tests pass 3/3; x86_64 passes the same three plus the install-name audit.
Unix and both PE builds complete. This is a cold parser correction, not a
queue, lifetime or rendering-policy transition; no new TLA claim is made.

The same offline probe now completes all 21 history rows:

| Surface object | Event / record | Locator classification |
|---|---|---|
| 4294967298 | 12 / 1 | Full clear, flags 7 |
| 4294967303 | 12 / 1 | Full clear, flags 7 |
| 4294967309 | 5468 / 8 | Full clear, flags 1 |
| 4294967319 | 5402 / 6 | Full clear, flags 2 |
| 4294967368 | 5576 / 1 | Shader-read candidate (1×1 R32F) |
| 4294967297 | 6011 / 1 | Draw write, unknown coverage |
| 4294968987 | 6052 / 1 | Present read |

These are locator observations, **not complete first-read proofs**: carried
texture bindings, physical alias equivalence and shader write coverage are not
closed by this probe. A full-clear label alone is not aspect/alias admission.
It is unsafe to assume every attachment is cleared before use or to recreate
the two current wrappers with unrelated physical backing.

## Remaining gate

Preserve original touch/discard state independently of seed uploads, establish
physical alias closure and first-read content authority, then compare Normal
replay against the original GPU output. Only after those gates can this input
support correctness-qualified CPU-removal experiments. It is one interval,
not whole-tail coverage. H477's largest default CPU owner and removable amount
remain unresolved; no FPS or CPU-savings claim follows from capture success.
