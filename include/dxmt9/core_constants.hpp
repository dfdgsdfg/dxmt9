#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dxmt9 {
class Device;     // forward decl — defined in src/dxmt9/dxmt9_device.hpp
class Presenter;  // forward decl — defined in src/dxmt9/dxmt9_presenter.hpp
}

namespace dxmt9::core {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using f32 = float;
using HRESULT = i32;
using HResult = HRESULT;

constexpr HRESULT S_OK = 0;
constexpr HRESULT S_FALSE = 1;
constexpr HRESULT D3D_OK = 0;
constexpr HRESULT E_NOTIMPL = static_cast<HRESULT>(0x80004001);
constexpr HRESULT D3DERR_INVALIDCALL = static_cast<HRESULT>(0x8876086c);
constexpr HRESULT D3DERR_NOTAVAILABLE = static_cast<HRESULT>(0x8876086a);
constexpr HRESULT D3DERR_DEVICELOST = static_cast<HRESULT>(0x88760868);
constexpr HRESULT D3DERR_DEVICENOTRESET = static_cast<HRESULT>(0x88760869);
constexpr HRESULT D3DERR_NOTFOUND = static_cast<HRESULT>(0x88760866);
constexpr HRESULT S_PRESENT_OCCLUDED = static_cast<HRESULT>(0x08760878);

inline constexpr u32 kMaxAdapters = 4;
inline constexpr u32 kMaxRenderTargets = 4;
inline constexpr u32 kDefaultFrameLatency = 4;
inline constexpr u32 kMaxFrameLatency = 30;
inline constexpr u32 kMaxStreams = 16;
inline constexpr u32 kMaxFragmentSamplers = 16;
inline constexpr u32 kMaxVertexTextureSamplers = 4;
inline constexpr u32 kVertexTextureSampler0 = kMaxFragmentSamplers;
inline constexpr u32 kMaxTextures = kMaxFragmentSamplers + kMaxVertexTextureSamplers;
inline constexpr u32 kMaxSamplers = kMaxTextures;
inline constexpr u32 kMaxTextureStages = 8;
inline constexpr u32 kMaxLights = 8;
inline constexpr u32 kMaxClipPlanes = 6;
inline constexpr u32 kMaxVertexConstants = 256;
inline constexpr u32 kMaxPixelConstants = 224;
inline constexpr u32 kMaxIntegerConstants = 16;
inline constexpr u32 kMaxBoolConstants = 16;
inline constexpr u32 kMaxStateSlots = 256;
inline constexpr u32 kMaxTextureStageStates = 64;
inline constexpr u32 kMaxSamplerStates = 64;
// D3D9 MAXD3DDECLLENGTH is 64 (including the end-marker). Snapshot capacity
// kept fixed at 32 — production fixed-function and shader pipelines emit far
// fewer vertex elements per declaration; the capacity bounds the worst case.
inline constexpr u32 kMaxVertexElements = 32;

struct Handle {
  u64 value = 0;

  constexpr explicit operator bool() const noexcept { return value != 0; }
  friend constexpr bool operator==(const Handle&, const Handle&) = default;
};

using BufferHandle = Handle;
using TextureHandle = Handle;
using SurfaceHandle = Handle;
using SwapChainHandle = Handle;

// Queue-local opaque identifier for the per-swapchain Presenter binding.
// CommandQueue encodes the value as (generation << 32) | (slot + 1) so a
// zero value is always the invalid sentinel — never collides with a real
// binding. Callers treat the value as opaque.
//
// PE/unix wire records (`SwapDesc` crossing the bridge) carry only this
// POD identifier; the unix-side queue resolves it back to a Presenter
// pointer + pending drawable token. Keeping the wire pointer-free is the
// codebase convention (`agents/rules/codebase_conventions.rules.md`,
// "Cross-boundary records must be POD, … pointer-free").
struct PresentId {
  u64 value = 0;

