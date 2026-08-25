---
type: "Spec Log"
title: "Winemetal Log"
description: "Domain-level maintenance history for Winemetal specs."
tags: [specs, log, winemetal]
---

# Winemetal Log

Domain-level maintenance history for `winemetal/` specs. Keep current implementation and evidence status in the owning gap document listed from [root gap](../gap.md), and use this log for structural edits, migrations, and older detail that should not stay in the current overview.

## 2026-08-25

- Made WSI qualification compose with deployment loader capabilities, required
  exact legacy runtime identities, and added the loader-pass / WSI-fail
  verification class for stock current Wine.
- Retired `R-WMB-7.1`-`R-WMB-7.4`, whose unconditional aggregate/direct
  `dlsym` order conflicted with the selected `extescape-v1` first,
  exact-qualified legacy second, fail-closed acquisition contract.

## 2026-07-08

- Added this domain log as the maintenance history companion for the domain specs.
