// Pure value-level spec for the PE-side shared-handle / user-memory
// argument-validation helpers in src/d3d9/d3d9_pe_device.cpp:
//   * validateSharedHandleForTexture       — CreateTexture / CreateCubeTexture
//                                             / CreateVolumeTexture
//   * validateSharedHandleForBuffer         — CreateVertexBuffer / CreateIndexBuffer
//   * validateSharedHandleForSurface        — CreateOffscreenPlainSurface(Ex)
//   * validateSharedHandleForDefaultSurface — CreateRenderTarget(Ex) /
//                                             CreateDepthStencilSurface(Ex)
//
// Why a value-level spec?  d3d9_pe_device.cpp is built only on the PE
// (Windows) side (it includes <windows.h> / <d3d9.h>) so native dxmt9-core-*
// tests cannot instantiate D3D9DeviceImpl directly.  The established pattern
// (core_d3d9_device_validation_spec.cpp, core_d3d9_gamma_ramp_spec.cpp) is to
// mirror the small, self-contained PE-side pure logic here and pin its
// observable contract.  The implementation file is the source of truth; any
// drift here or there is a regression.
//
// Behavioral oracle — Wine d3d9 (commit
// 6e073d28dee3af7f4c965daec94644e0f9f92727):
//   * dlls/d3d9/device.c d3d9_device_CreateTexture / CreateCubeTexture /
//     CreateVolumeTexture / CreateRenderTarget / CreateDepthStencilSurface /
//     CreateOffscreenPlainSurface / CreateVertexBuffer / CreateIndexBuffer.
//
// Wine contract, per resource class, for a non-NULL pSharedHandle:
//   non-extended device              -> E_NOTIMPL  (every class)
//   extended device:
//     Texture/Cube/Volume:
//       SYSTEMMEM + 1 mip (2D only)  -> S_OK   (caller memory aliased)
//       SYSTEMMEM + != 1 mip         -> D3DERR_INVALIDCALL
//       SYSTEMMEM (cube/volume)      -> D3DERR_INVALIDCALL (no alias scope)
//       SCRATCH / other non-DEFAULT  -> D3DERR_INVALIDCALL
//       DEFAULT                      -> S_OK   (FIXME; handle ignored, created)
//     Vertex/Index buffer:
//       non-DEFAULT pool             -> D3DERR_NOTAVAILABLE
//       DEFAULT                      -> S_OK   (FIXME; handle ignored, created)
//     Offscreen plain surface:
//       SYSTEMMEM                    -> S_OK   (caller memory aliased)
//       SCRATCH / other non-DEFAULT  -> D3DERR_INVALIDCALL
//       DEFAULT                      -> S_OK   (FIXME; handle ignored, created)
//     Render target / depth stencil (DEFAULT-pool only):
//       extended                     -> S_OK   (FIXME; handle ignored, created)
//
// PE conformance oracle functions (read, not modified) that encode these
// expectations:
//   * tests/conformance/d3d9/d3d9_conformance_resource.c:
//       test_shared_handle_policy     (non-Ex: 5 classes -> E_NOTIMPL, null out)
//       test_ex_shared_handle_policy  (Ex: SYSTEMMEM/SCRATCH cases)
//
// The crucial regression this spec guards: the extended + D3DPOOL_DEFAULT
// path must return S_OK (Wine proceeds, ignoring the handle), NOT the former
// placeholder E_NOTIMPL.  Cross-process sharing is still not wired; only the
// observable HRESULT parity is asserted here.

#include "core_spec_fixtures.hpp"

#include <cstdint>

using namespace dxmt9::core::spec;

