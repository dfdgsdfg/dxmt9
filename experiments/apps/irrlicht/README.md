# Irrlicht Managed Lights

Repo-local D3D9 fixed-function experiment binary used for the
`irrlicht-managed-lights` entry.

This is not a verbatim build of the upstream Irrlicht sample. It is a
dxmt9-targeted harness that preserves the relevant experiment shape:

- fixed-function render path
- dynamic `SetLight` / `LightEnable` updates
- material + fog state churn
- texture-stage modulation without programmable shaders

The purpose of this entry is to start the fifth real-app experiment lane around
Irrlicht-style managed lights while remaining honest about current dxmt9
capabilities.
