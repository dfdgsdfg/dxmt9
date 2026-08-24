#include "d3d9_pe_state_shadow.hpp"
#include "d3d9_pe_validated_object.hpp"

#include <concepts>
#include <cstddef>
#include <iostream>

using namespace dxmt9::d3d9::pe;

template<typename T>
concept HasPublicWriter = requires(T& value) { value.writer(); };

template<typename T>
concept HasPublicMaintenance = requires(T& value) { value.maintenance(); };

static_assert(!HasPublicWriter<PeHotStateShadow>);
static_assert(!HasPublicMaintenance<PeHotStateShadow>);
static_assert(requires(PeHotStateShadow& shadow, RenderStateSlot slot) {
  { shadow.transition() } -> std::same_as<PeHotStateShadow::Transition>;
  shadow.transition().setRenderState(slot, 1u);
  { shadow.consume() } -> std::same_as<PeHotStateShadow::Consumer>;
});
static_assert(sizeof(PeHotStateShadow::Transition) == sizeof(void*));
static_assert(sizeof(PeHotStateShadow::Consumer) == sizeof(void*));
static_assert(sizeof(PeHotStateShadow) == 44944u);

using ValidatedSurfaceObject = D3D9PeValidatedObject<
    IDirect3DSurface9, D9CSurface, dxmt9::d3d9::pe::SurfaceRef>;
static_assert(!std::is_aggregate_v<ValidatedSurfaceObject>);
static_assert(!std::is_constructible_v<ValidatedSurfaceObject>);
template<typename Writer, typename Output>
concept HasValidatorWriter = requires(Output* output) {
  Writer::clear(output);
};
static_assert(!HasValidatorWriter<D3D9PeValidatedObjectWriter,
                                  D3D9PeValidatedSurface>);
template<typename T>
concept HasRawSurfaceFactory = requires(T* raw) {
  StateBlockComRefFactory<StateBlockRenderTargetTag>::fromValidated(raw);
};
template<typename T>
concept HasRawBufferFactory = requires(T* raw) {
  StateBlockBufferRefFactory::fromValidated(raw);
};
static_assert(!HasRawSurfaceFactory<IDirect3DSurface9>);
static_assert(!HasRawBufferFactory<IDirect3DVertexBuffer9>);
static_assert(requires(D3D9PeValidatedSurface& output) {
  output.publicIdentity();
  output.ownerDevice();
  output.raw();
  output.wire();
});

int main() {
  std::cout << "PE shadow capability surface spec passed\n";
  return 0;
}
