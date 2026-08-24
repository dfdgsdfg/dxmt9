#pragma once

#include "d3d9_pe_chunk_builder.hpp"
#include "d3d9_pe_com_membership.hpp"
#include "d3d9_pe_state_shadow.hpp"

#include <cstddef>
#include <cstdint>
#include <type_traits>

template<typename Public, typename Raw, typename Wire>
class D3D9PeValidatedObjectStorage;

template<typename Public, typename Raw, typename Wire>
struct D3D9PeValidatedObject {
  constexpr Public* publicIdentity() const noexcept { return publicIdentity_; }
  constexpr const void* ownerDevice() const noexcept { return ownerDevice_; }
  constexpr Raw* raw() const noexcept { return raw_; }
  constexpr const Wire& wire() const noexcept { return wire_; }
  constexpr std::uint64_t localMetadata() const noexcept {
    return localMetadata_;
  }
  constexpr dxmt9::d3d9::pe::PeConcreteObjectKind kind() const noexcept {
    return kind_;
  }

  template<typename Tag>
  constexpr StateBlockComRefCapability<Tag> stateBlockCapability() const
      noexcept {
    static_assert(std::is_same_v<Public,
                                 typename StateBlockComRefTraits<Tag>::raw_type>,
                  "StateBlock capability must preserve the public interface type");
    return StateBlockComRefCapability<Tag>(publicIdentity_);
  }

  constexpr StateBlockBufferRefCapability stateBlockBufferCapability() const
      noexcept {
    static_assert(std::is_same_v<Public, IDirect3DVertexBuffer9>,
                  "only vertex-buffer validation can mint a stream capability");
    return StateBlockBufferRefCapability(
        static_cast<IDirect3DVertexBuffer9*>(publicIdentity_));
  }

 private:
  friend class D3D9PeValidatedObjectStorage<Public, Raw, Wire>;
  constexpr D3D9PeValidatedObject() noexcept = default;

  Public* publicIdentity_ = nullptr;
  const void* ownerDevice_ = nullptr;
  Raw* raw_ = nullptr;
  Wire wire_{};
  std::uint64_t localMetadata_ = 0u;
  dxmt9::d3d9::pe::PeConcreteObjectKind kind_ =
      dxmt9::d3d9::pe::PeConcreteObjectKind::Surface;

  friend struct D3D9PeValidatedObjectWriter;
};

template<typename Public, typename Raw, typename Wire>
class D3D9PeValidatedObjectStorage {
 public:
  constexpr D3D9PeValidatedObjectStorage() noexcept = default;

  constexpr Public* publicIdentity() const noexcept {
    return object()->publicIdentity();
  }
  constexpr const void* ownerDevice() const noexcept {
    return object()->ownerDevice();
  }
  constexpr Raw* raw() const noexcept { return object()->raw(); }
  constexpr const Wire& wire() const noexcept { return object()->wire(); }
  constexpr std::uint64_t localMetadata() const noexcept {
    return object()->localMetadata();
  }
  constexpr dxmt9::d3d9::pe::PeConcreteObjectKind kind() const noexcept {
    return object()->kind();
  }

  template<typename Tag>
  constexpr StateBlockComRefCapability<Tag> stateBlockCapability() const
      noexcept {
    return object()->template stateBlockCapability<Tag>();
  }
  constexpr StateBlockBufferRefCapability stateBlockBufferCapability() const
      noexcept {
    return object()->stateBlockBufferCapability();
  }

 private:
  using Object = D3D9PeValidatedObject<Public, Raw, Wire>;
  Object* object() noexcept { return &object_; }
  const Object* object() const noexcept { return &object_; }
  Object object_{};
  friend struct D3D9PeValidatedObjectWriter;
};

struct D3D9PeValidatedObjectWriter;

template<typename Public, typename Raw, typename Wire>
using D3D9PeValidatedObjectOutput =
    D3D9PeValidatedObjectStorage<Public, Raw, Wire>;

using D3D9PeValidatedSurface = D3D9PeValidatedObjectOutput<
    IDirect3DSurface9, D9CSurface, dxmt9::d3d9::pe::SurfaceRef>;
using D3D9PeValidatedTexture = D3D9PeValidatedObjectOutput<
    IDirect3DBaseTexture9, D9CTexture, dxmt9::d3d9::pe::TextureRef>;
using D3D9PeValidatedVertexBuffer = D3D9PeValidatedObjectOutput<
    IDirect3DVertexBuffer9, D9CBuffer, dxmt9::d3d9::pe::BufferRef>;
using D3D9PeValidatedIndexBuffer = D3D9PeValidatedObjectOutput<
    IDirect3DIndexBuffer9, D9CBuffer, dxmt9::d3d9::pe::BufferRef>;
using D3D9PeValidatedVertexShader = D3D9PeValidatedObjectOutput<
    IDirect3DVertexShader9, D9CShader, dxmt9::d3d9::pe::ShaderRef>;
using D3D9PeValidatedPixelShader = D3D9PeValidatedObjectOutput<
    IDirect3DPixelShader9, D9CShader, dxmt9::d3d9::pe::ShaderRef>;
using D3D9PeValidatedDeclaration = D3D9PeValidatedObjectOutput<
    IDirect3DVertexDeclaration9, D9CVertexDecl,
    dxmt9::d3d9::pe::DeclarationRef>;
using D3D9PeValidatedQuery = D3D9PeValidatedObjectOutput<
    IDirect3DQuery9, D9CQuery, dxmt9::d3d9::pe::QueryRef>;

template<typename Writer, typename Output>
concept D3D9PeCanClearValidatedOutput = requires(Output* output) {
  Writer::clear(output);
};

template<typename Factory, typename Raw>
concept D3D9PeCanFromRawStateBlockRef = requires(Raw* raw) {
  Factory::fromValidated(raw);
};

static_assert(!std::is_aggregate_v<D3D9PeValidatedObject<
                  IDirect3DSurface9, D9CSurface,
                  dxmt9::d3d9::pe::SurfaceRef>>);
static_assert(!std::is_constructible_v<D3D9PeValidatedObject<
                  IDirect3DSurface9, D9CSurface,
                  dxmt9::d3d9::pe::SurfaceRef>>);
static_assert(!D3D9PeCanClearValidatedOutput<D3D9PeValidatedObjectWriter,
                                             D3D9PeValidatedSurface>);
static_assert(!D3D9PeCanFromRawStateBlockRef<
                  StateBlockComRefFactory<StateBlockRenderTargetTag>,
                  IDirect3DSurface9>);
static_assert(!D3D9PeCanFromRawStateBlockRef<StateBlockBufferRefFactory,
                                             IDirect3DVertexBuffer9>);
