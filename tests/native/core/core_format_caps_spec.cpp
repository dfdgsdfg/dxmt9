#include "core_spec_fixtures.hpp"

using namespace dxmt9::core;
using namespace dxmt9::core::fixture;
using namespace dxmt9::core::spec;

namespace {

void testFormatAndCaps() {
  const auto* a8r8g8b8 = findFormatInfo(Format::A8R8G8B8);
  check(a8r8g8b8 != nullptr, "A8R8G8B8 format info missing");
  checkEq(a8r8g8b8->backendFormat, BackendPixelFormat::BGRA8Unorm, "A8R8G8B8 backend format");
  checkEq(a8r8g8b8->support, FormatClass::Required, "A8R8G8B8 support class");
  checkEq(a8r8g8b8->bytesPerPixel, 4u, "A8R8G8B8 bytes per pixel");
  check(a8r8g8b8->renderTarget, "A8R8G8B8 should be renderable");

  const auto* l8 = findFormatInfo(Format::L8);
  check(l8 != nullptr, "L8 format info missing");
  check(!l8->renderTarget, "L8 must not be reported as a render target");

  const auto* r8g8b8 = findFormatInfo(Format::R8G8B8);
  check(r8g8b8 != nullptr, "R8G8B8 format info missing");
  checkEq(r8g8b8->support, FormatClass::Unsupported, "R8G8B8 support class");

  const auto* dxt1 = findFormatInfo(Format::DXT1);
  check(dxt1 != nullptr, "DXT1 format info missing");
  checkEq(dxt1->backendFormat, BackendPixelFormat::BC1_RGBA, "DXT1 backend format");
  checkEq(dxmt9::convert::toPixelFormat(Format::DXT1, BackendLimits{}),
          WMTPixelFormatBC1_RGBA, "DXT1 WMT format");
  checkEq(dxmt9::convert::toCullMode(static_cast<u32>(CullMode::Cw)),
          WMTCullModeFront, "D3DCULL_CW culls clockwise front faces");
  checkEq(dxmt9::convert::toCullMode(static_cast<u32>(CullMode::Ccw)),
          WMTCullModeBack, "D3DCULL_CCW culls counter-clockwise back faces");

  BackendLimits limits{};
  limits.maxTextureSize = 4096;
  limits.maxColorAttachments = 2;
  limits.maxAnisotropy = 8;
  limits.supportsBgr10A2 = false;
  limits.supportsDepth32FloatStencil8 = false;

  Factory factory(limits);
  checkEq(factory.adapterCount(), size_t{1}, "adapter count");
  checkEq(factory.caps(0).maxTextureWidth, 4096u, "max texture width");
  checkEq(factory.caps(0).maxTextureHeight, 4096u, "max texture height");
  checkEq(factory.caps(0).numSimultaneousRTs, 2u, "simultaneous RT count");
  checkEq(factory.caps(0).maxAnisotropy, 8u, "max anisotropy");

  checkEq(factory.checkDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, true), D3D_OK,
          "HAL windowed device type");
  checkEq(factory.checkDeviceType(0, DeviceType::Hal, Format::A8R8G8B8, Format::A8R8G8B8, false), D3D_OK,
          "HAL fullscreen device type");
  checkEq(factory.checkDeviceType(0, DeviceType::Ref, Format::A8R8G8B8, Format::A8R8G8B8, true),
          D3DERR_NOTAVAILABLE, "non-HAL device type");
  const auto modes = factory.enumAdapterModes(0, Format::A8R8G8B8);
  check(!modes.empty(), "adapter modes");
  checkEq(modes.front().width, 640u, "first adapter mode width");
  checkEq(modes.front().height, 480u, "first adapter mode height");
  checkEq(modes.front().format, Format::A8R8G8B8, "first adapter mode format");
  const auto displayMode = factory.getAdapterDisplayMode(0);
  checkEq(displayMode.width, 1920u, "adapter display width");
  checkEq(displayMode.height, 1080u, "adapter display height");
  checkEq(displayMode.format, Format::A8R8G8B8, "adapter display format");
  const auto identifier = factory.getAdapterIdentifier(0);
  checkEq(identifier.description, std::string("NVIDIA GeForce 6800"), "adapter description");
  checkEq(identifier.monitor, 1u, "adapter monitor");
  checkEq(factory.getAdapterMonitor(0), 1u, "adapter monitor lookup");