  constexpr explicit operator bool() const noexcept { return value != 0; }
  friend constexpr bool operator==(const PresentId&, const PresentId&) = default;
};

enum class Pool : u32 {
  Default,
  Managed,
  SystemMem,
  Scratch,
};

enum class DeviceType : u32 {
  Hal,
  Ref,
  NullRef,
};

enum class PrimitiveType : u32 {
  PointList,
  LineList,
  LineStrip,
  TriangleList,
  TriangleStrip,
  TriangleFan,
};

enum class IndexType : u32 {
  UInt16,
  UInt32,
};

enum class TextureType : u32 {
  TwoD,
  Cube,
  Volume,
  Array2D,
};

enum class TextureOp : u32 {
  Disable = 1,
  SelectArg1 = 2,
  SelectArg2 = 3,
  Modulate = 4,
  Modulate2x = 5,
  Modulate4x = 6,
  Add = 7,
  AddSigned = 8,
  AddSigned2x = 9,
  Subtract = 10,
  AddSmooth = 11,
  DotProduct3 = 24,
  Lerp = 26,
  BumpEnvMap = 22,
  BumpEnvMapLuminance = 23,
};

enum class CompareFunc : u32 {
  Never = 1,
  Less = 2,
  Equal = 3,
  LessEqual = 4,
  Greater = 5,
  NotEqual = 6,
  GreaterEqual = 7,
  Always = 8,
};

enum class BlendOp : u32 {
  Add = 1,
  Subtract = 2,
  RevSubtract = 3,
  Min = 4,
  Max = 5,
};

enum class StencilOp : u32 {
  Keep = 1,
  Zero = 2,
  Replace = 3,
  IncrSat = 4,
  DecrSat = 5,
  Invert = 6,
  Incr = 7,
  Decr = 8,
};

enum class BlendFactor : u32 {
  Zero = 1,
  One = 2,
  SrcColor = 3,
  InvSrcColor = 4,
  SrcAlpha = 5,
  InvSrcAlpha = 6,
  DestAlpha = 7,
  InvDestAlpha = 8,
  DestColor = 9,
  InvDestColor = 10,
  SrcAlphaSat = 11,
  BothSrcAlpha = 12,
  BothInvSrcAlpha = 13,
  BlendFactor = 14,
  InvBlendFactor = 15,
};

enum class CullMode : u32 {
  None = 1,
  Cw = 2,
  Ccw = 3,
};

enum class FogMode : u32 {
  None = 0,
  Linear = 1,
  Exp = 2,
  Exp2 = 3,
};

enum class LightType : u32 {
  Directional = 1,
  Point = 2,
  Spot = 3,
};

enum class PresentInterval : u32 {
  Immediate = 0,
  Default = 1,
  Two = 2,
};

enum class MultiSampleType : u32 {
  None = 1,
  Two = 2,
  Four = 4,
  Eight = 8,
};

enum class QueryType : u32 {
  Event,
  Occlusion,
  Timestamp,
  TimestampDisjoint,
  TimestampFreq,
};

enum class StateBlockType : u32 {
  Recorded = 0,
  All = 1,
  PixelState = 2,
  VertexState = 3,
};

enum class Format : u32 {
  Unknown,
  A8R8G8B8,
  X8R8G8B8,
  A8B8G8R8,
  X8B8G8R8,
  R5G6B5,
  A1R5G5B5,
  X1R5G5B5,
  A4R4G4B4,
  A8,
  R8G8B8,
  A16B16G16R16F,
  A32B32G32R32F,
  G16R16F,
  R16F,
  G32R32F,
  R32F,
  A16B16G16R16,
  G16R16,
  A2R10G10B10,
  A2B10G10R10,
  L8,
  L16,
  A8L8,
  V8U8,
  Q8W8V8U8,
  V16U16,
  CxV8U8,
  DXT1,
  DXT2,
  DXT3,
  DXT4,
  DXT5,
  ATI1,
  BC4,
  ATI2,
  BC5,
  D24S8,
  D24X8,
  D16,
  D32,
  D32F_LOCKABLE,
  D16_LOCKABLE,
  D15S1,
  D24X4S4,
  D24FS8,
  S8_LOCKABLE,
  INDEX16,
  INDEX32,
};

enum class BackendPixelFormat : u32 {
  Unknown,
  BGRA8Unorm,
  RGBA8Unorm,
  B5G6R5Unorm,
  BGR5A1Unorm,
  ABGR4Unorm,
  A8Unorm,
  RGBA16Float,
  RGBA32Float,
  RG16Float,
  R16Float,
  RG32Float,
  R32Float,
  RGBA16Unorm,
  RG16Unorm,
  RGB10A2Unorm,
  BGR10A2Unorm,
  R8Unorm,
  R16Unorm,
  RG8Unorm,
  RG8Snorm,
  RGBA8Snorm,
  RG16Snorm,
  BC1_RGBA,
  BC2_RGBA,
  BC3_RGBA,
  BC4_RUnorm,
  BC5_RGUnorm,
  Depth24Unorm_Stencil8,
  Depth32Float,
  Depth32Float_Stencil8,
  Depth16Unorm,
};

enum class FormatClass {
  Required,
  Optional,
  Unsupported,
};

inline constexpr u32 UsageTexture = 1u << 0;
inline constexpr u32 UsageRenderTarget = 1u << 1;
inline constexpr u32 UsageDepthStencil = 1u << 2;
inline constexpr u32 UsageDynamic = 1u << 3;
inline constexpr u32 UsageAutoGenMipmap = 1u << 4;
inline constexpr u32 UsageVertexBuffer = 1u << 5;
inline constexpr u32 UsageIndexBuffer = 1u << 6;
inline constexpr u32 UsageWriteOnly = 1u << 7;
inline constexpr u32 UsageDiscard = 1u << 8;
inline constexpr u32 UsageNoOverwrite = 1u << 9;

inline constexpr u32 RS_LIGHTING = 137;
inline constexpr u32 RS_SPECULAR_ENABLE = 29;
inline constexpr u32 RS_NORMALIZE_NORMALS = 143;
inline constexpr u32 RS_FOG_TABLE_MODE = 35;
inline constexpr u32 RS_FOG_FROM_VERTEX = 140;
inline constexpr u32 RS_RANGE_FOG = 48;
inline constexpr u32 RS_ALPHA_TEST_ENABLE = 15;
inline constexpr u32 RS_ALPHA_FUNC = 25;
inline constexpr u32 RS_ALPHA_REF = 24;
inline constexpr u32 RS_FOG_ENABLE = 28;
inline constexpr u32 RS_FOG_COLOR = 34;
inline constexpr u32 RS_FOG_START = 36;
inline constexpr u32 RS_FOG_END = 37;
inline constexpr u32 RS_FOG_DENSITY = 38;
inline constexpr u32 RS_AMBIENT = 139;
inline constexpr u32 RS_DIFFUSE_MATERIAL_SOURCE = 145;
inline constexpr u32 RS_SPECULAR_MATERIAL_SOURCE = 146;
inline constexpr u32 RS_AMBIENT_MATERIAL_SOURCE = 147;
inline constexpr u32 RS_EMISSIVE_MATERIAL_SOURCE = 148;
inline constexpr u32 RS_VERTEX_BLEND = 151;
inline constexpr u32 RS_CLIP_PLANE_ENABLE = 152;
inline constexpr u32 RS_POINT_SPRITE_ENABLE = 156;
inline constexpr u32 RS_POINT_SCALE_ENABLE = 157;
inline constexpr u32 RS_INDEXED_VERTEX_BLEND_ENABLE = 167;
inline constexpr u32 RS_CULL_MODE = 22;
inline constexpr u32 RS_FILL_MODE = 8;
inline constexpr u32 RS_Z_WRITE_ENABLE = 14;
inline constexpr u32 RS_Z_FUNC = 23;
inline constexpr u32 RS_SRC_BLEND = 19;
inline constexpr u32 RS_DEST_BLEND = 20;
inline constexpr u32 RS_BLEND_OP = 171;
inline constexpr u32 RS_SCISSOR_TEST_ENABLE = 174;
inline constexpr u32 RS_COLOR_WRITE_ENABLE = 168;
inline constexpr u32 RS_Z_ENABLE = 7;
inline constexpr u32 RS_ALPHABLEND_ENABLE = 27;
inline constexpr u32 RS_BLEND_FACTOR = 193;
inline constexpr u32 RS_SRGB_WRITE_ENABLE = 194;
inline constexpr u32 RS_SEPARATE_ALPHA_BLEND_ENABLE = 206;
inline constexpr u32 RS_SRC_BLEND_ALPHA = 207;
inline constexpr u32 RS_DEST_BLEND_ALPHA = 208;
inline constexpr u32 RS_BLEND_OP_ALPHA = 209;
inline constexpr u32 RS_STENCIL_ENABLE = 52;
inline constexpr u32 RS_STENCIL_FUNC = 56;
inline constexpr u32 RS_STENCIL_FAIL = 53;
inline constexpr u32 RS_STENCIL_ZFAIL = 54;
inline constexpr u32 RS_STENCIL_PASS = 55;
inline constexpr u32 RS_STENCIL_REF = 57;
inline constexpr u32 RS_STENCIL_MASK = 58;
inline constexpr u32 RS_STENCIL_WRITEMASK = 59;
inline constexpr u32 RS_STENCIL_CCW_FUNC = 189;
inline constexpr u32 RS_STENCIL_CCW_FAIL = 186;
inline constexpr u32 RS_STENCIL_CCW_ZFAIL = 187;
inline constexpr u32 RS_STENCIL_CCW_PASS = 188;
inline constexpr u32 RS_STENCIL_CCW_REF = RS_STENCIL_REF;
inline constexpr u32 RS_STENCIL_CCW_MASK = RS_STENCIL_MASK;
inline constexpr u32 RS_STENCIL_CCW_WRITEMASK = RS_STENCIL_WRITEMASK;
inline constexpr u32 RS_TEXTURE_FACTOR = 60;

inline constexpr u32 TSS_COLOR_OP = 1;
inline constexpr u32 TSS_COLOR_ARG1 = 2;
inline constexpr u32 TSS_COLOR_ARG2 = 3;
inline constexpr u32 TSS_ALPHA_OP = 4;
inline constexpr u32 TSS_ALPHA_ARG1 = 5;
inline constexpr u32 TSS_ALPHA_ARG2 = 6;
inline constexpr u32 TSS_BUMPENVMAT00 = 7;
inline constexpr u32 TSS_BUMPENVMAT01 = 8;
inline constexpr u32 TSS_BUMPENVMAT10 = 9;
inline constexpr u32 TSS_BUMPENVMAT11 = 10;
inline constexpr u32 TSS_RESULT_ARG = 28;
inline constexpr u32 TSS_TEXCOORD_INDEX = 11;
inline constexpr u32 TSS_BUMPENVLSCALE = 22;
inline constexpr u32 TSS_BUMPENVLOFFSET = 23;
inline constexpr u32 TSS_TEXTURE_TRANSFORM_FLAGS = 24;
inline constexpr u32 TSS_CONSTANT = 32;      // D3DTSS_CONSTANT.
inline constexpr u32 TSS_TEXTURE_TYPE = 63;  // Internal shader-source texture kind key.

inline constexpr u32 SAMP_ADDRESS_U = 1;
inline constexpr u32 SAMP_ADDRESS_V = 2;
inline constexpr u32 SAMP_ADDRESS_W = 3;
inline constexpr u32 SAMP_MAG_FILTER = 5;
inline constexpr u32 SAMP_MIN_FILTER = 6;
inline constexpr u32 SAMP_MIP_FILTER = 7;
inline constexpr u32 SAMP_MIPMAP_LOD_BIAS = 8;
inline constexpr u32 SAMP_MAX_ANISOTROPY = 10;
inline constexpr u32 SAMP_SRGB_TEXTURE = 11;
inline constexpr u32 SAMP_BORDER_COLOR = 15;
inline constexpr u32 QUERY_GETDATA_FLUSH = 1u << 0;

inline constexpr u32 XFORM_WORLD_BASE = 1;
inline constexpr u32 XFORM_VIEW = 100;
inline constexpr u32 XFORM_PROJECTION = 101;
inline constexpr u32 XFORM_TEXTURE_BASE = 200;
inline constexpr u32 kMaxTransformSlots = XFORM_TEXTURE_BASE + kMaxTextureStages;

inline constexpr u32 MAX_TEXTURE_STAGE_INDEX = 7;

struct Rect {
  i32 left = 0;
  i32 top = 0;
  i32 right = 0;
  i32 bottom = 0;

