#pragma once

#include "d3d9_pe.hpp"

#include <cstddef>

struct D3D9PeRecorderFlush {
  virtual HRESULT FlushPeRecorderForChild() = 0;
  virtual bool IsStateBlockRecordingForChild() const = 0;
  virtual void InvalidateStateBlockShadowForChild() = 0;
  virtual void AddDefaultPoolResourceRefForChild() = 0;
  virtual void ReleaseDefaultPoolResourceRefForChild() = 0;
  virtual bool IsChunkRecorderEnabledForChild() const = 0;
  virtual HRESULT AppendRecordForChild(const void *data, size_t bytes) = 0;

protected:
  ~D3D9PeRecorderFlush() = default;
};

IDirect3DSurface9 *CreatePeSurface(D9CSurface *surface,
                                   IDirect3DDevice9 *device,
                                   IUnknown *container,
                                   D3D9PeRecorderFlush *recorder = nullptr,
                                   bool trackDefaultPool = true);
IDirect3DTexture9 *CreatePeTexture(D9CTexture *texture,
                                   IDirect3DDevice9 *device,
                                   D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DVolumeTexture9 *
CreatePeVolumeTexture(D9CTexture *texture, IDirect3DDevice9 *device,
                      D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DCubeTexture9 *
CreatePeCubeTexture(D9CTexture *texture, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DVertexBuffer9 *
CreatePeVertexBuffer(D9CBuffer *buffer, IDirect3DDevice9 *device,
                     D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DIndexBuffer9 *
CreatePeIndexBuffer(D9CBuffer *buffer, IDirect3DDevice9 *device,
                    D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DVertexShader9 *CreatePeVertexShader(D9CShader *shader,
                                             IDirect3DDevice9 *device);
IDirect3DPixelShader9 *CreatePePixelShader(D9CShader *shader,
                                           IDirect3DDevice9 *device);
IDirect3DVertexDeclaration9 *CreatePeVertexDecl(D9CVertexDecl *decl,
                                                IDirect3DDevice9 *device);
IDirect3DQuery9 *CreatePeQuery(D9CQuery *query, IDirect3DDevice9 *device,
                               D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DStateBlock9 *
CreatePeStateBlock(D9CStateBlock *stateBlock, IDirect3DDevice9 *device,
                   D3D9PeRecorderFlush *recorder = nullptr);
IDirect3DSwapChain9Ex *
CreatePeSwapChain(D9CSwapChain *swapChain, IDirect3DDevice9 *device,
                  D3D9PeRecorderFlush *recorder = nullptr,
                  bool extended = false);

D9CSurface *D3D9PeRawSurface(IDirect3DSurface9 *surface);
D9CTexture *D3D9PeRawTexture(IDirect3DBaseTexture9 *texture);
D9CBuffer *D3D9PeRawVertexBuffer(IDirect3DVertexBuffer9 *buffer);
D9CBuffer *D3D9PeRawIndexBuffer(IDirect3DIndexBuffer9 *buffer);
D9CShader *D3D9PeRawVertexShader(IDirect3DVertexShader9 *shader);
D9CShader *D3D9PeRawPixelShader(IDirect3DPixelShader9 *shader);
D9CVertexDecl *D3D9PeRawVertexDecl(IDirect3DVertexDeclaration9 *decl);
