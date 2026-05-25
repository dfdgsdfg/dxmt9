// R-FORMAT-11 — RESZ MSAA→INTZ depth resolve.
//
// Spec: specs/d3d9/formats/{requirements,design}.md R-FORMAT-11. Two seams
// under test, both deterministic and GPU-free:
//
//   (a) RESZ trigger detection — the pure value transform
//       core::isReszDepthResolveSentinel(key, value): writing the exact
//       sentinel 0x7FA05000 to D3DRS_POINTSIZE (RS code 154) is a RESZ
//       command, not a point size; any other key or value is not.
//
//   (b) Depth-resolve request construction — Device::reszDepthResolve(
//       msaaDepth, intzDest) builds a core::DepthResolveDesc whose source is
//       the bound multisampled depth surface and whose destination is the
//       single-sample INTZ texture's level-0 surface, then submits it through
//       the backend queue. Driven through a RecordingBackend observer (the
//       fake-backend pattern the existing core specs use) so the constructed
//       desc is asserted without a Metal device.
//
// (a) is also covered as a unit in state_draw_transform_spec; it is repeated
// here so this spec stands alone as the R-FORMAT-11 contract surface, and is
// extended with the desc-construction coverage that the sentinel test does
// not reach.

#include "../core/core_spec_fixtures.hpp"

#include <bit>
#include <memory>

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

// --- (a) RESZ trigger detection -------------------------------------------

void testReszSentinelDetection() {
  // The exact sentinel on RS_POINTSIZE is the resolve trigger.
  check(isReszDepthResolveSentinel(RS_POINTSIZE, kReszDepthResolveSentinel),
        "RESZ sentinel on RS_POINTSIZE is recognised as the resolve trigger");
  checkEq(kReszDepthResolveSentinel, 0x7FA05000u,
          "RESZ sentinel value is exactly 0x7FA05000");
  checkEq(RS_POINTSIZE, 154u, "D3DRS_POINTSIZE is render-state code 154");

  // Ordinary point sizes (and near-miss values) on RS_POINTSIZE are NOT the
  // trigger — the write keeps its point-size meaning.
  const u32 pointSizeOneBits = std::bit_cast<u32>(1.0f);
  check(!isReszDepthResolveSentinel(RS_POINTSIZE, pointSizeOneBits),
        "ordinary point size (1.0f bits) is not the RESZ sentinel");
  check(!isReszDepthResolveSentinel(RS_POINTSIZE, 0u),
        "zero point size is not the RESZ sentinel");
  check(!isReszDepthResolveSentinel(RS_POINTSIZE, kReszDepthResolveSentinel - 1u),
        "near-miss below the sentinel does not trigger");
  check(!isReszDepthResolveSentinel(RS_POINTSIZE, kReszDepthResolveSentinel + 1u),
        "near-miss above the sentinel does not trigger");

  // The sentinel VALUE written to a different render state is not the
  // trigger — only RS_POINTSIZE carries the RESZ contract.
  check(!isReszDepthResolveSentinel(RS_POINTSIZE_MIN, kReszDepthResolveSentinel),
        "sentinel value on RS_POINTSIZE_MIN does not trigger a resolve");
  check(!isReszDepthResolveSentinel(RS_POINTSIZE_MAX, kReszDepthResolveSentinel),
        "sentinel value on RS_POINTSIZE_MAX does not trigger a resolve");
  check(!isReszDepthResolveSentinel(RS_FILL_MODE, kReszDepthResolveSentinel),
        "sentinel value on a non-pointsize state does not trigger a resolve");
}

// --- (b) Depth-resolve request construction -------------------------------

struct ReszFixture {
  std::shared_ptr<RecordingBackend> backend = std::make_shared<RecordingBackend>();
  std::shared_ptr<Device> device;

  ReszFixture() {
    Factory factory(BackendLimits{}, backend);
    PresentParameters params{};
    params.backBufferWidth = 64;
    params.backBufferHeight = 64;
    params.backBufferFormat = Format::A8R8G8B8;
    device = factory.createDevice(0, params);
  }
};