  friend constexpr bool operator==(const Rect&, const Rect&) = default;
};

struct Viewport {
  u32 x = 0;
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
  f32 minZ = 0.0f;
  f32 maxZ = 1.0f;

  friend constexpr bool operator==(const Viewport&, const Viewport&) = default;
};

struct ColorRGBA {
  f32 r = 0.0f;
  f32 g = 0.0f;
  f32 b = 0.0f;
  f32 a = 1.0f;

  friend constexpr bool operator==(const ColorRGBA&, const ColorRGBA&) = default;
};

using ClipPlane = std::array<f32, 4>;

struct Matrix4x4 {
  std::array<f32, 16> m{};

  friend constexpr bool operator==(const Matrix4x4&, const Matrix4x4&) = default;
};

struct Material {
  ColorRGBA emissive{};
  ColorRGBA ambient{};
  ColorRGBA diffuse{1.0f, 1.0f, 1.0f, 1.0f};
  ColorRGBA specular{};
  f32 power = 0.0f;

  friend constexpr bool operator==(const Material&, const Material&) = default;
};

struct Light {
  LightType type = LightType::Directional;
  bool enabled = false;
  ColorRGBA diffuse{1.0f, 1.0f, 1.0f, 1.0f};
  ColorRGBA specular{1.0f, 1.0f, 1.0f, 1.0f};
  ColorRGBA ambient{};
  std::array<f32, 3> position{};
  std::array<f32, 3> direction{0.0f, 0.0f, 1.0f};
  f32 range = 1000.0f;
  f32 falloff = 1.0f;
  f32 attenuation0 = 1.0f;
  f32 attenuation1 = 0.0f;
  f32 attenuation2 = 0.0f;
  f32 theta = 0.5f;
  f32 phi = 1.0f;

