#pragma once

#include "d3d9_pe_child_context.hpp"
#include "d3d9_pe_diagnostic_observer.hpp"

IDirect3DSurface9 *CreatePeSurface(
    D9CSurface *, IDirect3DDevice9 *, IUnknown *,
    D3D9PeSurfaceTextureContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr, bool = true, void * = nullptr,
    int32_t = 0) noexcept;
IDirect3DTexture9 *CreatePeTexture(
    D9CTexture *, IDirect3DDevice9 *, D3D9PeSurfaceTextureContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr, void * = nullptr,
    int32_t = 0) noexcept;
IDirect3DVolumeTexture9 *CreatePeVolumeTexture(
    D9CTexture *, IDirect3DDevice9 *, D3D9PeSurfaceTextureContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr) noexcept;
IDirect3DCubeTexture9 *CreatePeCubeTexture(
    D9CTexture *, IDirect3DDevice9 *, D3D9PeSurfaceTextureContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr) noexcept;
IDirect3DVertexBuffer9 *CreatePeVertexBuffer(
    D9CBuffer *, IDirect3DDevice9 *, D3D9PeBufferContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr) noexcept;
IDirect3DIndexBuffer9 *CreatePeIndexBuffer(
    D9CBuffer *, IDirect3DDevice9 *, D3D9PeBufferContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr) noexcept;
IDirect3DVertexShader9 *CreatePeVertexShader(
    D9CShader *, IDirect3DDevice9 *, std::uint64_t,
    D3D9PeShaderDeclarationContext * = nullptr) noexcept;
IDirect3DPixelShader9 *CreatePePixelShader(
    D9CShader *, IDirect3DDevice9 *, std::uint64_t,
    D3D9PeShaderDeclarationContext * = nullptr) noexcept;
IDirect3DVertexDeclaration9 *CreatePeVertexDecl(
    D9CVertexDecl *, IDirect3DDevice9 *,
    D3D9PeShaderDeclarationContext * = nullptr) noexcept;
IDirect3DQuery9 *CreatePeQuery(
    D9CQuery *, IDirect3DDevice9 *, D3D9PeQueryContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr) noexcept;
IDirect3DStateBlock9 *CreatePeStateBlock(
    D9CStateBlock *, IDirect3DDevice9 *, D3D9PeStateBlockContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr,
    StateBlockCaptureDisposition = StateBlockCaptureDisposition::All) noexcept;
IDirect3DSwapChain9Ex *CreatePeSwapChain(
    D9CSwapChain *, IDirect3DDevice9 *, D3D9PePresentationContext * = nullptr,
    D3D9PeDiagnosticObserver * = nullptr, bool = false,
    DWORD = 0) noexcept;
