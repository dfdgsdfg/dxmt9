sample-d3d9-water-rt is a repo-local Direct3D 9 sample focused on the render path that commercial water effects commonly use:

- render to texture
- sample the render target on a later pass
- projected texture coordinates
- alpha blended overlay composition

The sample renders a stylized harbor scene into an offscreen render target, presents that texture to the backbuffer, and then draws a lower-screen water layer that samples the scene texture with projective UV distortion.

It is intended as a narrow regression target for bugs like:

- black water surfaces
- missing reflection/refraction
- broken projected UV math
- blend-state regressions on water overlays
