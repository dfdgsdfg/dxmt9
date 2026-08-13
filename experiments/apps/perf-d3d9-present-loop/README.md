# perf-d3d9-present-loop

Repo-local standard-Present probe. The default mode renders `Clear` followed
by ordinary `IDirect3DDevice9::Present` to isolate drawable pacing.

For a bounded textured frame-tape capture, opt in to one fixed-function
`DrawPrimitiveUP` per frame:

```sh
PRESENT_LOOP_TEXTURED=1 PRESENT_LOOP_ITERATIONS=2 \
  wine perf-d3d9-present-loop.exe
```

The first Present arms capture and the second closes the captured interval, so
the checker requires two iterations. The textured mode uses inline
`D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1` vertices and one fully
initialized, non-uniform, single-level `D3DPOOL_DEFAULT` dynamic 4x4
`D3DFMT_A8R8G8B8` texture. It calls ordinary `Present`, not `PresentEx`; the
default clear-only mode is unchanged.
