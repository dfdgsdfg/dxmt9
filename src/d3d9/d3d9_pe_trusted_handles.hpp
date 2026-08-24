#pragma once

// Internal child-TU seam. These helpers are intentionally absent from the
// public child declaration: callers use them only after wrapper admission or
// to read a wrapper's cached wire reference.
#include "d3d9_pe.hpp"
#include "d3d9_pe_chunk_builder.hpp"

const dxmt9::d3d9::pe::SurfaceRef &
D3D9PeSurfaceRef(IDirect3DSurface9 *surface);
const dxmt9::d3d9::pe::TextureRef &
D3D9PeTextureRef(IDirect3DBaseTexture9 *texture);
const dxmt9::d3d9::pe::BufferRef &
D3D9PeVertexBufferRef(IDirect3DVertexBuffer9 *buffer);
const dxmt9::d3d9::pe::BufferRef &
D3D9PeIndexBufferRef(IDirect3DIndexBuffer9 *buffer);
const dxmt9::d3d9::pe::ShaderRef &
D3D9PeVertexShaderRef(IDirect3DVertexShader9 *shader);
const dxmt9::d3d9::pe::ShaderRef &
D3D9PePixelShaderRef(IDirect3DPixelShader9 *shader);
const dxmt9::d3d9::pe::DeclarationRef &
D3D9PeVertexDeclRef(IDirect3DVertexDeclaration9 *decl);
const dxmt9::d3d9::pe::QueryRef &
D3D9PeQueryRef(IDirect3DQuery9 *query);

