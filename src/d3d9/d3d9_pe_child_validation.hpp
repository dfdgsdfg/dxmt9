#pragma once

#include "d3d9_pe.hpp"
#include "d3d9_pe_com_membership.hpp"
#include "d3d9_pe_validated_object.hpp"

HRESULT D3D9PeValidateSurface(
    IDirect3DSurface9 *, const void *, D3D9PeValidatedSurface *,
    dxmt9::d3d9::pe::PeSurfaceQualification =
        dxmt9::d3d9::pe::PeSurfaceQualification::Any) noexcept;
HRESULT D3D9PeValidateTexture(
    IDirect3DBaseTexture9 *, const void *, D3D9PeValidatedTexture *) noexcept;
HRESULT D3D9PeValidateVertexBuffer(
    IDirect3DVertexBuffer9 *, const void *, D3D9PeValidatedVertexBuffer *) noexcept;
HRESULT D3D9PeValidateIndexBuffer(
    IDirect3DIndexBuffer9 *, const void *, D3D9PeValidatedIndexBuffer *) noexcept;
HRESULT D3D9PeValidateVertexShader(
    IDirect3DVertexShader9 *, const void *, D3D9PeValidatedVertexShader *) noexcept;
HRESULT D3D9PeValidatePixelShader(
    IDirect3DPixelShader9 *, const void *, D3D9PeValidatedPixelShader *) noexcept;
HRESULT D3D9PeValidateVertexDecl(
    IDirect3DVertexDeclaration9 *, const void *, D3D9PeValidatedDeclaration *) noexcept;
HRESULT D3D9PeValidateQuery(
    IDirect3DQuery9 *, const void *, D3D9PeValidatedQuery *) noexcept;

void D3D9PeInvalidateVertexBufferReadonlyCache(
    const D3D9PeValidatedVertexBuffer &) noexcept;