  // specs/d3d9/gap_d3d9.md §C.7 — D3DCAPS9::AlphaCmpCaps must be sourced from the
  // dedicated alphaCmpCaps slot (not from alphaBlendCaps) and must
  // expose the standard D3D9 comparison set. Every Metal-capable GPU
  // supports all eight comparison ops.
  constexpr u32 kD3DPCMPCAPS_NEVER        = 0x00000001u;
  constexpr u32 kD3DPCMPCAPS_LESS         = 0x00000002u;
  constexpr u32 kD3DPCMPCAPS_EQUAL        = 0x00000004u;
  constexpr u32 kD3DPCMPCAPS_LESSEQUAL    = 0x00000008u;
  constexpr u32 kD3DPCMPCAPS_GREATER      = 0x00000010u;
  constexpr u32 kD3DPCMPCAPS_NOTEQUAL     = 0x00000020u;
  constexpr u32 kD3DPCMPCAPS_GREATEREQUAL = 0x00000040u;
  constexpr u32 kD3DPCMPCAPS_ALWAYS       = 0x00000080u;
  constexpr u32 kAlphaCmpRequired = kD3DPCMPCAPS_NEVER | kD3DPCMPCAPS_LESS |
                                    kD3DPCMPCAPS_EQUAL | kD3DPCMPCAPS_LESSEQUAL |
                                    kD3DPCMPCAPS_GREATER | kD3DPCMPCAPS_NOTEQUAL |
                                    kD3DPCMPCAPS_GREATEREQUAL |
                                    kD3DPCMPCAPS_ALWAYS;
  const auto &caps0 = factory.caps(0);
  check(caps0.alphaCmpCaps != 0u,
        "AlphaCmpCaps must be non-zero (zero GUID-equivalent for legacy apps)");
  check((caps0.alphaCmpCaps & kAlphaCmpRequired) == kAlphaCmpRequired,
        "AlphaCmpCaps must expose the full eight-op comparison set");
  // Cross-check via the C ABI marshaller. fillCCaps populates both
  // the legacy alphaBlendCaps carrier slot and the dedicated
  // alphaCmpCaps slot; both must agree, and the dedicated slot is
  // the canonical source for D3DCAPS9::AlphaCmpCaps.
  D9CCaps cCaps{};
  dxmt9::d3d9::devicec::fillCCaps(caps0, &cCaps);
  checkEq(cCaps.alphaCmpCaps, caps0.alphaCmpCaps,
          "fillCCaps must mirror core::DeviceCaps::alphaCmpCaps into the "
          "dedicated D9CCaps::alphaCmpCaps slot (specs/d3d9/gap_d3d9.md §C.7 fix)");
  checkEq(cCaps.alphaBlendCaps, caps0.alphaCmpCaps,
          "fillCCaps must keep the legacy alphaBlendCaps carrier in sync "
          "for back-compat with older PE bridge reads");

  // specs/d3d9/gap_d3d9.md §C.9 — D3DADAPTER_IDENTIFIER9::DeviceIdentifier must be a
  // non-zero, byte-stable per-adapter GUID. Several legacy D3D9
  // titles refuse to launch when it's the zero GUID (they use it as
  // an installation fingerprint).
  bool anyNonZero = false;
  for (auto byte : identifier.deviceIdentifier) {
    if (byte != 0u) {
      anyNonZero = true;
      break;
    }
  }
  check(anyNonZero,
        "AdapterIdentifier::deviceIdentifier must not be the zero GUID");

  // Two consecutive calls must return byte-equal GUIDs (determinism
  // contract — used as an installation fingerprint).
  const auto identifier2 = factory.getAdapterIdentifier(0);
  checkEq(identifier2.deviceIdentifier == identifier.deviceIdentifier, true,
          "AdapterIdentifier::deviceIdentifier must be byte-stable across "
          "consecutive GetAdapterIdentifier calls");

  // WHQL level intentionally stays 0 — Apple Silicon GPUs are not
  // WHQL-certified, which the D3D9 spec allows.
  checkEq(identifier.whqlLevel, 0u,
          "AdapterIdentifier::whqlLevel stays 0 on Apple Silicon");

