---
type: "Spec Log"
title: "Deployment Log"
description: "Domain-level maintenance history for Deployment specs."
tags: [specs, log, deploy]
---

# Deployment Log

Domain-level maintenance history for `deploy/` specs. Keep current implementation and evidence status in [gap](gap.md), and use this log for structural edits, migrations, and older detail that should not stay in the current overview.

## 2026-08-25

- Split deployment compatibility into unixlib-loader and Metal-surface
  capabilities, replaced the `ntdll.dll` helper-export assumption with a cold
  loader adapter, and defined schema 2 plus coexistence checksum gates.

## 2026-07-08

- Added this domain log as the maintenance history companion for the domain specs.
