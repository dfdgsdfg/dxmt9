# Roadmap

This repo is starting from a blank slate. The initial goal is to establish the
core shape of a D3D9-to-Metal translation layer that can eventually plug into
Wine on macOS.

## Phase 0

- Repository bootstrap
- Build system
- Public header surface
- Smoke test

## Phase 1

- D3D9 enums, types, and COM interfaces
- Adapter and device lifecycle
- Resource creation and lifetime tracking

## Phase 2

- Command translation
- Metal command queue and render pass mapping
- Shader translation and pipeline state compilation

## Phase 3

- Wine DLL integration
- Configuration and logging
- Real application bring-up