  checkEq(factory.checkDeviceFormat(0, Format::A8R8G8B8, UsageTexture), D3D_OK, "A8R8G8B8 texture support");
  checkEq(factory.checkDeviceFormat(0, Format::L8, UsageRenderTarget), D3DERR_NOTAVAILABLE,
          "L8 render-target support");
  checkEq(factory.checkDeviceFormat(0, Format::A8R8G8B8,
                                    UsageRenderTarget | UsageQueryPostPixelShaderBlending),
          D3D_OK, "A8R8G8B8 post-pixel blending query support");
  checkEq(factory.checkDeviceFormat(0, Format::R32F,
                                    UsageRenderTarget | UsageQueryPostPixelShaderBlending),
          D3D_OK, "R32F post-pixel blending query support");
  checkEq(factory.checkDeviceFormat(0, Format::G32R32F,
                                    UsageRenderTarget | UsageQueryPostPixelShaderBlending),
          D3DERR_NOTAVAILABLE, "G32R32F post-pixel blending query is unavailable");
  check((dxmt9::d3d9::devicec::checkDeviceFormatUsageFromD3D(0x00080000u) &
         UsageQueryPostPixelShaderBlending) != 0u,
        "D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING reaches CheckDeviceFormat core usage");
  checkEq(factory.checkDeviceFormat(0, Format::A2B10G10R10, UsageTexture), D3DERR_NOTAVAILABLE,
          "A2B10G10R10 support gate");
  checkEq(factory.checkDeviceFormat(1, Format::A8R8G8B8, UsageTexture), D3DERR_INVALIDCALL,
          "invalid adapter index");
  checkEq(factory.checkDeviceMultiSampleType(0, Format::A8R8G8B8, MultiSampleType::Four), D3D_OK,
          "4x MSAA support");
  checkEq(factory.checkDeviceMultiSampleType(0, Format::A8R8G8B8, MultiSampleType::Eight), D3DERR_NOTAVAILABLE,
          "8x MSAA support");
}

void testSigned3DcAndUnsupportedFormatCaps() {
  BackendLimits limits{};
  limits.supportsBgr10A2 = true;
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;
  Factory factory(limits);

  const auto* v8u8 = findFormatInfo(Format::V8U8);
  check(v8u8 != nullptr, "V8U8 format info missing");
  checkEq(v8u8->backendFormat, BackendPixelFormat::RG8Snorm,
          "V8U8 backend format");
  checkEq(v8u8->support, FormatClass::Required, "V8U8 support class");
  checkEq(v8u8->bytesPerPixel, 2u, "V8U8 bytes per pixel");
  checkEq(dxmt9::convert::toPixelFormat(Format::V8U8, limits),
          WMTPixelFormatRG8Snorm, "V8U8 WMT signed-normal pixel format");
  checkEq(factory.checkDeviceFormat(0, Format::V8U8, UsageTexture), D3D_OK,
          "V8U8 texture caps");

  const auto* q8w8v8u8 = findFormatInfo(Format::Q8W8V8U8);
  check(q8w8v8u8 != nullptr, "Q8W8V8U8 format info missing");
  checkEq(q8w8v8u8->backendFormat, BackendPixelFormat::RGBA8Snorm,
          "Q8W8V8U8 backend format");
  checkEq(q8w8v8u8->bytesPerPixel, 4u, "Q8W8V8U8 bytes per pixel");
  checkEq(dxmt9::convert::toPixelFormat(Format::Q8W8V8U8, limits),
          WMTPixelFormatRGBA8Snorm,
          "Q8W8V8U8 WMT signed-normal pixel format");

  const auto* v16u16 = findFormatInfo(Format::V16U16);
  check(v16u16 != nullptr, "V16U16 format info missing");
  checkEq(v16u16->backendFormat, BackendPixelFormat::RG16Snorm,
          "V16U16 backend format");
  checkEq(v16u16->bytesPerPixel, 4u, "V16U16 bytes per pixel");
  checkEq(dxmt9::convert::toPixelFormat(Format::V16U16, limits),
          WMTPixelFormatRG16Snorm,
          "V16U16 WMT signed-normal pixel format");

  const auto* ati2 = findFormatInfo(Format::ATI2);
  check(ati2 != nullptr, "ATI2 format info missing");
  checkEq(ati2->backendFormat, BackendPixelFormat::BC5_RGUnorm,
          "ATI2 backend format");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(843666497u), Format::ATI2,
          "ATI2 FOURCC maps to core format");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::BC5), 843666497u,
          "BC5 maps to ATI2 D3D FOURCC");
  checkEq(dxmt9::convert::toPixelFormat(Format::ATI2, limits),
          WMTPixelFormatBC5_RGUnorm, "ATI2 WMT BC5 pixel format");
  checkEq(formatBlockBytes(Format::ATI2), 16u, "ATI2 block bytes");
  checkEq(formatRowPitch(Format::ATI2, 5u), 32u,
          "ATI2 row pitch rounds to 4-wide blocks");
  checkEq(formatByteSize(Format::ATI2, 5u, 7u), std::size_t{64},
          "ATI2 byte size rounds to 4x4 blocks");
  checkEq(factory.checkDeviceFormat(0, Format::ATI2, UsageTexture), D3D_OK,
          "ATI2 texture caps");
  checkEq(factory.checkDeviceFormat(0, Format::ATI2, UsageRenderTarget),
          D3DERR_NOTAVAILABLE, "ATI2 render-target caps rejected");

  const auto* cxv8u8 = findFormatInfo(Format::CxV8U8);
  check(cxv8u8 != nullptr, "CxV8U8 format info missing");
  checkEq(cxv8u8->support, FormatClass::Unsupported,
          "CxV8U8 unsupported vendor format class");
  checkEq(cxv8u8->backendFormat, BackendPixelFormat::Unknown,
          "CxV8U8 has no backend format");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(117u), Format::CxV8U8,
          "CxV8U8 D3DFORMAT maps to explicit unsupported core value");
  checkEq(factory.checkDeviceFormat(0, Format::CxV8U8, UsageTexture),
          D3DERR_NOTAVAILABLE, "CxV8U8 texture caps rejected");
  checkEq(factory.checkDeviceFormat(0, Format::Unknown, UsageTexture),
          D3DERR_NOTAVAILABLE, "unknown/null-like format caps rejected");
}