  friend constexpr bool operator==(const Light&, const Light&) = default;
};

struct VertexElement {
  u16 stream = 0;
  u16 offset = 0;
  u32 type = 0;
  u32 method = 0;
  u32 usage = 0;
  u32 usageIndex = 0;

  friend constexpr bool operator==(const VertexElement&, const VertexElement&) = default;
};

struct StreamBinding {
  std::shared_ptr<class Buffer> buffer;
  u32 offset = 0;
  u32 stride = 0;

  friend bool operator==(const StreamBinding&, const StreamBinding&) = default;
};

struct ShaderBytecode {
  std::vector<u8> bytes;
  u64 hash = 0;

  friend bool operator==(const ShaderBytecode&, const ShaderBytecode&) = default;
};

struct ShaderConstantUsageBounds {
  std::uint16_t floatCount = 0;
  std::uint16_t intCount = 0;
  std::uint16_t boolCount = 0;
  bool indexedFloat = false;
  bool indexedInt = false;
  bool indexedBool = false;
  bool unknown = true;

  friend constexpr bool operator==(const ShaderConstantUsageBounds&,
                                   const ShaderConstantUsageBounds&) = default;
};

struct FfpVertexKey {
  bool lightingEnabled = false;
  bool specularEnabled = false;
  bool normalizeNormals = false;
  std::array<bool, kMaxLights> lightEnabled{};
  std::array<u32, kMaxLights> lightType{};
  std::array<u32, 4> colorMaterialMode{};
  FogMode fogMode = FogMode::None;
  bool fogFromVertex = false;
  bool rangeFog = false;
  std::array<u32, kMaxTextureStages> texCoordGen{};
  std::array<u32, kMaxTextureStages> texTransformFlags{};
  u32 vertexBlend = 0;
  bool indexedVertexBlend = false;
  u32 clipPlaneMask = 0;
  u64 hash = 0;

