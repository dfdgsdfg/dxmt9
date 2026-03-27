# Specifications

This directory contains specifications for dxmt9 — a Wine/D3D9-to-Metal translation
layer. Specs describe *what* the system must be and do, not how to build it.

## Structure

```
specs/
├── core/               Wine-facing D3D9 layer
│   ├── requirements.md D3D9 COM contracts, state machine rules, resource semantics
│   └── design.md       COM object model, device state structure, core/backend boundary
├── backend/            Metal translation layer
│   ├── requirements.md Translation correctness, command encoding, PSO cache invariants
│   └── design.md       Command queue, encoder lifecycle, resource allocation model
└── experiments/        Compatibility validation
    ├── requirements.md What each experiment must establish
    └── design.md       Experiment structure and acceptance criteria
```