void testVendorDepthPseudoFormats() {
  // specs/d3d9/gap_d3d9.md §C.5 — DF16/DF24 are vendor depth-as-texture pseudo-
  // formats (FOURCC 'DF16'/'DF24'), handled exactly like INTZ: a
  // depth-stencil texture that is also sampleable. They previously fell
  // through fmtFromD3D to Format::Unknown -> NOTAVAILABLE.
  //
  // FOURCC bytes (little-endian, matching the INTZ literal convention):
  //   'DF16' = 'D'|'F'<<8|'1'<<16|'6'<<24 = 0x36314644 = 909198916
  //   'DF24' = 'D'|'F'<<8|'2'<<16|'4'<<24 = 0x34324644 = 875710020
  constexpr u32 kD3DFmtDF16 = 909198916u;   // 0x36314644
  constexpr u32 kD3DFmtDF24 = 875710020u;   // 0x34324644

  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtDF16), Format::DF16,
          "DF16 FOURCC maps to core Format::DF16");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtDF24), Format::DF24,
          "DF24 FOURCC maps to core Format::DF24");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::DF16), kD3DFmtDF16,
          "DF16 core format round-trips back to FOURCC");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::DF24), kD3DFmtDF24,
          "DF24 core format round-trips back to FOURCC");

  // Mirror INTZ's format-table shape: sampleable depth-stencil texture,
  // not a color render target.
  const auto* df16 = findFormatInfo(Format::DF16);
  check(df16 != nullptr, "DF16 format info missing");
  if (df16 != nullptr) {
    check(df16->depthStencil, "DF16 is a depth-stencil format");
    check(!df16->renderTarget, "DF16 is not a color render target");
    checkEq(df16->backendFormat, BackendPixelFormat::Depth16Unorm,
            "DF16 backend pixel format is Depth16Unorm");
  }
  const auto* df24 = findFormatInfo(Format::DF24);
  check(df24 != nullptr, "DF24 format info missing");
  if (df24 != nullptr) {
    check(df24->depthStencil, "DF24 is a depth-stencil format");
    check(!df24->renderTarget, "DF24 is not a color render target");
    checkEq(df24->backendFormat, BackendPixelFormat::Depth32Float,
            "DF24 backend pixel format mirrors INTZ's Depth32Float target");
  }

  // CheckDeviceFormat: like INTZ, depth-stencil texture usage succeeds
  // and color render-target usage is rejected.
  BackendLimits limits{};
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;
  Factory factory(limits);
  checkEq(factory.checkDeviceFormat(0, Format::DF16, UsageDepthStencil), D3D_OK,
          "DF16 depth-stencil texture support");
  checkEq(factory.checkDeviceFormat(0, Format::DF24, UsageDepthStencil), D3D_OK,
          "DF24 depth-stencil texture support");
  checkEq(factory.checkDeviceFormat(0, Format::DF16, UsageRenderTarget),
          D3DERR_NOTAVAILABLE, "DF16 color render-target usage rejected");
  checkEq(factory.checkDeviceFormat(0, Format::DF24, UsageRenderTarget),
          D3DERR_NOTAVAILABLE, "DF24 color render-target usage rejected");
}