  friend constexpr bool operator==(const FfpVertexKey&, const FfpVertexKey&) = default;
};

struct FfpPixelStage {
  u32 colorOp = 0;
  u32 colorArg1 = 0;
  u32 colorArg2 = 0;
  u32 alphaOp = 0;
  u32 alphaArg1 = 0;
  u32 alphaArg2 = 0;
  u32 resultArg = 0;
  u32 texType = 0;
  u32 texCoordIndex = 0;

  friend constexpr bool operator==(const FfpPixelStage&, const FfpPixelStage&) = default;
};

struct FfpPixelKey {
  std::array<FfpPixelStage, kMaxTextureStages> stages{};
  FogMode fogMode = FogMode::None;
  bool alphaTestEnable = false;
  u32 alphaTestFunc = 0;
  u64 hash = 0;

  friend constexpr bool operator==(const FfpPixelKey&, const FfpPixelKey&) = default;
};

struct ShaderRef {
  enum class Kind {
    None,
    Bytecode,
    FixedFunctionVertex,
    FixedFunctionPixel,
  };

  Kind kind = Kind::None;
  u64 hash = 0;
  ShaderBytecode bytecode;
  std::optional<FfpVertexKey> vertexKey;
  std::optional<FfpPixelKey> pixelKey;

  friend bool operator==(const ShaderRef&, const ShaderRef&) = default;
};

struct RenderTargetAttachment {
  Handle handle{};
  u32 level = 0;
  u32 sampleCount = 1;

  friend constexpr bool operator==(const RenderTargetAttachment&, const RenderTargetAttachment&) = default;
};

struct ViewportScissor {
  Viewport viewport{};
  Rect scissor{};
  bool scissorEnabled = false;

