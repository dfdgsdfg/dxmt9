---
domain: present-pacing
workload: STALKER Clear-fusion private failure qualification
title: "Submitted-prefix fault exposes early release of session completion ownership"
type: evidence
status: private-lifetime-witness-not-resource-uaf-proof
updated: 2026-10-15
source: traces/stkcop-clear-submitted-failure-h555/{contract.md,submitted_spec.cpp,run.py,verify.py}; traces/stkcop-clear-submitted-failure-h555/r1/{build-receipt.json,results.json,verification.json}
related: present-pacing-clear-failure-relay.554.md; present-pacing-clear-failure-drain.556.md
---

# Terminal status alone does not drain previously submitted work

H555 tests unchanged H554 images with a real already-committed Clear prefix.
An independent same-queue SharedEvent blocker keeps that prefix incomplete
while source 2 triggers failed-open. The existing split seam commits a real
command buffer; PerNRecords=2 fixes the split. A completion-callback capture
owns a synthetic sentinel exclusively through the actual pending session.

All four candidate cases (one/two later sources × ordinary/exact stores)
release that sentinel before the GPU gate opens, without invoking its callback.
The submitted buffer is still Committed/Scheduled. Four serial OFF controls
retain ownership until actual completion and invoke the callback exactly once.
Identity-qualified terminal status and no inline completion remain correct;
the missing property is retention/drain across the earlier submission.

After opening the gate, readback shows the earlier color Clear executed and
the failed source's depth Clear did not. A failed source cannot roll back a
prefix that was already submitted. The completion waterline must not be
advanced to conceal that partial execution.

This is an actual session-ownership witness with a synthetic payload, **not
proof of a game resource use-after-free**. Metal's own retained references and
resource-specific owners need separate analysis. It disproves an assumption
that H554's function handler runs before session payload destruction; C++
unwinds those locals first. No Wine staging, canonical edit or timing occurs.

`verify.py` independently checks eight logs, image/source hashes, identities,
terminal counters and witness markers. The subsequent private repair is
[H556](present-pacing-clear-failure-drain.556.md).