void testD32LockableAndQ16W16V16U16Formats() {
  // specs/d3d9/gap_d3d9.md §C.12 #7 — D3DFMT_D32_LOCKABLE (code 84) and
  // D3DFMT_Q16W16V16U16 (code 110) previously fell through fmtFromD3D to
  // Format::Unknown -> NOTAVAILABLE. D32_LOCKABLE mirrors the existing
  // D32F_LOCKABLE (lockable 32-bit depth; Metal has no 32-bit-int depth so
  // both target Depth32Float). Q16W16V16U16 is a 4x16-bit SIGNED normalized
  // color format mirroring the existing (unsigned) A16B16G16R16, but mapped
  // to the signed Metal pixel format RGBA16Snorm.
  constexpr u32 kD3DFmtD32Lockable = 84u;
  constexpr u32 kD3DFmtQ16W16V16U16 = 110u;

  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtD32Lockable),
          Format::D32_LOCKABLE, "D32_LOCKABLE code 84 maps to Format::D32_LOCKABLE");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtQ16W16V16U16),
          Format::Q16W16V16U16, "Q16W16V16U16 code 110 maps to Format::Q16W16V16U16");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::D32_LOCKABLE), kD3DFmtD32Lockable,
          "D32_LOCKABLE round-trips back to code 84");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::Q16W16V16U16), kD3DFmtQ16W16V16U16,
          "Q16W16V16U16 round-trips back to code 110");

  // D32_LOCKABLE: depth-stencil, lockable, not a color render target.
  // Mirrors D32F_LOCKABLE onto Depth32Float (Metal has no 32-bit-int depth).
  const auto* d32l = findFormatInfo(Format::D32_LOCKABLE);
  check(d32l != nullptr, "D32_LOCKABLE format info missing");
  if (d32l != nullptr) {
    check(d32l->depthStencil, "D32_LOCKABLE is a depth-stencil format");
    check(!d32l->renderTarget, "D32_LOCKABLE is not a color render target");
    check(d32l->lockable, "D32_LOCKABLE is lockable");
    checkEq(d32l->bytesPerPixel, 4u, "D32_LOCKABLE is 4 bytes per pixel");
    checkEq(d32l->backendFormat, BackendPixelFormat::Depth32Float,
            "D32_LOCKABLE backend pixel format mirrors D32F_LOCKABLE's Depth32Float");
  }

  // Q16W16V16U16: 4x16-bit signed normalized color, lockable, 8 bpp.
  const auto* q16 = findFormatInfo(Format::Q16W16V16U16);
  check(q16 != nullptr, "Q16W16V16U16 format info missing");
  if (q16 != nullptr) {
    check(!q16->depthStencil, "Q16W16V16U16 is a color format, not depth-stencil");
    check(q16->lockable, "Q16W16V16U16 is lockable");
    checkEq(q16->bytesPerPixel, 8u, "Q16W16V16U16 is 8 bytes per pixel");
    checkEq(q16->backendFormat, BackendPixelFormat::RGBA16Snorm,
            "Q16W16V16U16 backend pixel format is the signed RGBA16Snorm");
  }

  // Runtime Metal pixel-format mapping.
  checkEq(dxmt9::convert::toPixelFormat(Format::D32_LOCKABLE, BackendLimits{}),
          WMTPixelFormatDepth32Float, "D32_LOCKABLE -> Depth32Float WMT format");
  checkEq(dxmt9::convert::toPixelFormat(Format::Q16W16V16U16, BackendLimits{}),
          WMTPixelFormatRGBA16Snorm, "Q16W16V16U16 -> RGBA16Snorm WMT format");

  // Depth aspect: D32_LOCKABLE has a depth aspect; Q16W16V16U16 does not.
  check(dxmt9::convert::formatHasDepthAspect(Format::D32_LOCKABLE),
        "D32_LOCKABLE has a depth aspect");
  check(!dxmt9::convert::formatHasDepthAspect(Format::Q16W16V16U16),
        "Q16W16V16U16 has no depth aspect");

  // CheckDeviceFormat outcomes mirror the analogs: D32_LOCKABLE accepts
  // depth-stencil usage like D32F_LOCKABLE; Q16W16V16U16 is a sampleable
  // color texture and rejects depth-stencil usage.
  BackendLimits limits{};
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;
  Factory factory(limits);
  checkEq(factory.checkDeviceFormat(0, Format::D32_LOCKABLE, UsageDepthStencil),
          D3D_OK, "D32_LOCKABLE depth-stencil texture support");
  checkEq(factory.checkDeviceFormat(0, Format::D32_LOCKABLE, UsageTexture), D3D_OK,
          "D32_LOCKABLE texture usage support");
  checkEq(factory.checkDeviceFormat(0, Format::Q16W16V16U16, UsageTexture), D3D_OK,
          "Q16W16V16U16 texture usage support");
  checkEq(factory.checkDeviceFormat(0, Format::Q16W16V16U16, UsageDepthStencil),
          D3DERR_NOTAVAILABLE, "Q16W16V16U16 depth-stencil usage rejected");
}