  friend constexpr bool operator==(const ViewportScissor&, const ViewportScissor&) = default;
};

struct DrawCallArgs {
  PrimitiveType primitiveType = PrimitiveType::TriangleList;
  u32 primitiveCount = 0;
  u32 startVertex = 0;
  i32 baseVertexIndex = 0;
  u32 startIndex = 0;
  IndexType indexType = IndexType::UInt16;
};

// Per-chunk resource retention entry. Mirrors the wire-format D9CChunk
// HandleEntry but lives in dxmt9::core so the runtime can mark
// resources without depending on the d3d9/device_c.h header. Kind tag
// dispatches to the right pool table at bulk-mark time.
enum class ChunkHandleKind : u32 {
  Texture = 0,
  Surface = 1,
  Buffer  = 2,
  Shader  = 3,
  VertexDecl = 4,
};

struct ChunkHandleEntry {
  ChunkHandleKind kind = ChunkHandleKind::Texture;
  Handle handle{};
};

struct ClearDesc {
  std::array<RenderTargetAttachment, kMaxRenderTargets> colorAttachments{};
  RenderTargetAttachment depthStencil{};
  bool clearColor = false;
  bool clearDepth = false;
  bool clearStencil = false;
  ColorRGBA color{};
  f32 depth = 1.0f;
  u32 stencil = 0;
  std::vector<Rect> rects;
};

struct SwapDesc {
  Handle window{};
  // D3D9 present source is the swapchain backbuffer. The queue keeps a
  // fallback heuristic for old test paths, but normal device/swapchain
  // presents must fill this explicitly.
  Handle sourceSurface{};
  u32 width = 0;
  u32 height = 0;
  Format format = Format::A8R8G8B8;
  PresentInterval interval = PresentInterval::Default;
  bool windowed = true;
  u32 backBufferCount = 1;
  bool displaySyncEnabled = true;
  MultiSampleType multiSampleType = MultiSampleType::None;
  // Queue-local handle for the per-window Presenter binding registered by
  // the owning core::SwapChain. The unix-side CommandQueue resolves this
  // back to a Presenter* (and any pending drawable token from
  // acquire-before-present experiments). A zero value means no presenter
  // is associated with this packet — typical for the test / no-Metal
  // path. Keeping a POD identifier here (instead of a raw Presenter
  // pointer + shared_ptr<PresentDrawableToken>) is what makes SwapDesc
  // safe to cross the PE/unix wire.
  PresentId presentId{};
  bool drawableTokenRequired = false;
  // Per-present back-channels — DeviceImpl::present() fills these from its
  // own observers + maxFrameLatency_ before forwarding to the queue. Frame
  // latency is the app-facing pacing limit, not the Metal drawable count.
  u32 maxFrameLatency = kDefaultFrameLatency;
  std::function<void(bool)> notifyPresentationStatus{};
};

struct PresentParameters {
  u32 backBufferWidth = 0;
  u32 backBufferHeight = 0;
  Format backBufferFormat = Format::A8R8G8B8;
  u32 backBufferCount = 1;
  bool windowed = true;
  PresentInterval presentationInterval = PresentInterval::Default;
  u32 presentationIntervalRaw = 0;
  Handle deviceWindow{};
  bool enableAutoDepthStencil = false;
  Format autoDepthStencilFormat = Format::D24S8;
  u32 swapEffect = 1;
  bool discardSwapEffect = true;
  MultiSampleType multiSampleType = MultiSampleType::None;
};

struct BufferDesc {
  u64 size = 0;
  Pool pool = Pool::Default;
  u32 usage = 0;
};

struct TextureDesc {
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 levels = 1;
  Format format = Format::A8R8G8B8;
  TextureType type = TextureType::TwoD;
  Pool pool = Pool::Default;
  u32 usage = 0;
};

struct SurfaceDesc {
  u32 width = 0;
  u32 height = 0;
  Format format = Format::A8R8G8B8;
  Pool pool = Pool::Default;
  u32 usage = 0;
  bool renderTarget = false;
  bool depthStencil = false;
  MultiSampleType multiSampleType = MultiSampleType::None;
};

struct BackendLimits {
  u32 maxTextureSize = 16384;
  u32 maxColorAttachments = kMaxRenderTargets;
  u32 maxAnisotropy = 16;
  bool supportsDepth24Stencil8 = true;
  bool supportsBgr10A2 = true;
  bool supportsTimestampQueries = false;
  bool supportsOcclusionQueries = true;
  bool supportsDepth32FloatStencil8 = true;
  bool supportsSampleCount2 = true;
  bool supportsSampleCount4 = true;
  bool supportsSampleCount8 = false;
};

struct DisplayMode {
  u32 width = 0;
  u32 height = 0;
  u32 refreshRate = 60;
  Format format = Format::A8R8G8B8;
};

enum class DisplayScanLineOrdering : u32 {
  Unknown = 0,
  Progressive = 1,
  Interlaced = 2,
};

enum class DisplayRotation : u32 {
  Identity = 1,
  Rotate90 = 2,
  Rotate180 = 3,
  Rotate270 = 4,
};

struct DisplayModeEx {
  u32 width = 0;
  u32 height = 0;
  u32 refreshRate = 60;
  Format format = Format::A8R8G8B8;
  DisplayScanLineOrdering scanLineOrdering = DisplayScanLineOrdering::Progressive;
};

struct DisplayModeFilter {
  Format format = Format::Unknown;
};

struct Luid {
  u32 lowPart = 0;
  i32 highPart = 0;
};

struct AdapterInfo {
  u32 ordinal = 0;
  std::string name = "Adapter 0";
  u64 registryId = 0;
  u32 displayId = 0;
  DisplayMode displayMode{};
};

struct AdapterIdentifier {
  std::string description;
  std::string deviceName;
  std::string driver;
  u64 driverVersion = 0;
  u32 vendorId = 0;
  u32 deviceId = 0;
  u32 subSysId = 0;
  u32 revision = 0;
  u32 monitor = 0;
};

struct DeviceCaps {
  DeviceType deviceType = DeviceType::Hal;
  u32 caps = 0;
  u32 caps2 = 0;
  u32 caps3 = 0;
  u32 presentationIntervals = 0;
  u32 cursorCaps = 0;
  u32 primitiveMiscCaps = 0;
  u32 rasterCaps = 0;
  u32 zCmpCaps = 0;
  u32 alphaCmpCaps = 0;
  u32 shadeCaps = 0;
  u32 textureCaps = 0;
  u32 textureFilterCaps = 0;
  u32 cubetextureFilterCaps = 0;
  u32 volumeTextureFilterCaps = 0;
  u32 linePatternCaps = 0;
  u32 maxAnisotropy = 16;
  u32 maxUserClipPlanes = 6;
  f32 maxVertexW = 1.0e10f;
  f32 guardBandLeft = -8192.0f;
  f32 guardBandRight = 8192.0f;
  f32 guardBandTop = -8192.0f;
  f32 guardBandBottom = 8192.0f;
  f32 extentsAdjust = 0.0f;
  u32 stencilCaps = 0;
  u32 srcBlendCaps = 0;
  u32 destBlendCaps = 0;
  u32 alphaBlendCaps = 0;
  u32 textureBlendCaps = 0;
  u32 textureAddressCaps = 0;
  u32 volumeTextureAddressCaps = 0;
  u32 lineCaps = 0;
  u32 vertexShaderVersion = 0xfffe0300;
  u32 pixelShaderVersion = 0xffff0300;
  u32 maxVertexShaderConst = kMaxVertexConstants;
  f32 pixelShader1xMaxValue = 1024.0f;
  u32 ps20Caps = 0x0000001f;
  u32 ps20DynamicFlowControlDepth = 24;
  u32 ps20NumTemps = 32;
  u32 ps20StaticFlowControlDepth = 4;
  u32 ps20NumInstructionSlots = 512;
  u32 vs20Caps = 0x00000001;
  u32 vs20DynamicFlowControlDepth = 24;
  u32 vs20NumTemps = 32;
  u32 vs20StaticFlowControlDepth = 4;
  u32 maxTextureWidth = 16384;
  u32 maxTextureHeight = 16384;
  u32 maxVolumeExtent = 2048;
  u32 maxTextureRepeat = 32768;
  u32 maxTextureAspectRatio = 16384;
  f32 maxTextureLODBias = 16.0f;
  u32 maxSimultaneousTextures = 8;
  u32 maxActiveLights = kMaxLights;
  u32 numSimultaneousRTs = kMaxRenderTargets;
  u32 maxRenderTargetWidth = 16384;
  u32 maxRenderTargetHeight = 16384;
  u32 maxStreams = kMaxStreams;
  u32 maxStreamStride = 1024;
  u32 maxPrimitiveCount = 5592405;
  u32 maxVertexIndex = 16777215;
  u32 maxVertexBlendMatrices = 4;
  u32 maxVertexBlendMatrixIndex = 0;
  u32 fvfCaps = 0x00100008;
  u32 vertexProcessingCaps = 0x0000013b;
  u32 devCaps = 0;
  u32 devCaps2 = 0;
  u32 declTypes = 0x000003ff;
  u32 stretchRectFilterCaps = 0;
  f32 maxPointSize = 64.0f;
  u32 masterAdapterOrdinal = 0;
  u32 adapterOrdinalInGroup = 0;
  u32 numberOfAdaptersInGroup = 1;
  u32 vertexTextureFilterCaps = 0x01000100;
  u32 maxVShaderInstructionsExecuted = 65535;
  u32 maxPShaderInstructionsExecuted = 65535;
  u32 maxVertexShader30InstructionSlots = 512;
  u32 maxPixelShader30InstructionSlots = 512;
};

struct FormatInfo {
  Format format = Format::Unknown;
  BackendPixelFormat backendFormat = BackendPixelFormat::Unknown;
  FormatClass support = FormatClass::Unsupported;
  u32 bytesPerPixel = 0;
  bool renderTarget = false;
  bool depthStencil = false;
  bool compressed = false;
  bool lockable = true;
};

struct LockedRegion {
  void* data = nullptr;
  u32 pitch = 0;
};

struct SurfaceCopyDesc {
  Handle source{};
  Handle destination{};
  Rect sourceRect{};
  Rect destinationRect{};
  u32 sourceLevel = 0;
  u32 destinationLevel = 0;
  bool linear = false;
  u32 sourceSampleCount = 1;
  u32 destinationSampleCount = 1;
};

struct StretchRectDesc {
  Handle source{};
  Handle destination{};
  Rect sourceRect{};
  Rect destinationRect{};
  bool linear = false;
  u32 sourceSampleCount = 1;
  u32 destinationSampleCount = 1;
};

struct ReadbackDesc {
  Handle source{};
  Handle destination{};
  Rect sourceRect{};
  u32 sourceLevel = 0;
  u32 sourceSampleCount = 1;
  u32 destinationSampleCount = 1;
};

struct ReadbackPixels {
  std::vector<u8> bytes;
  u32 pitch = 0;
};

struct ColorFillDesc {
  Handle destination{};
  Rect rect{};
  bool hasRect = false;
  ColorRGBA color{};
};

const std::vector<FormatInfo>& formatTable();
const FormatInfo* findFormatInfo(Format format);
FormatClass classifyFormat(Format format);
BackendPixelFormat backendPixelFormat(Format format);
bool formatSupportsUsage(Format format, u32 usage, const BackendLimits& limits);
bool isCompressedFormat(Format format);
u64 hashBytes(std::span<const std::byte> bytes);
u64 hashString(std::string_view text);

DeviceCaps makeDefaultCaps(const BackendLimits& limits);
std::array<f32, 2> halfPixelFixup(const Viewport& viewport);

constexpr u32 sampleCount(MultiSampleType type) noexcept {
  switch (type) {
    case MultiSampleType::None:
      return 1;
    case MultiSampleType::Two:
      return 2;
    case MultiSampleType::Four:
      return 4;
    case MultiSampleType::Eight:
      return 8;
  }
  return 1;
}

std::vector<u32> decomposeTriangleFanIndices(std::span<const u32> indices);
std::vector<u8> convertTextureUpload(Format format, u32 width, u32 height, std::span<const u8> input);
u32 bytesPerPixel(Format format);
u32 formatBlockWidth(Format format);
u32 formatBlockHeight(Format format);
u32 formatBlockBytes(Format format);
u32 formatRowPitch(Format format, u32 width);
u32 formatRowCount(Format format, u32 height);
std::size_t formatByteSize(Format format, u32 width, u32 height);
std::string formatName(Format format);
std::string backendFormatName(BackendPixelFormat format);
// makeBackendDevice and makeSimBackendDevice have been retired. The upper
// runtime creation path is dxmt9::CreateDXMT9Device() (see
// src/dxmt9/dxmt9_device.hpp). Downstream consumers receive the
// std::shared_ptr<BackendDevice> via dxmt9::Device::backend().

}  // namespace dxmt9::core