void testReszBuildsDepthResolveDesc() {
  ReszFixture fx;

  // Bound multisampled depth-stencil surface = the resolve SOURCE.
  SurfaceDesc msaaDesc{};
  msaaDesc.width = 64;
  msaaDesc.height = 64;
  msaaDesc.format = Format::D24S8;
  msaaDesc.pool = Pool::Default;
  msaaDesc.usage = UsageDepthStencil;
  msaaDesc.depthStencil = true;
  msaaDesc.multiSampleType = MultiSampleType::Four;
  auto msaaDepth = fx.device->createSurface(msaaDesc);
  check(msaaDepth != nullptr, "MSAA depth source surface created");
  check(msaaDepth->valid(), "MSAA depth source surface is valid");

  // Single-sample INTZ destination texture = the resolve DESTINATION; the
  // resolve writes into its level-0 surface.
  TextureDesc intzDesc{};
  intzDesc.width = 64;
  intzDesc.height = 64;
  intzDesc.levels = 1;
  intzDesc.format = Format::INTZ;
  intzDesc.type = TextureType::TwoD;
  intzDesc.pool = Pool::Default;
  intzDesc.usage = UsageTexture | UsageDepthStencil;
  auto intzDest = fx.device->createTexture(intzDesc);
  check(intzDest != nullptr, "INTZ destination texture created");
  auto intzLevel0 = intzDest->surfaceLevel(0);
  check(intzLevel0 != nullptr, "INTZ destination level-0 surface resolves");

  const auto before = fx.backend->depthResolves.size();
  const HResult hr = fx.device->reszDepthResolve(msaaDepth, intzDest);
  checkEq(hr, D3D_OK, "reszDepthResolve returns D3D_OK");

  // Exactly one DepthResolveDesc submitted, carrying the MSAA source handle
  // and the INTZ destination's LEVEL-0 surface handle (not the texture
  // handle) — that is the seam the encoder reads back via findSurface.
  checkEq(fx.backend->depthResolves.size(), before + std::size_t{1},
          "reszDepthResolve submits exactly one depth-resolve request");
  const auto& desc = fx.backend->depthResolves.back();
  checkEq(desc.msaaDepth, msaaDepth->handle(),
          "depth-resolve source is the bound multisampled depth surface");
  checkEq(desc.intzDest, intzLevel0->handle(),
          "depth-resolve destination is the INTZ texture's level-0 surface");
  // The two endpoints must be distinct handles — a resolve into the source
  // itself would be a no-op / malformed request.
  check(desc.msaaDepth != desc.intzDest,
        "depth-resolve source and destination are distinct surfaces");
}

void testReszMissingBindingIsNoOp() {
  // R-FORMAT-11 fire-and-forget contract: a missing source or destination is
  // a benign no-op (D3D_OK, no request submitted), mirroring real-hardware
  // RESZ and the PE emit's no-op guard — not an error.
  ReszFixture fx;

  TextureDesc intzDesc{};
  intzDesc.width = 32;
  intzDesc.height = 32;
  intzDesc.levels = 1;
  intzDesc.format = Format::INTZ;
  intzDesc.usage = UsageTexture | UsageDepthStencil;
  auto intzDest = fx.device->createTexture(intzDesc);

  const auto before = fx.backend->depthResolves.size();

  // Null MSAA source.
  checkEq(fx.device->reszDepthResolve(nullptr, intzDest), D3D_OK,
          "reszDepthResolve with null MSAA source is a no-op (D3D_OK)");
  // Null INTZ destination.
  SurfaceDesc msaaDesc{};
  msaaDesc.width = 32;
  msaaDesc.height = 32;
  msaaDesc.format = Format::D24S8;
  msaaDesc.usage = UsageDepthStencil;
  msaaDesc.depthStencil = true;
  msaaDesc.multiSampleType = MultiSampleType::Four;
  auto msaaDepth = fx.device->createSurface(msaaDesc);
  checkEq(fx.device->reszDepthResolve(msaaDepth, nullptr), D3D_OK,
          "reszDepthResolve with null INTZ destination is a no-op (D3D_OK)");

  checkEq(fx.backend->depthResolves.size(), before,
          "no depth-resolve request submitted when a binding is missing");
}

}  // namespace

int main() {
  try {
    testReszSentinelDetection();
    testReszBuildsDepthResolveDesc();
    testReszMissingBindingIsNoOp();
  } catch (const TestFailure& error) {
    std::cerr << "resz_depth_resolve_spec failed: " << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "resz_depth_resolve_spec unexpected exception: "
              << error.what() << '\n';
    return EXIT_FAILURE;
  }
  std::cout << "resz_depth_resolve_spec passed\n";
  return EXIT_SUCCESS;
}