void testFourccPseudoFormatClassification() {
  // specs/d3d9/formats/requirements.md R-FORMAT-11..14 — classification of
  // five vendor FOURCC pseudo-formats. CLASSIFICATION ONLY: the runtime
  // behaviour (RESZ depth resolve, NULL colorless pass, ATOC
  // alpha-to-coverage render state) is wired by separate follow-up agents.
  //
  // FOURCC bytes (little-endian, matching the INTZ literal convention
  //   value = c0 | c1<<8 | c2<<16 | c3<<24):
  //   'RESZ' = 'R'|'E'<<8|'S'<<16|'Z'<<24 = 0x5A534552 = 1515406674
  //   'NULL' = 'N'|'U'<<8|'L'<<16|'L'<<24 = 0x4C4C554E = 1280070990
  //   'ATOC' = 'A'|'T'<<8|'O'<<16|'C'<<24 = 0x434F5441 = 1129272385
  //   'NVDB' = 'N'|'V'<<8|'D'<<16|'B'<<24 = 0x4244564E = 1111774798
  //   'RAWZ' = 'R'|'A'<<8|'W'<<16|'Z'<<24 = 0x5A574152 = 1515667794
  constexpr u32 kD3DFmtResz = 1515406674u;  // 0x5A534552
  constexpr u32 kD3DFmtNull = 1280070990u;  // 0x4C4C554E
  constexpr u32 kD3DFmtAtoc = 1129272385u;  // 0x434F5441
  constexpr u32 kD3DFmtNvdb = 1111774798u;  // 0x4244564E
  constexpr u32 kD3DFmtRawz = 1515667794u;  // 0x5A574152

  // fmtFromD3D / fmtToD3D round-trips.
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtResz), Format::Resz,
          "RESZ FOURCC maps to core Format::Resz");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtNull), Format::NullRt,
          "NULL FOURCC maps to core Format::NullRt");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtAtoc), Format::Atoc,
          "ATOC FOURCC maps to core Format::Atoc");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtNvdb), Format::Nvdb,
          "NVDB FOURCC maps to core Format::Nvdb");
  checkEq(dxmt9::d3d9::devicec::fmtFromD3D(kD3DFmtRawz), Format::Rawz,
          "RAWZ FOURCC maps to core Format::Rawz");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::Resz), kD3DFmtResz,
          "RESZ core format round-trips back to FOURCC");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::NullRt), kD3DFmtNull,
          "NULL core format round-trips back to FOURCC");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::Atoc), kD3DFmtAtoc,
          "ATOC core format round-trips back to FOURCC");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::Nvdb), kD3DFmtNvdb,
          "NVDB core format round-trips back to FOURCC");
  checkEq(dxmt9::d3d9::devicec::fmtToD3D(Format::Rawz), kD3DFmtRawz,
          "RAWZ core format round-trips back to FOURCC");

  // R-FORMAT-12: NULL is a render-target-capable classification (no real
  // color storage; colorless-pass behaviour deferred to a follow-up agent).
  const auto* nullRt = findFormatInfo(Format::NullRt);
  check(nullRt != nullptr, "NULL format info missing");
  if (nullRt != nullptr) {
    check(nullRt->renderTarget, "NULL is render-target capable");
    check(!nullRt->depthStencil, "NULL is not a depth-stencil format");
    check(!nullRt->compressed, "NULL is not compressed");
  }

  // R-FORMAT-14 + RAWZ: explicitly classified Unsupported so they cannot
  // slip through as ordinary color formats (R-FORMAT-7). No Metal mapping.
  const auto* nvdb = findFormatInfo(Format::Nvdb);
  check(nvdb != nullptr, "NVDB format info missing");
  if (nvdb != nullptr) {
    checkEq(nvdb->support, FormatClass::Unsupported, "NVDB is Unsupported");
    checkEq(nvdb->backendFormat, BackendPixelFormat::Unknown,
            "NVDB has no Metal pixel format");
  }
  const auto* rawz = findFormatInfo(Format::Rawz);
  check(rawz != nullptr, "RAWZ format info missing");
  if (rawz != nullptr) {
    checkEq(rawz->support, FormatClass::Unsupported, "RAWZ is Unsupported");
    checkEq(rawz->backendFormat, BackendPixelFormat::Unknown,
            "RAWZ has no Metal pixel format");
  }

  BackendLimits limits{};
  limits.supportsDepth24Stencil8 = true;
  limits.supportsDepth32FloatStencil8 = true;
  Factory factory(limits);

  // R-FORMAT-12: CheckDeviceFormat(NULL, RENDERTARGET) -> D3D_OK.
  checkEq(factory.checkDeviceFormat(0, Format::NullRt, UsageRenderTarget),
          D3D_OK, "NULL render-target usage supported");

  // R-FORMAT-11: CheckDeviceFormat(RESZ) -> D3DERR_NOTAVAILABLE. RESZ is a
  // write-only MSAA-depth-resolve trigger (the D3DRS_POINTSIZE sentinel), NOT
  // a queryable/creatable surface, so the cap query must report no support
  // (Wine vendor_policy_resz_caps). The resolve trigger path is separate and
  // does not consult CheckDeviceFormat.
  checkEq(factory.checkDeviceFormat(0, Format::Resz, 0u), D3DERR_NOTAVAILABLE,
          "RESZ CheckDeviceFormat reports NOTAVAILABLE");

  // R-FORMAT-13: CheckDeviceFormat(ATOC) -> D3D_OK consistently. ATOC is a
  // render-state hack surfaced as a creatable-looking FOURCC; classification
  // reports support, the alpha-to-coverage behaviour is a follow-up agent.
  checkEq(factory.checkDeviceFormat(0, Format::Atoc, 0u), D3D_OK,
          "ATOC usage supported");

  // R-FORMAT-14 + RAWZ: both probes report NOTAVAILABLE for any usage.
  checkEq(factory.checkDeviceFormat(0, Format::Nvdb, 0u), D3DERR_NOTAVAILABLE,
          "NVDB reports NOTAVAILABLE");
  checkEq(factory.checkDeviceFormat(0, Format::Rawz, 0u), D3DERR_NOTAVAILABLE,
          "RAWZ reports NOTAVAILABLE");
  checkEq(factory.checkDeviceFormat(0, Format::Nvdb, UsageTexture),
          D3DERR_NOTAVAILABLE, "NVDB texture usage rejected");
  checkEq(factory.checkDeviceFormat(0, Format::Rawz, UsageTexture),
          D3DERR_NOTAVAILABLE, "RAWZ texture usage rejected");
}