namespace {

// ---------------------------------------------------------------------------
// HRESULT / D3DPOOL mirrors.  We do not pull in <d3d9.h> (Win32-only header);
// the validators take primitive scalars precisely so they can be mirrored
// portably and pinned at value level.
// ---------------------------------------------------------------------------

constexpr int32_t kD3D_OK = 0;
constexpr int32_t kE_NOTIMPL = static_cast<int32_t>(0x80004001u);
constexpr int32_t kD3DERR_INVALIDCALL = static_cast<int32_t>(0x8876086cu);
constexpr int32_t kD3DERR_NOTAVAILABLE = static_cast<int32_t>(0x8876086au);

// D3DPOOL (<d3d9types.h>): DEFAULT=0, MANAGED=1, SYSTEMMEM=2, SCRATCH=3.
constexpr uint32_t kPoolDefault = 0u;
constexpr uint32_t kPoolManaged = 1u;
constexpr uint32_t kPoolSystemMem = 2u;
constexpr uint32_t kPoolScratch = 3u;

constexpr bool kHandle = true;   // pSharedHandle != NULL
constexpr bool kNoHandle = false;  // pSharedHandle == NULL

// ---------------------------------------------------------------------------
// Mirrors of the PE-side pure validators (src/d3d9/d3d9_pe_device.cpp).
// Kept byte-for-byte equivalent in logic.
// ---------------------------------------------------------------------------

// Mirrors validateSharedHandleForTexture().  allowSystemMemUserMemory is true
// only for 2D textures (cube/volume pass false).
int32_t mirrorTexture(bool extended, bool hasHandle, uint32_t pool,
                      uint32_t levels, bool allowSystemMemUserMemory) {
  if (!hasHandle) return kD3D_OK;
  if (!extended) return kE_NOTIMPL;
  if (pool == kPoolSystemMem) {
    if (!allowSystemMemUserMemory) return kD3DERR_INVALIDCALL;
    if (levels != 1u) return kD3DERR_INVALIDCALL;
    return kD3D_OK;
  }
  if (pool != kPoolDefault) return kD3DERR_INVALIDCALL;
  return kD3D_OK;  // extended + DEFAULT: Wine proceeds (handle ignored).
}

// Mirrors validateSharedHandleForBuffer().
int32_t mirrorBuffer(bool extended, bool hasHandle, uint32_t pool) {
  if (!hasHandle) return kD3D_OK;
  if (!extended) return kE_NOTIMPL;
  if (pool != kPoolDefault) return kD3DERR_NOTAVAILABLE;
  return kD3D_OK;  // extended + DEFAULT: Wine proceeds (handle ignored).
}

// Mirrors validateSharedHandleForSurface().  allowSystemMemUserMemory is true
// for offscreen plain surfaces.
int32_t mirrorSurface(bool extended, bool hasHandle, uint32_t pool,
                      bool allowSystemMemUserMemory) {
  if (!hasHandle) return kD3D_OK;
  if (!extended) return kE_NOTIMPL;
  if (pool == kPoolSystemMem) {
    if (!allowSystemMemUserMemory) return kD3DERR_INVALIDCALL;
    return kD3D_OK;
  }
  if (pool == kPoolScratch) return kD3DERR_INVALIDCALL;
  if (pool != kPoolDefault) return kD3DERR_INVALIDCALL;
  return kD3D_OK;  // extended + DEFAULT: Wine proceeds (handle ignored).
}

// Mirrors validateSharedHandleForDefaultSurface() (RT / DS are DEFAULT-only).
int32_t mirrorDefaultSurface(bool extended, bool hasHandle) {
  if (!hasHandle) return kD3D_OK;
  if (!extended) return kE_NOTIMPL;
  return kD3D_OK;  // extended: Wine proceeds (handle ignored).
}

// ---------------------------------------------------------------------------

void testNonExtendedAlwaysNotImpl() {
  // test_shared_handle_policy: every resource class with a non-NULL handle on
  // a NON-extended device returns E_NOTIMPL (out-pointer nulled at call site).
  // CreateTexture DEFAULT.
  checkEq(mirrorTexture(/*extended=*/false, kHandle, kPoolDefault, 1u, true),
          kE_NOTIMPL, "non-Ex CreateTexture DEFAULT + handle -> E_NOTIMPL");
  // CreateTexture SYSTEMMEM.
  checkEq(mirrorTexture(/*extended=*/false, kHandle, kPoolSystemMem, 1u, true),
          kE_NOTIMPL, "non-Ex CreateTexture SYSTEMMEM + handle -> E_NOTIMPL");
  // CreateOffscreenPlainSurface DEFAULT.
  checkEq(mirrorSurface(/*extended=*/false, kHandle, kPoolDefault, true),
          kE_NOTIMPL, "non-Ex CreateOffscreenPlainSurface + handle -> E_NOTIMPL");
  // CreateVertexBuffer DEFAULT.
  checkEq(mirrorBuffer(/*extended=*/false, kHandle, kPoolDefault),
          kE_NOTIMPL, "non-Ex CreateVertexBuffer DEFAULT + handle -> E_NOTIMPL");
  // CreateIndexBuffer DEFAULT.
  checkEq(mirrorBuffer(/*extended=*/false, kHandle, kPoolDefault),
          kE_NOTIMPL, "non-Ex CreateIndexBuffer DEFAULT + handle -> E_NOTIMPL");
  // RT / DS surfaces.
  checkEq(mirrorDefaultSurface(/*extended=*/false, kHandle),
          kE_NOTIMPL, "non-Ex CreateRenderTarget + handle -> E_NOTIMPL");
}

void testNullHandleAlwaysOk() {
  // A NULL pSharedHandle is always the normal create path -> S_OK, regardless
  // of extended/pool.  No class rejects the absence of a handle.
  checkEq(mirrorTexture(false, kNoHandle, kPoolManaged, 4u, true), kD3D_OK,
          "no handle -> texture create proceeds");
  checkEq(mirrorBuffer(false, kNoHandle, kPoolManaged), kD3D_OK,
          "no handle -> buffer create proceeds");
  checkEq(mirrorSurface(false, kNoHandle, kPoolScratch, true), kD3D_OK,
          "no handle -> surface create proceeds");
  checkEq(mirrorDefaultSurface(false, kNoHandle), kD3D_OK,
          "no handle -> RT/DS create proceeds");
}

void testExtendedTextureSystemMem() {
  // test_ex_shared_handle_policy: SYSTEMMEM 2D texture, exactly one mip ->
  // S_OK (caller memory aliased).
  checkEq(mirrorTexture(true, kHandle, kPoolSystemMem, 1u, /*alias=*/true),
          kD3D_OK, "Ex 2D texture SYSTEMMEM 1 mip + handle -> S_OK (aliased)");
  // 0 mips (auto) -> INVALIDCALL.
  checkEq(mirrorTexture(true, kHandle, kPoolSystemMem, 0u, true),
          kD3DERR_INVALIDCALL, "Ex 2D texture SYSTEMMEM auto-mip + handle -> INVALIDCALL");
  // > 1 mip -> INVALIDCALL.
  checkEq(mirrorTexture(true, kHandle, kPoolSystemMem, 4u, true),
          kD3DERR_INVALIDCALL, "Ex 2D texture SYSTEMMEM 4 mip + handle -> INVALIDCALL");
  // Cube/volume SYSTEMMEM (no alias scope) -> INVALIDCALL even at 1 mip.
  checkEq(mirrorTexture(true, kHandle, kPoolSystemMem, 1u, /*alias=*/false),
          kD3DERR_INVALIDCALL, "Ex cube/volume SYSTEMMEM + handle -> INVALIDCALL");
}

void testExtendedTextureScratchAndOther() {
  // SCRATCH -> INVALIDCALL.
  checkEq(mirrorTexture(true, kHandle, kPoolScratch, 1u, true),
          kD3DERR_INVALIDCALL, "Ex texture SCRATCH + handle -> INVALIDCALL");
  // MANAGED (non-DEFAULT, non-SYSTEMMEM) -> INVALIDCALL.
  checkEq(mirrorTexture(true, kHandle, kPoolManaged, 1u, true),
          kD3DERR_INVALIDCALL, "Ex texture MANAGED + handle -> INVALIDCALL");
}

void testExtendedDefaultPoolProceeds() {
  // THE regression guard: extended + DEFAULT + handle must be S_OK (Wine
  // logs a FIXME then creates the resource, ignoring the handle), NOT the
  // former placeholder E_NOTIMPL.
  checkEq(mirrorTexture(true, kHandle, kPoolDefault, 1u, true), kD3D_OK,
          "Ex texture DEFAULT + handle -> S_OK (handle ignored, created)");
  checkEq(mirrorTexture(true, kHandle, kPoolDefault, 1u, false), kD3D_OK,
          "Ex cube/volume DEFAULT + handle -> S_OK (handle ignored, created)");
  checkEq(mirrorBuffer(true, kHandle, kPoolDefault), kD3D_OK,
          "Ex vertex/index buffer DEFAULT + handle -> S_OK (handle ignored)");
  checkEq(mirrorSurface(true, kHandle, kPoolDefault, true), kD3D_OK,
          "Ex offscreen surface DEFAULT + handle -> S_OK (handle ignored)");
  checkEq(mirrorDefaultSurface(true, kHandle), kD3D_OK,
          "Ex render target / depth stencil + handle -> S_OK (handle ignored)");
  // And confirm none of the DEFAULT cases is E_NOTIMPL anymore.
  check(mirrorTexture(true, kHandle, kPoolDefault, 1u, true) != kE_NOTIMPL,
        "Ex texture DEFAULT must not be E_NOTIMPL");
  check(mirrorBuffer(true, kHandle, kPoolDefault) != kE_NOTIMPL,
        "Ex buffer DEFAULT must not be E_NOTIMPL");
  check(mirrorSurface(true, kHandle, kPoolDefault, true) != kE_NOTIMPL,
        "Ex surface DEFAULT must not be E_NOTIMPL");
  check(mirrorDefaultSurface(true, kHandle) != kE_NOTIMPL,
        "Ex RT/DS DEFAULT must not be E_NOTIMPL");
}

void testExtendedBuffer() {
  // VB/IB non-DEFAULT pool -> NOTAVAILABLE (distinct from texture INVALIDCALL).
  checkEq(mirrorBuffer(true, kHandle, kPoolSystemMem), kD3DERR_NOTAVAILABLE,
          "Ex vertex/index buffer SYSTEMMEM + handle -> NOTAVAILABLE");
  checkEq(mirrorBuffer(true, kHandle, kPoolScratch), kD3DERR_NOTAVAILABLE,
          "Ex vertex/index buffer SCRATCH + handle -> NOTAVAILABLE");
  checkEq(mirrorBuffer(true, kHandle, kPoolManaged), kD3DERR_NOTAVAILABLE,
          "Ex vertex/index buffer MANAGED + handle -> NOTAVAILABLE");
}

void testExtendedOffscreenSurface() {
  // SYSTEMMEM -> S_OK (aliased).
  checkEq(mirrorSurface(true, kHandle, kPoolSystemMem, true), kD3D_OK,
          "Ex offscreen surface SYSTEMMEM + handle -> S_OK (aliased)");
  // SCRATCH -> INVALIDCALL.
  checkEq(mirrorSurface(true, kHandle, kPoolScratch, true), kD3DERR_INVALIDCALL,
          "Ex offscreen surface SCRATCH + handle -> INVALIDCALL");
  // MANAGED -> INVALIDCALL.
  checkEq(mirrorSurface(true, kHandle, kPoolManaged, true), kD3DERR_INVALIDCALL,
          "Ex offscreen surface MANAGED + handle -> INVALIDCALL");
}

}  // namespace

int main() {
  try {
    testNonExtendedAlwaysNotImpl();
    testNullHandleAlwaysOk();
    testExtendedTextureSystemMem();
    testExtendedTextureScratchAndOther();
    testExtendedDefaultPoolProceeds();
    testExtendedBuffer();
    testExtendedOffscreenSurface();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }
  return EXIT_SUCCESS;
}