void testHelpers() {
  const Viewport viewport{0, 0, 800, 600, 0.0f, 1.0f};
  const auto fixup = halfPixelFixup(viewport);
  checkNear(fixup[0], 1.0f / 800.0f, 1.0e-6f, "half-pixel X fixup");
  checkNear(fixup[1], 1.0f / 600.0f, 1.0e-6f, "half-pixel Y fixup");

  const auto zeroFixup = halfPixelFixup(Viewport{});
  checkEq(zeroFixup[0], 0.0f, "zero viewport X fixup");
  checkEq(zeroFixup[1], 0.0f, "zero viewport Y fixup");

  const std::vector<u32> fanIndices{0, 1, 2, 3};
  const auto triangles = decomposeTriangleFanIndices(fanIndices);
  const std::vector<u32> expected{0, 1, 2, 0, 2, 3};
  checkEq(triangles, expected, "triangle fan decomposition");

  const std::vector<u8> upload{0x22, 0x33, 0x44, 0x55};
  const auto converted = convertTextureUpload(Format::A8R8G8B8, 1, 1, upload);
  checkBytes(std::span<const u8>(converted.data(), converted.size()),
             std::span<const u8>(upload.data(), upload.size()), "texture upload conversion");

  check(hashString("dxmt9") != 0, "hashString should not be zero");
}

void testDeviceCPresentIntervalMapping() {
  constexpr u32 kD3DPRESENT_INTERVAL_DEFAULT = 0x00000000u;
  constexpr u32 kD3DPRESENT_INTERVAL_ONE = 0x00000001u;
  constexpr u32 kD3DPRESENT_INTERVAL_TWO = 0x00000002u;
  constexpr u32 kD3DPRESENT_INTERVAL_IMMEDIATE = 0x80000000u;

  using dxmt9::d3d9::devicec::presentIntervalFromD3D;
  using dxmt9::d3d9::devicec::presentIntervalToD3D;

  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_DEFAULT), PresentInterval::Default,
          "default present interval bridge");
  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_ONE), PresentInterval::Default,
          "one present interval bridge");
  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_TWO), PresentInterval::Two,
          "two present interval bridge");
  checkEq(presentIntervalFromD3D(kD3DPRESENT_INTERVAL_IMMEDIATE), PresentInterval::Immediate,
          "immediate present interval bridge");

  checkEq(presentIntervalToD3D(PresentInterval::Immediate), kD3DPRESENT_INTERVAL_IMMEDIATE,
          "immediate present interval bridge return");
  checkEq(presentIntervalToD3D(PresentInterval::Default), kD3DPRESENT_INTERVAL_ONE,
          "default present interval bridge return");
  checkEq(presentIntervalToD3D(PresentInterval::Two), kD3DPRESENT_INTERVAL_TWO,
          "two present interval bridge return");

  D9CPresentParams cParams{};
  cParams.presentationInterval = kD3DPRESENT_INTERVAL_IMMEDIATE;
  checkEq(dxmt9::d3d9::devicec::ppFromC(cParams).presentationInterval, PresentInterval::Immediate,
          "present params preserve immediate interval");

  // specs/d3d9/gap_d3d9.md C.12: MultiSampleQuality / Flags / FullScreen_RefreshRateInHz must
  // propagate from the C ABI struct into core::PresentParameters via ppFromC.
  D9CPresentParams cMs{};
  cMs.multiSampleQuality = 3u;
  cMs.flags = 0x800u;  // D3DPRESENTFLAG_LOCKABLE_BACKBUFFER
  cMs.fullScreenRefreshRateHz = 60u;
  auto pMs = dxmt9::d3d9::devicec::ppFromC(cMs);
  checkEq(pMs.multiSampleQuality, 3u, "present params copy multiSampleQuality");
  checkEq(pMs.flags, 0x800u, "present params copy flags");
  checkEq(pMs.fullScreenRefreshRateInHz, 60u, "present params copy fullscreen refresh rate");
}

void testRingArenaExhaustionFallsBack() {
  dxmt9::scratch::RingArena arena{64};

  auto* first = arena.allocateBytes(48, 16, 1);
  check(first != nullptr, "ring arena first allocation");
  auto* second = arena.allocateBytes(32, 16, 2);
  check(second == nullptr, "ring arena exhaustion returns null");

  arena.reclaim(1);
  auto* third = arena.allocateBytes(32, 16, 3);
  check(third != nullptr, "ring arena reuses reclaimed storage");

  arena.reclaim(3);
  auto* fourth = arena.allocateBytes(64, 16, 4);
  check(fourth != nullptr, "ring arena resets after full reclaim");
}

}  // namespace

int main() {
  try {
    testFormatAndCaps();
    testSigned3DcAndUnsupportedFormatCaps();
    testVendorDepthPseudoFormats();
    testD32LockableAndQ16W16V16U16Formats();
    testFourccPseudoFormatClassification();
    testHelpers();
    testDeviceCPresentIntervalMapping();
    testRingArenaExhaustionFallsBack();
  } catch (const TestFailure& error) {
    std::cerr << error.what() << '\n';
    return EXIT_FAILURE;
  } catch (const std::exception& error) {
    std::cerr << "unexpected exception: " << error.what() << '\n';
    return EXIT_FAILURE;
  }

  return EXIT_SUCCESS;
}
