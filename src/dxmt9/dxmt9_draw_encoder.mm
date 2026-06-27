#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "dxmt9_draw_encoder.hpp"
#include "dxmt9_draw_encoder_internal.hpp"
#include "dxmt9_argbuf_hybrid.hpp"
#include "dxmt9_blit_encoders.hpp"

#include "dxmt9/assert.hpp"
#include "dxmt9_command_queue.hpp"
#include "dxmt9_debug_alloc_guard.hpp"
#include "dxmt9_device.hpp"
#include "dxmt9_debug_trace.hpp"
#include "dxmt9_draw_state.hpp"
#include "dxmt9_ffp_shaders.hpp"
#include "dxmt9_format_convert.hpp"
#include "dxmt9_perf_counters.hpp"
#include "dxmt9_pipeline_cache.hpp"
#include "dxmt9_presenter.hpp"
#include "dxmt9_queue.hpp"
#include "dxmt9_resource_pool.hpp"
#include "dxmt9_ring_arena.hpp"
#include "dxmt9_signposts.hpp"
#include "util/log/log.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace dxmt9::encoders {

using core::ClearDesc;
using core::Handle;
using core::IndexType;
using core::SamplerSnapshot;
using core::SAMP_ADDRESS_U;
using core::SAMP_ADDRESS_V;
using core::SAMP_ADDRESS_W;
using core::SAMP_BORDER_COLOR;
using core::SAMP_MAG_FILTER;
using core::SAMP_MAX_ANISOTROPY;
using core::SAMP_MAX_MIP_LEVEL;
using core::SAMP_MIN_FILTER;
using core::SAMP_MIP_FILTER;
using core::kMaxSamplers;
using core::kMaxTextureStages;

using core::CompareFunc;
using core::TextureOp;

using core::RS_ALPHABLEND_ENABLE;
using core::RS_ALPHA_FUNC;
using core::RS_ALPHA_REF;
using core::RS_ALPHA_TEST_ENABLE;
using core::RS_BLEND_OP;
using core::RS_BLEND_OP_ALPHA;
using core::RS_BLEND_FACTOR;
using core::RS_COLOR_WRITE_ENABLE;
using core::RS_CULL_MODE;
using core::RS_DEST_BLEND;
using core::RS_DEST_BLEND_ALPHA;
using core::RS_POINT_SCALE_ENABLE;
using core::RS_POINT_SPRITE_ENABLE;
using core::RS_POINTSIZE;
using core::RS_POINTSIZE_MAX;
using core::RS_POINTSIZE_MIN;
using core::RS_SEPARATE_ALPHA_BLEND_ENABLE;
using core::RS_SRC_BLEND;
using core::RS_SRC_BLEND_ALPHA;
using core::RS_TEXTURE_FACTOR;
using core::RS_Z_ENABLE;
using core::RS_Z_FUNC;
using core::RS_Z_WRITE_ENABLE;

using core::TSS_ALPHA_ARG1;
using core::TSS_ALPHA_ARG2;
using core::TSS_ALPHA_OP;
using core::TSS_COLOR_ARG1;
using core::TSS_COLOR_ARG2;
using core::TSS_COLOR_OP;
using core::TSS_TEXCOORD_INDEX;
using core::TSS_TEXTURE_TRANSFORM_FLAGS;

using dxmt9::ffp::kD3DDeclTypeD3DColor;
using dxmt9::ffp::kD3DDeclTypeFloat1;
using dxmt9::ffp::kD3DDeclTypeFloat2;
using dxmt9::ffp::kD3DDeclTypeFloat3;
using dxmt9::ffp::kD3DDeclTypeFloat4;
using dxmt9::ffp::kD3DDeclUsageColor;
using dxmt9::ffp::kD3DDeclUsagePosition;
using dxmt9::ffp::kD3DDeclUsagePositionT;
using dxmt9::ffp::kD3DDeclUsageTexcoord;

using dxmt9::convert::formatHasDepthAspect;
using dxmt9::convert::formatHasStencilAspect;
using dxmt9::convert::toPixelFormat;
using dxmt9::convert::toCullMode;
using dxmt9::convert::toIndexType;
using dxmt9::convert::toPrimitiveType;
using dxmt9::ffp::computeVertexDeclStreamStride;
using dxmt9::ffp::computeVertexDeclStride;
using dxmt9::ffp::decodeFixedFunctionVertexLayout;

using dxmt9::core::metalqueue::emitQueueTraceLine;
using dxmt9::core::metalqueue::emitTextureTraceLine;
using dxmt9::core::metalqueue::queueTraceEnabled;

using dxmt9::state::DrawVolatile;
using dxmt9::state::FfpPsConsts;
using dxmt9::state::FfpVsConsts;
using dxmt9::state::PsConsts;
using dxmt9::state::SamplerLodBias;
using dxmt9::state::VsConsts;
using dxmt9::state::anySamplerLodBiasNonzero;
using dxmt9::state::buildDrawVolatile;
using dxmt9::state::buildFfpPsConsts;
using dxmt9::state::buildSamplerLodBias;
using dxmt9::state::buildFfpVsConsts;
using dxmt9::state::buildPsConsts;
using dxmt9::state::buildVsConsts;
using dxmt9::state::makeDepthStencilKey;

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using i32 = std::int32_t;
using f32 = float;

namespace {

// M1/M2 — printf-style label/group-name builder. Returns a non-owning
// WMT::String view backed by an autoreleased NSString. Lifetime is safe
// because the receiving setLabel:/pushDebugGroup: selector retains
// immediately and encodeChunk runs inside an @autoreleasepool.
template <std::size_t Cap = 96>
WMT::String makeLabelStringFmt(const char* fmt, ...) {
  char buf[Cap];
  va_list args;
  va_start(args, fmt);
  std::vsnprintf(buf, sizeof(buf), fmt, args);
  va_end(args);
  return WMT::String::string(buf, WMTUTF8StringEncoding);
}

std::optional<u64> parseEnvU64Auto(const char* name) {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return std::nullopt;
  }
  char* end = nullptr;
  const u64 value = static_cast<u64>(std::strtoull(env, &end, 0));
  if (end == env || (end && *end != '\0')) {
    return std::nullopt;
  }
  return value;
}

std::string getenvStringLocal(const char* name) {
  const char* env = std::getenv(name);
  return env && env[0] != '\0' ? std::string(env) : std::string{};
}

bool getenvFlagLocal(const char* name) {
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return false;
  }
  return std::strcmp(env, "0") != 0 &&
         std::strcmp(env, "false") != 0 &&
         std::strcmp(env, "FALSE") != 0 &&
         std::strcmp(env, "off") != 0 &&
         std::strcmp(env, "OFF") != 0;
}

std::vector<u64> parseEnvU64ListAuto(const char* name) {
  std::vector<u64> values;
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return values;
  }
  const char* cursor = env;
  while (*cursor != '\0') {
    while (*cursor == ',' || *cursor == ';' || *cursor == ':' ||
           std::isspace(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }
    char* end = nullptr;
    const u64 value = static_cast<u64>(std::strtoull(cursor, &end, 0));
    if (end == cursor) {
      break;
    }
    values.push_back(value);
    cursor = end;
  }
  return values;
}

bool traceEncodeProgressEnabled() {
  static const bool enabled = getenvFlagLocal("DXMT9_TRACE_ENCODE_PROGRESS");
  return enabled;
}

std::optional<u64> traceEncodeProgressSeqFilter() {
  static const std::optional<u64> filter =
      parseEnvU64Auto("DXMT9_TRACE_ENCODE_PROGRESS_SEQ");
  return filter;
}

bool traceEncodeProgressForSeq(u64 seqId) {
  if (!traceEncodeProgressEnabled()) {
    return false;
  }
  const auto filter = traceEncodeProgressSeqFilter();
  return !filter.has_value() || *filter == seqId;
}

const char* metalCommandKindName(core::MetalCommandKind kind) {
  switch (kind) {
  case core::MetalCommandKind::DrawRun:
    return "DrawRun";
  case core::MetalCommandKind::Clear:
    return "Clear";
  case core::MetalCommandKind::SurfaceCopy:
    return "SurfaceCopy";
  case core::MetalCommandKind::StretchRect:
    return "StretchRect";
  case core::MetalCommandKind::Readback:
    return "Readback";
  case core::MetalCommandKind::ColorFill:
    return "ColorFill";
  case core::MetalCommandKind::DepthResolve:
    return "DepthResolve";
  case core::MetalCommandKind::Present:
    return "Present";
  }
  return "Unknown";
}

void emitEncodeProgressDrawStage(u64 seqId,
                                 std::uint32_t commandIndex,
                                 u64 commandDrawIndex,
                                 u64 commandDrawCount,
                                 const char* stage) {
  if (!traceEncodeProgressForSeq(seqId)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-encode-progress]"
      << " stage=draw." << stage
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " command=" << commandIndex
      << " draw=" << static_cast<unsigned long long>(commandDrawIndex)
      << "/" << static_cast<unsigned long long>(commandDrawCount);
  emitQueueTraceLine(out.str());
}

struct VisibilityScoutConfig {
  bool enabled = false;
  std::string path;
  u64 maxResultsPerEncoder = 65536;
  debug::RenderEncoderSelector row{};
  debug::RenderEncoderSelectorList rows{};
};

const VisibilityScoutConfig& visibilityScoutConfig() {
  static const VisibilityScoutConfig config = [] {
    VisibilityScoutConfig result{};
    result.path = getenvStringLocal("DXMT9_VISIBILITY_SCOUT_PATH");
    result.maxResultsPerEncoder =
        parseEnvU64Auto("DXMT9_VISIBILITY_SCOUT_MAX_RESULTS_PER_ENCODER")
            .value_or(result.maxResultsPerEncoder);
    result.maxResultsPerEncoder = std::max<u64>(1, result.maxResultsPerEncoder);
    result.row =
        debug::makeRenderEncoderSelector(
            getenvStringLocal("DXMT9_VISIBILITY_SCOUT_ROW"));
    result.rows =
        debug::makeRenderEncoderSelectorList(
            getenvStringLocal("DXMT9_VISIBILITY_SCOUT_ROWS"));
    result.enabled =
        !result.path.empty() &&
        (getenvFlagLocal("DXMT9_VISIBILITY_SCOUT") ||
         result.row.enabled ||
         result.rows.enabled);
    return result;
  }();
  return config;
}

bool visibilityScoutWantsPass(u64 seqId, u64 encoderIndex) {
  const auto& config = visibilityScoutConfig();
  if (!config.enabled) {
    return false;
  }
  if (config.row.enabled &&
      !debug::renderEncoderSelectorMatches(config.row, seqId, encoderIndex)) {
    return false;
  }
  if (config.rows.enabled &&
      !debug::renderEncoderSelectorListMatches(config.rows, seqId, encoderIndex)) {
    return false;
  }
  return true;
}

struct VisibilityScoutDrawRecord {
  u64 seqId = 0;
  u64 encoderIndex = 0;
  std::uint32_t commandIndex = 0;
  u64 drawOrdinal = 0;
  std::uint32_t resultIndex = 0;
  std::uint32_t metalDrawIndex = 0;
  std::uint32_t primitiveType = 0;
  std::uint32_t sourcePrimitiveCount = 0;
  u64 submittedPrimitiveCount = 0;
  u64 submittedElementCount = 0;
  std::uint32_t indexed = 0;
  std::uint32_t expandedIndexed = 0;
  std::uint32_t splitChunk = 0;
  u64 rt0 = 0;
  u64 depth = 0;
  std::uint32_t textureMask = 0;
  std::uint32_t colorWrite = 0;
  std::uint32_t zEnable = 0;
  std::uint32_t zWrite = 0;
  std::uint32_t zFunc = 0;
  std::uint32_t alphaBlend = 0;
  std::uint32_t alphaTest = 0;
  std::uint32_t scissor = 0;
  std::uint32_t cull = 0;
  std::uint32_t fill = 0;
};

struct VisibilityScoutPass {
  WMT::Reference<WMT::Buffer> buffer{};
  std::uint64_t* results = nullptr;
  std::vector<VisibilityScoutDrawRecord> records;
  std::string path;
  u64 seqId = 0;
  u64 encoderIndex = 0;
  std::uint32_t capacity = 0;
  std::uint32_t metalDrawIndex = 0;
  bool overflow = false;
};

std::optional<VisibilityScoutPass> makeVisibilityScoutPass(
    WMT::Device device,
    u64 seqId,
    u64 encoderIndex) {
  if (!device || !visibilityScoutWantsPass(seqId, encoderIndex)) {
    return std::nullopt;
  }
  const auto& config = visibilityScoutConfig();
  const auto capacity = static_cast<std::uint32_t>(
      std::min<u64>(config.maxResultsPerEncoder,
                    std::numeric_limits<std::uint32_t>::max()));
  WMTBufferInfo info{};
  info.length = static_cast<u64>(capacity) * sizeof(std::uint64_t);
  info.options = WMTResourceStorageModeShared;
  auto buffer = device.newBuffer(info);
  if (!buffer || !info.memory.ptr) {
    return std::nullopt;
  }
  std::memset(info.memory.ptr, 0, static_cast<std::size_t>(info.length));

  VisibilityScoutPass pass{};
  pass.buffer = std::move(buffer);
  pass.results = static_cast<std::uint64_t*>(info.memory.ptr);
  pass.path = config.path;
  pass.seqId = seqId;
  pass.encoderIndex = encoderIndex;
  pass.capacity = capacity;
  pass.records.reserve(capacity);
  return pass;
}

VisibilityScoutDrawRecord makeVisibilityScoutDrawRecord(
    const VisibilityScoutPass& pass,
    core::FlatDrawStateView drawState,
    const core::ViewportScissor& viewport,
    WMTPrimitiveType primitiveType,
    const ParamView& pv,
    u64 drawOrdinal,
    std::uint32_t commandIndex,
    u64 submittedPrimitiveCount,
    u64 submittedElementCount,
    bool indexed,
    bool expandedIndexed,
    std::uint32_t splitChunk,
    WMTCullMode cullMode,
    WMTTriangleFillMode fillMode) {
  const auto& hot = *drawState.hot;
  VisibilityScoutDrawRecord record{};
  record.seqId = pass.seqId;
  record.encoderIndex = pass.encoderIndex;
  record.commandIndex = commandIndex;
  record.drawOrdinal = drawOrdinal;
  record.metalDrawIndex = pass.metalDrawIndex;
  record.primitiveType = static_cast<std::uint32_t>(primitiveType);
  record.sourcePrimitiveCount = pv.primitiveCount;
  record.submittedPrimitiveCount = submittedPrimitiveCount;
  record.submittedElementCount = submittedElementCount;
  record.indexed = indexed ? 1u : 0u;
  record.expandedIndexed = expandedIndexed ? 1u : 0u;
  record.splitChunk = splitChunk;
  record.rt0 = hot.colorAttachments[0].handle.value;
  record.depth = hot.depthStencil.handle.value;
  record.textureMask = hot.textureMask;
  record.colorWrite =
      core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
  record.zEnable = core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u);
  record.zWrite = core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u);
  record.zFunc =
      core::flatStateOr(hot.renderStates, RS_Z_FUNC,
                        static_cast<u32>(core::CompareFunc::LessEqual));
  record.alphaBlend =
      core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u);
  record.alphaTest =
      core::flatStateOr(hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u);
  record.scissor = viewport.scissorEnabled ? 1u : 0u;
  record.cull = static_cast<std::uint32_t>(cullMode);
  record.fill = static_cast<std::uint32_t>(fillMode);
  return record;
}

std::optional<std::uint32_t> beginVisibilityScoutDraw(
    VisibilityScoutPass* pass,
    WMT::RenderCommandEncoder& encoder,
    VisibilityScoutDrawRecord record) {
  if (!pass) {
    return std::nullopt;
  }
  if (pass->records.size() >= pass->capacity) {
    pass->overflow = true;
    return std::nullopt;
  }
  record.resultIndex = static_cast<std::uint32_t>(pass->records.size());
  pass->records.push_back(record);
  ++pass->metalDrawIndex;
  encoder.setVisibilityResultMode(WMTVisibilityResultModeCounting,
                                  static_cast<u64>(record.resultIndex) *
                                      sizeof(std::uint64_t));
  return record.resultIndex;
}

void endVisibilityScoutDraw(VisibilityScoutPass* pass,
                            WMT::RenderCommandEncoder& encoder,
                            std::optional<std::uint32_t> resultIndex) {
  if (!pass || !resultIndex.has_value()) {
    return;
  }
  encoder.setVisibilityResultMode(WMTVisibilityResultModeDisabled, 0);
}

void appendVisibilityScoutCsv(const std::string& path,
                              WMT::Reference<WMT::Buffer> buffer,
                              const std::uint64_t* results,
                              std::vector<VisibilityScoutDrawRecord> records,
                              bool overflow) {
  (void)buffer;
  if (path.empty() || results == nullptr || records.empty()) {
    return;
  }
  std::filesystem::path outputPath(path);
  if (const auto parent = outputPath.parent_path(); !parent.empty()) {
    std::error_code createError;
    std::filesystem::create_directories(parent, createError);
  }
  static std::mutex outputMutex;
  std::lock_guard lock(outputMutex);
  std::error_code sizeError;
  const bool needsHeader =
      !std::filesystem::exists(outputPath, sizeError) ||
      std::filesystem::file_size(outputPath, sizeError) == 0;
  std::ofstream out(path, std::ios::app);
  if (!out) {
    return;
  }
  if (needsHeader) {
    out << "seq,encoder,command,draw_ordinal,result_index,metal_draw_index,"
           "primitive_type,source_primitive_count,submitted_primitive_count,"
           "submitted_element_count,indexed,expanded_indexed,split_chunk,"
           "visible_samples,rt0,depth,texture_mask,color_write,z_enable,"
           "z_write,z_func,alpha_blend,alpha_test,scissor,cull,fill,overflow\n";
  }
  for (const auto& record : records) {
    const auto visibleSamples = results[record.resultIndex];
    out << record.seqId << ','
        << record.encoderIndex << ','
        << record.commandIndex << ','
        << record.drawOrdinal << ','
        << record.resultIndex << ','
        << record.metalDrawIndex << ','
        << record.primitiveType << ','
        << record.sourcePrimitiveCount << ','
        << record.submittedPrimitiveCount << ','
        << record.submittedElementCount << ','
        << record.indexed << ','
        << record.expandedIndexed << ','
        << record.splitChunk << ','
        << visibleSamples << ','
        << record.rt0 << ','
        << record.depth << ','
        << record.textureMask << ','
        << record.colorWrite << ','
        << record.zEnable << ','
        << record.zWrite << ','
        << record.zFunc << ','
        << record.alphaBlend << ','
        << record.alphaTest << ','
        << record.scissor << ','
        << record.cull << ','
        << record.fill << ','
        << (overflow ? 1u : 0u) << '\n';
  }
}

void enqueueVisibilityScoutCompletion(
    VisibilityScoutPass& pass,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!pass.results || pass.records.empty()) {
    return;
  }
  completionCallbacks.push_back(
      [path = std::move(pass.path),
       buffer = std::move(pass.buffer),
       results = pass.results,
       records = std::move(pass.records),
       overflow = pass.overflow]() mutable {
        appendVisibilityScoutCsv(path, std::move(buffer), results,
                                 std::move(records), overflow);
      });
  pass.results = nullptr;
}

struct DepthAttachmentDumpConfig {
  bool enabled = false;
  u64 handle = 0;
  std::optional<u64> seq;
  std::optional<u64> enc;
  std::string path;
};

struct ColorAttachmentDumpConfig {
  bool enabled = false;
  bool afterDraw = false;
  std::optional<u64> handle;
  std::optional<u64> index;
  std::optional<u64> seq;
  std::optional<u64> enc;
  std::optional<u64> draw;
  std::vector<u64> draws;
  std::optional<u64> commandIndex;
  std::optional<u64> commandIndexMin;
  std::optional<u64> commandIndexMax;
  std::optional<u64> texture0;
  std::vector<u64> texture0s;
  std::string path;
  std::string dir;
  std::string roiSummaryPath;
  u64 brightThreshold = 220;
  u64 whiteThreshold = 240;
  u64 warmRedThreshold = 180;
  u64 warmGreenThreshold = 110;
  u64 warmBlueMargin = 32;
  struct Roi {
    u32 left = 0;
    u32 top = 0;
    u32 right = 0;
    u32 bottom = 0;
    std::string name;
  };
  std::vector<Roi> rois;
};

std::vector<ColorAttachmentDumpConfig::Roi> parseColorAttachmentRois(
    const char* name) {
  std::vector<ColorAttachmentDumpConfig::Roi> rois;
  const char* env = std::getenv(name);
  if (!env || env[0] == '\0') {
    return rois;
  }

  const char* cursor = env;
  while (*cursor != '\0') {
    while (*cursor == ';' || std::isspace(static_cast<unsigned char>(*cursor))) {
      ++cursor;
    }
    if (*cursor == '\0') {
      break;
    }

    char* end = nullptr;
    const auto left = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ',') {
      break;
    }
    cursor = end + 1;
    const auto top = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ',') {
      break;
    }
    cursor = end + 1;
    const auto right = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ',') {
      break;
    }
    cursor = end + 1;
    const auto bottom = std::strtoull(cursor, &end, 10);
    if (end == cursor || right <= left || bottom <= top ||
        right > std::numeric_limits<u32>::max() ||
        bottom > std::numeric_limits<u32>::max()) {
      break;
    }
    cursor = end;

    std::string roiName;
    if (*cursor == ':') {
      ++cursor;
      const char* nameStart = cursor;
      while (*cursor != '\0' && *cursor != ';') {
        ++cursor;
      }
      roiName.assign(nameStart, cursor);
    }
    if (roiName.empty()) {
      std::ostringstream generated;
      generated << left << "," << top << "," << right << "," << bottom;
      roiName = generated.str();
    }

    ColorAttachmentDumpConfig::Roi roi{};
    roi.left = static_cast<u32>(left);
    roi.top = static_cast<u32>(top);
    roi.right = static_cast<u32>(right);
    roi.bottom = static_cast<u32>(bottom);
    roi.name = std::move(roiName);
    rois.push_back(std::move(roi));

    while (*cursor != '\0' && *cursor != ';') {
      ++cursor;
    }
  }
  return rois;
}

const DepthAttachmentDumpConfig& depthAttachmentDumpConfig() {
  static const DepthAttachmentDumpConfig config = [] {
    DepthAttachmentDumpConfig result{};
    result.handle =
        parseEnvU64Auto("DXMT9_DUMP_DEPTH_ATTACHMENT_HANDLE").value_or(0);
    result.seq = parseEnvU64Auto("DXMT9_DUMP_DEPTH_ATTACHMENT_SEQ");
    result.enc = parseEnvU64Auto("DXMT9_DUMP_DEPTH_ATTACHMENT_ENC");
    result.path = getenvStringLocal("DXMT9_DUMP_DEPTH_ATTACHMENT_PATH");
    result.enabled = result.handle != 0 && !result.path.empty();
    return result;
  }();
  return config;
}

const ColorAttachmentDumpConfig& colorAttachmentDumpConfig() {
  static const ColorAttachmentDumpConfig config = [] {
    ColorAttachmentDumpConfig result{};
    result.afterDraw =
        getenvFlagLocal("DXMT9_DUMP_COLOR_ATTACHMENT_AFTER_DRAW");
    result.handle = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_HANDLE");
    result.index = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_INDEX");
    result.seq = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_SEQ");
    result.enc = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_ENC");
    result.draw = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_DRAW");
    result.draws = parseEnvU64ListAuto("DXMT9_DUMP_COLOR_ATTACHMENT_DRAWS");
    result.commandIndex =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX");
    result.commandIndexMin =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MIN");
    result.commandIndexMax =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_COMMAND_INDEX_MAX");
    result.texture0 = parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0");
    result.texture0s =
        parseEnvU64ListAuto("DXMT9_DUMP_COLOR_ATTACHMENT_TEXTURE0S");
    result.path = getenvStringLocal("DXMT9_DUMP_COLOR_ATTACHMENT_PATH");
    result.dir = getenvStringLocal("DXMT9_DUMP_COLOR_ATTACHMENT_DIR");
    result.roiSummaryPath =
        getenvStringLocal("DXMT9_DUMP_COLOR_ATTACHMENT_ROI_SUMMARY_PATH");
    result.rois = parseColorAttachmentRois("DXMT9_DUMP_COLOR_ATTACHMENT_ROIS");
    result.brightThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_BRIGHT_THRESHOLD")
            .value_or(220ull);
    result.whiteThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WHITE_THRESHOLD")
            .value_or(240ull);
    result.warmRedThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_RED_THRESHOLD")
            .value_or(180ull);
    result.warmGreenThreshold =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_GREEN_THRESHOLD")
            .value_or(110ull);
    result.warmBlueMargin =
        parseEnvU64Auto("DXMT9_DUMP_COLOR_ATTACHMENT_WARM_BLUE_MARGIN")
            .value_or(32ull);
    result.enabled = (!result.path.empty() ||
                      !result.dir.empty() ||
                      !result.roiSummaryPath.empty()) &&
                     (result.handle.has_value() ||
                      result.index.has_value() ||
                      result.seq.has_value() ||
                      result.enc.has_value() ||
                      result.draw.has_value() ||
                      !result.draws.empty() ||
                      result.commandIndex.has_value() ||
                      result.commandIndexMin.has_value() ||
                      result.commandIndexMax.has_value() ||
                      result.texture0.has_value() ||
                      !result.texture0s.empty());
    return result;
  }();
  return config;
}

struct DrawTextureDumpConfig {
  bool enabled = false;
  bool texture0Any = false;
  std::vector<u64> handles;
  std::optional<u64> texture0Width;
  std::optional<u64> texture0Height;
  std::optional<u64> texture0Format;
  std::optional<u64> seq;
  std::optional<u64> enc;
  std::string dir;
};

const DrawTextureDumpConfig& drawTextureDumpConfig() {
  static const DrawTextureDumpConfig config = [] {
    DrawTextureDumpConfig result{};
    result.handles = parseEnvU64ListAuto("DXMT9_DUMP_DRAW_TEXTURE_HANDLES");
    if (const auto handle = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE_HANDLE")) {
      result.handles.push_back(*handle);
    }
    result.texture0Any = getenvFlagLocal("DXMT9_DUMP_DRAW_TEXTURE0_ANY");
    result.texture0Width = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE0_WIDTH");
    result.texture0Height = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE0_HEIGHT");
    result.texture0Format = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE0_FORMAT");
    result.seq = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE_SEQ");
    result.enc = parseEnvU64Auto("DXMT9_DUMP_DRAW_TEXTURE_ENC");
    result.dir = getenvStringLocal("DXMT9_DUMP_DRAW_TEXTURE_DIR");
    result.enabled = (!result.handles.empty() ||
                      result.texture0Any ||
                      result.texture0Width.has_value() ||
                      result.texture0Height.has_value() ||
                      result.texture0Format.has_value()) &&
                     !result.dir.empty();
    return result;
  }();
  return config;
}

bool drawTextureDumpWantsTexture(u32 textureIndex,
                                 core::Handle handle,
                                 const resources::TextureRecord& record) {
  const auto& config = drawTextureDumpConfig();
  if (!config.enabled || !handle) {
    return false;
  }
  if (!config.handles.empty() &&
      std::find(config.handles.begin(), config.handles.end(),
                handle.value) == config.handles.end()) {
    return false;
  }
  if (!config.texture0Any &&
      !config.texture0Width.has_value() &&
      !config.texture0Height.has_value() &&
      !config.texture0Format.has_value()) {
    return true;
  }
  if (textureIndex != 0u) {
    return false;
  }
  if (config.texture0Width.has_value() &&
      record.desc.width != *config.texture0Width) {
    return false;
  }
  if (config.texture0Height.has_value() &&
      record.desc.height != *config.texture0Height) {
    return false;
  }
  if (config.texture0Format.has_value() &&
      static_cast<u64>(record.desc.format) != *config.texture0Format) {
    return false;
  }
  return true;
}

struct ActiveDepthAttachmentDump {
  core::Handle handle{};
  WMT::Reference<WMT::Texture> texture{};
  core::Format format = core::Format::Unknown;
  enum WMTPixelFormat metalPixelFormat = WMTPixelFormatInvalid;
  u32 width = 0;
  u32 height = 0;
  u64 seq = 0;
  u64 enc = 0;
  bool hasDepth = false;
  bool hasStencil = false;
};

struct ActiveColorAttachmentDump {
  core::Handle handle{};
  WMT::Reference<WMT::Texture> texture{};
  core::Format format = core::Format::Unknown;
  enum WMTPixelFormat metalPixelFormat = WMTPixelFormatInvalid;
  u32 width = 0;
  u32 height = 0;
  u32 index = 0;
  u64 seq = 0;
  u64 enc = 0;
  u64 draw = 0;
  u64 commandIndex = 0;
  u64 commandDrawIndex = 0;
  u64 commandDrawCount = 0;
  u64 texture0 = 0;
  bool afterDraw = false;
};

struct ColorAttachmentReadbackRegion {
  u32 x = 0;
  u32 y = 0;
  u32 width = 0;
  u32 height = 0;
};

ColorAttachmentReadbackRegion colorAttachmentDumpReadbackRegion(
    const ActiveColorAttachmentDump& active) {
  const auto& config = colorAttachmentDumpConfig();
  if (!config.path.empty() || !config.dir.empty() || config.rois.empty()) {
    return ColorAttachmentReadbackRegion{0, 0, active.width, active.height};
  }

  u32 left = active.width;
  u32 top = active.height;
  u32 right = 0;
  u32 bottom = 0;
  for (const auto& roi : config.rois) {
    const u32 clippedLeft = std::min(roi.left, active.width);
    const u32 clippedTop = std::min(roi.top, active.height);
    const u32 clippedRight = std::min(roi.right, active.width);
    const u32 clippedBottom = std::min(roi.bottom, active.height);
    if (clippedRight <= clippedLeft || clippedBottom <= clippedTop) {
      continue;
    }
    left = std::min(left, clippedLeft);
    top = std::min(top, clippedTop);
    right = std::max(right, clippedRight);
    bottom = std::max(bottom, clippedBottom);
  }
  if (right <= left || bottom <= top) {
    return ColorAttachmentReadbackRegion{0, 0, 0, 0};
  }
  return ColorAttachmentReadbackRegion{left, top, right - left, bottom - top};
}

struct ActiveDrawTextureDump {
  core::Handle handle{};
  WMT::Texture texture{};
  core::Format format = core::Format::Unknown;
  core::TextureType type = core::TextureType::TwoD;
  enum WMTPixelFormat storageMetalPixelFormat = WMTPixelFormatInvalid;
  enum WMTPixelFormat shaderMetalPixelFormat = WMTPixelFormatInvalid;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 levels = 1;
  u32 textureIndex = 0;
  u32 stage = 0;
  bool vertexStage = false;
  bool srgb = false;
  bool shaderReadView = false;
  u64 seq = 0;
  u64 enc = 0;
};

bool depthAttachmentDumpMatches(const ActiveDepthAttachmentDump& active) {
  const auto& config = depthAttachmentDumpConfig();
  if (!config.enabled || !active.handle || !active.texture) {
    return false;
  }
  if (config.handle != active.handle.value) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != active.seq) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != active.enc) {
    return false;
  }
  static std::atomic_bool started{false};
  bool expected = false;
  return started.compare_exchange_strong(expected, true);
}

bool colorAttachmentDumpMatches(const ActiveColorAttachmentDump& active) {
  const auto& config = colorAttachmentDumpConfig();
  if (!config.enabled || !active.handle || !active.texture) {
    return false;
  }
  if (config.afterDraw != active.afterDraw) {
    return false;
  }
  if (config.handle.has_value() && *config.handle != active.handle.value) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != active.seq) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != active.enc) {
    return false;
  }
  if (config.draw.has_value() && *config.draw != active.draw) {
    return false;
  }
  if (!config.draws.empty() &&
      std::find(config.draws.begin(), config.draws.end(), active.draw) ==
          config.draws.end()) {
    return false;
  }
  if (config.commandIndex.has_value() &&
      *config.commandIndex != active.commandIndex) {
    return false;
  }
  if (config.commandIndexMin.has_value() &&
      active.commandIndex < *config.commandIndexMin) {
    return false;
  }
  if (config.commandIndexMax.has_value() &&
      active.commandIndex > *config.commandIndexMax) {
    return false;
  }
  if (config.texture0.has_value() && *config.texture0 != active.texture0) {
    return false;
  }
  if (!config.texture0s.empty() &&
      std::find(config.texture0s.begin(), config.texture0s.end(),
                active.texture0) == config.texture0s.end()) {
    return false;
  }
  if (config.dir.empty()) {
    if (!config.roiSummaryPath.empty()) {
      std::ostringstream key;
      key << (active.afterDraw ? "after" : "pass")
          << ":s" << active.seq
          << ":e" << active.enc
          << ":i" << active.index
          << ":d" << active.draw
          << ":c" << active.commandIndex
          << ":ci" << active.commandDrawIndex
          << ":t" << active.texture0;
      static std::mutex mutex;
      static std::unordered_set<std::string> dumped;
      std::lock_guard lock(mutex);
      return dumped.insert(key.str()).second;
    }
    static std::atomic_bool started{false};
    bool expected = false;
    return started.compare_exchange_strong(expected, true);
  }
  std::ostringstream key;
  key << (active.afterDraw ? "after" : "pass")
      << ":s" << active.seq
      << ":e" << active.enc
      << ":i" << active.index
      << ":d" << active.draw
      << ":c" << active.commandIndex
      << ":ci" << active.commandDrawIndex
      << ":t" << active.texture0;
  static std::mutex mutex;
  static std::unordered_set<std::string> dumped;
  std::lock_guard lock(mutex);
  return dumped.insert(key.str()).second;
}

bool colorAttachmentDumpAfterDrawWantsSplit(
    const ActiveColorAttachmentDump& active,
    core::FlatDrawStateView drawState,
    u64 encoderDrawIndex,
    u64 commandIndex) {
  const auto& config = colorAttachmentDumpConfig();
  if (!config.enabled || !config.afterDraw || !active.handle || !active.texture ||
      !drawState.hot) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != active.seq) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != active.enc) {
    return false;
  }
  if (config.draw.has_value() && *config.draw != encoderDrawIndex) {
    return false;
  }
  if (!config.draws.empty() &&
      std::find(config.draws.begin(), config.draws.end(), encoderDrawIndex) ==
          config.draws.end()) {
    return false;
  }
  if (config.commandIndex.has_value() &&
      *config.commandIndex != commandIndex) {
    return false;
  }
  if (config.commandIndexMin.has_value() &&
      commandIndex < *config.commandIndexMin) {
    return false;
  }
  if (config.commandIndexMax.has_value() &&
      commandIndex > *config.commandIndexMax) {
    return false;
  }
  const u64 texture0 = drawState.hot->textures[0]
      ? drawState.hot->textures[0].value
      : 0ull;
  if (config.texture0.has_value() && *config.texture0 != texture0) {
    return false;
  }
  if (!config.texture0s.empty() &&
      std::find(config.texture0s.begin(), config.texture0s.end(), texture0) ==
          config.texture0s.end()) {
    return false;
  }
  return config.draw.has_value() ||
         !config.draws.empty() ||
         config.commandIndex.has_value() ||
         config.commandIndexMin.has_value() ||
         config.commandIndexMax.has_value() ||
         config.texture0.has_value() ||
         !config.texture0s.empty();
}

std::string colorAttachmentDumpPath(const ActiveColorAttachmentDump& active) {
  const auto& config = colorAttachmentDumpConfig();
  if (config.dir.empty()) {
    return config.path;
  }
  std::ostringstream name;
  name << "color-s" << active.seq
       << "-e" << active.enc;
  if (active.afterDraw) {
    name << "-after-draw-d" << active.draw;
    name << "-ci" << active.commandIndex;
    name << "-cmd-d" << active.commandDrawIndex
         << "-of" << active.commandDrawCount;
  } else {
    name << "-pass-end";
  }
  if (active.texture0 != 0) {
    name << "-tex0x" << std::hex << active.texture0 << std::dec;
  }
  name << ".bin";
  return (std::filesystem::path(config.dir) / name.str()).string();
}

bool drawTextureDumpPassMatches(u64 seq, u64 enc) {
  const auto& config = drawTextureDumpConfig();
  if (!config.enabled) {
    return false;
  }
  if (config.seq.has_value() && *config.seq != seq) {
    return false;
  }
  if (config.enc.has_value() && *config.enc != enc) {
    return false;
  }
  return true;
}

u64 drawTextureDumpKey(const ActiveDrawTextureDump& active) {
  u64 key = active.handle.value;
  key ^= static_cast<u64>(active.srgb ? 0x9e37u : 0u) << 48u;
  key ^= static_cast<u64>(active.vertexStage ? 0x51u : 0x23u) << 40u;
  return key;
}

bool drawTextureDumpAlreadyQueued(std::span<const ActiveDrawTextureDump> active,
                                  const ActiveDrawTextureDump& candidate) {
  const u64 key = drawTextureDumpKey(candidate);
  return std::any_of(active.begin(), active.end(), [&](const auto& entry) {
    return drawTextureDumpKey(entry) == key;
  });
}

bool drawTextureDumpMarkStarted(const ActiveDrawTextureDump& active) {
  static std::mutex mutex;
  static std::unordered_set<u64> started;
  const u64 key = drawTextureDumpKey(active);
  std::lock_guard lock(mutex);
  return started.insert(key).second;
}

std::string drawTextureDumpShaderStageName(const ActiveDrawTextureDump& active) {
  return active.vertexStage ? "vertex" : "fragment";
}

std::string drawTextureDumpJsonPath(const ActiveDrawTextureDump& active) {
  std::ostringstream filename;
  filename << "texture-h0x" << std::hex << active.handle.value << std::dec
           << "-seq" << active.seq
           << "-enc" << active.enc
           << "-" << drawTextureDumpShaderStageName(active) << active.stage
           << "-" << (active.srgb ? "srgb" : "linear")
           << ".json";
  return (std::filesystem::path(drawTextureDumpConfig().dir) /
          filename.str()).string();
}

std::string drawTextureDumpSubresourceBasename(
    const ActiveDrawTextureDump& active,
    u32 level,
    u32 slice) {
  std::ostringstream filename;
  filename << "texture-h0x" << std::hex << active.handle.value << std::dec
           << "-seq" << active.seq
           << "-enc" << active.enc
           << "-" << drawTextureDumpShaderStageName(active) << active.stage
           << "-" << (active.srgb ? "srgb" : "linear")
           << "-slice" << slice
           << "-level" << level
           << ".bin";
  return filename.str();
}

struct TextureSubresourceReadback {
  u32 level = 0;
  u32 slice = 0;
  u32 width = 0;
  u32 height = 0;
  u32 depth = 1;
  u32 rowBytes = 0;
  u64 bytesPerImage = 0;
  u64 byteCount = 0;
  std::string basename;
  WMT::Reference<WMT::Buffer> buffer{};
  const void* bytes = nullptr;
};

struct TextureSidecarReadbackBatch {
  ActiveDrawTextureDump active{};
  std::vector<TextureSubresourceReadback> subresources;
};

void writeDepthAttachmentDump(const ActiveDepthAttachmentDump& active,
                              std::string path,
                              u32 rowBytes,
                              u64 byteCount,
                              const void* bytes) {
  if (!bytes || path.empty() || byteCount == 0) {
    return;
  }
  try {
    const std::filesystem::path binaryPath(path);
    if (const auto parent = binaryPath.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    {
      std::ofstream out(binaryPath, std::ios::binary | std::ios::trunc);
      out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(byteCount));
    }
    {
      std::ofstream meta(path + ".json", std::ios::trunc);
      meta << "{\n"
           << "  \"handle\": \"0x" << std::hex << active.handle.value << std::dec << "\",\n"
           << "  \"seq\": " << active.seq << ",\n"
           << "  \"enc\": " << active.enc << ",\n"
           << "  \"format\": " << static_cast<u32>(active.format) << ",\n"
           << "  \"formatName\": \"" << core::formatName(active.format) << "\",\n"
           << "  \"metalPixelFormat\": " << static_cast<u32>(active.metalPixelFormat) << ",\n"
           << "  \"width\": " << active.width << ",\n"
           << "  \"height\": " << active.height << ",\n"
           << "  \"rowBytes\": " << rowBytes << ",\n"
           << "  \"byteCount\": " << byteCount << ",\n"
           << "  \"hasDepth\": " << (active.hasDepth ? 1 : 0) << ",\n"
           << "  \"hasStencil\": " << (active.hasStencil ? 1 : 0) << "\n"
           << "}\n";
    }
    std::ostringstream out;
    out << "[dxmt9-depth] dump handle=0x" << std::hex << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " size=" << active.width << "x" << active.height
        << " rowBytes=" << rowBytes
        << " bytes=" << byteCount
        << " path=" << path;
    emitTextureTraceLine(out.str());
  } catch (const std::exception& e) {
    std::ostringstream out;
    out << "[dxmt9-depth] dump write-failed handle=0x" << std::hex
        << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " path=" << path
        << " error=" << e.what();
    emitTextureTraceLine(out.str());
  }
}

void writeColorAttachmentDump(const ActiveColorAttachmentDump& active,
                              std::string path,
                              u32 rowBytes,
                              u64 byteCount,
                              const void* bytes) {
  if (!bytes || path.empty() || byteCount == 0) {
    return;
  }
  try {
    const std::filesystem::path binaryPath(path);
    if (const auto parent = binaryPath.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }
    {
      std::ofstream out(binaryPath, std::ios::binary | std::ios::trunc);
      out.write(static_cast<const char*>(bytes), static_cast<std::streamsize>(byteCount));
    }
    {
      std::ofstream meta(path + ".json", std::ios::trunc);
      meta << "{\n"
           << "  \"handle\": \"0x" << std::hex << active.handle.value << std::dec << "\",\n"
           << "  \"seq\": " << active.seq << ",\n"
           << "  \"enc\": " << active.enc << ",\n"
           << "  \"index\": " << active.index << ",\n"
           << "  \"afterDraw\": " << (active.afterDraw ? 1 : 0) << ",\n"
           << "  \"draw\": " << active.draw << ",\n"
           << "  \"commandIndex\": " << active.commandIndex << ",\n"
           << "  \"commandDrawIndex\": " << active.commandDrawIndex << ",\n"
           << "  \"commandDrawCount\": " << active.commandDrawCount << ",\n"
           << "  \"texture0\": \"0x" << std::hex << active.texture0 << std::dec << "\",\n"
           << "  \"format\": " << static_cast<u32>(active.format) << ",\n"
           << "  \"formatName\": \"" << core::formatName(active.format) << "\",\n"
           << "  \"metalPixelFormat\": " << static_cast<u32>(active.metalPixelFormat) << ",\n"
           << "  \"width\": " << active.width << ",\n"
           << "  \"height\": " << active.height << ",\n"
           << "  \"rowBytes\": " << rowBytes << ",\n"
           << "  \"byteCount\": " << byteCount << "\n"
           << "}\n";
    }
    std::ostringstream out;
    out << "[dxmt9-color] dump handle=0x" << std::hex << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " index=" << active.index
        << " afterDraw=" << (active.afterDraw ? 1 : 0)
        << " draw=" << active.draw
        << " commandIndex=" << active.commandIndex
        << " commandDrawIndex=" << active.commandDrawIndex
        << " commandDrawCount=" << active.commandDrawCount
        << " texture0=0x" << std::hex << active.texture0 << std::dec
        << " format=" << static_cast<unsigned>(active.format)
        << " size=" << active.width << "x" << active.height
        << " rowBytes=" << rowBytes
        << " bytes=" << byteCount
        << " path=" << path;
    emitTextureTraceLine(out.str());
  } catch (const std::exception& e) {
    std::ostringstream out;
    out << "[dxmt9-color] dump write-failed handle=0x" << std::hex
        << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " index=" << active.index
        << " path=" << path
        << " error=" << e.what();
    emitTextureTraceLine(out.str());
  }
}

void appendColorAttachmentRoiSummary(
    const ActiveColorAttachmentDump& active,
    std::string path,
    ColorAttachmentReadbackRegion region,
    u32 rowBytes,
    const void* bytes) {
  const auto& config = colorAttachmentDumpConfig();
  if (path.empty() || !bytes || region.width == 0 || region.height == 0) {
    return;
  }
  const u32 bpp = core::bytesPerPixel(active.format);
  if (bpp < 4) {
    return;
  }

  const auto rois = config.rois.empty()
      ? std::vector<ColorAttachmentDumpConfig::Roi>{
            ColorAttachmentDumpConfig::Roi{
                0, 0, active.width, active.height, "full"}}
      : config.rois;
  const auto* data = static_cast<const u8*>(bytes);

  try {
    const std::filesystem::path csvPath(path);
    if (const auto parent = csvPath.parent_path(); !parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    static std::mutex mutex;
    std::lock_guard lock(mutex);
    const bool writeHeader =
        !std::filesystem::exists(csvPath) ||
        std::filesystem::file_size(csvPath) == 0;
    std::ofstream out(csvPath, std::ios::app);
    if (writeHeader) {
      out << "handle,seq,enc,index,after_draw,draw,command_index,"
             "command_draw_index,command_draw_count,texture0,format,"
             "format_name,width,height,readback_x,readback_y,readback_width,"
             "readback_height,roi,roi_rect,pixels,avg_r,avg_g,avg_b,max_r,"
             "max_g,max_b,bright_pixels,white_pixels,warm_pixels,hot_x,hot_y,"
             "hot_r,hot_g,hot_b,warm_hot_x,warm_hot_y,warm_hot_r,warm_hot_g,"
             "warm_hot_b\n";
    }

    for (const auto& roi : rois) {
      const u32 left = std::min(roi.left, active.width);
      const u32 top = std::min(roi.top, active.height);
      const u32 right = std::min(roi.right, active.width);
      const u32 bottom = std::min(roi.bottom, active.height);
      if (right <= left || bottom <= top) {
        continue;
      }
      const u32 sampleLeft = std::max(left, region.x);
      const u32 sampleTop = std::max(top, region.y);
      const u32 sampleRight = std::min(right, region.x + region.width);
      const u32 sampleBottom = std::min(bottom, region.y + region.height);
      if (sampleRight <= sampleLeft || sampleBottom <= sampleTop) {
        continue;
      }

      u64 sumR = 0;
      u64 sumG = 0;
      u64 sumB = 0;
      u32 maxR = 0;
      u32 maxG = 0;
      u32 maxB = 0;
      u64 bright = 0;
      u64 white = 0;
      u64 warm = 0;
      u32 hotX = sampleLeft;
      u32 hotY = sampleTop;
      u32 hotR = 0;
      u32 hotG = 0;
      u32 hotB = 0;
      u32 hotMax = 0;
      u32 warmHotX = sampleLeft;
      u32 warmHotY = sampleTop;
      u32 warmHotR = 0;
      u32 warmHotG = 0;
      u32 warmHotB = 0;
      u32 warmHotScore = 0;
      const u64 pixels =
          static_cast<u64>(sampleRight - sampleLeft) *
          static_cast<u64>(sampleBottom - sampleTop);

      for (u32 y = sampleTop; y < sampleBottom; ++y) {
        const u64 rowOffset =
            static_cast<u64>(y - region.y) * rowBytes +
            static_cast<u64>(sampleLeft - region.x) * bpp;
        for (u32 x = sampleLeft; x < sampleRight; ++x) {
          const u64 offset = rowOffset + static_cast<u64>(x - sampleLeft) * bpp;
          const u32 b = data[offset + 0];
          const u32 g = data[offset + 1];
          const u32 r = data[offset + 2];
          sumR += r;
          sumG += g;
          sumB += b;
          maxR = std::max(maxR, r);
          maxG = std::max(maxG, g);
          maxB = std::max(maxB, b);
          const u32 channelMax = std::max({r, g, b});
          if (channelMax > config.brightThreshold) {
            ++bright;
          }
          if (r > config.whiteThreshold &&
              g > config.whiteThreshold &&
              b > config.whiteThreshold) {
            ++white;
          }
          const bool warmPixel =
              r >= config.warmRedThreshold &&
              g >= config.warmGreenThreshold &&
              static_cast<u64>(b) <=
                  static_cast<u64>(r) + config.warmBlueMargin;
          if (warmPixel) {
            ++warm;
            const u32 warmScore = r + g + b;
            if (warmScore > warmHotScore) {
              warmHotScore = warmScore;
              warmHotX = x;
              warmHotY = y;
              warmHotR = r;
              warmHotG = g;
              warmHotB = b;
            }
          }
          if (channelMax > hotMax) {
            hotMax = channelMax;
            hotX = x;
            hotY = y;
            hotR = r;
            hotG = g;
            hotB = b;
          }
        }
      }

      const double denom = pixels ? static_cast<double>(pixels) : 1.0;
      out << "0x" << std::hex << active.handle.value << std::dec
          << ',' << active.seq
          << ',' << active.enc
          << ',' << active.index
          << ',' << (active.afterDraw ? 1 : 0)
          << ',' << active.draw
          << ',' << active.commandIndex
          << ',' << active.commandDrawIndex
          << ',' << active.commandDrawCount
          << ",0x" << std::hex << active.texture0 << std::dec
          << ',' << static_cast<u32>(active.format)
          << ',' << core::formatName(active.format)
          << ',' << active.width
          << ',' << active.height
          << ',' << region.x
          << ',' << region.y
          << ',' << region.width
          << ',' << region.height
          << ',' << roi.name
          << ',' << left << ' ' << top << ' ' << right << ' ' << bottom
          << ',' << pixels
          << ',' << std::fixed << std::setprecision(3) << (sumR / denom)
          << ',' << (sumG / denom)
          << ',' << (sumB / denom)
          << ',' << maxR
          << ',' << maxG
          << ',' << maxB
          << ',' << bright
          << ',' << white
          << ',' << warm
          << ',' << hotX
          << ',' << hotY
          << ',' << hotR
          << ',' << hotG
          << ',' << hotB
          << ',' << warmHotX
          << ',' << warmHotY
          << ',' << warmHotR
          << ',' << warmHotG
          << ',' << warmHotB
          << '\n';
    }

    std::ostringstream trace;
    trace << "[dxmt9-color] roi-summary seq=" << active.seq
          << " enc=" << active.enc
          << " afterDraw=" << (active.afterDraw ? 1 : 0)
          << " draw=" << active.draw
          << " commandIndex=" << active.commandIndex
          << " texture0=0x" << std::hex << active.texture0 << std::dec
          << " rois=" << rois.size()
          << " path=" << path;
    emitTextureTraceLine(trace.str());
  } catch (const std::exception& e) {
    std::ostringstream trace;
    trace << "[dxmt9-color] roi-summary write-failed seq=" << active.seq
          << " enc=" << active.enc
          << " path=" << path
          << " error=" << e.what();
    emitTextureTraceLine(trace.str());
  }
}

void writeDrawTextureDump(const TextureSidecarReadbackBatch& batch) {
  const auto& active = batch.active;
  const std::string jsonPath = drawTextureDumpJsonPath(active);
  try {
    const std::filesystem::path metadataPath(jsonPath);
    const auto parent = metadataPath.parent_path();
    if (!parent.empty()) {
      std::filesystem::create_directories(parent);
    }

    for (const auto& subresource : batch.subresources) {
      if (!subresource.bytes || subresource.byteCount == 0 ||
          subresource.basename.empty()) {
        continue;
      }
      const auto binPath = parent / subresource.basename;
      std::ofstream out(binPath, std::ios::binary | std::ios::trunc);
      out.write(static_cast<const char*>(subresource.bytes),
                static_cast<std::streamsize>(subresource.byteCount));
    }

    {
      std::ofstream meta(jsonPath, std::ios::trunc);
      meta << "{\n"
           << "  \"handle\": \"0x" << std::hex << active.handle.value << std::dec << "\",\n"
           << "  \"seq\": " << active.seq << ",\n"
           << "  \"enc\": " << active.enc << ",\n"
           << "  \"textureIndex\": " << active.textureIndex << ",\n"
           << "  \"stage\": " << active.stage << ",\n"
           << "  \"shaderStage\": \"" << drawTextureDumpShaderStageName(active) << "\",\n"
           << "  \"srgb\": " << (active.srgb ? 1 : 0) << ",\n"
           << "  \"shaderReadView\": " << (active.shaderReadView ? 1 : 0) << ",\n"
           << "  \"format\": " << static_cast<u32>(active.format) << ",\n"
           << "  \"formatName\": \"" << core::formatName(active.format) << "\",\n"
           << "  \"type\": " << static_cast<u32>(active.type) << ",\n"
           << "  \"storageMetalPixelFormat\": "
           << static_cast<u32>(active.storageMetalPixelFormat) << ",\n"
           << "  \"shaderMetalPixelFormat\": "
           << static_cast<u32>(active.shaderMetalPixelFormat) << ",\n"
           << "  \"width\": " << active.width << ",\n"
           << "  \"height\": " << active.height << ",\n"
           << "  \"depth\": " << active.depth << ",\n"
           << "  \"levels\": " << active.levels << ",\n"
           << "  \"subresources\": [\n";
      for (std::size_t i = 0; i < batch.subresources.size(); ++i) {
        const auto& subresource = batch.subresources[i];
        meta << "    {\n"
             << "      \"level\": " << subresource.level << ",\n"
             << "      \"slice\": " << subresource.slice << ",\n"
             << "      \"width\": " << subresource.width << ",\n"
             << "      \"height\": " << subresource.height << ",\n"
             << "      \"depth\": " << subresource.depth << ",\n"
             << "      \"rowBytes\": " << subresource.rowBytes << ",\n"
             << "      \"bytesPerImage\": " << subresource.bytesPerImage << ",\n"
             << "      \"byteCount\": " << subresource.byteCount << ",\n"
             << "      \"path\": \"" << subresource.basename << "\"\n"
             << "    }" << (i + 1u == batch.subresources.size() ? "\n" : ",\n");
      }
      meta << "  ]\n"
           << "}\n";
    }

    std::ostringstream out;
    out << "[dxmt9-texture] draw-sidecar-dump handle=0x" << std::hex
        << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " stage=" << drawTextureDumpShaderStageName(active) << active.stage
        << " srgb=" << (active.srgb ? 1 : 0)
        << " format=" << static_cast<unsigned>(active.format)
        << " size=" << active.width << "x" << active.height
        << " levels=" << active.levels
        << " subresources=" << batch.subresources.size()
        << " path=" << jsonPath;
    emitTextureTraceLine(out.str());
  } catch (const std::exception& e) {
    std::ostringstream out;
    out << "[dxmt9-texture] draw-sidecar-dump write-failed handle=0x"
        << std::hex << active.handle.value << std::dec
        << " seq=" << active.seq
        << " enc=" << active.enc
        << " path=" << jsonPath
        << " error=" << e.what();
    emitTextureTraceLine(out.str());
  }
}

void maybeEncodeDepthAttachmentDump(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    const ActiveDepthAttachmentDump& active,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!depthAttachmentDumpMatches(active)) {
    return;
  }
  const u32 bpp = core::bytesPerPixel(active.format);
  if (bpp == 0 || active.width == 0 || active.height == 0) {
    return;
  }
  const u32 rowBytes = active.width * bpp;
  const u64 byteCount = static_cast<u64>(rowBytes) * active.height;
  WMTBufferInfo bufInfo{};
  bufInfo.length = byteCount;
  bufInfo.options = WMTResourceStorageModeShared;
  auto readbackBuffer = device.newBuffer(bufInfo);
  void* readbackMemory = bufInfo.memory.get_accessible_or_null();
  if (!readbackBuffer || !readbackMemory) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  blit.setLabel(makeLabelStringFmt(
      "Blit[DepthAttachmentDump seq=%llu enc=%llu handle=0x%llx]",
      static_cast<unsigned long long>(active.seq),
      static_cast<unsigned long long>(active.enc),
      static_cast<unsigned long long>(active.handle.value)));
  WMTOrigin origin{0, 0, 0};
  WMTSize size{active.width, active.height, 1};
  blit.copyFromTextureToBuffer(WMT::Texture{active.texture.handle}, 0, 0,
                               origin, size, WMT::Buffer{readbackBuffer.handle},
                               0, rowBytes, 0);
  blit.endEncoding();
  const auto path = depthAttachmentDumpConfig().path;
  completionCallbacks.push_back(
      [active, path, rowBytes, byteCount, readbackBuffer, readbackMemory] {
        (void)readbackBuffer;
        writeDepthAttachmentDump(active, path, rowBytes, byteCount, readbackMemory);
      });
}

void maybeEncodeColorAttachmentDump(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    const ActiveColorAttachmentDump& active,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!colorAttachmentDumpMatches(active)) {
    return;
  }
  const u32 bpp = core::bytesPerPixel(active.format);
  if (bpp == 0 || active.width == 0 || active.height == 0) {
    return;
  }
  const auto region = colorAttachmentDumpReadbackRegion(active);
  if (region.width == 0 || region.height == 0) {
    return;
  }
  const u32 rowBytes = region.width * bpp;
  const u64 byteCount = static_cast<u64>(rowBytes) * region.height;
  WMTBufferInfo bufInfo{};
  bufInfo.length = byteCount;
  bufInfo.options = WMTResourceStorageModeShared;
  auto readbackBuffer = device.newBuffer(bufInfo);
  void* readbackMemory = bufInfo.memory.get_accessible_or_null();
  if (!readbackBuffer || !readbackMemory) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  blit.setLabel(makeLabelStringFmt(
      "Blit[ColorAttachmentDump seq=%llu enc=%llu handle=0x%llx]",
      static_cast<unsigned long long>(active.seq),
      static_cast<unsigned long long>(active.enc),
      static_cast<unsigned long long>(active.handle.value)));
  WMTOrigin origin{region.x, region.y, 0};
  WMTSize size{region.width, region.height, 1};
  blit.copyFromTextureToBuffer(WMT::Texture{active.texture.handle}, 0, 0,
                               origin, size, WMT::Buffer{readbackBuffer.handle},
                               0, rowBytes, 0);
  blit.endEncoding();
  const auto path = colorAttachmentDumpPath(active);
  const auto summaryPath = colorAttachmentDumpConfig().roiSummaryPath;
  completionCallbacks.push_back(
      [active, path, summaryPath, region, rowBytes, byteCount,
       readbackBuffer, readbackMemory] {
        (void)readbackBuffer;
        if (!path.empty()) {
          writeColorAttachmentDump(active, path, rowBytes, byteCount, readbackMemory);
        }
        appendColorAttachmentRoiSummary(active, summaryPath, region, rowBytes,
                                        readbackMemory);
      });
}

void maybeCollectDrawTextureDump(
    std::vector<ActiveDrawTextureDump>& activeDumps,
    const resources::Pool& pool,
    core::FlatDrawStateView drawState,
    u64 seq,
    u64 enc) {
  if (!drawState.hot || !drawTextureDumpPassMatches(seq, enc)) {
    return;
  }
  const auto& hot = *drawState.hot;
  for (u32 textureIndex = 0; textureIndex < core::kMaxTextures; ++textureIndex) {
    const auto handle = hot.textures[textureIndex];
    const auto* record = pool.findTexture(handle.value);
    if (!record || !record->texture) {
      continue;
    }
    if (!drawTextureDumpWantsTexture(textureIndex, handle, *record)) {
      continue;
    }
    const bool vertexStage = textureIndex >= core::kVertexTextureSampler0;
    const u32 stage = vertexStage ? textureIndex - core::kVertexTextureSampler0
                                  : textureIndex;
    const bool srgbTexture =
        core::flatStateOr(hot.samplerStates[textureIndex],
                          core::SAMP_SRGB_TEXTURE, 0u) != 0;
    ActiveDrawTextureDump candidate{};
    candidate.handle = handle;
    candidate.texture = resources::textureForShaderRead(*record, srgbTexture);
    candidate.format = record->desc.format;
    candidate.type = record->desc.type;
    candidate.storageMetalPixelFormat =
        WMT::Texture{record->texture.handle}.pixelFormat();
    candidate.shaderMetalPixelFormat =
        candidate.texture ? candidate.texture.pixelFormat()
                          : WMTPixelFormatInvalid;
    candidate.width = std::max(1u, record->desc.width);
    candidate.height = std::max(1u, record->desc.height);
    candidate.depth = std::max(1u, record->desc.depth);
    candidate.levels = std::max(1u, record->desc.levels);
    candidate.textureIndex = textureIndex;
    candidate.stage = stage;
    candidate.vertexStage = vertexStage;
    candidate.srgb = srgbTexture;
    candidate.shaderReadView =
        (srgbTexture && record->srgbShaderReadTexture) ||
        (!srgbTexture && record->shaderReadTexture);
    candidate.seq = seq;
    candidate.enc = enc;
    if (!candidate.texture ||
        drawTextureDumpAlreadyQueued(activeDumps, candidate) ||
        !drawTextureDumpMarkStarted(candidate)) {
      continue;
    }
    activeDumps.push_back(candidate);
  }
}

void maybeEncodeDrawTextureDumps(
    WMT::CommandBuffer& commandBuffer,
    WMT::Reference<WMT::Device> device,
    std::span<const ActiveDrawTextureDump> activeDumps,
    std::vector<std::function<void()>>& completionCallbacks) {
  if (!drawTextureDumpConfig().enabled || activeDumps.empty()) {
    return;
  }
  auto blit = commandBuffer.blitCommandEncoder();
  if (!blit) {
    return;
  }
  blit.setLabel(makeLabelStringFmt(
      "Blit[DrawTextureDump count=%llu]",
      static_cast<unsigned long long>(activeDumps.size())));
  for (const auto& active : activeDumps) {
    auto batch = std::make_shared<TextureSidecarReadbackBatch>();
    batch->active = active;
    const u32 sliceCount =
        active.type == core::TextureType::Cube ? 6u : 1u;
    for (u32 slice = 0; slice < sliceCount; ++slice) {
      for (u32 level = 0; level < active.levels; ++level) {
        const u32 width = std::max(1u, active.width >> level);
        const u32 height = std::max(1u, active.height >> level);
        const u32 depth =
            active.type == core::TextureType::Volume
                ? std::max(1u, active.depth >> level)
                : 1u;
        const u32 rowBytes = core::formatRowPitch(active.format, width);
        const u64 bytesPerImage =
            static_cast<u64>(core::formatByteSize(active.format, width, height));
        const u64 byteCount = bytesPerImage * depth;
        if (rowBytes == 0 || bytesPerImage == 0 || byteCount == 0) {
          continue;
        }
        WMTBufferInfo bufInfo{};
        bufInfo.length = byteCount;
        bufInfo.options = WMTResourceStorageModeShared;
        auto readbackBuffer = device.newBuffer(bufInfo);
        const void* readbackMemory = bufInfo.memory.get_accessible_or_null();
        if (!readbackBuffer || !readbackMemory) {
          continue;
        }
        TextureSubresourceReadback subresource{};
        subresource.level = level;
        subresource.slice = slice;
        subresource.width = width;
        subresource.height = height;
        subresource.depth = depth;
        subresource.rowBytes = rowBytes;
        subresource.bytesPerImage = bytesPerImage;
        subresource.byteCount = byteCount;
        subresource.basename =
            drawTextureDumpSubresourceBasename(active, level, slice);
        subresource.buffer = readbackBuffer;
        subresource.bytes = readbackMemory;
        WMTOrigin origin{0, 0, 0};
        WMTSize size{width, height, depth};
        blit.copyFromTextureToBuffer(
            active.texture,
            active.type == core::TextureType::Volume ? 0u : slice,
            level, origin, size, WMT::Buffer{readbackBuffer.handle}, 0,
            rowBytes, active.type == core::TextureType::Volume ? bytesPerImage : 0);
        batch->subresources.push_back(std::move(subresource));
      }
    }
    if (!batch->subresources.empty()) {
      completionCallbacks.push_back([batch] {
        writeDrawTextureDump(*batch);
      });
    }
  }
  blit.endEncoding();
}

bool colorAttachmentAliasesTracedTexture(resources::Pool& pool,
                                         const core::FlatDrawStateRecord& hot,
                                         std::size_t* attachmentIndex = nullptr) {
  const u64 wanted = debug::traceTextureHandle();
  if (wanted == 0) {
    return false;
  }
  for (std::size_t i = 0; i < hot.colorAttachments.size(); ++i) {
    const auto handle = hot.colorAttachments[i].handle;
    if (!handle) {
      continue;
    }
    const auto* surface = pool.findSurface(handle.value);
    if (surface && surface->aliasTexture.value == wanted) {
      if (attachmentIndex) {
        *attachmentIndex = i;
      }
      return true;
    }
  }
  return false;
}

void traceRenderTargetWriteForTexture(resources::Pool& pool,
                                      const core::FlatDrawStateRecord& hot,
                                      u64 seqId,
                                      u64 drawOrdinal) {
  std::size_t attachmentIndex = 0;
  if (!colorAttachmentAliasesTracedTexture(pool, hot, &attachmentIndex)) {
    return;
  }
  const auto handle = hot.colorAttachments[attachmentIndex].handle;
  std::ostringstream out;
  out << "[dxmt9-texture] render-target-write seq="
      << static_cast<unsigned long long>(seqId)
      << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
      << " colorIndex=" << attachmentIndex
      << " surface=0x" << std::hex << handle.value << std::dec
      << " texture=0x" << std::hex << debug::traceTextureHandle() << std::dec
      << " colorWrite=" << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu)
      << " srgbWrite=" << core::flatStateOr(hot.renderStates, core::RS_SRGB_WRITE_ENABLE, 0u)
      << " alphaBlend=" << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
      << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
      << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
      << " tex0=0x" << std::hex << hot.textures[0].value << std::dec;
  emitTextureTraceLine(out.str());
}

thread_local DrawBindingPacketCache gDrawBindingPacketCache;

// M2 — RAII debug-group helper. Pairs a pushDebugGroup with the
// matching popDebugGroup on scope exit, even on early-return paths.
// Holds a non-owning view of the encoder; the caller retains the
// encoder's lifetime through Reference<>.
class DebugGroupScope {
 public:
  DebugGroupScope(WMT::CommandEncoder encoder, WMT::String name)
      : encoder_(encoder) {
    if (encoder_ && name) {
      encoder_.pushDebugGroup(name);
      active_ = true;
    }
  }

  ~DebugGroupScope() {
    if (active_) {
      encoder_.popDebugGroup();
    }
  }

  // Non-copyable, non-movable — RAII pair must stay paired with one
  // scope entry.
  DebugGroupScope(const DebugGroupScope&) = delete;
  DebugGroupScope& operator=(const DebugGroupScope&) = delete;
  DebugGroupScope(DebugGroupScope&&) = delete;
  DebugGroupScope& operator=(DebugGroupScope&&) = delete;

 private:
  WMT::CommandEncoder encoder_{};
  bool active_ = false;
};

class PerfScope {
 public:
  explicit PerfScope(void (*record)(std::uint64_t)) : record_(record) {
    if (record_) {
      started_ = std::chrono::steady_clock::now();
    }
  }
  ~PerfScope() {
    if (!record_) {
      return;
    }
    const auto elapsed = std::chrono::steady_clock::now() - started_;
    record_(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()));
  }

  PerfScope(const PerfScope&) = delete;
  PerfScope& operator=(const PerfScope&) = delete;

 private:
  void (*record_)(std::uint64_t) = nullptr;
  std::chrono::steady_clock::time_point started_{};
};

bool suppressRecordedMetalCalls(const EncodeContext& ctx) noexcept {
  return ctx.drawRecorder && ctx.drawRecorder->suppressMetalCalls;
}

bool suppressBaseStateLookup(const EncodeContext& ctx) noexcept {
  return ctx.drawRecorder && ctx.drawRecorder->suppressBaseStateLookup;
}

void recordedSetRenderPipelineState(EncodeContext& ctx,
                                    WMT::RenderCommandEncoder& encoder,
                                    WMT::RenderPipelineState pipeline) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setRenderPipelineState) {
    recorder->setRenderPipelineState(recorder->userdata, pipeline);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setRenderPipelineState(pipeline);
  }
}

void recordedSetDepthStencilState(EncodeContext& ctx,
                                  WMT::RenderCommandEncoder& encoder,
                                  WMT::DepthStencilState depthStencil,
                                  std::uint8_t stencilRef = 0) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setDepthStencilState) {
    recorder->setDepthStencilState(recorder->userdata, depthStencil, stencilRef);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setDepthStencilState(depthStencil, stencilRef);
  }
}

void recordedSetBlendColorAndStencilRef(EncodeContext& ctx,
                                        WMT::RenderCommandEncoder& encoder,
                                        float red,
                                        float green,
                                        float blue,
                                        float alpha,
                                        std::uint8_t stencilRef) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setBlendColorAndStencilRef) {
    recorder->setBlendColorAndStencilRef(
        recorder->userdata, red, green, blue, alpha, stencilRef);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setBlendColorAndStencilRef(red, green, blue, alpha, stencilRef);
  }
}

void recordedSetViewport(EncodeContext& ctx,
                         WMT::RenderCommandEncoder& encoder,
                         WMTViewport viewport) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setViewport) {
    recorder->setViewport(recorder->userdata, viewport);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setViewport(viewport);
  }
}

void recordedSetScissorRect(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTScissorRect rect) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setScissorRect) {
    recorder->setScissorRect(recorder->userdata, rect);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setScissorRect(rect);
  }
}

void recordedSetRasterizerState(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMTTriangleFillMode fillMode,
                                WMTCullMode cullMode,
                                WMTDepthClipMode depthClipMode,
                                WMTWinding winding,
                                float depthBias,
                                float slopeScale,
                                float depthBiasClamp) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setRasterizerState) {
    recorder->setRasterizerState(recorder->userdata, fillMode, cullMode,
                                 depthClipMode, winding, depthBias,
                                 slopeScale, depthBiasClamp);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setRasterizerState(fillMode, cullMode, depthClipMode, winding,
                               depthBias, slopeScale, depthBiasClamp);
  }
}

void recordedSetFragmentTexture(EncodeContext& ctx,
                                WMT::RenderCommandEncoder& encoder,
                                WMT::Texture texture,
                                std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setFragmentTexture) {
    recorder->setFragmentTexture(recorder->userdata, texture, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setFragmentTexture(texture, index);
  }
}

void recordedSetFragmentSamplerState(EncodeContext& ctx,
                                     WMT::RenderCommandEncoder& encoder,
                                     WMT::SamplerState sampler,
                                     std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setFragmentSamplerState) {
    recorder->setFragmentSamplerState(recorder->userdata, sampler, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setFragmentSamplerState(sampler, index);
  }
}

void recordedSetVertexTexture(EncodeContext& ctx,
                              WMT::RenderCommandEncoder& encoder,
                              WMT::Texture texture,
                              std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexTexture) {
    recorder->setVertexTexture(recorder->userdata, texture, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexTexture(texture, index);
  }
}

void recordedSetVertexSamplerState(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMT::SamplerState sampler,
                                   std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexSamplerState) {
    recorder->setVertexSamplerState(recorder->userdata, sampler, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexSamplerState(sampler, index);
  }
}

void recordedSetVertexBuffer(EncodeContext& ctx,
                             WMT::RenderCommandEncoder& encoder,
                             WMT::Buffer buffer,
                             u64 offset,
                             std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexBuffer) {
    recorder->setVertexBuffer(recorder->userdata, buffer, offset, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexBuffer(buffer, offset, index);
  }
}

void recordedSetVertexBytes(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            const void* bytes,
                            u64 length,
                            std::uint8_t index) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->setVertexBytes) {
    recorder->setVertexBytes(recorder->userdata, bytes, length, index);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.setVertexBytes(bytes, length, index);
  }
}

void recordedDrawPrimitives(EncodeContext& ctx,
                            WMT::RenderCommandEncoder& encoder,
                            WMTPrimitiveType primitiveType,
                            u64 vertexStart,
                            u64 vertexCount) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->drawPrimitives) {
    recorder->drawPrimitives(recorder->userdata, primitiveType,
                             vertexStart, vertexCount);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.drawPrimitives(primitiveType, vertexStart, vertexCount);
  }
}

void recordedDrawIndexedPrimitives(EncodeContext& ctx,
                                   WMT::RenderCommandEncoder& encoder,
                                   WMTPrimitiveType primitiveType,
                                   WMTIndexType indexType,
                                   u64 indexCount,
                                   WMT::Buffer indexBuffer,
                                   u64 indexBufferOffset,
                                   u32 instanceCount,
                                   i32 baseVertex,
                                   u32 baseInstance) {
  if (auto* recorder = ctx.drawRecorder;
      recorder && recorder->drawIndexedPrimitives) {
    recorder->drawIndexedPrimitives(recorder->userdata, primitiveType,
                                    indexType, indexCount, indexBuffer,
                                    indexBufferOffset, instanceCount,
                                    baseVertex, baseInstance);
  }
  if (!suppressRecordedMetalCalls(ctx)) {
    encoder.drawIndexedPrimitives(primitiveType, indexType, indexCount,
                                  indexBuffer, indexBufferOffset,
                                  instanceCount, baseVertex, baseInstance);
  }
}

void countTextureBind() {
  perf::countBaseStateBind(1, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}

void countSamplerBind() {
  perf::countBaseStateBind(0, 1, 0, 0, 0, 0, 0, 0, 0, 0);
}

void countTextureBindSkipped() {
  perf::countBaseStateBindSkip(1, 0);
}

void countSamplerBindSkipped() {
  perf::countBaseStateBindSkip(0, 1);
}

void countVertexBufferBind() {
  perf::countBaseStateBind(0, 0, 1, 0, 0, 0, 0, 0, 0, 0);
}

void countIndexBufferBind() {
  perf::countBaseStateBind(0, 0, 0, 1, 0, 0, 0, 0, 0, 0);
}

void countUniformBufferBinds(std::uint32_t count) {
  perf::countBaseStateBind(0, 0, 0, 0, count, 0, 0, 0, 0, 0);
}

void countPipelineBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 1, 0, 0, 0, 0);
}

void countDepthStateBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 1, 0, 0, 0);
}

void countViewportBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 1, 0, 0);
}

void countScissorBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 0, 1, 0);
}

void countRasterizerBind() {
  perf::countBaseStateBind(0, 0, 0, 0, 0, 0, 0, 0, 0, 1);
}

void countVertexBufferBindSkipped() {
  perf::countBaseStateBindSkipExtended(1, 0, 0, 0, 0, 0, 0);
}

void countPipelineBindSkipped() {
  perf::countBaseStateBindSkipExtended(0, 0, 1, 0, 0, 0, 0);
}

void countDepthStateBindSkipped() {
  perf::countBaseStateBindSkipExtended(0, 0, 0, 1, 0, 0, 0);
}

u64 textureSamplerShadowHash(u64 tag,
                             std::uint8_t stage,
                             obj_handle_t handle) noexcept {
  u64 seed = drawBindingPacketHashMix(tag, stage);
  return drawBindingPacketHashMix(seed, static_cast<u64>(handle));
}

bool textureSamplerShadowMatches(const TextureSamplerBindShadowSlot& slot,
                                 u64 hash,
                                 obj_handle_t handle) noexcept {
  return slot.valid && slot.hash == hash && slot.handle == handle;
}

void textureSamplerShadowStore(TextureSamplerBindShadowSlot& slot,
                               u64 hash,
                               obj_handle_t handle) noexcept {
  slot.valid = true;
  slot.hash = hash;
  slot.handle = handle;
}

u64 samplerBindShadowHash(u64 tag,
                          std::uint8_t stage,
                          u64 samplerStateHash,
                          u32 textureLod,
                          bool supportArgumentBuffers) noexcept {
  u64 seed = drawBindingPacketHashMix(tag, stage);
  seed = drawBindingPacketHashMix(seed, textureLod);
  seed = drawBindingPacketHashMix(seed, supportArgumentBuffers ? 1ull : 0ull);
  return drawBindingPacketHashMix(seed, samplerStateHash);
}

bool samplerBindShadowMatches(const SamplerBindShadowSlot& slot,
                              u64 hash,
                              const core::FlatStateSet<core::kMaxSamplerStates>& states,
                              u32 textureLod,
                              bool supportArgumentBuffers) noexcept {
  return slot.valid &&
         slot.hash == hash &&
         slot.textureLod == textureLod &&
         slot.supportArgumentBuffers == supportArgumentBuffers &&
         drawBindingPacketFlatStateSetsEqual(slot.samplerStates, states);
}

void samplerBindShadowStore(SamplerBindShadowSlot& slot,
                            u64 hash,
                            const core::FlatStateSet<core::kMaxSamplerStates>& states,
                            u32 textureLod,
                            bool supportArgumentBuffers,
                            obj_handle_t handle) noexcept {
  slot.valid = true;
  slot.hash = hash;
  slot.handle = handle;
  slot.textureLod = textureLod;
  slot.supportArgumentBuffers = supportArgumentBuffers;
  slot.samplerStates = states;
}

bool bindShadowMatches(const TextureSamplerBindShadowSlot& slot,
                       obj_handle_t handle) noexcept {
  return slot.valid && slot.handle == handle;
}

void bindShadowStore(TextureSamplerBindShadowSlot& slot,
                     obj_handle_t handle) noexcept {
  slot.valid = true;
  slot.hash = 0;
  slot.handle = handle;
}

bool bufferBindShadowMatches(const BufferBindShadowSlot& slot,
                             obj_handle_t handle,
                             u64 offset) noexcept {
  return slot.valid && slot.handle == handle && slot.offset == offset;
}

void bufferBindShadowStore(BufferBindShadowSlot& slot,
                           obj_handle_t handle,
                           u64 offset) noexcept {
  slot.valid = true;
  slot.handle = handle;
  slot.offset = offset;
}

bool x8ShaderAlphaFillEnabledForDiagnostics();

bool effectDrawTraceEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

std::optional<u64> effectDrawTraceSeqFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_SEQ");
  return value;
}

std::optional<u64> effectDrawTraceSeqMinFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_SEQ_MIN");
  return value;
}

std::optional<u64> effectDrawTraceSeqMaxFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_SEQ_MAX");
  return value;
}

bool effectDrawTraceSeqMatches(u64 seqId) {
  if (const auto seqFilter = effectDrawTraceSeqFilter();
      seqFilter.has_value()) {
    return *seqFilter == seqId;
  }
  if (const auto minFilter = effectDrawTraceSeqMinFilter();
      minFilter.has_value() && seqId < *minFilter) {
    return false;
  }
  if (const auto maxFilter = effectDrawTraceSeqMaxFilter();
      maxFilter.has_value() && seqId > *maxFilter) {
    return false;
  }
  return true;
}

std::optional<u64> effectDrawTraceEncoderFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_ENC");
  return value;
}

std::optional<u64> effectDrawTraceTexture0Filter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0");
  return value;
}

std::optional<u64> effectDrawTraceTexture0WidthFilter() {
  static const auto value =
      parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_WIDTH");
  return value;
}

std::optional<u64> effectDrawTraceTexture0HeightFilter() {
  static const auto value =
      parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_HEIGHT");
  return value;
}

std::optional<u64> effectDrawTraceTexture0FormatFilter() {
  static const auto value =
      parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_TEXTURE0_FORMAT");
  return value;
}

std::optional<u64> effectDrawTracePrimitiveTypeFilter() {
  static const auto value = parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_PRIMITIVE_TYPE");
  return value;
}

bool effectDrawTracePointSpriteOnly() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_POINT_SPRITE");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool effectDrawTraceIncludeNonAlpha() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_NON_ALPHA");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool effectDrawTraceIncludeUntextured() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_INCLUDE_UNTEXTURED");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool effectDrawTraceGeometryEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

u64 effectDrawTraceGeometryMaxRefs() {
  static const u64 value = [] {
    const auto envValue =
        parseEnvU64Auto("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY_MAX_REFS");
    return envValue.value_or(384ull);
  }();
  return value;
}

struct IndexReuseMeasure {
  u64 references = 0;
  u64 unique = 0;
  u64 cacheMiss16 = 0;
  u64 cacheMiss32 = 0;
  u64 cacheMiss64 = 0;
  u32 minIndex = 0;
  u32 maxIndex = 0;
  u32 firstIndex = 0;
  u32 lastIndex = 0;
  u64 adjacentDeltaAbsSum = 0;
  u32 adjacentDeltaMax = 0;
  u64 backwardJumps = 0;
  u64 triangleIndexSpanSum = 0;
  u32 triangleIndexSpanMax = 0;
  bool available = false;
};

struct ActiveEncoderBreakdown {
  static_assert(core::kMaxStreams <= perf::kEncoderBreakdownMaxStreams);

  template <std::size_t Size>
  struct CbufHistory {
    bool valid = false;
    u64 validBytes = 0;
    std::array<std::byte, Size> bytes{};
  };

  struct StreamBindReason {
    bool first = false;
    bool handleChange = false;
    bool offsetChange = false;
  };

  struct VsOutLayoutCacheEntry {
    bool valid = false;
    u64 sourceKey = 0;
    bool tileFfpMode = false;
    u32 layoutKey = 0;
  };

  struct ShaderSourceHashCacheEntry {
    bool valid = false;
    u64 sourceKey = 0;
    bool tileFfpBaseColor = false;
    bool argbufHybridMode = false;
    bool argbufResourceArray = false;
    bool argbufDirectCbufMode = false;
    bool samplerLodBias = false;
    u32 x8AlphaOneTextureMask = 0;
    u64 vertexSourceHash = 0;
    u64 pixelSourceHash = 0;
  };

  template <std::size_t Capacity>
  struct UniqueHandleSet {
    static constexpr std::size_t kCapacity = Capacity;
    std::array<u64, kCapacity> handles{};
    std::size_t count = 0;
    bool overflowed = false;
  };

  enum class TransientVertexSource {
    User,
    Preupload,
    DeclFallback,
    ExpandedMain,
    ExpandedExtra,
    StagedStream,
  };

  enum class TransientIndexSource {
    User,
    Preupload,
    ShadowFallback,
    ProbeReorder,
    OptimizedOrder,
    StagedIb,
  };

  bool enabled = false;
  perf::EncoderBreakdown stats{};
  bool ibValid = false;
  u64 ibHandle = 0;
  bool psoValid = false;
  u64 psoHandle = 0;
  bool shaderVariantValid = false;
  u64 shaderVariant = 0;
  bool vsOutLayoutValid = false;
  u32 vsOutLayout = 0;
  bool blendStateValid = false;
  u64 blendState = 0;
  std::array<StreamBindReason, perf::kEncoderBreakdownMaxStreams> streamBindReasons{};
  UniqueHandleSet<2048> streamUniqueHandles{};
  std::array<UniqueHandleSet<512>, perf::kEncoderBreakdownMaxStreams> streamUniqueHandlesByStream{};
  UniqueHandleSet<2048> ibUniqueHandles{};
  UniqueHandleSet<2048> psoUniqueHandles{};
  UniqueHandleSet<2048> shaderVariantUnique{};
  UniqueHandleSet<2048> vsOutLayoutUnique{};
  UniqueHandleSet<2048> blendStateUnique{};
  UniqueHandleSet<2048> x8RtTextureBindingUniqueHandles{};
  UniqueHandleSet<4096> drawGeometrySignatures{};
  std::array<VsOutLayoutCacheEntry, 128> vsOutLayoutCache{};
  std::size_t vsOutLayoutCacheNext = 0;
  std::array<ShaderSourceHashCacheEntry, 128> shaderSourceHashCache{};
  std::size_t shaderSourceHashCacheNext = 0;
  CbufHistory<sizeof(VsConsts)> vsHistory{};
  CbufHistory<sizeof(FfpVsConsts)> ffpVsHistory{};
  bool drawGeometrySignatureValid = false;
  u64 drawGeometrySignatureLast = 0;

  std::string streamExtraBindingsSummary() const {
    std::ostringstream out;
    bool first = true;
    for (std::size_t stream = 1; stream < stats.streams.size(); ++stream) {
      const auto& slot = stats.streams[stream];
      if (!slot.valid || slot.samples == 0) {
        continue;
      }
      if (!first) {
        out << ';';
      }
      first = false;
      out << 's' << stream << ":0x" << std::hex << slot.lastHandle << std::dec
          << '@' << slot.lastOffset << '/' << slot.lastStride;
    }
    return out.str();
  }

  void begin(u64 seqId, u64 encoderIndex, u64 rtHandle, u64 depthHandle) {
    enabled = perf::encoderBreakdownEnabled();
    if (enabled && !perf::encoderBreakdownSeqAllowed(seqId)) {
      enabled = false;
    }
    stats = {};
    ibValid = false;
    ibHandle = 0;
    psoValid = false;
    psoHandle = 0;
    shaderVariantValid = false;
    shaderVariant = 0;
    vsOutLayoutValid = false;
    vsOutLayout = 0;
    blendStateValid = false;
    blendState = 0;
    streamBindReasons = {};
    streamUniqueHandles = {};
    streamUniqueHandlesByStream = {};
    ibUniqueHandles = {};
    psoUniqueHandles = {};
    shaderVariantUnique = {};
    vsOutLayoutUnique = {};
    blendStateUnique = {};
    x8RtTextureBindingUniqueHandles = {};
    drawGeometrySignatures = {};
    vsOutLayoutCache = {};
    vsOutLayoutCacheNext = 0;
    shaderSourceHashCache = {};
    shaderSourceHashCacheNext = 0;
    vsHistory = {};
    ffpVsHistory = {};
    drawGeometrySignatureValid = false;
    drawGeometrySignatureLast = 0;
    stats.seqId = seqId;
    stats.encoderIndex = encoderIndex;
    stats.rtHandle = rtHandle;
    stats.depthHandle = depthHandle;
    if (!enabled) {
      return;
    }
  }

  void recordAttachmentMetadata(const resources::Pool& pool,
                                const core::FlatDrawStateRecord& hot) {
    if (!enabled) {
      return;
    }
    auto fillSurface = [&](core::Handle handle,
                           u64& format,
                           u64& width,
                           u64& height,
                           u64& bytesPerPixel,
                           u64& aliasTexture,
                           u64& textureUsage,
                           u64& formatSwizzle,
                           u64& textureNeedsView) {
      const auto* surface = pool.findSurface(handle.value);
      if (!surface) {
        return;
      }
      format = static_cast<u64>(surface->desc.format);
      width = surface->desc.width;
      height = surface->desc.height;
      bytesPerPixel = core::bytesPerPixel(surface->desc.format);
      formatSwizzle = convert::formatNeedsShaderReadSwizzle(surface->desc.format) ? 1u : 0u;
      aliasTexture = surface->aliasTexture.value;
      const auto* texture =
          surface->aliasTexture ? pool.findTexture(surface->aliasTexture.value) : nullptr;
      if (!texture) {
        return;
      }
      textureUsage = texture->desc.usage;
      textureNeedsView = convert::textureNeedsShaderReadView(texture->desc) ? 1u : 0u;
    };

    fillSurface(hot.colorAttachments[0].handle,
                stats.rtFormat,
                stats.rtWidth,
                stats.rtHeight,
                stats.rtBytesPerPixel,
                stats.rtAliasTexture,
                stats.rtTextureUsage,
                stats.rtFormatNeedsShaderReadSwizzle,
                stats.rtTextureNeedsShaderReadView);
    fillSurface(hot.depthStencil.handle,
                stats.depthFormat,
                stats.depthWidth,
                stats.depthHeight,
                stats.depthBytesPerPixel,
                stats.depthAliasTexture,
                stats.depthTextureUsage,
                stats.depthFormatNeedsShaderReadSwizzle,
                stats.depthTextureNeedsShaderReadView);
  }

  void recordRenderPassActions(const RenderPassActionSummary& summary) {
    if (!enabled) {
      return;
    }
    stats.colorAttachmentCount = summary.colorAttachmentCount;
    stats.color0Included = summary.color0Included;
    stats.color0LoadAction = summary.color0LoadAction;
    stats.color0StoreAction = summary.color0StoreAction;
    stats.color0Clear = summary.color0Clear;
    stats.colorLoadBytes = summary.colorLoadBytes;
    stats.colorStoreBytes = summary.colorStoreBytes;
    stats.depthIncluded = summary.depthIncluded;
    stats.depthLoadAction = summary.depthLoadAction;
    stats.depthStoreAction = summary.depthStoreAction;
    stats.depthClear = summary.depthClear;
    stats.depthLoadBytes = summary.depthLoadBytes;
    stats.depthStoreBytes = summary.depthStoreBytes;
    stats.stencilIncluded = summary.stencilIncluded;
    stats.stencilLoadAction = summary.stencilLoadAction;
    stats.stencilStoreAction = summary.stencilStoreAction;
    stats.stencilClear = summary.stencilClear;
    stats.stencilLoadBytes = summary.stencilLoadBytes;
    stats.stencilStoreBytes = summary.stencilStoreBytes;
  }

  void recordFragmentTextureBinding(u32 stage,
                                    core::Handle textureHandle,
                                    const resources::TextureRecord* texture) {
    if (!enabled || !texture || !textureHandle || stage >= 64u) {
      return;
    }
    ++stats.fragmentTextureBindingSamples;
    stats.fragmentTextureBindingMaskOr |= 1ull << stage;
    const bool x8Format =
        texture->desc.format == core::Format::X8R8G8B8 ||
        texture->desc.format == core::Format::X8B8G8R8;
    const bool renderTargetTexture =
        (texture->desc.usage & core::UsageRenderTarget) != 0u ||
        (texture->desc.usage & core::UsageDepthStencil) != 0u;
    if (!x8Format || !renderTargetTexture) {
      return;
    }
    ++stats.x8RtTextureBindingSamples;
    stats.x8RtTextureBindingMaskOr |= 1ull << stage;
    stats.x8RtTextureBindingLastStage = stage;
    stats.x8RtTextureBindingLastHandle = textureHandle.value;
    if (convert::textureNeedsShaderReadView(texture->desc)) {
      ++stats.x8RtTextureBindingShaderReadViewSamples;
    }
    if (x8ShaderAlphaFillEnabledForDiagnostics()) {
      ++stats.x8ShaderAlphaFillSamples;
      stats.x8ShaderAlphaFillMaskOr |= 1ull << stage;
    }
    if (stats.rtAliasTexture != 0 && textureHandle.value == stats.rtAliasTexture) {
      ++stats.x8RtTextureBindingActiveRtAliasSamples;
    }
    recordUnique(x8RtTextureBindingUniqueHandles,
                 textureHandle.value,
                 stats.x8RtTextureBindingUniqueHandles,
                 stats.x8RtTextureBindingUniqueHandleOverflows);
  }

  void emit(perf::EncoderSplitReason reason) {
    if (!enabled) {
      return;
    }
    stats.endReason = reason;
    perf::emitEncoderBreakdown(stats);
    enabled = false;
  }

  static u64 triangleEstimateFor(core::PrimitiveType primitiveType,
                                 u32 primitiveCount) {
    switch (primitiveType) {
      case core::PrimitiveType::TriangleList:
      case core::PrimitiveType::TriangleStrip:
      case core::PrimitiveType::TriangleFan:
        return primitiveCount;
      default:
        return 0;
    }
  }

  static u64 mixSignature(u64 seed, u64 value) {
    return drawBindingPacketHashMix(seed, value);
  }

  u64 makeDrawGeometrySignature(core::PrimitiveType primitiveType,
                                u32 primitiveCount,
                                u64 vertexCount,
                                bool indexed,
                                bool expandedIndexed,
                                bool fixedFunction,
                                bool preTransformed,
                                u32 textureMask,
                                u32 stream0Stride,
                                i32 drawVertexBaseIndex,
                                u32 drawVertexStreamOffset,
                                u32 startIndex,
                                core::IndexType indexType,
                                const core::FlatRenderStateSet& renderStates,
                                const core::ViewportScissor& viewport,
                                WMTCullMode cullMode,
                                WMTTriangleFillMode fillMode) const {
    u64 seed = 0x7be1d1f73c46a715ull;
    seed = mixSignature(seed, static_cast<u32>(primitiveType));
    seed = mixSignature(seed, primitiveCount);
    seed = mixSignature(seed, vertexCount);
    seed = mixSignature(seed, indexed ? 1ull : 0ull);
    seed = mixSignature(seed, expandedIndexed ? 1ull : 0ull);
    seed = mixSignature(seed, fixedFunction ? 1ull : 0ull);
    seed = mixSignature(seed, preTransformed ? 1ull : 0ull);
    seed = mixSignature(seed, textureMask);
    seed = mixSignature(seed, stream0Stride);
    seed = mixSignature(seed, static_cast<u64>(static_cast<std::int64_t>(drawVertexBaseIndex)));
    seed = mixSignature(seed, drawVertexStreamOffset);
    seed = mixSignature(seed, startIndex);
    seed = mixSignature(seed, static_cast<u32>(indexType));
    seed = mixSignature(seed, psoHandle);
    seed = mixSignature(seed, shaderVariant);
    seed = mixSignature(seed, vertexShaderHashForSignature());
    seed = mixSignature(seed, pixelShaderHashForSignature());
    seed = mixSignature(seed, vsOutLayout);
    seed = mixSignature(seed, ibValid ? ibHandle : 0ull);
    for (const auto& stream : stats.streams) {
      if (!stream.valid) {
        continue;
      }
      seed = mixSignature(seed, stream.lastHandle);
      seed = mixSignature(seed, stream.lastOffset);
      seed = mixSignature(seed, stream.lastStride);
    }
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_Z_ENABLE, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_Z_FUNC, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_SRC_BLEND, 0u));
    seed = mixSignature(seed, core::flatStateOr(renderStates, RS_DEST_BLEND, 0u));
    seed = mixSignature(seed, static_cast<u32>(cullMode));
    seed = mixSignature(seed, static_cast<u32>(fillMode));
    seed = mixSignature(seed, viewport.scissorEnabled ? 1ull : 0ull);
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.left));
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.top));
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.right));
    seed = mixSignature(seed, static_cast<u32>(viewport.scissor.bottom));
    return seed ? seed : 1ull;
  }

  u64 vertexShaderHashForSignature() const {
    return stats.vertexShaderLast;
  }

  u64 pixelShaderHashForSignature() const {
    return stats.pixelShaderLast;
  }

  void recordDrawGeometrySignature(u64 signature) {
    ++stats.drawGeometrySignatureSamples;
    stats.drawGeometrySignatureLast = signature;
    if (drawGeometrySignatureValid && drawGeometrySignatureLast == signature) {
      ++stats.drawGeometrySignatureConsecutiveDuplicates;
    }
    drawGeometrySignatureValid = true;
    drawGeometrySignatureLast = signature;

    for (std::size_t i = 0; i < drawGeometrySignatures.count; ++i) {
      if (drawGeometrySignatures.handles[i] == signature) {
        ++stats.drawGeometrySignatureDuplicates;
        return;
      }
    }
    if (drawGeometrySignatures.count >= drawGeometrySignatures.handles.size()) {
      if (!drawGeometrySignatures.overflowed) {
        drawGeometrySignatures.overflowed = true;
        ++stats.drawGeometrySignatureUniqueOverflows;
      }
      return;
    }
    drawGeometrySignatures.handles[drawGeometrySignatures.count++] = signature;
    ++stats.drawGeometrySignatureUnique;
  }

  void recordDrawSize(u32 primitiveCount, u64 vertexCount) {
    if (stats.drawPrimitiveCountMin == 0 || primitiveCount < stats.drawPrimitiveCountMin) {
      stats.drawPrimitiveCountMin = primitiveCount;
    }
    if (primitiveCount > stats.drawPrimitiveCountMax) {
      stats.drawPrimitiveCountMax = primitiveCount;
    }
    if (stats.drawVertexCountMin == 0 || vertexCount < stats.drawVertexCountMin) {
      stats.drawVertexCountMin = vertexCount;
    }
    if (vertexCount > stats.drawVertexCountMax) {
      stats.drawVertexCountMax = vertexCount;
    }

    if (primitiveCount < 64) {
      ++stats.drawPrimitiveBucket1_63;
    } else if (primitiveCount < 256) {
      ++stats.drawPrimitiveBucket64_255;
    } else if (primitiveCount < 1024) {
      ++stats.drawPrimitiveBucket256_1023;
    } else if (primitiveCount < 4096) {
      ++stats.drawPrimitiveBucket1024_4095;
    } else {
      ++stats.drawPrimitiveBucket4096Plus;
    }

    if (vertexCount < 256) {
      ++stats.drawVertexBucket1_255;
    } else if (vertexCount < 1024) {
      ++stats.drawVertexBucket256_1023;
    } else if (vertexCount < 4096) {
      ++stats.drawVertexBucket1024_4095;
    } else if (vertexCount < 16384) {
      ++stats.drawVertexBucket4096_16383;
    } else {
      ++stats.drawVertexBucket16384Plus;
    }
  }

  void recordSplitLargeIndexedDraw(u32 primitiveCount,
                                   u32 primitiveLimit,
                                   u64 stream0SpanLimit,
                                   u64 maxChunkStream0Span,
                                   u32 metalDraws) {
    if (!enabled || metalDraws <= 1u) {
      return;
    }
    ++stats.splitLargeIndexedSourceDraws;
    stats.splitLargeIndexedMetalDraws += metalDraws;
    stats.splitLargeIndexedExtraDraws += static_cast<u64>(metalDraws - 1u);
    stats.splitLargeIndexedPrimitiveCount += primitiveCount;
    if (primitiveLimit != 0u) {
      stats.splitLargeIndexedPrimitiveLimit =
          stats.splitLargeIndexedPrimitiveLimit == 0
              ? primitiveLimit
              : std::min<std::uint64_t>(stats.splitLargeIndexedPrimitiveLimit,
                                        primitiveLimit);
    }
    if (stream0SpanLimit != 0u) {
      stats.splitLargeIndexedStream0SpanLimit =
          stats.splitLargeIndexedStream0SpanLimit == 0
              ? stream0SpanLimit
              : std::min<std::uint64_t>(stats.splitLargeIndexedStream0SpanLimit,
                                        stream0SpanLimit);
    }
    stats.splitLargeIndexedChunkStream0SpanMax =
        std::max(stats.splitLargeIndexedChunkStream0SpanMax,
                 maxChunkStream0Span);
  }

  void recordIndexedOrderProbe(bool applied, u64 bytes) {
    if (!enabled) {
      return;
    }
    if (!applied) {
      ++stats.indexedOrderProbeSkipped;
      return;
    }
    ++stats.indexedOrderProbeDraws;
    stats.indexedOrderProbeBytes += bytes;
  }

  void recordIndexedOrderOptimization(bool applied, u64 bytes) {
    if (!enabled) {
      return;
    }
    if (!applied) {
      ++stats.indexedOrderOptimizedSkipped;
      return;
    }
    ++stats.indexedOrderOptimizedDraws;
    stats.indexedOrderOptimizedBytes += bytes;
  }

  static u64 rectAreaPixels(const core::Rect& rect) {
    const auto width = std::max(0, rect.right - rect.left);
    const auto height = std::max(0, rect.bottom - rect.top);
    return static_cast<u64>(width) * static_cast<u64>(height);
  }

  void recordScissorRectProbe(bool applied,
                              const core::Rect& originalRect,
                              const core::Rect& overrideRect) {
    if (!enabled) {
      return;
    }
    if (!applied) {
      ++stats.probeScissorRectSkipped;
      return;
    }
    ++stats.probeScissorRectDraws;
    const auto originalArea = rectAreaPixels(originalRect);
    const auto overrideArea = rectAreaPixels(overrideRect);
    stats.probeScissorRectAreaDeltaPixels +=
        originalArea > overrideArea ? originalArea - overrideArea
                                    : overrideArea - originalArea;
  }

  void emitIndexedOrderProbeDraw(bool probeEligible,
                                 bool probeApplied,
                                 bool optimizedEligible,
                                 bool optimizedApplied,
                                 bool scissorRectEligible,
                                 bool scissorRectApplied,
                                 bool alphaBlendProbeApplied,
                                 bool depthWriteProbeApplied,
                                 bool depthFuncProbeApplied,
                                 bool fragmentlessDepthOnlyProbeApplied,
                                 bool splitEligible,
                                 bool splitWouldApply,
                                 u32 splitChunkCount,
                                 u32 splitMaxChunksPerDraw,
                                 u64 splitStream0SpanLimit,
                                 u64 splitChunkStream0SpanMax,
                                 u64 splitPrimitiveCount,
                                 u64 reorderBytes,
                                 IndexReuseMeasure originalIndexReuse,
                                 IndexReuseMeasure effectiveIndexReuse,
                                 IndexReuseMeasure candidateIndexReuse,
                                 bool candidateBuilt,
                                 bool candidateGatePassed,
                                 u64 drawOrdinal,
                                 u64 commandIndex,
                                 core::PrimitiveType primitiveType,
                                 u32 primitiveCount,
                                 u64 vertexCount,
                                 u32 textureMask,
                                 std::span<const core::Handle> fragmentTextures,
                                 const core::FlatRenderStateSet& renderStates,
                                 const core::ViewportScissor& viewport,
                                 WMTCullMode cullMode,
                                 WMTTriangleFillMode fillMode,
                                 i32 baseVertexIndex,
                                 u32 startIndex,
                                 core::IndexType indexType,
                                 u64 indexBufferHandle,
                                 const char* effectiveIndexSource,
                                 u64 effectiveIndexOffset,
                                 u64 effectiveIndexBytes,
                                 u64 stream0Handle,
                                 u64 stream0Offset,
                                 u64 stream0Stride,
                                 const char* streamExtraBindings,
                                 u64 vertexConstantsHash,
                                 u64 pixelConstantsHash,
                                 u64 uniformPayloadHash,
                                 const core::Rect& originalScissor) {
    if (!enabled) {
      return;
    }
    const bool depthEnabled =
        core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
    const bool depthWrite =
        depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
    const bool alphaBlendEnabled =
        core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
    const bool alphaTestEnabled =
        core::flatStateOr(renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u;
    const bool stencilEnabled =
        core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
    const bool clipPlaneEnabled =
        core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;
    const auto colorWrite =
        core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
    const auto srcBlend = core::flatStateOr(renderStates, RS_SRC_BLEND, 0u);
    const auto dstBlend = core::flatStateOr(renderStates, RS_DEST_BLEND, 0u);
    const auto blendOp = core::flatStateOr(renderStates, RS_BLEND_OP, 0u);
    const auto separateAlpha =
        core::flatStateOr(renderStates, RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u);
    const auto srcBlendAlpha =
        core::flatStateOr(renderStates, RS_SRC_BLEND_ALPHA, srcBlend);
    const auto dstBlendAlpha =
        core::flatStateOr(renderStates, RS_DEST_BLEND_ALPHA, dstBlend);
    const auto blendOpAlpha =
        core::flatStateOr(renderStates, RS_BLEND_OP_ALPHA, blendOp);
    const auto depthFunc = core::flatStateOr(
        renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual));
    auto textureHandleValue = [&](std::size_t stage) -> u64 {
      return stage < fragmentTextures.size() ? fragmentTextures[stage].value : 0ull;
    };
    auto streamByteMin = [](const IndexReuseMeasure& measure,
                            i32 baseVertex,
                            u64 streamOffset,
                            u64 streamStride) -> u64 {
      if (!measure.available || streamStride == 0u) {
        return 0u;
      }
      const auto minVertex =
          static_cast<std::int64_t>(baseVertex) +
          static_cast<std::int64_t>(measure.minIndex);
      if (minVertex < 0) {
        return 0u;
      }
      return streamOffset + static_cast<u64>(minVertex) * streamStride;
    };
    auto streamByteMax = [](const IndexReuseMeasure& measure,
                            i32 baseVertex,
                            u64 streamOffset,
                            u64 streamStride) -> u64 {
      if (!measure.available || streamStride == 0u) {
        return 0u;
      }
      const auto maxVertex =
          static_cast<std::int64_t>(baseVertex) +
          static_cast<std::int64_t>(measure.maxIndex);
      if (maxVertex < 0) {
        return 0u;
      }
      return streamOffset + static_cast<u64>(maxVertex) * streamStride;
    };
    const auto originalStreamByteMin =
        streamByteMin(originalIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto originalStreamByteMax =
        streamByteMax(originalIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto effectiveStreamByteMin =
        streamByteMin(effectiveIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto effectiveStreamByteMax =
        streamByteMax(effectiveIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto candidateStreamByteMin =
        streamByteMin(candidateIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    const auto candidateStreamByteMax =
        streamByteMax(candidateIndexReuse, baseVertexIndex, stream0Offset, stream0Stride);
    std::fprintf(
        stderr,
        "[dxmt9-perf-indexed-probe-draw seq=%llu encoder=%llu "
        "encoder_draw_index=%llu draw_ordinal=%llu command_index=%llu "
        "eligible=%u applied=%u "
        "optimized_eligible=%u optimized_applied=%u "
        "scissor_rect_eligible=%u scissor_rect_applied=%u "
        "alpha_blend_probe_applied=%u depth_write_probe_applied=%u "
        "depth_func_probe_applied=%u "
        "fragmentless_depth_only_probe_applied=%u reorder_bytes=%llu "
        "split_eligible=%u split_would_apply=%u split_chunk_count=%u "
        "split_max_chunks_per_draw=%u split_stream0_span_limit=%llu "
        "split_chunk_stream0_span_max=%llu split_primitive_count=%llu "
        "original_index_available=%u original_index_unique=%llu "
        "original_index_min=%u original_index_max=%u original_index_span=%llu "
        "original_index_first=%u original_index_last=%u "
        "original_cache_miss16=%llu original_cache_miss32=%llu "
        "original_cache_miss64=%llu original_adjacent_delta_sum=%llu "
        "original_adjacent_delta_max=%u original_backward_jumps=%llu "
        "original_triangle_index_span_sum=%llu "
        "original_triangle_index_span_max=%u "
        "original_stream0_byte_min=%llu original_stream0_byte_max=%llu "
        "original_stream0_byte_span=%llu "
        "effective_index_available=%u effective_index_unique=%llu "
        "effective_index_min=%u effective_index_max=%u effective_index_span=%llu "
        "effective_index_first=%u effective_index_last=%u "
        "effective_cache_miss16=%llu effective_cache_miss32=%llu "
        "effective_cache_miss64=%llu effective_adjacent_delta_sum=%llu "
        "effective_adjacent_delta_max=%u effective_backward_jumps=%llu "
        "effective_triangle_index_span_sum=%llu "
        "effective_triangle_index_span_max=%u "
        "effective_stream0_byte_min=%llu effective_stream0_byte_max=%llu "
        "effective_stream0_byte_span=%llu "
        "candidate_built=%u candidate_gate_passed=%u "
        "candidate_index_available=%u candidate_index_unique=%llu "
        "candidate_index_min=%u candidate_index_max=%u candidate_index_span=%llu "
        "candidate_index_first=%u candidate_index_last=%u "
        "candidate_cache_miss16=%llu candidate_cache_miss32=%llu "
        "candidate_cache_miss64=%llu candidate_adjacent_delta_sum=%llu "
        "candidate_adjacent_delta_max=%u candidate_backward_jumps=%llu "
        "candidate_triangle_index_span_sum=%llu "
        "candidate_triangle_index_span_max=%u "
        "candidate_stream0_byte_min=%llu candidate_stream0_byte_max=%llu "
        "candidate_stream0_byte_span=%llu "
        "primitive_type=%u primitive_count=%u vertex_count=%llu "
        "texture_mask=0x%x "
        "texture0=0x%llx texture1=0x%llx texture2=0x%llx texture3=0x%llx "
        "texture4=0x%llx texture5=0x%llx texture6=0x%llx texture7=0x%llx "
        "color_write=0x%x alpha_blend=%u "
        "src_blend=%u dst_blend=%u blend_op=%u separate_alpha=%u "
        "src_blend_alpha=%u dst_blend_alpha=%u blend_op_alpha=%u "
        "alpha_test=%u depth_enabled=%u "
        "depth_write=%u depth_func=%u stencil=%u clip_plane=%u scissor=%u "
        "scissor_l=%d scissor_t=%d scissor_r=%d scissor_b=%d "
        "original_scissor_l=%d original_scissor_t=%d "
        "original_scissor_r=%d original_scissor_b=%d "
        "cull=%u fill=%u base_vertex=%d start_index=%u index_type=%u "
        "index_buffer=0x%llx effective_index_source=%s "
        "effective_index_offset=%llu effective_index_bytes=%llu "
        "stream0_handle=0x%llx stream0_offset=%llu "
        "stream0_stride=%llu stream_extra_bindings=%s "
        "pso=0x%llx shader_variant=0x%llx "
        "vs=0x%llx ps=0x%llx vs_constants_hash=0x%llx "
        "ps_constants_hash=0x%llx uniform_payload_hash=0x%llx "
        "vsout=0x%x]\n",
        static_cast<unsigned long long>(stats.seqId),
        static_cast<unsigned long long>(stats.encoderIndex),
        static_cast<unsigned long long>(stats.drawCalls),
        static_cast<unsigned long long>(drawOrdinal),
        static_cast<unsigned long long>(commandIndex),
        probeEligible ? 1u : 0u,
        probeApplied ? 1u : 0u,
        optimizedEligible ? 1u : 0u,
        optimizedApplied ? 1u : 0u,
        scissorRectEligible ? 1u : 0u,
        scissorRectApplied ? 1u : 0u,
        alphaBlendProbeApplied ? 1u : 0u,
        depthWriteProbeApplied ? 1u : 0u,
        depthFuncProbeApplied ? 1u : 0u,
        fragmentlessDepthOnlyProbeApplied ? 1u : 0u,
        static_cast<unsigned long long>(reorderBytes),
        splitEligible ? 1u : 0u,
        splitWouldApply ? 1u : 0u,
        splitChunkCount,
        splitMaxChunksPerDraw,
        static_cast<unsigned long long>(splitStream0SpanLimit),
        static_cast<unsigned long long>(splitChunkStream0SpanMax),
        static_cast<unsigned long long>(splitPrimitiveCount),
        originalIndexReuse.available ? 1u : 0u,
        static_cast<unsigned long long>(originalIndexReuse.unique),
        originalIndexReuse.minIndex,
        originalIndexReuse.maxIndex,
        static_cast<unsigned long long>(
            originalIndexReuse.available
                ? static_cast<u64>(originalIndexReuse.maxIndex) -
                      static_cast<u64>(originalIndexReuse.minIndex) + 1u
                : 0u),
        originalIndexReuse.firstIndex,
        originalIndexReuse.lastIndex,
        static_cast<unsigned long long>(originalIndexReuse.cacheMiss16),
        static_cast<unsigned long long>(originalIndexReuse.cacheMiss32),
        static_cast<unsigned long long>(originalIndexReuse.cacheMiss64),
        static_cast<unsigned long long>(originalIndexReuse.adjacentDeltaAbsSum),
        originalIndexReuse.adjacentDeltaMax,
        static_cast<unsigned long long>(originalIndexReuse.backwardJumps),
        static_cast<unsigned long long>(originalIndexReuse.triangleIndexSpanSum),
        originalIndexReuse.triangleIndexSpanMax,
        static_cast<unsigned long long>(originalStreamByteMin),
        static_cast<unsigned long long>(originalStreamByteMax),
        static_cast<unsigned long long>(
            originalStreamByteMax >= originalStreamByteMin
                ? originalStreamByteMax - originalStreamByteMin
                : 0u),
        effectiveIndexReuse.available ? 1u : 0u,
        static_cast<unsigned long long>(effectiveIndexReuse.unique),
        effectiveIndexReuse.minIndex,
        effectiveIndexReuse.maxIndex,
        static_cast<unsigned long long>(
            effectiveIndexReuse.available
                ? static_cast<u64>(effectiveIndexReuse.maxIndex) -
                      static_cast<u64>(effectiveIndexReuse.minIndex) + 1u
                : 0u),
        effectiveIndexReuse.firstIndex,
        effectiveIndexReuse.lastIndex,
        static_cast<unsigned long long>(effectiveIndexReuse.cacheMiss16),
        static_cast<unsigned long long>(effectiveIndexReuse.cacheMiss32),
        static_cast<unsigned long long>(effectiveIndexReuse.cacheMiss64),
        static_cast<unsigned long long>(effectiveIndexReuse.adjacentDeltaAbsSum),
        effectiveIndexReuse.adjacentDeltaMax,
        static_cast<unsigned long long>(effectiveIndexReuse.backwardJumps),
        static_cast<unsigned long long>(effectiveIndexReuse.triangleIndexSpanSum),
        effectiveIndexReuse.triangleIndexSpanMax,
        static_cast<unsigned long long>(effectiveStreamByteMin),
        static_cast<unsigned long long>(effectiveStreamByteMax),
        static_cast<unsigned long long>(
            effectiveStreamByteMax >= effectiveStreamByteMin
                ? effectiveStreamByteMax - effectiveStreamByteMin
                : 0u),
        candidateBuilt ? 1u : 0u,
        candidateGatePassed ? 1u : 0u,
        candidateIndexReuse.available ? 1u : 0u,
        static_cast<unsigned long long>(candidateIndexReuse.unique),
        candidateIndexReuse.minIndex,
        candidateIndexReuse.maxIndex,
        static_cast<unsigned long long>(
            candidateIndexReuse.available
                ? static_cast<u64>(candidateIndexReuse.maxIndex) -
                      static_cast<u64>(candidateIndexReuse.minIndex) + 1u
                : 0u),
        candidateIndexReuse.firstIndex,
        candidateIndexReuse.lastIndex,
        static_cast<unsigned long long>(candidateIndexReuse.cacheMiss16),
        static_cast<unsigned long long>(candidateIndexReuse.cacheMiss32),
        static_cast<unsigned long long>(candidateIndexReuse.cacheMiss64),
        static_cast<unsigned long long>(candidateIndexReuse.adjacentDeltaAbsSum),
        candidateIndexReuse.adjacentDeltaMax,
        static_cast<unsigned long long>(candidateIndexReuse.backwardJumps),
        static_cast<unsigned long long>(candidateIndexReuse.triangleIndexSpanSum),
        candidateIndexReuse.triangleIndexSpanMax,
        static_cast<unsigned long long>(candidateStreamByteMin),
        static_cast<unsigned long long>(candidateStreamByteMax),
        static_cast<unsigned long long>(
            candidateStreamByteMax >= candidateStreamByteMin
                ? candidateStreamByteMax - candidateStreamByteMin
                : 0u),
        static_cast<unsigned>(primitiveType),
        primitiveCount,
        static_cast<unsigned long long>(vertexCount),
        textureMask,
        static_cast<unsigned long long>(textureHandleValue(0)),
        static_cast<unsigned long long>(textureHandleValue(1)),
        static_cast<unsigned long long>(textureHandleValue(2)),
        static_cast<unsigned long long>(textureHandleValue(3)),
        static_cast<unsigned long long>(textureHandleValue(4)),
        static_cast<unsigned long long>(textureHandleValue(5)),
        static_cast<unsigned long long>(textureHandleValue(6)),
        static_cast<unsigned long long>(textureHandleValue(7)),
        colorWrite,
        alphaBlendEnabled ? 1u : 0u,
        srcBlend,
        dstBlend,
        blendOp,
        separateAlpha,
        srcBlendAlpha,
        dstBlendAlpha,
        blendOpAlpha,
        alphaTestEnabled ? 1u : 0u,
        depthEnabled ? 1u : 0u,
        depthWrite ? 1u : 0u,
        depthFunc,
        stencilEnabled ? 1u : 0u,
        clipPlaneEnabled ? 1u : 0u,
        viewport.scissorEnabled ? 1u : 0u,
        static_cast<int>(viewport.scissor.left),
        static_cast<int>(viewport.scissor.top),
        static_cast<int>(viewport.scissor.right),
        static_cast<int>(viewport.scissor.bottom),
        static_cast<int>(originalScissor.left),
        static_cast<int>(originalScissor.top),
        static_cast<int>(originalScissor.right),
        static_cast<int>(originalScissor.bottom),
        static_cast<unsigned>(cullMode),
        static_cast<unsigned>(fillMode),
        baseVertexIndex,
        startIndex,
        static_cast<unsigned>(indexType),
        static_cast<unsigned long long>(indexBufferHandle),
        effectiveIndexSource ? effectiveIndexSource : "",
        static_cast<unsigned long long>(effectiveIndexOffset),
        static_cast<unsigned long long>(effectiveIndexBytes),
        static_cast<unsigned long long>(stream0Handle),
        static_cast<unsigned long long>(stream0Offset),
        static_cast<unsigned long long>(stream0Stride),
        streamExtraBindings ? streamExtraBindings : "",
        static_cast<unsigned long long>(psoHandle),
        static_cast<unsigned long long>(shaderVariant),
        static_cast<unsigned long long>(stats.vertexShaderLast),
        static_cast<unsigned long long>(stats.pixelShaderLast),
        static_cast<unsigned long long>(vertexConstantsHash),
        static_cast<unsigned long long>(pixelConstantsHash),
        static_cast<unsigned long long>(uniformPayloadHash),
        vsOutLayout);
  }

  void recordIndexedVertexReuse(IndexReuseMeasure measure) {
    if (!enabled) {
      return;
    }
    stats.indexedVertexReferenceCount += measure.references;
    if (!measure.available) {
      ++stats.indexedVertexReuseSkipped;
      return;
    }
    ++stats.indexedVertexReuseSamples;
    stats.indexedUniqueVertexEstimate += measure.unique;
    stats.indexedVertexCacheMissEstimate16 += measure.cacheMiss16;
    stats.indexedVertexCacheMissEstimate32 += measure.cacheMiss32;
    stats.indexedVertexCacheMissEstimate64 += measure.cacheMiss64;
  }

  void recordIndexedCacheOptCandidate(IndexReuseMeasure original,
                                      IndexReuseMeasure candidate,
                                      u64 bytes) {
    if (!enabled) {
      return;
    }
    if (!original.available || !candidate.available) {
      ++stats.indexedCacheOptCandidateSkipped;
      return;
    }
    ++stats.indexedCacheOptCandidateDraws;
    stats.indexedCacheOptCandidateBytes += bytes;
    stats.indexedCacheOptCandidateOriginalMiss16 += original.cacheMiss16;
    stats.indexedCacheOptCandidateOriginalMiss32 += original.cacheMiss32;
    stats.indexedCacheOptCandidateOriginalMiss64 += original.cacheMiss64;
    stats.indexedCacheOptCandidateMiss16 += candidate.cacheMiss16;
    stats.indexedCacheOptCandidateMiss32 += candidate.cacheMiss32;
    stats.indexedCacheOptCandidateMiss64 += candidate.cacheMiss64;
  }

  void recordIndexedCacheOptCandidateGate(bool passed,
                                          u64 primitiveCount,
                                          bool opaqueDepth,
                                          bool screenBlend) {
    if (!enabled) {
      return;
    }
    if (passed) {
      ++stats.indexedCacheOptCandidateGatePass;
    } else {
      ++stats.indexedCacheOptCandidateGateFail;
    }
    if (opaqueDepth) {
      ++stats.indexedCacheOptCandidateOpaqueDepthDraws;
    }
    if (screenBlend) {
      ++stats.indexedCacheOptCandidateScreenBlendDraws;
    }
    if (primitiveCount < 64) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket1_63;
    } else if (primitiveCount < 256) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket64_255;
    } else if (primitiveCount < 1024) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket256_1023;
    } else if (primitiveCount < 4096) {
      ++stats.indexedCacheOptCandidatePrimitiveBucket1024_4095;
    } else {
      ++stats.indexedCacheOptCandidatePrimitiveBucket4096Plus;
    }
  }

  void recordReorderedIndexCacheLookup(bool hit,
                                       bool rejected,
                                       bool created,
                                       u64 createdBytes) {
    if (!enabled) {
      return;
    }
    ++stats.reorderedIndexCacheLookups;
    if (hit) {
      ++stats.reorderedIndexCacheHits;
    } else if (rejected) {
      ++stats.reorderedIndexCacheRejectedHits;
    } else {
      ++stats.reorderedIndexCacheMisses;
    }
    if (created) {
      ++stats.reorderedIndexCacheCreated;
      stats.reorderedIndexCacheCreatedBytes += createdBytes;
    }
  }

  void recordTileFfpCoverage(const pipeline::TileFfpSelection& eligibility,
                             bool routedTile,
                             u32 primitiveCount,
                             u64 vertexCount) {
    if (!enabled) {
      return;
    }
    auto addDraw = [&](std::uint64_t& draws,
                       std::uint64_t& primitives,
                       std::uint64_t& vertices) {
      ++draws;
      primitives += primitiveCount;
      vertices += vertexCount;
    };
    auto addDrawNoVertices = [&](std::uint64_t& draws,
                                 std::uint64_t& primitives) {
      ++draws;
      primitives += primitiveCount;
    };

    if (routedTile) {
      addDraw(stats.tileFfpRoutedTileDraws,
              stats.tileFfpRoutedTilePrimitives,
              stats.tileFfpRoutedTileVertices);
    } else {
      addDraw(stats.tileFfpRoutedPortableDraws,
              stats.tileFfpRoutedPortablePrimitives,
              stats.tileFfpRoutedPortableVertices);
    }

    if (eligibility.decision == pipeline::TileFfpDecision::Tile) {
      addDraw(stats.tileFfpEligibleDraws,
              stats.tileFfpEligiblePrimitives,
              stats.tileFfpEligibleVertices);
      return;
    }

    switch (eligibility.reason) {
      case pipeline::TileFfpFallbackReason::GpuFamily:
        addDrawNoVertices(stats.tileFfpFallbackGpuFamilyDraws,
                          stats.tileFfpFallbackGpuFamilyPrimitives);
        break;
      case pipeline::TileFfpFallbackReason::NotFfp:
        addDrawNoVertices(stats.tileFfpFallbackNotFfpDraws,
                          stats.tileFfpFallbackNotFfpPrimitives);
        break;
      case pipeline::TileFfpFallbackReason::Precision:
        addDrawNoVertices(stats.tileFfpFallbackPrecisionDraws,
                          stats.tileFfpFallbackPrecisionPrimitives);
        break;
      case pipeline::TileFfpFallbackReason::UnsupportedState:
        addDrawNoVertices(stats.tileFfpFallbackUnsupportedStateDraws,
                          stats.tileFfpFallbackUnsupportedStatePrimitives);
        break;
      case pipeline::TileFfpFallbackReason::None:
        break;
    }
  }

  void recordDrawIssue(core::PrimitiveType primitiveType,
                       u32 primitiveCount,
                       u64 vertexCount,
                       bool indexed,
                       bool expandedIndexed,
                       bool fixedFunction,
                       bool preTransformed,
                       u32 textureMask,
                       u32 stream0Stride,
                       i32 drawVertexBaseIndex,
                       u32 drawVertexStreamOffset,
                       i32 d3dBaseVertexIndex,
                       bool nativeBaseVertexRequested,
                       bool nativeBaseVertexUsed,
                       u32 startIndex,
                       core::IndexType indexType,
                       const core::FlatRenderStateSet& renderStates,
                       const core::ViewportScissor& viewport,
                       WMTCullMode cullMode,
                       WMTTriangleFillMode fillMode) {
    if (!enabled) {
      return;
    }
    ++stats.drawCalls;
    if (indexed) {
      ++stats.indexedDraws;
      ++stats.indexedBaseVertexSamples;
      if (d3dBaseVertexIndex != 0) {
        ++stats.indexedBaseVertexNonZeroDraws;
      }
      if (d3dBaseVertexIndex < 0) {
        ++stats.indexedBaseVertexNegativeDraws;
      } else if (d3dBaseVertexIndex > 0) {
        ++stats.indexedBaseVertexPositiveDraws;
      }
      if (stats.indexedBaseVertexSamples == 1 ||
          d3dBaseVertexIndex < stats.indexedBaseVertexMin) {
        stats.indexedBaseVertexMin = d3dBaseVertexIndex;
      }
      if (stats.indexedBaseVertexSamples == 1 ||
          d3dBaseVertexIndex > stats.indexedBaseVertexMax) {
        stats.indexedBaseVertexMax = d3dBaseVertexIndex;
      }
      if (nativeBaseVertexRequested) {
        ++stats.nativeBaseVertexRequestedDraws;
        if (nativeBaseVertexUsed) {
          ++stats.nativeBaseVertexUsedDraws;
        } else if (d3dBaseVertexIndex < 0) {
          ++stats.nativeBaseVertexSkippedNegativeDraws;
        }
      }
    }
    if (expandedIndexed) {
      ++stats.expandedIndexedDraws;
    }
    if (fixedFunction) {
      ++stats.ffpDraws;
    } else {
      ++stats.programmableDraws;
    }
    if (preTransformed) {
      ++stats.preTransformedDraws;
    }
    if (textureMask != 0) {
      ++stats.texturedDraws;
    }
    switch (cullMode) {
      case WMTCullModeNone:
        ++stats.cullNoneDraws;
        break;
      case WMTCullModeFront:
        ++stats.cullFrontDraws;
        break;
      case WMTCullModeBack:
        ++stats.cullBackDraws;
        break;
    }
    switch (fillMode) {
      case WMTTriangleFillModeFill:
        ++stats.fillSolidDraws;
        break;
      case WMTTriangleFillModeLines:
        ++stats.fillWireframeDraws;
        break;
    }
    const bool depthEnabled =
        core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
    const bool depthWrite =
        depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
    const auto depthFunc = static_cast<core::CompareFunc>(core::flatStateOr(
        renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual)));
    if (depthEnabled) {
      ++stats.depthEnabledDraws;
      if (depthWrite) {
        ++stats.depthWriteDraws;
      }
      switch (depthFunc) {
        case core::CompareFunc::Less:
          ++stats.depthFuncLessDraws;
          break;
        case core::CompareFunc::LessEqual:
          ++stats.depthFuncLessEqualDraws;
          break;
        case core::CompareFunc::Always:
          ++stats.depthFuncAlwaysDraws;
          break;
        default:
          ++stats.depthFuncOtherDraws;
          break;
      }
    }
    const bool scissorEnabled = viewport.scissorEnabled;
    if (scissorEnabled) {
      ++stats.scissorEnabledDraws;
    }
    const bool alphaBlendEnabled =
        core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
    if (alphaBlendEnabled) {
      ++stats.alphaBlendEnabledDraws;
      if (textureMask != 0) {
        ++stats.alphaBlendTexturedDraws;
        stats.alphaBlendTexturedPrimitives += primitiveCount;
        stats.alphaBlendTexturedVertices += vertexCount;
      }
      if (primitiveCount <= 63u) {
        ++stats.alphaBlendSmallDraws;
        stats.alphaBlendSmallPrimitives += primitiveCount;
        stats.alphaBlendSmallVertices += vertexCount;
      }
    }
    recordBlendState(renderStates);
    const bool alphaTestEnabled =
        core::flatStateOr(renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u;
    if (alphaTestEnabled) {
      ++stats.alphaTestEnabledDraws;
      if (!debug::disableAlphaTest()) {
        ++stats.alphaTestEffectiveDraws;
      }
    }
    const auto colorWrite =
        core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
    const auto addRouteClass =
        [&](std::uint64_t& draws, std::uint64_t& primitives,
            std::uint64_t& vertices) {
          ++draws;
          primitives += primitiveCount;
          vertices += vertexCount;
        };
    if (depthWrite && colorWrite == 0u && !alphaBlendEnabled && !alphaTestEnabled) {
      addRouteClass(stats.routeDepthOnlyDraws,
                    stats.routeDepthOnlyPrimitives,
                    stats.routeDepthOnlyVertices);
    } else if (textureMask != 0) {
      addRouteClass(stats.routeProgrammableTexturedDraws,
                    stats.routeProgrammableTexturedPrimitives,
                    stats.routeProgrammableTexturedVertices);
    } else {
      addRouteClass(stats.routeProgrammableColorDraws,
                    stats.routeProgrammableColorPrimitives,
                    stats.routeProgrammableColorVertices);
    }
    if (alphaBlendEnabled) {
      stats.routeAlphaBlendPrimitives += primitiveCount;
    }
    if (alphaTestEnabled) {
      stats.routeAlphaTestPrimitives += primitiveCount;
    }
    const bool clipPlaneEnabled =
        core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;
    if (clipPlaneEnabled) {
      ++stats.clipPlaneEnabledDraws;
    }
    if (indexed && primitiveType == core::PrimitiveType::TriangleList) {
      auto addIndexedTriangleClass =
          [&](std::uint64_t& draws, std::uint64_t& primitives,
              std::uint64_t& vertices) {
            ++draws;
            primitives += primitiveCount;
            vertices += vertexCount;
          };
      const bool solidFill = fillMode == WMTTriangleFillModeFill;
      const bool stencilEnabled =
          core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
      const bool depthFuncPreservesOpaqueOrder =
          depthFunc == core::CompareFunc::Less ||
          depthFunc == core::CompareFunc::LessEqual;
      const bool opaqueDepthWrite =
          solidFill && depthWrite && depthFuncPreservesOpaqueOrder &&
          !alphaBlendEnabled && !alphaTestEnabled && !stencilEnabled &&
          !clipPlaneEnabled;
      if (opaqueDepthWrite) {
        addIndexedTriangleClass(stats.indexedTriangleOpaqueDepthWriteDraws,
                                stats.indexedTriangleOpaqueDepthWritePrimitives,
                                stats.indexedTriangleOpaqueDepthWriteVertices);
      }
      if (depthEnabled && !depthWrite) {
        addIndexedTriangleClass(stats.indexedTriangleDepthReadDraws,
                                stats.indexedTriangleDepthReadPrimitives,
                                stats.indexedTriangleDepthReadVertices);
      }
      if (alphaBlendEnabled) {
        addIndexedTriangleClass(stats.indexedTriangleAlphaBlendDraws,
                                stats.indexedTriangleAlphaBlendPrimitives,
                                stats.indexedTriangleAlphaBlendVertices);
      }
      if (scissorEnabled) {
        addIndexedTriangleClass(stats.indexedTriangleScissorDraws,
                                stats.indexedTriangleScissorPrimitives,
                                stats.indexedTriangleScissorVertices);
      }
      if (textureMask != 0) {
        addIndexedTriangleClass(stats.indexedTriangleTexturedDraws,
                                stats.indexedTriangleTexturedPrimitives,
                                stats.indexedTriangleTexturedVertices);
      }
      if (primitiveCount >= 4096) {
        addIndexedTriangleClass(stats.indexedTriangleLarge4096Draws,
                                stats.indexedTriangleLarge4096Primitives,
                                stats.indexedTriangleLarge4096Vertices);
        if (opaqueDepthWrite) {
          addIndexedTriangleClass(
              stats.indexedTriangleLarge4096OpaqueDepthWriteDraws,
              stats.indexedTriangleLarge4096OpaqueDepthWritePrimitives,
              stats.indexedTriangleLarge4096OpaqueDepthWriteVertices);
        }
        if (depthEnabled && !depthWrite) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096DepthReadDraws,
                                  stats.indexedTriangleLarge4096DepthReadPrimitives,
                                  stats.indexedTriangleLarge4096DepthReadVertices);
        }
        if (alphaBlendEnabled) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096AlphaBlendDraws,
                                  stats.indexedTriangleLarge4096AlphaBlendPrimitives,
                                  stats.indexedTriangleLarge4096AlphaBlendVertices);
        }
        if (scissorEnabled) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096ScissorDraws,
                                  stats.indexedTriangleLarge4096ScissorPrimitives,
                                  stats.indexedTriangleLarge4096ScissorVertices);
        }
        if (textureMask != 0) {
          addIndexedTriangleClass(stats.indexedTriangleLarge4096TexturedDraws,
                                  stats.indexedTriangleLarge4096TexturedPrimitives,
                                  stats.indexedTriangleLarge4096TexturedVertices);
        }
      }
    }
    switch (primitiveType) {
      case core::PrimitiveType::PointList:
        ++stats.pointDraws;
        break;
      case core::PrimitiveType::LineList:
      case core::PrimitiveType::LineStrip:
        ++stats.lineDraws;
        break;
      case core::PrimitiveType::TriangleList:
      case core::PrimitiveType::TriangleStrip:
      case core::PrimitiveType::TriangleFan:
        ++stats.triangleDraws;
        break;
    }
    stats.primitiveCount += primitiveCount;
    stats.triangleEstimate += triangleEstimateFor(primitiveType, primitiveCount);
    stats.vertexCount += vertexCount;
    stats.textureMaskOr |= textureMask;
    recordDrawSize(primitiveCount, vertexCount);
    recordDrawGeometrySignature(makeDrawGeometrySignature(
        primitiveType,
        primitiveCount,
        vertexCount,
        indexed,
        expandedIndexed,
        fixedFunction,
        preTransformed,
        textureMask,
        stream0Stride,
        indexed ? d3dBaseVertexIndex : drawVertexBaseIndex,
        drawVertexStreamOffset,
        startIndex,
        indexType,
        renderStates,
        viewport,
        cullMode,
        fillMode));
    if (stream0Stride != 0) {
      if (stats.stream0StrideMin == 0 || stream0Stride < stats.stream0StrideMin) {
        stats.stream0StrideMin = stream0Stride;
      }
      stats.stream0StrideMax = std::max<std::uint64_t>(stats.stream0StrideMax,
                                                       stream0Stride);
    }
  }

  void recordStreamState(u32 stream, u64 handle, u64 offset, u64 stride) {
    if (!enabled || stream >= stats.streams.size()) {
      return;
    }
    auto& last = stats.streams[stream];
    const bool firstSample = last.samples == 0;
    bool handleChanged = false;
    bool offsetChanged = false;
    last.valid = true;
    ++last.samples;
    ++stats.streamStateSamples;
    if (!firstSample) {
      if (last.lastHandle != handle) {
        ++last.handleChanges;
        ++stats.streamHandleChanges;
        handleChanged = true;
      }
      if (last.lastOffset != offset) {
        ++last.offsetChanges;
        ++stats.streamOffsetChanges;
        offsetChanged = true;
      }
      if (last.lastStride != stride) {
        ++last.strideChanges;
        ++stats.streamStrideChanges;
      }
    }
    streamBindReasons[stream] = StreamBindReason{
        .first = firstSample,
        .handleChange = handleChanged,
        .offsetChange = offsetChanged,
    };
    last.lastHandle = handle;
    last.lastOffset = offset;
    last.lastStride = stride;
    if (stream == 0) {
      stats.stream0LastHandle = handle;
      stats.stream0LastOffset = offset;
      stats.stream0LastStride = stride;
    }
  }

  void recordStreamMetalBind(u32 stream) {
    if (!enabled) {
      return;
    }
    ++stats.streamMetalBinds;
    if (stream < stats.streams.size()) {
      auto& slot = stats.streams[stream];
      auto& reason = streamBindReasons[stream];
      slot.valid = true;
      ++slot.metalBinds;
      if (reason.first) {
        ++slot.metalBindFirsts;
        ++stats.streamMetalBindFirsts;
      }
      if (reason.handleChange) {
        ++slot.metalBindHandleChanges;
        ++stats.streamMetalBindHandleChanges;
      }
      if (reason.offsetChange) {
        ++slot.metalBindOffsetChanges;
        ++stats.streamMetalBindOffsetChanges;
      }
      reason = {};
    }
  }

  template <std::size_t Capacity>
  bool recordUnique(UniqueHandleSet<Capacity>& set, u64 handle, u64& uniqueCounter,
                    u64& overflowCounter) {
    if (!enabled || handle == 0) {
      return false;
    }
    for (std::size_t i = 0; i < set.count; ++i) {
      if (set.handles[i] == handle) {
        return false;
      }
    }
    if (set.count >= set.handles.size()) {
      if (!set.overflowed) {
        set.overflowed = true;
        ++overflowCounter;
      }
      return false;
    }
    set.handles[set.count++] = handle;
    ++uniqueCounter;
    return true;
  }

  static void addPoolBucket(core::Pool pool,
                            u64& defaultPool,
                            u64& managedPool,
                            u64& systemMemPool,
                            u64& scratchPool) {
    switch (pool) {
      case core::Pool::Default:
        ++defaultPool;
        break;
      case core::Pool::Managed:
        ++managedPool;
        break;
      case core::Pool::SystemMem:
        ++systemMemPool;
        break;
      case core::Pool::Scratch:
        ++scratchPool;
        break;
    }
  }

  void recordStreamResource(u32 stream, u64 handle, const core::BufferDesc& desc) {
    if (recordUnique(streamUniqueHandles, handle, stats.streamUniqueHandles,
                     stats.streamUniqueHandleOverflows)) {
      stats.streamUniqueBytes += desc.size;
      if ((desc.usage & core::UsageDynamic) != 0) {
        ++stats.streamUniqueDynamicHandles;
      }
      if ((desc.usage & core::UsageWriteOnly) != 0) {
        ++stats.streamUniqueWriteOnlyHandles;
      }
      addPoolBucket(desc.pool,
                    stats.streamUniqueDefaultPoolHandles,
                    stats.streamUniqueManagedPoolHandles,
                    stats.streamUniqueSystemMemPoolHandles,
                    stats.streamUniqueScratchPoolHandles);
    }
    if (stream >= stats.streams.size()) {
      return;
    }
    auto& slot = stats.streams[stream];
    slot.valid = true;
    if (!recordUnique(streamUniqueHandlesByStream[stream], handle,
                      slot.uniqueHandles, slot.uniqueHandleOverflows)) {
      return;
    }
    slot.uniqueBytes += desc.size;
    if ((desc.usage & core::UsageDynamic) != 0) {
      ++slot.uniqueDynamicHandles;
    }
    if ((desc.usage & core::UsageWriteOnly) != 0) {
      ++slot.uniqueWriteOnlyHandles;
    }
    addPoolBucket(desc.pool,
                  slot.uniqueDefaultPoolHandles,
                  slot.uniqueManagedPoolHandles,
                  slot.uniqueSystemMemPoolHandles,
                  slot.uniqueScratchPoolHandles);
  }

  void recordIndexBufferResource(u64 handle, const core::BufferDesc& desc) {
    if (!recordUnique(ibUniqueHandles, handle, stats.ibUniqueHandles,
                      stats.ibUniqueHandleOverflows)) {
      return;
    }
    stats.ibUniqueBytes += desc.size;
    if ((desc.usage & core::UsageDynamic) != 0) {
      ++stats.ibUniqueDynamicHandles;
    }
    if ((desc.usage & core::UsageWriteOnly) != 0) {
      ++stats.ibUniqueWriteOnlyHandles;
    }
    addPoolBucket(desc.pool,
                  stats.ibUniqueDefaultPoolHandles,
                  stats.ibUniqueManagedPoolHandles,
                  stats.ibUniqueSystemMemPoolHandles,
                  stats.ibUniqueScratchPoolHandles);
  }

  void recordIndexBufferState(u64 handle) {
    if (!enabled) {
      return;
    }
    ++stats.ibStateSamples;
    if (ibValid && ibHandle != handle) {
      ++stats.ibHandleChanges;
    }
    ibValid = true;
    ibHandle = handle;
    stats.ibLastHandle = handle;
  }

  void recordIndexBufferMetalBind() {
    if (enabled) {
      ++stats.ibMetalBinds;
    }
  }

  void recordPsoState(u64 handle,
                      u64 variantHash,
                      u32 layoutKey,
                      u64 vertexShaderHash,
                      u64 pixelShaderHash,
                      u64 vertexShaderSourceHash,
                      u64 pixelShaderSourceHash) {
    if (!enabled) {
      return;
    }
    ++stats.psoStateSamples;
    if (psoValid && psoHandle != handle) {
      ++stats.psoHandleChanges;
    }
    psoValid = true;
    psoHandle = handle;
    stats.psoLastHandle = handle;
    recordUnique(psoUniqueHandles, handle, stats.psoUniqueHandles,
                 stats.psoUniqueHandleOverflows);

    if (shaderVariantValid && shaderVariant != variantHash) {
      ++stats.shaderVariantChanges;
    }
    shaderVariantValid = true;
    shaderVariant = variantHash;
    stats.shaderVariantLast = variantHash;
    stats.vertexShaderLast = vertexShaderHash;
    stats.pixelShaderLast = pixelShaderHash;
    stats.vertexShaderSourceLast = vertexShaderSourceHash;
    stats.pixelShaderSourceLast = pixelShaderSourceHash;
    recordUnique(shaderVariantUnique, variantHash, stats.shaderVariantUnique,
                 stats.shaderVariantUniqueOverflows);

    if (vsOutLayoutValid && vsOutLayout != layoutKey) {
      ++stats.vsOutLayoutChanges;
    }
    vsOutLayoutValid = true;
    vsOutLayout = layoutKey;
    stats.vsOutLayoutLast = layoutKey;
    recordUnique(vsOutLayoutUnique, static_cast<u64>(layoutKey),
                 stats.vsOutLayoutUnique, stats.vsOutLayoutUniqueOverflows);
  }

  void recordBlendState(const core::FlatRenderStateSet& renderStates) {
    if (!enabled) {
      return;
    }
    const auto blendEnable = core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u);
    const auto srcBlend = core::flatStateOr(
        renderStates, RS_SRC_BLEND, static_cast<u32>(core::BlendFactor::One));
    const auto dstBlend = core::flatStateOr(
        renderStates, RS_DEST_BLEND, static_cast<u32>(core::BlendFactor::Zero));
    const auto blendOp = core::flatStateOr(
        renderStates, RS_BLEND_OP, static_cast<u32>(core::BlendOp::Add));
    const auto separateAlpha = core::flatStateOr(
        renderStates, RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u);
    const auto srcBlendAlpha = core::flatStateOr(
        renderStates, RS_SRC_BLEND_ALPHA, srcBlend);
    const auto dstBlendAlpha = core::flatStateOr(
        renderStates, RS_DEST_BLEND_ALPHA, dstBlend);
    const auto blendOpAlpha = core::flatStateOr(
        renderStates, RS_BLEND_OP_ALPHA, blendOp);
    const auto blendFactor = core::flatStateOr(renderStates, RS_BLEND_FACTOR, 0xffffffffu);
    const auto colorWrite = core::flatStateOr(renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);

    u64 signature = 0x61b451b9273d8fd5ull;
    signature = drawBindingPacketHashMix(signature, blendEnable);
    signature = drawBindingPacketHashMix(signature, srcBlend);
    signature = drawBindingPacketHashMix(signature, dstBlend);
    signature = drawBindingPacketHashMix(signature, blendOp);
    signature = drawBindingPacketHashMix(signature, separateAlpha);
    signature = drawBindingPacketHashMix(signature, srcBlendAlpha);
    signature = drawBindingPacketHashMix(signature, dstBlendAlpha);
    signature = drawBindingPacketHashMix(signature, blendOpAlpha);
    signature = drawBindingPacketHashMix(signature, blendFactor);
    signature = drawBindingPacketHashMix(signature, colorWrite);
    signature = signature ? signature : 1ull;

    ++stats.blendStateSamples;
    if (blendStateValid && blendState != signature) {
      ++stats.blendStateChanges;
    }
    blendStateValid = true;
    blendState = signature;
    stats.blendStateLast = signature;
    recordUnique(blendStateUnique, signature, stats.blendStateUnique,
                 stats.blendStateUniqueOverflows);

    const bool rgbNoop =
        srcBlend == static_cast<u32>(core::BlendFactor::One) &&
        dstBlend == static_cast<u32>(core::BlendFactor::Zero) &&
        blendOp == static_cast<u32>(core::BlendOp::Add);
    const bool alphaNoop =
        separateAlpha == 0u ||
        (srcBlendAlpha == static_cast<u32>(core::BlendFactor::One) &&
         dstBlendAlpha == static_cast<u32>(core::BlendFactor::Zero) &&
         blendOpAlpha == static_cast<u32>(core::BlendOp::Add));
    if (blendEnable != 0u && rgbNoop && alphaNoop) {
      ++stats.blendEnabledNoopDraws;
    }

    const bool rgbAdd = blendOp == static_cast<u32>(core::BlendOp::Add);
    if (blendEnable != 0u && rgbAdd) {
      if (srcBlend == static_cast<u32>(core::BlendFactor::InvDestColor) &&
          dstBlend == static_cast<u32>(core::BlendFactor::One)) {
        ++stats.blendScreenDraws;
      }
      if (srcBlend == static_cast<u32>(core::BlendFactor::One) &&
          dstBlend == static_cast<u32>(core::BlendFactor::One)) {
        ++stats.blendAdditiveDraws;
      }
      if (srcBlend == static_cast<u32>(core::BlendFactor::SrcAlpha) &&
          dstBlend == static_cast<u32>(core::BlendFactor::InvSrcAlpha)) {
        ++stats.blendAlphaCompositeDraws;
      }
    }

    const auto isConstantBlend = [](u32 factor) {
      return factor == static_cast<u32>(core::BlendFactor::BlendFactor) ||
             factor == static_cast<u32>(core::BlendFactor::InvBlendFactor);
    };
    if (blendEnable != 0u &&
        (isConstantBlend(srcBlend) || isConstantBlend(dstBlend) ||
         (separateAlpha != 0u &&
          (isConstantBlend(srcBlendAlpha) || isConstantBlend(dstBlendAlpha))))) {
      ++stats.blendConstantFactorDraws;
    }
  }

  bool findCachedVsOutLayout(u64 sourceKey, bool tileFfpMode, u32& layoutKey) {
    if (!enabled) {
      return false;
    }
    for (const auto& entry : vsOutLayoutCache) {
      if (entry.valid && entry.sourceKey == sourceKey &&
          entry.tileFfpMode == tileFfpMode) {
        layoutKey = entry.layoutKey;
        ++stats.vsOutLayoutCacheHits;
        return true;
      }
    }
    ++stats.vsOutLayoutCacheMisses;
    return false;
  }

  void storeCachedVsOutLayout(u64 sourceKey, bool tileFfpMode, u32 layoutKey) {
    if (!enabled || vsOutLayoutCache.empty()) {
      return;
    }
    auto& entry = vsOutLayoutCache[vsOutLayoutCacheNext++ % vsOutLayoutCache.size()];
    entry = VsOutLayoutCacheEntry{
        .valid = true,
        .sourceKey = sourceKey,
        .tileFfpMode = tileFfpMode,
        .layoutKey = layoutKey,
    };
  }

  bool findCachedShaderSourceHashes(u64 sourceKey,
                                    bool tileFfpBaseColor,
                                    bool argbufHybridMode,
                                    bool argbufResourceArray,
                                    bool argbufDirectCbufMode,
                                    bool samplerLodBias,
                                    u32 x8AlphaOneTextureMask,
                                    u64& vertexSourceHash,
                                    u64& pixelSourceHash) const {
    if (!enabled) {
      return false;
    }
    for (const auto& entry : shaderSourceHashCache) {
      if (entry.valid &&
          entry.sourceKey == sourceKey &&
          entry.tileFfpBaseColor == tileFfpBaseColor &&
          entry.argbufHybridMode == argbufHybridMode &&
          entry.argbufResourceArray == argbufResourceArray &&
          entry.argbufDirectCbufMode == argbufDirectCbufMode &&
          entry.samplerLodBias == samplerLodBias &&
          entry.x8AlphaOneTextureMask == x8AlphaOneTextureMask) {
        vertexSourceHash = entry.vertexSourceHash;
        pixelSourceHash = entry.pixelSourceHash;
        return true;
      }
    }
    return false;
  }

  void storeCachedShaderSourceHashes(u64 sourceKey,
                                     bool tileFfpBaseColor,
                                     bool argbufHybridMode,
                                     bool argbufResourceArray,
                                     bool argbufDirectCbufMode,
                                     bool samplerLodBias,
                                     u32 x8AlphaOneTextureMask,
                                     u64 vertexSourceHash,
                                     u64 pixelSourceHash) {
    if (!enabled || shaderSourceHashCache.empty()) {
      return;
    }
    auto& entry =
        shaderSourceHashCache[shaderSourceHashCacheNext++ %
                              shaderSourceHashCache.size()];
    entry = ShaderSourceHashCacheEntry{
        .valid = true,
        .sourceKey = sourceKey,
        .tileFfpBaseColor = tileFfpBaseColor,
        .argbufHybridMode = argbufHybridMode,
        .argbufResourceArray = argbufResourceArray,
        .argbufDirectCbufMode = argbufDirectCbufMode,
        .samplerLodBias = samplerLodBias,
        .x8AlphaOneTextureMask = x8AlphaOneTextureMask,
        .vertexSourceHash = vertexSourceHash,
        .pixelSourceHash = pixelSourceHash,
    };
  }

  void addArgbufTableBytes(u64 bytes) {
    if (enabled) {
      stats.argbufTableBytes += bytes;
    }
  }

  void addArgbufCbufBytes(u64 bytes) {
    if (enabled) {
      stats.argbufCbufBytes += bytes;
    }
  }

  void addArgbufCbufBytes(u32 argbufIndex, u64 bytes) {
    if (!enabled || bytes == 0) {
      return;
    }
    stats.argbufCbufBytes += bytes;
    switch (argbufIndex) {
      case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
        stats.argbufCbufVsBytes += bytes;
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
        stats.argbufCbufFfpVsBytes += bytes;
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
        stats.argbufCbufPsBytes += bytes;
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
        stats.argbufCbufFfpPsBytes += bytes;
        break;
      default:
        break;
    }
  }

  void addArgbufCbufBindings(
      const dxmt9::argbuf_hybrid::ConstantBufferBindings& bindings) {
    if (!enabled) {
      return;
    }
    for (u32 i = 0; i < bindings.entries.size(); ++i) {
      addArgbufCbufBytes(i, bindings.entries[i].bytes);
    }
  }

  struct ByteDelta {
    u64 first = 0;
    u64 changed = 0;
    u64 unchanged = 0;
  };

  template <std::size_t Size>
  ByteDelta compareRange(CbufHistory<Size>& history,
                         const std::byte* current,
                         u64 uploadBytes,
                         u64 offset,
                         u64 length) {
    ByteDelta delta{};
    const u64 begin = std::min(offset, uploadBytes);
    const u64 end = std::min(offset + length, uploadBytes);
    if (begin >= end) {
      return delta;
    }
    for (u64 i = begin; i < end; ++i) {
      if (!history.valid || i >= history.validBytes) {
        ++delta.first;
      } else if (history.bytes[static_cast<std::size_t>(i)] !=
                 current[static_cast<std::size_t>(i)]) {
        ++delta.changed;
      } else {
        ++delta.unchanged;
      }
    }
    return delta;
  }

  template <std::size_t Size>
  ByteDelta compareWhole(CbufHistory<Size>& history,
                         const std::byte* current,
                         u64 uploadBytes) {
    return compareRange(history, current, uploadBytes, 0, Size);
  }

  template <std::size_t Size>
  void updateHistory(CbufHistory<Size>& history,
                     const std::byte* current,
                     u64 uploadBytes) {
    const u64 clampedBytes = std::min<u64>(uploadBytes, Size);
    std::memcpy(history.bytes.data(), current, static_cast<std::size_t>(clampedBytes));
    history.valid = true;
    history.validBytes = std::max(history.validBytes, clampedBytes);
  }

  void recordVsUploadContent(const void* data, u64 bytes) {
    if (!enabled || !data || bytes == 0) {
      return;
    }
    const auto* current = static_cast<const std::byte*>(data);
    const u64 uploadBytes = std::min<u64>(bytes, sizeof(VsConsts));
    const auto total = compareWhole(vsHistory, current, uploadBytes);
    stats.argbufCbufVsFirstBytes += total.first;
    stats.argbufCbufVsRewriteChangedBytes += total.changed;
    stats.argbufCbufVsRewriteUnchangedBytes += total.unchanged;

    auto addVsGroup = [&](u64 offset, u64 length, u64& counter) {
      const auto delta = compareRange(vsHistory, current, uploadBytes, offset, length);
      counter += delta.changed;
    };
    addVsGroup(offsetof(VsConsts, vsFloatConst), sizeof(VsConsts::vsFloatConst),
               stats.argbufCbufVsFloatChangedBytes);
    addVsGroup(offsetof(VsConsts, vsIntConst), sizeof(VsConsts::vsIntConst),
               stats.argbufCbufVsIntChangedBytes);
    addVsGroup(offsetof(VsConsts, vsBoolConst), sizeof(VsConsts::vsBoolConst),
               stats.argbufCbufVsBoolChangedBytes);
    updateHistory(vsHistory, current, uploadBytes);
  }

  void recordVsUploadPlan(const uniform::DirtyState& dirty,
                          uniform::ShaderConstantUsageBounds usage,
                          uniform::ShaderConstantUploadPlan plan) {
    if (!enabled) {
      return;
    }
    ++stats.argbufCbufVsUploads;
    if (plan.fullStructRequired) {
      ++stats.argbufCbufVsFullStructUploads;
    }
    if (usage.unknown) {
      ++stats.argbufCbufVsUsageUnknownUploads;
    }
    if (usage.indexedFloat) {
      ++stats.argbufCbufVsUsageIndexedFloatUploads;
    }
    stats.argbufCbufVsPlanFloatRegsSum += plan.floatCount;
    stats.argbufCbufVsPlanFloatRegsMax =
        std::max<u64>(stats.argbufCbufVsPlanFloatRegsMax, plan.floatCount);
    stats.argbufCbufVsDirtyFloatRegsSum += dirty.maxChangedVsF;
    stats.argbufCbufVsDirtyFloatRegsMax =
        std::max<u64>(stats.argbufCbufVsDirtyFloatRegsMax, dirty.maxChangedVsF);
    stats.argbufCbufVsUsageFloatRegsSum += usage.floatCount;
    stats.argbufCbufVsUsageFloatRegsMax =
        std::max<u64>(stats.argbufCbufVsUsageFloatRegsMax, usage.floatCount);
  }

  void recordFfpVsUploadContent(const void* data, u64 bytes) {
    if (!enabled || !data || bytes == 0) {
      return;
    }
    const auto* current = static_cast<const std::byte*>(data);
    const u64 uploadBytes = std::min<u64>(bytes, sizeof(FfpVsConsts));
    const auto total = compareWhole(ffpVsHistory, current, uploadBytes);
    stats.argbufCbufFfpVsFirstBytes += total.first;
    stats.argbufCbufFfpVsRewriteChangedBytes += total.changed;
    stats.argbufCbufFfpVsRewriteUnchangedBytes += total.unchanged;

    auto addFfpVsGroup = [&](u64 offset, u64 length, u64& counter) {
      const auto delta = compareRange(ffpVsHistory, current, uploadBytes, offset, length);
      counter += delta.changed;
    };
    addFfpVsGroup(offsetof(FfpVsConsts, ffpWorldViewProj),
                  offsetof(FfpVsConsts, materialEmissive) -
                      offsetof(FfpVsConsts, ffpWorldViewProj),
                  stats.argbufCbufFfpVsMatrixChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, materialEmissive),
                  offsetof(FfpVsConsts, lightDiffuse) -
                      offsetof(FfpVsConsts, materialEmissive),
                  stats.argbufCbufFfpVsMaterialChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, lightDiffuse),
                  offsetof(FfpVsConsts, ffpBlendWorldViewProj) -
                      offsetof(FfpVsConsts, lightDiffuse),
                  stats.argbufCbufFfpVsLightChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, ffpBlendWorldViewProj),
                  sizeof(FfpVsConsts::ffpBlendWorldViewProj),
                  stats.argbufCbufFfpVsBlendChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, ffpTextureTransforms),
                  sizeof(FfpVsConsts::ffpTextureTransforms),
                  stats.argbufCbufFfpVsTexTransformChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, clipPlanes),
                  sizeof(FfpVsConsts::clipPlanes),
                  stats.argbufCbufFfpVsClipChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, halfPixelFixup),
                  offsetof(FfpVsConsts, fogStart) -
                      offsetof(FfpVsConsts, halfPixelFixup),
                  stats.argbufCbufFfpVsViewportChangedBytes);
    addFfpVsGroup(offsetof(FfpVsConsts, fogStart),
                  sizeof(FfpVsConsts) - offsetof(FfpVsConsts, fogStart),
                  stats.argbufCbufFfpVsFogPointChangedBytes);
    updateHistory(ffpVsHistory, current, uploadBytes);
  }

  void recordArgbufCbufUploadContent(u32 argbufIndex,
                                     const void* data,
                                     u64 bytes,
                                     u64 /*hostStructBytes*/) {
    switch (argbufIndex) {
      case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
        recordVsUploadContent(data, bytes);
        break;
      case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
        recordFfpVsUploadContent(data, bytes);
        break;
      default:
        break;
    }
  }

  void addSetVertexBytes(u64 bytes, u32 slot) {
    if (!enabled) {
      return;
    }
    ++stats.setVertexBytesCalls;
    stats.setVertexBytesBytes += bytes;
    if (slot == 5) {
      ++stats.setVertexBytesSlot5Calls;
      stats.setVertexBytesSlot5Bytes += bytes;
    } else {
      ++stats.setVertexBytesOtherCalls;
      stats.setVertexBytesOtherBytes += bytes;
    }
  }

  void addTransientVertexBytes(u64 bytes, TransientVertexSource source) {
    if (!enabled) {
      return;
    }
    stats.transientVertexBytes += bytes;
    switch (source) {
      case TransientVertexSource::User:
        stats.transientVertexUserBytes += bytes;
        break;
      case TransientVertexSource::Preupload:
        stats.transientVertexPreuploadBytes += bytes;
        break;
      case TransientVertexSource::DeclFallback:
        stats.transientVertexDeclFallbackBytes += bytes;
        break;
      case TransientVertexSource::ExpandedMain:
        stats.transientVertexExpandedMainBytes += bytes;
        break;
      case TransientVertexSource::ExpandedExtra:
        stats.transientVertexExpandedExtraBytes += bytes;
        break;
      case TransientVertexSource::StagedStream:
        stats.transientVertexStagedStreamBytes += bytes;
        break;
    }
  }

  void addTransientIndexBytes(u64 bytes, TransientIndexSource source) {
    if (!enabled) {
      return;
    }
    stats.transientIndexBytes += bytes;
    switch (source) {
      case TransientIndexSource::User:
        stats.transientIndexUserBytes += bytes;
        break;
      case TransientIndexSource::Preupload:
        stats.transientIndexPreuploadBytes += bytes;
        break;
      case TransientIndexSource::ShadowFallback:
        stats.transientIndexShadowFallbackBytes += bytes;
        break;
      case TransientIndexSource::ProbeReorder:
        stats.transientIndexProbeReorderBytes += bytes;
        break;
      case TransientIndexSource::OptimizedOrder:
        stats.transientIndexOptimizedOrderBytes += bytes;
        break;
      case TransientIndexSource::StagedIb:
        stats.transientIndexStagedIbBytes += bytes;
        break;
    }
  }
};

void traceEffectDraw(const ActiveEncoderBreakdown* encoderBreakdown,
                     const core::FlatDrawStateRecord& hot,
                     const resources::Pool& pool,
                     u64 seqId,
                     u64 drawOrdinal,
                     std::uint32_t commandIndex,
                     u64 commandDrawIndex,
                     u64 commandDrawCount,
                     core::PrimitiveType primitiveType,
                     u32 primitiveCount,
                     u64 vertexCount,
                     bool indexedDraw,
                     bool fixedFunctionPath,
                     bool preTransformed,
                     u64 vertexShaderHash,
                     u64 pixelShaderHash) {
  if (!effectDrawTraceEnabled()) {
    return;
  }
  const auto encoderIndex = encoderBreakdown ? encoderBreakdown->stats.encoderIndex : 0ull;
  if (!effectDrawTraceSeqMatches(seqId)) {
    return;
  }
  if (const auto encFilter = effectDrawTraceEncoderFilter();
      encFilter.has_value() && *encFilter != encoderIndex) {
    return;
  }
  const bool alphaBlendEnabled =
      core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  if (!alphaBlendEnabled && !effectDrawTraceIncludeNonAlpha()) {
    return;
  }
  if (hot.textureMask == 0u && !effectDrawTraceIncludeUntextured()) {
    return;
  }
  const auto srcBlend = core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u);
  const auto dstBlend = core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u);
  const auto blendOp = core::flatStateOr(hot.renderStates, RS_BLEND_OP, 0u);
  const bool depthEnabled =
      core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
  const auto depthFunc = core::flatStateOr(
      hot.renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual));
  const float depthBias = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_DEPTH_BIAS, 0u));
  const float slopeScale = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  const bool pointSpriteState =
      core::flatStateOr(hot.renderStates, RS_POINT_SPRITE_ENABLE, 0u) != 0u;
  const bool pointSpriteCandidate =
      pointSpriteState && primitiveType == core::PrimitiveType::PointList;
  if (effectDrawTracePointSpriteOnly() && !pointSpriteCandidate) {
    return;
  }
  if (const auto primitiveTypeFilter = effectDrawTracePrimitiveTypeFilter();
      primitiveTypeFilter.has_value() &&
      static_cast<u64>(primitiveType) != *primitiveTypeFilter) {
    return;
  }
  const bool pointScaleState =
      core::flatStateOr(hot.renderStates, RS_POINT_SCALE_ENABLE, 0u) != 0u;
  const float pointSize = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE, std::bit_cast<u32>(1.0f)));
  const float pointSizeMin = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE_MIN, std::bit_cast<u32>(1.0f)));
  const float pointSizeMax = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, RS_POINTSIZE_MAX, std::bit_cast<u32>(64.0f)));
  const auto colorWrite =
      core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
  const auto encoderDrawIndex =
      encoderBreakdown ? encoderBreakdown->stats.drawCalls + 1ull : 0ull;
  std::array<const resources::TextureRecord*, core::kMaxTextures> textureRecords{};
  for (std::size_t i = 0; i < core::kMaxTextures; ++i) {
    textureRecords[i] =
        hot.textures[i] ? pool.findTexture(hot.textures[i].value) : nullptr;
  }
  if (const auto handleFilter = effectDrawTraceTexture0Filter();
      handleFilter.has_value() &&
      (!hot.textures[0] || hot.textures[0].value != *handleFilter)) {
    return;
  }
  if (const auto widthFilter = effectDrawTraceTexture0WidthFilter();
      widthFilter.has_value() &&
      (!textureRecords[0] || textureRecords[0]->desc.width != *widthFilter)) {
    return;
  }
  if (const auto heightFilter = effectDrawTraceTexture0HeightFilter();
      heightFilter.has_value() &&
      (!textureRecords[0] || textureRecords[0]->desc.height != *heightFilter)) {
    return;
  }
  if (const auto formatFilter = effectDrawTraceTexture0FormatFilter();
      formatFilter.has_value() &&
      (!textureRecords[0] ||
       static_cast<u64>(textureRecords[0]->desc.format) != *formatFilter)) {
    return;
  }
  std::ostringstream out;
  out << "[dxmt9-effect-draw"
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " encoder=" << static_cast<unsigned long long>(encoderIndex)
      << " encoder_draw_index=" << static_cast<unsigned long long>(encoderDrawIndex)
      << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
      << " command_index=" << commandIndex
      << " command_draw_index=" << static_cast<unsigned long long>(commandDrawIndex)
      << " command_draw_count=" << static_cast<unsigned long long>(commandDrawCount)
      << " primitive_type=" << static_cast<unsigned>(primitiveType)
      << " primitive_count=" << primitiveCount
      << " vertex_count=" << static_cast<unsigned long long>(vertexCount)
      << " small=" << (primitiveCount <= 63u ? 1u : 0u)
      << " indexed=" << (indexedDraw ? 1u : 0u)
      << " ffp=" << (fixedFunctionPath ? 1u : 0u)
      << " pretransformed=" << (preTransformed ? 1u : 0u)
      << " point_sprite_state=" << (pointSpriteState ? 1u : 0u)
      << " point_sprite_candidate=" << (pointSpriteCandidate ? 1u : 0u)
      << " point_scale=" << (pointScaleState ? 1u : 0u)
      << " point_size=" << pointSize
      << " point_size_min=" << pointSizeMin
      << " point_size_max=" << pointSizeMax
      << " vs_hash=0x" << std::hex << vertexShaderHash
      << " ps_hash=0x" << pixelShaderHash
      << " texture_mask=0x" << std::hex << hot.textureMask
      << " texture0=0x" << hot.textures[0].value
      << " texture1=0x" << hot.textures[1].value
      << " texture2=0x" << hot.textures[2].value
      << " texture3=0x" << hot.textures[3].value
      << " texture4=0x" << hot.textures[4].value
      << " texture5=0x" << hot.textures[5].value
      << " texture6=0x" << hot.textures[6].value
      << " texture7=0x" << hot.textures[7].value
      << std::dec
      << " texture0_width=" << (textureRecords[0] ? textureRecords[0]->desc.width : 0u)
      << " texture0_height=" << (textureRecords[0] ? textureRecords[0]->desc.height : 0u)
      << " texture0_format="
      << (textureRecords[0] ? static_cast<u32>(textureRecords[0]->desc.format) : 0u)
      << " texture1_width=" << (textureRecords[1] ? textureRecords[1]->desc.width : 0u)
      << " texture1_height=" << (textureRecords[1] ? textureRecords[1]->desc.height : 0u)
      << " texture1_format="
      << (textureRecords[1] ? static_cast<u32>(textureRecords[1]->desc.format) : 0u)
      << " texture2_width=" << (textureRecords[2] ? textureRecords[2]->desc.width : 0u)
      << " texture2_height=" << (textureRecords[2] ? textureRecords[2]->desc.height : 0u)
      << " texture2_format="
      << (textureRecords[2] ? static_cast<u32>(textureRecords[2]->desc.format) : 0u)
      << " texture3_width=" << (textureRecords[3] ? textureRecords[3]->desc.width : 0u)
      << " texture3_height=" << (textureRecords[3] ? textureRecords[3]->desc.height : 0u)
      << " texture3_format="
      << (textureRecords[3] ? static_cast<u32>(textureRecords[3]->desc.format) : 0u)
      << " texture4_width=" << (textureRecords[4] ? textureRecords[4]->desc.width : 0u)
      << " texture4_height=" << (textureRecords[4] ? textureRecords[4]->desc.height : 0u)
      << " texture4_format="
      << (textureRecords[4] ? static_cast<u32>(textureRecords[4]->desc.format) : 0u)
      << " texture5_width=" << (textureRecords[5] ? textureRecords[5]->desc.width : 0u)
      << " texture5_height=" << (textureRecords[5] ? textureRecords[5]->desc.height : 0u)
      << " texture5_format="
      << (textureRecords[5] ? static_cast<u32>(textureRecords[5]->desc.format) : 0u)
      << " texture6_width=" << (textureRecords[6] ? textureRecords[6]->desc.width : 0u)
      << " texture6_height=" << (textureRecords[6] ? textureRecords[6]->desc.height : 0u)
      << " texture6_format="
      << (textureRecords[6] ? static_cast<u32>(textureRecords[6]->desc.format) : 0u)
      << " texture7_width=" << (textureRecords[7] ? textureRecords[7]->desc.width : 0u)
      << " texture7_height=" << (textureRecords[7] ? textureRecords[7]->desc.height : 0u)
      << " texture7_format="
      << (textureRecords[7] ? static_cast<u32>(textureRecords[7]->desc.format) : 0u)
      << " src_blend=" << srcBlend
      << " dst_blend=" << dstBlend
      << " blend_op=" << blendOp
      << " alpha_test="
      << (core::flatStateOr(hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u ? 1u : 0u)
      << " depth_enabled=" << (depthEnabled ? 1u : 0u)
      << " depth_write=" << (depthWrite ? 1u : 0u)
      << " depth_func=" << depthFunc
      << " depth_bias=" << depthBias
      << " slope_scale_depth_bias=" << slopeScale
      << " color_write=0x" << std::hex << colorWrite << std::dec
      << " scissor=" << (hot.viewport.scissorEnabled ? 1u : 0u)
      << ']';
  emitQueueTraceLine(out.str());
}

struct StreamIbStagingCache {
  struct Entry {
    u64 sourceHandle = 0;
    WMT::Buffer buffer{};
    u64 offset = 0;
    std::size_t size = 0;
  };

  static constexpr std::size_t kMaxEntries = 512;

  bool enabled = false;
  std::array<Entry, kMaxEntries> entries{};
  std::size_t count = 0;

  void begin(bool active) noexcept {
    enabled = active;
    entries = {};
    count = 0;
  }

  CommandQueue::TransientBufferSlice findOrStage(
      EncodeContext& ctx,
      u64 seqId,
      u64 sourceHandle,
      const resources::BufferRecord* record,
      ActiveEncoderBreakdown* encoderBreakdown,
      bool indexBuffer) {
    if (!enabled || sourceHandle == 0 || !record) {
      return {};
    }
    for (std::size_t i = 0; i < count; ++i) {
      const auto& entry = entries[i];
      if (entry.sourceHandle == sourceHandle && entry.buffer) {
        return CommandQueue::TransientBufferSlice{
            .buffer = entry.buffer,
            .offset = entry.offset,
            .size = entry.size,
        };
      }
    }
    if (count >= entries.size()) {
      return {};
    }

    std::span<const u8> sourceBytes;
    if (!record->shadow.empty()) {
      sourceBytes = record->shadow;
    } else if (record->contents && record->desc.size > 0) {
      sourceBytes = std::span<const u8>(
          static_cast<const u8*>(record->contents),
          static_cast<std::size_t>(record->desc.size));
    }
    if (sourceBytes.empty()) {
      return {};
    }

    auto slice = ctx.queue.uploadTransientBuffer(
        std::span<const std::byte>(
            reinterpret_cast<const std::byte*>(sourceBytes.data()),
            sourceBytes.size()),
        16,
        seqId);
    if (!slice) {
      return {};
    }
    entries[count++] = Entry{
        .sourceHandle = sourceHandle,
        .buffer = slice.buffer,
        .offset = slice.offset,
        .size = slice.size,
    };
    if (encoderBreakdown) {
      if (indexBuffer) {
        encoderBreakdown->addTransientIndexBytes(
            static_cast<u64>(slice.size),
            ActiveEncoderBreakdown::TransientIndexSource::StagedIb);
      } else {
        encoderBreakdown->addTransientVertexBytes(
            static_cast<u64>(slice.size),
            ActiveEncoderBreakdown::TransientVertexSource::StagedStream);
      }
    }
    return slice;
  }
};

bool streamIbStagingActive(const StreamIbStagingCache* cache) noexcept {
  return cache && cache->enabled;
}

struct ArgbufCbufCache {
  bool valid = false;
  u64 payloadHash = 0;
  dxmt9::argbuf_hybrid::ConstantBufferBindings bindings{};
  bool ffpVsValid = false;
  std::array<std::byte, sizeof(FfpVsConsts)> ffpVsBytes{};

  void reset() noexcept {
    valid = false;
    payloadHash = 0;
    bindings = {};
    ffpVsValid = false;
    ffpVsBytes = {};
  }

  bool matches(u64 hash) const noexcept {
    return valid && payloadHash == hash && bindings.complete();
  }

  bool hasBinding(u32 argbufIndex) const noexcept {
    return argbufIndex < bindings.entries.size() &&
           static_cast<bool>(bindings.entries[argbufIndex]);
  }

  dxmt9::argbuf_hybrid::ConstantBufferBinding binding(u32 argbufIndex) const noexcept {
    return argbufIndex < bindings.entries.size()
               ? bindings.entries[argbufIndex]
               : dxmt9::argbuf_hybrid::ConstantBufferBinding{};
  }

  bool hasMatchingBinding(u32 argbufIndex, u64 contentHash, u64 bytes) const noexcept {
    return argbufIndex < bindings.entries.size() &&
           bindings.entries[argbufIndex].contentMatches(contentHash, bytes);
  }

  bool hasMatchingIdentity(u32 argbufIndex, u64 identityHash, u64 bytes) const noexcept {
    return argbufIndex < bindings.entries.size() &&
           bindings.entries[argbufIndex].identityMatches(identityHash, bytes);
  }

  bool hasMatchingFfpVs(const FfpVsConsts& host) const noexcept {
    const auto& binding =
        bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex];
    return ffpVsValid && binding &&
           std::memcmp(ffpVsBytes.data(), &host, sizeof(FfpVsConsts)) == 0;
  }

  dxmt9::argbuf_hybrid::ConstantBufferBinding ffpVsBinding() const noexcept {
    return bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex];
  }

  void merge(u64 hash,
             const dxmt9::argbuf_hybrid::ConstantBufferBindings& written) noexcept {
    for (std::size_t i = 0; i < bindings.entries.size(); ++i) {
      if (written.entries[i]) {
        bindings.entries[i] = written.entries[i];
      }
    }
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    } else {
      valid = false;
    }
  }

  void promotePayloadHash(u64 hash) noexcept {
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    }
  }

  void storeFfpVs(u64 hash,
                  const FfpVsConsts& host,
                  WMT::Buffer buffer,
                  u64 offset,
                  u64 bytes) noexcept {
    bindings.entries[dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex] =
        dxmt9::argbuf_hybrid::ConstantBufferBinding{
            .buffer = buffer,
            .offset = offset,
            .bytes = bytes,
            .contentHash = dxmt9::argbuf_hybrid::hashConstantBufferBytes(
                &host, bytes),
            .identityHash = 0,
        };
    std::memcpy(ffpVsBytes.data(), &host, sizeof(FfpVsConsts));
    ffpVsValid = true;
    if (bindings.complete()) {
      payloadHash = hash;
      valid = true;
    } else {
      valid = false;
    }
  }
};

struct ArgbufCbufIdentityProbe {
  u64 bytes = 0;
  u64 hash = 0;
};

u64 makeArgbufCbufIdentityHash(u64 tag, u64 sourceHash, u64 bytes) noexcept {
  u64 hash = drawBindingPacketHashMix(tag, sourceHash);
  hash = drawBindingPacketHashMix(hash, bytes);
  return hash;
}

u64 drawStateVertexCbufSourceHash(core::FlatDrawStateView drawState) noexcept {
  if (drawState.hasUniformPayload() &&
      drawState.uniformPayload().vertexConstantsHash != 0) {
    return drawState.uniformPayload().vertexConstantsHash;
  }
  return drawState.hot ? drawState.hot->vertexConstantsHash : 0;
}

u64 drawStatePixelCbufSourceHash(core::FlatDrawStateView drawState) noexcept {
  if (drawState.hasUniformPayload() &&
      drawState.uniformPayload().pixelConstantsHash != 0) {
    return drawState.uniformPayload().pixelConstantsHash;
  }
  return drawState.hot ? drawState.hot->pixelConstantsHash : 0;
}

u64 makeArgbufFfpPsIdentityHash(core::FlatDrawStateView drawState,
                                u64 bytes) noexcept {
  if (!drawState.hot) return 0;
  u64 hash = drawBindingPacketHashMix(
      0x6666705f70735f63ull, drawState.hot->key.renderStateHash);
  for (const auto stageHash : drawState.hot->key.textureStageStateHashes) {
    hash = drawBindingPacketHashMix(hash, stageHash);
  }
  hash = drawBindingPacketHashMix(hash, bytes);
  return hash;
}

void stampArgbufCbufBindingIdentities(
    dxmt9::argbuf_hybrid::ConstantBufferBindings& bindings,
    core::FlatDrawStateView drawState) noexcept {
  if (!drawState.hot) return;
  auto stamp = [&](u32 argbufIndex, u64 identityHash) {
    if (argbufIndex < bindings.entries.size() && bindings.entries[argbufIndex]) {
      bindings.entries[argbufIndex].identityHash = identityHash;
    }
  };
  const auto& entries = bindings.entries;
  if (dxmt9::argbuf_hybrid::kConstantBufferVsIndex < entries.size()) {
    const auto& binding =
        entries[dxmt9::argbuf_hybrid::kConstantBufferVsIndex];
    stamp(dxmt9::argbuf_hybrid::kConstantBufferVsIndex,
          makeArgbufCbufIdentityHash(
              0x76735f636275665full,
              drawStateVertexCbufSourceHash(drawState), binding.bytes));
  }
  if (dxmt9::argbuf_hybrid::kConstantBufferPsIndex < entries.size()) {
    const auto& binding =
        entries[dxmt9::argbuf_hybrid::kConstantBufferPsIndex];
    stamp(dxmt9::argbuf_hybrid::kConstantBufferPsIndex,
          makeArgbufCbufIdentityHash(
              0x70735f636275665full,
              drawStatePixelCbufSourceHash(drawState), binding.bytes));
  }
  if (dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex < entries.size()) {
    const auto& binding =
        entries[dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex];
    stamp(dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex,
          makeArgbufFfpPsIdentityHash(drawState, binding.bytes));
  }
}

void recordArgbufCbufUploadForBreakdown(void* userdata,
                                        u32 argbufIndex,
                                        const void* data,
                                        u64 bytes,
                                        u64 hostStructBytes) {
  if (!userdata) {
    return;
  }
  static_cast<ActiveEncoderBreakdown*>(userdata)->recordArgbufCbufUploadContent(
      argbufIndex, data, bytes, hostStructBytes);
}

bool blendFactorNeedsConstantColor(const core::FlatRenderStateSet& rs) {
  const auto isConstantBlend = [](u32 factor) {
    return factor == static_cast<u32>(core::BlendFactor::BlendFactor) ||
           factor == static_cast<u32>(core::BlendFactor::InvBlendFactor);
  };
  if (core::flatStateOr(rs, RS_ALPHABLEND_ENABLE, 0u) == 0u) {
    return false;
  }
  if (isConstantBlend(core::flatStateOr(rs, RS_SRC_BLEND,
                                        static_cast<u32>(core::BlendFactor::One))) ||
      isConstantBlend(core::flatStateOr(rs, RS_DEST_BLEND,
                                        static_cast<u32>(core::BlendFactor::Zero)))) {
    return true;
  }
  if (core::flatStateOr(rs, RS_SEPARATE_ALPHA_BLEND_ENABLE, 0u) == 0u) {
    return false;
  }
  return isConstantBlend(core::flatStateOr(rs, RS_SRC_BLEND_ALPHA,
                                           static_cast<u32>(core::BlendFactor::One))) ||
         isConstantBlend(core::flatStateOr(rs, RS_DEST_BLEND_ALPHA,
                                           static_cast<u32>(core::BlendFactor::Zero)));
}

std::array<float, 4> decodeD3DBlendFactor(u32 argb) {
  constexpr float scale = 1.0f / 255.0f;
  return {
      static_cast<float>((argb >> 16) & 0xffu) * scale,
      static_cast<float>((argb >> 8) & 0xffu) * scale,
      static_cast<float>(argb & 0xffu) * scale,
      static_cast<float>((argb >> 24) & 0xffu) * scale,
  };
}

constexpr u64 kFragmentTextureShadowTag = 0x667261675f746578ull;
constexpr u64 kFragmentSamplerShadowTag = 0x667261675f73616dull;
constexpr u64 kVertexTextureShadowTag = 0x766572745f746578ull;
constexpr u64 kVertexSamplerShadowTag = 0x766572745f73616dull;

bool splitPresentBeforeAcquireEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_SPLIT_PRESENT_ACQUIRE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool presentBoundaryAfterAcquireEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PRESENT_BOUNDARY_AFTER_ACQUIRE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool renderEncoderGpuTimeEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ENCODER_GPU_TIME");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool encoderBreakdownCbufContentEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ENCODER_BREAKDOWN_CBUF_CONTENT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool textureSamplerDirectSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_TEXTURE_SAMPLER_DIRECT_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool streamBindPhaseSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_STREAM_BIND_PHASE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool bindingPacketPlanSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_BINDING_PACKET_PLAN_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool drawIssueSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_DRAW_ISSUE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufCbufProbeSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_CBUF_PROBE_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufReopenSplitPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_REOPEN_SPLIT");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufCbufDirtyIdentityPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_CBUF_DIRTY_IDENTITY");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufPayloadDeltaPerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_PAYLOAD_DELTA");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

bool argbufPayloadDeltaSourcePerfEnabled() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_PERF_ARGBUF_PAYLOAD_DELTA_SOURCE");
    return env && env[0] != '\0' && env[0] != '0';
  }();
  return enabled;
}

using PerfCounterFn = void (*)(std::uint64_t);

PerfCounterFn argbufCbufCachedRepointCpuRecorder(u32 argbufIndex) noexcept {
  switch (argbufIndex) {
    case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointVsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointPsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointFfpVsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
      return perf::countEncodeDrawArgbufCbufCachedRepointFfpPsCpuTime;
    default:
      return nullptr;
  }
}

PerfCounterFn argbufCbufContentProbeCpuRecorder(u32 argbufIndex) noexcept {
  switch (argbufIndex) {
    case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
      return perf::countEncodeDrawArgbufCbufContentProbeVsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
      return perf::countEncodeDrawArgbufCbufContentProbePsCpuTime;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
      return perf::countEncodeDrawArgbufCbufContentProbeFfpPsCpuTime;
    default:
      return nullptr;
  }
}

void countArgbufCbufCachedRepointStage(u32 argbufIndex, u64 bytes) noexcept {
  switch (argbufIndex) {
    case dxmt9::argbuf_hybrid::kConstantBufferVsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointVsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointVsBytes(bytes);
      break;
    case dxmt9::argbuf_hybrid::kConstantBufferPsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointPsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointPsBytes(bytes);
      break;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointFfpVsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointFfpVsBytes(bytes);
      break;
    case dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex:
      perf::countEncodeDrawArgbufCbufCachedRepointFfpPsCalls(1u);
      perf::countEncodeDrawArgbufCbufCachedRepointFfpPsBytes(bytes);
      break;
    default:
      break;
  }
}

// R-BACK-2.29..2.32 — env-driven mid-chunk commit policy. Read once at
// process start; subsequent runs need a re-launch to change it. This is
// the cleanest invariant under R-BACK-2.31 (deterministic split
// decisions, no wallclock or GPU-feedback inputs).
enum class MidChunkCommitPolicy : std::uint8_t {
  Off,
  PerRenderPass,
  PerNRecords,
};

MidChunkCommitPolicy midChunkCommitPolicy() {
  static const MidChunkCommitPolicy policy = [] {
    // R-BACK-2.34 — production default flipped from Off to PerRenderPass
    // 2026-05-10. The X1 chain-probe measurement showed wall-time -5%,
    // encode CPU -63%, present_acquire_wait -20% under the cap=4 from
    // R-BACK-2.33 (`docs/boundary-baseline-measurements.md`). SFIV
    // heavy-scene (U1) was neutral on fps but -44% on
    // `gpu_command_buffer_time_ms` p99. The cap from R-BACK-2.33
    // bounds tile-flush + commit overhead at ~2.1 ms / frame on the
    // SFIV envelope per `docs/research/g-axis-tuning.md`.
    // `DXMT9_MID_CHUNK_COMMIT_POLICY=off` remains a one-line opt-out.
    const char* env = std::getenv("DXMT9_MID_CHUNK_COMMIT_POLICY");
    if (!env || env[0] == '\0') return MidChunkCommitPolicy::PerRenderPass;
    if (std::strcmp(env, "per-render-pass") == 0) {
      return MidChunkCommitPolicy::PerRenderPass;
    }
    if (std::strcmp(env, "per-n-records") == 0) {
      return MidChunkCommitPolicy::PerNRecords;
    }
    if (std::strcmp(env, "off") == 0) {
      return MidChunkCommitPolicy::Off;
    }
    // Unrecognized token → fall back to the production default rather
    // than silently turning the policy off. R-BACK-2.31 determinism
    // is preserved because the env is read-once and the table of
    // accepted tokens is closed.
    return MidChunkCommitPolicy::PerRenderPass;
  }();
  return policy;
}

std::uint32_t midChunkCommitNRecords() {
  static const std::uint32_t n = [] {
    const char* env = std::getenv("DXMT9_MID_CHUNK_COMMIT_RECORDS");
    if (!env || env[0] == '\0') return 64u;
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    if (parsed <= 0 || end == env) return 64u;
    return static_cast<std::uint32_t>(parsed);
  }();
  return n;
}

// R-BACK-2.33 — per-chunk sub-CB chain length cap. The encode thread
// stops splitting once a chunk has produced this many sub-CBs (counting
// the chain tail toward the cap), so a 27-render-pass chunk does not
// turn into a 27-CB chain whose tile-flush + commit overhead overwhelms
// the pipelining win. 4 was chosen by `docs/research/g-axis-tuning.md`
// against an estimated TBDR cost model; it is configurable so empirical
// re-measurement can move the default. Read once at process start.
std::uint32_t midChunkCommitCapPerRenderPass() {
  static const std::uint32_t cap = [] {
    const char* env = std::getenv("DXMT9_MID_CHUNK_COMMIT_CAP_PER_RENDER_PASS");
    if (!env || env[0] == '\0') return 4u;
    char* end = nullptr;
    const long parsed = std::strtol(env, &end, 10);
    // 0 disables the cap (unbounded chain). Negative or unparseable
    // tokens fall back to the default to preserve R-BACK-2.31 determinism.
    if (parsed < 0 || end == env) return 4u;
    return static_cast<std::uint32_t>(parsed);
  }();
  return cap;
}

WMTWinding frontFaceWinding() {
  return debug::frontFaceCounterClockwise() ? WMTWindingCounterClockwise : WMTWindingClockwise;
}

WMTCullMode applyDebugCullOverride(WMTCullMode cullMode) {
  const char* env = std::getenv("DXMT_DEBUG_FORCE_CULL_MODE");
  if (!env || env[0] == '\0') {
    return cullMode;
  }
  if (std::strcmp(env, "none") == 0) {
    return WMTCullModeNone;
  }
  if (std::strcmp(env, "front") == 0) {
    return WMTCullModeFront;
  }
  if (std::strcmp(env, "back") == 0) {
    return WMTCullModeBack;
  }
  return cullMode;
}

WMTTriangleFillMode triangleFillModeFromRenderState(
    const core::FlatRenderStateSet& renderStates) {
  constexpr u32 kD3DFillWireframe = 2u;
  return core::flatStateOr(renderStates, core::RS_FILL_MODE, 3u) == kD3DFillWireframe
             ? WMTTriangleFillModeLines
             : WMTTriangleFillModeFill;
}

void setRasterizerCullMode(EncodeContext& ctx,
                           WMT::RenderCommandEncoder& encoder,
                           const core::FlatRenderStateSet& renderStates,
                           WMTCullMode cullMode) {
  cullMode = applyDebugCullOverride(cullMode);
  // D3D9 RS_DEPTH_BIAS / RS_SLOPE_SCALE_DEPTH_BIAS are stored as DWORDs but
  // semantically float; bit_cast restores the IEEE 754 layout that
  // MTLRenderCommandEncoder.setDepthBias:slopeScale:clamp: expects. clamp is
  // not exposed by D3D9 RS and is left at 0.0f (Metal's "unbounded" sentinel).
  const float depthBias = std::bit_cast<float>(
      core::flatStateOr(renderStates, core::RS_DEPTH_BIAS, 0u));
  const float slopeScale = std::bit_cast<float>(
      core::flatStateOr(renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  recordedSetRasterizerState(ctx, encoder, triangleFillModeFromRenderState(renderStates), cullMode,
                             WMTDepthClipModeClip, frontFaceWinding(),
                             depthBias, slopeScale, 0.0f);
  countRasterizerBind();
}

// Attachment key + hazard bloom used by encodeChunk to decide whether to
// flush + restart the render pass between commands. Previously file-local
// to backend_metal.mm.
struct AttachmentKey {
  std::array<u64, core::kMaxRenderTargets> colorHandles{};
  u64 depthHandle = 0;
  u32 sampleCount = 1;
  friend bool operator==(const AttachmentKey&, const AttachmentKey&) = default;
};

struct ArgbufPayloadDeltaKey {
  u64 hash = 0;
  u64 vertexConstantsHash = 0;
  u64 pixelConstantsHash = 0;
};

struct ArgbufPayloadDeltaComponentKey {
  u64 vsFloatHash = 0;
  u64 vsIntHash = 0;
  u64 vsBoolHash = 0;
  u64 psFloatHash = 0;
  u64 psIntHash = 0;
  u64 psBoolHash = 0;
};

struct RenderEncoderGpuAttachment {
  std::array<WMTSampleBufferAttachmentInfo, 1> attachments{};
  core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample sample{};
  bool active = false;

  std::span<const WMTSampleBufferAttachmentInfo> span() const {
    return active ? std::span<const WMTSampleBufferAttachmentInfo>(
                        attachments.data(), attachments.size())
                  : std::span<const WMTSampleBufferAttachmentInfo>{};
  }
};

constexpr std::uint32_t kMaxRenderEncoderGpuSamples = 8192;

struct EncodeChunkSessionStorage {
  WMT::Reference<WMT::RenderCommandEncoder> activeRenderEncoder{};
  WMT::Reference<WMT::BlitCommandEncoder> activeBlitEncoder{};
  std::vector<std::function<void()>> postCommitCallbacks;
  std::vector<std::function<void()>> completionCallbacks;
  std::optional<core::metalcapture::MetalCaptureRequest> metalCaptureRequest;
  AttachmentKey activeKey{};
  HazardProbe activeWriteHazard{};
  bool hasActiveRender = false;
  // R-BACK-13.1 / 13.6: current render encoder's chosen FFP path.
  bool activePassUsesTileFfp = false;
  // R-BACK-12.22 / 12.24: current render encoder's sticky argbuf state.
  bool activePassUsesArgbufHybrid = false;
  // R-BACK-12.22..12.26: resource-array sub-mode of the sticky argbuf table.
  bool activePassUsesArgbufResourceArray = false;
  bool activePassUsesArgbufDirectCbuf = false;
  // Current pass backing transient slab. Slot 30 is bound once at pass open.
  WMT::Buffer activeArgbufStorage{};
  std::uint64_t activeArgbufOffset = 0;
  std::optional<core::FlatDrawStateKey> activeDrawStateKey;
  bool activeDrawStateUsesPrefetchedPsoLayout = false;
  std::optional<core::ClearDesc> pendingClear;
  std::size_t pendingClearCommandIndex =
      std::numeric_limits<std::size_t>::max();
  // R-BACK-15.4: color attachment handles bound on the active render encoder.
  std::array<core::Handle, core::kMaxRenderTargets> activeColorHandles{};
  ActiveColorAttachmentDump activeColorAttachmentDump{};
  ActiveDepthAttachmentDump activeDepthAttachmentDump{};
  std::vector<ActiveDrawTextureDump> activeDrawTextureDumps;
  u64 activeRenderEncoderSeq = 0;
  u64 activeRenderEncoderIndex = 0;
  uniform::DirtyState uniformDirty{};
  std::optional<u64> lastArgbufPayloadHash;
  std::optional<ArgbufPayloadDeltaKey> lastArgbufPayloadDeltaKey;
  std::optional<ArgbufPayloadDeltaComponentKey>
      lastArgbufPayloadDeltaComponentKey;
  std::optional<core::DrawUniformPayload> lastArgbufPayloadDeltaPayload;
  ArgbufCbufCache argbufCbufCache;
  StreamIbStagingCache activeStreamIbStaging;
  TextureSamplerBindShadow textureSamplerShadow{};
  ActiveEncoderBreakdown activeEncoderBreakdown;
  std::optional<VisibilityScoutPass> activeVisibilityScout;
  u64 renderEncoderIndex = 0;
  WMT::Reference<WMT::CounterSampleBuffer> renderEncoderGpuSampleBuffer{};
  std::vector<core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample>
      renderEncoderGpuSamples;
  std::uint32_t renderEncoderGpuSampleCursor = 0;
  std::uint32_t requestedRenderEncoderGpuSamples = 0;
  std::uint64_t committedSubCommandBuffers = 0;
  bool initialized = false;
};

void initializeEncodeChunkSessionStorage(
    EncodeChunkSessionStorage& state,
    const uniform::DirtyState& dirty) {
  if (state.initialized) {
    return;
  }
  state.uniformDirty = dirty;
  state.initialized = true;
}

EncodeChunkSessionStorage makeEncodeChunkSessionStorage(
    const uniform::DirtyState& dirty) {
  EncodeChunkSessionStorage state{};
  initializeEncodeChunkSessionStorage(state, dirty);
  return state;
}

void initializeEncodeChunkSessionGpuSamplingStorage(
    EncodeChunkSessionStorage& state,
    WMT::Device device,
    std::size_t commandCount) {
  if (state.requestedRenderEncoderGpuSamples != 0) {
    return;
  }
  if (!renderEncoderGpuTimeEnabled() ||
      !device.supportsCounterSampling(WMTCounterSamplingPointAtStageBoundary)) {
    return;
  }
  state.requestedRenderEncoderGpuSamples =
      static_cast<std::uint32_t>(std::min<std::size_t>(
          kMaxRenderEncoderGpuSamples,
          std::max<std::size_t>(2u, commandCount * 2u + 16u)));
  state.renderEncoderGpuSampleBuffer =
      device.newCounterSampleBuffer(state.requestedRenderEncoderGpuSamples,
                                    /*shared=*/true);
  if (!state.renderEncoderGpuSampleBuffer) {
    state.requestedRenderEncoderGpuSamples = 0;
  }
}

std::size_t sessionGpuSamplingCommandCount(
    const core::ChunkSlot& slot,
    std::size_t currentCommandCount,
    std::span<const core::metalqueue::ReadySlotSnapshot> lookaheadSources) noexcept {
  if (lookaheadSources.empty()) {
    return currentCommandCount;
  }

  constexpr std::size_t kMaxSampledCommands =
      (kMaxRenderEncoderGpuSamples - 16u) / 2u;
  std::size_t total = 0;
  bool includesCurrentSlot = false;
  auto addCommands = [&](std::size_t count) noexcept {
    if (total >= kMaxSampledCommands) {
      return;
    }
    const std::size_t remaining = kMaxSampledCommands - total;
    total += std::min(count, remaining);
  };
  for (const auto& source : lookaheadSources) {
    if (!source.slot && source.seqId == 0) {
      continue;
    }
    includesCurrentSlot =
        includesCurrentSlot || source.slot == &slot ||
        source.seqId == slot.seqId;
    addCommands(source.commandCount);
  }
  if (!includesCurrentSlot) {
    addCommands(currentCommandCount);
  }
  return std::min(kMaxSampledCommands,
                  std::max<std::size_t>(currentCommandCount, total));
}

AttachmentKey makeAttachmentKey(const core::FlatDrawStateRecord& hot) {
  AttachmentKey key;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    key.colorHandles[i] = hot.colorAttachments[i].handle.value;
    key.sampleCount = std::max(key.sampleCount, hot.colorAttachments[i].sampleCount);
  }
  key.depthHandle = hot.depthStencil.handle.value;
  key.sampleCount = std::max(key.sampleCount, hot.depthStencil.sampleCount);
  return key;
}

AttachmentKey makeAttachmentKey(const core::ClearDesc& clear) {
  AttachmentKey key;
  if (clear.clearColor) {
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      key.colorHandles[i] = clear.colorAttachments[i].handle.value;
      key.sampleCount = std::max(key.sampleCount, clear.colorAttachments[i].sampleCount);
    }
  }
  if (clear.clearDepth || clear.clearStencil) {
    key.depthHandle = clear.depthStencil.handle.value;
    key.sampleCount = std::max(key.sampleCount, clear.depthStencil.sampleCount);
  }
  return key;
}

struct RenderPassFrameKey {
  u64 color0 = 0;
  u64 depth = 0;
  u32 sampleCount = 1;

  bool valid() const noexcept {
    return color0 != 0 || depth != 0;
  }

  friend bool operator==(const RenderPassFrameKey&, const RenderPassFrameKey&) = default;
};

RenderPassFrameKey makeRenderPassFrameKey(const core::FlatDrawStateRecord& hot) {
  return RenderPassFrameKey{
      .color0 = hot.colorAttachments[0].handle.value,
      .depth = hot.depthStencil.handle.value,
      .sampleCount = std::max(hot.colorAttachments[0].sampleCount,
                              hot.depthStencil.sampleCount),
  };
}

std::size_t renderPassReentryTopLimit() {
  static const std::size_t value = []() -> std::size_t {
    const char* env = std::getenv("DXMT9_PERF_RENDER_PASS_REENTRY_TOP");
    if (!env || env[0] == '\0' || env[0] == '0') {
      return 0;
    }
    char* end = nullptr;
    const auto parsed = std::strtoull(env, &end, 10);
    if (end == env || parsed == 1u) {
      return 8;
    }
    return static_cast<std::size_t>(std::min<unsigned long long>(parsed, 32ull));
  }();
  return perf::enabled() ? value : 0;
}

struct RenderPassAttachmentFootprint {
  std::uint64_t color0Bytes = 0;
  std::uint64_t depthBytes = 0;

  std::uint64_t totalBytes() const noexcept {
    return color0Bytes + depthBytes;
  }
};

RenderPassAttachmentFootprint estimateRenderPassAttachmentFootprintBytes(
    EncodeContext& ctx,
    const core::FlatDrawStateRecord& hot) {
  RenderPassAttachmentFootprint footprint{};
  auto addSurface = [&](core::Handle handle) {
    if (!handle) {
      return std::uint64_t{0};
    }
    const auto* surface = ctx.pool.findSurface(handle.value);
    if (!surface || !surface->texture) {
      return std::uint64_t{0};
    }
    return static_cast<std::uint64_t>(surface->desc.width) *
           static_cast<std::uint64_t>(surface->desc.height) *
           static_cast<std::uint64_t>(core::bytesPerPixel(surface->desc.format));
  };
  footprint.color0Bytes = addSurface(hot.colorAttachments[0].handle);
  footprint.depthBytes = addSurface(hot.depthStencil.handle);
  return footprint;
}

struct RenderPassStoreProofSummary {
  perf::RenderPassColorStoreProof color =
      perf::RenderPassColorStoreProof::BlockNullColor;
  perf::RenderPassDepthStoreProof depth =
      perf::RenderPassDepthStoreProof::BlockNullDepth;
  std::uint32_t colorTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t depthTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
};

struct RenderPassReentryTopEntry {
  RenderPassFrameKey a{};
  RenderPassFrameKey b{};
  std::uint64_t count = 0;
  std::uint64_t preservationBytes = 0;
  std::uint64_t priorASeq = 0;
  std::uint64_t priorAEncoder = 0;
  std::uint32_t priorAPass = 0;
  std::uint64_t firstSeq = 0;
  std::uint64_t firstEncoder = 0;
  std::uint32_t firstPass = 0;
  std::uint64_t firstBSeq = 0;
  std::uint64_t firstBEncoder = 0;
  std::uint32_t firstBPass = 0;
  std::uint64_t lastSeq = 0;
  std::uint64_t lastEncoder = 0;
  std::uint32_t lastPass = 0;
  std::uint64_t lastBSeq = 0;
  std::uint64_t lastBEncoder = 0;
  std::uint32_t lastBPass = 0;
  bool bReadsAColor = false;
  bool bReadsADepth = false;
  bool aReadsBColor = false;
  bool aReadsBDepth = false;
  perf::RenderPassColorStoreProof aColorProof =
      perf::RenderPassColorStoreProof::BlockNullColor;
  perf::RenderPassDepthStoreProof aDepthProof =
      perf::RenderPassDepthStoreProof::BlockNullDepth;
  perf::RenderPassColorStoreProof bColorProof =
      perf::RenderPassColorStoreProof::BlockNullColor;
  perf::RenderPassDepthStoreProof bDepthProof =
      perf::RenderPassDepthStoreProof::BlockNullDepth;
  std::uint32_t aColorTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t aDepthTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t bColorTouchDistance =
      std::numeric_limits<std::uint32_t>::max();
  std::uint32_t bDepthTouchDistance =
      std::numeric_limits<std::uint32_t>::max();

  bool used() const noexcept {
    return count != 0;
  }
};

struct RenderPassReadRelation {
  bool color = false;
  bool depth = false;
};

RenderPassReadRelation readRelationToKey(const core::FlatDrawStateRecord& hot,
                                         RenderPassFrameKey key) noexcept {
  RenderPassReadRelation relation{};
  if (!key.valid()) {
    return relation;
  }
  const auto& textures = hot.textures;
  const std::uint32_t mask = hot.textureMask;
  for (std::size_t i = 0; i < textures.size(); ++i) {
    if ((mask & (1u << i)) == 0) {
      continue;
    }
    const u64 handle = textures[i].value;
    relation.color = relation.color || (key.color0 != 0 && handle == key.color0);
    relation.depth = relation.depth || (key.depth != 0 && handle == key.depth);
  }
  return relation;
}

struct PendingRenderPassReentryTop {
  RenderPassFrameKey a{};
  RenderPassFrameKey b{};
  std::uint64_t preservationBytes = 0;
  std::uint64_t priorASeq = 0;
  std::uint64_t priorAEncoder = 0;
  std::uint32_t priorAPass = 0;
  std::uint64_t seq = 0;
  std::uint64_t encoder = 0;
  std::uint32_t pass = 0;
  std::uint64_t bSeq = 0;
  std::uint64_t bEncoder = 0;
  std::uint32_t bPass = 0;
  bool bReadsAColor = false;
  bool bReadsADepth = false;
  RenderPassStoreProofSummary aProof{};
  RenderPassStoreProofSummary bProof{};
};

struct RenderPassFrameTracker {
  std::array<RenderPassFrameKey, 64> seen{};
  std::array<std::uint32_t, 64> lastSeenPassIndex{};
  std::array<std::uint64_t, 64> lastSeenSeq{};
  std::array<std::uint64_t, 64> lastSeenEncoder{};
  std::array<std::uint32_t, 64> lastSeenPass{};
  std::array<RenderPassReentryTopEntry, 32> reentryTop{};
  std::size_t seenCount = 0;
  std::uint32_t passIndex = 0;
  std::uint64_t frameIndex = 0;
  std::optional<RenderPassFrameKey> last{};
  std::uint64_t lastSeq = 0;
  std::uint64_t lastEncoder = 0;
  std::uint32_t lastPass = 0;
  RenderPassStoreProofSummary lastProof{};
  std::optional<RenderPassFrameKey> previousKeyForCurrent{};
  bool currentReadsPreviousColor = false;
  bool currentReadsPreviousDepth = false;
  std::optional<PendingRenderPassReentryTop> pendingReentry{};

  void reset() {
    finalizePendingReentry(currentReadsPreviousColor, currentReadsPreviousDepth);
    emitReentryTop();
    seenCount = 0;
    lastSeenPassIndex.fill(0);
    lastSeenSeq.fill(0);
    lastSeenEncoder.fill(0);
    lastSeenPass.fill(0);
    reentryTop = {};
    passIndex = 0;
    ++frameIndex;
    last.reset();
    lastSeq = 0;
    lastEncoder = 0;
    lastPass = 0;
    lastProof = {};
    previousKeyForCurrent.reset();
    currentReadsPreviousColor = false;
    currentReadsPreviousDepth = false;
    pendingReentry.reset();
  }

  std::optional<std::size_t> findSeenIndex(RenderPassFrameKey key) const noexcept {
    for (std::size_t i = 0; i < seenCount; ++i) {
      if (seen[i] == key) {
        return i;
      }
    }
    return std::nullopt;
  }

  void remember(RenderPassFrameKey key,
                std::uint32_t currentPassIndex,
                std::uint64_t seq,
                std::uint64_t encoder) noexcept {
    if (seenCount < seen.size()) {
      seen[seenCount] = key;
      lastSeenPassIndex[seenCount] = currentPassIndex;
      lastSeenSeq[seenCount] = seq;
      lastSeenEncoder[seenCount] = encoder;
      lastSeenPass[seenCount] = currentPassIndex;
      ++seenCount;
    }
  }

  void recordReentryTop(RenderPassFrameKey a,
                        RenderPassFrameKey b,
                        std::uint64_t preservationBytes,
                        std::uint64_t priorASeq,
                        std::uint64_t priorAEncoder,
                        std::uint32_t priorAPass,
                        std::uint64_t seq,
                        std::uint64_t encoder,
                        std::uint64_t bSeq,
                        std::uint64_t bEncoder,
                        std::uint32_t bPass,
                        bool bReadsAColor,
                        bool bReadsADepth,
                        bool aReadsBColor,
                        bool aReadsBDepth,
                        RenderPassStoreProofSummary aProof,
                        RenderPassStoreProofSummary bProof,
                        std::uint32_t currentPassIndex) noexcept {
    if (renderPassReentryTopLimit() == 0) {
      return;
    }
    RenderPassReentryTopEntry* insert = nullptr;
    RenderPassReentryTopEntry* weakest = nullptr;
    for (auto& entry : reentryTop) {
      if (entry.used() && entry.a == a && entry.b == b &&
          entry.firstEncoder == encoder && entry.firstBEncoder == bEncoder &&
          entry.priorASeq == priorASeq &&
          entry.priorAEncoder == priorAEncoder &&
          entry.priorAPass == priorAPass &&
          entry.bReadsAColor == bReadsAColor &&
          entry.bReadsADepth == bReadsADepth &&
          entry.aReadsBColor == aReadsBColor &&
          entry.aReadsBDepth == aReadsBDepth &&
          entry.aColorProof == aProof.color &&
          entry.aDepthProof == aProof.depth &&
          entry.bColorProof == bProof.color &&
          entry.bDepthProof == bProof.depth &&
          entry.aColorTouchDistance == aProof.colorTouchDistance &&
          entry.aDepthTouchDistance == aProof.depthTouchDistance &&
          entry.bColorTouchDistance == bProof.colorTouchDistance &&
          entry.bDepthTouchDistance == bProof.depthTouchDistance) {
        ++entry.count;
        entry.preservationBytes += preservationBytes;
        entry.lastSeq = seq;
        entry.lastEncoder = encoder;
        entry.lastPass = currentPassIndex;
        entry.lastBSeq = bSeq;
        entry.lastBEncoder = bEncoder;
        entry.lastBPass = bPass;
        return;
      }
      if (!entry.used() && !insert) {
        insert = &entry;
      }
      if (entry.used() && (!weakest || entry.preservationBytes < weakest->preservationBytes)) {
        weakest = &entry;
      }
    }
    if (!insert) {
      insert = weakest;
      if (!insert || insert->preservationBytes >= preservationBytes) {
        return;
      }
    }
    *insert = RenderPassReentryTopEntry{
        .a = a,
        .b = b,
        .count = 1,
        .preservationBytes = preservationBytes,
        .priorASeq = priorASeq,
        .priorAEncoder = priorAEncoder,
        .priorAPass = priorAPass,
        .firstSeq = seq,
        .firstEncoder = encoder,
        .firstPass = currentPassIndex,
        .firstBSeq = bSeq,
        .firstBEncoder = bEncoder,
        .firstBPass = bPass,
        .lastSeq = seq,
        .lastEncoder = encoder,
        .lastPass = currentPassIndex,
        .lastBSeq = bSeq,
        .lastBEncoder = bEncoder,
        .lastBPass = bPass,
        .bReadsAColor = bReadsAColor,
        .bReadsADepth = bReadsADepth,
        .aReadsBColor = aReadsBColor,
        .aReadsBDepth = aReadsBDepth,
        .aColorProof = aProof.color,
        .aDepthProof = aProof.depth,
        .bColorProof = bProof.color,
        .bDepthProof = bProof.depth,
        .aColorTouchDistance = aProof.colorTouchDistance,
        .aDepthTouchDistance = aProof.depthTouchDistance,
        .bColorTouchDistance = bProof.colorTouchDistance,
        .bDepthTouchDistance = bProof.depthTouchDistance,
    };
  }

  void finalizePendingReentry(bool aReadsBColor, bool aReadsBDepth) noexcept {
    if (!pendingReentry.has_value()) {
      return;
    }
    const auto pending = *pendingReentry;
    pendingReentry.reset();
    recordReentryTop(pending.a, pending.b, pending.preservationBytes,
                     pending.priorASeq, pending.priorAEncoder,
                     pending.priorAPass, pending.seq, pending.encoder, pending.bSeq,
                     pending.bEncoder, pending.bPass, pending.bReadsAColor,
                     pending.bReadsADepth, aReadsBColor, aReadsBDepth,
                     pending.aProof, pending.bProof, pending.pass);
  }

  void emitReentryTop() const {
    const std::size_t limit = renderPassReentryTopLimit();
    if (limit == 0) {
      return;
    }
    std::array<const RenderPassReentryTopEntry*, 32> sorted{};
    std::size_t count = 0;
    for (const auto& entry : reentryTop) {
      if (entry.used()) {
        sorted[count++] = &entry;
      }
    }
    if (count == 0) {
      return;
    }
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(count),
              [](const auto* a, const auto* b) {
                if (a->preservationBytes != b->preservationBytes) {
                  return a->preservationBytes > b->preservationBytes;
                }
                return a->count > b->count;
              });
    const std::size_t rows = std::min(count, limit);
    for (std::size_t i = 0; i < rows; ++i) {
      const auto& entry = *sorted[i];
      std::fprintf(
          stderr,
          "[dxmt9-perf-render-pass-reentry frame=%llu rank=%zu "
          "a_rt=0x%llx a_depth=0x%llx a_samples=%u "
          "b_rt=0x%llx b_depth=0x%llx b_samples=%u "
          "count=%llu preservation_bytes=%llu "
          "prior_a_seq=%llu prior_a_encoder=%llu prior_a_pass=%u "
          "first_seq=%llu first_encoder=%llu first_pass=%u "
          "first_b_seq=%llu first_b_encoder=%llu first_b_pass=%u "
          "last_seq=%llu last_encoder=%llu last_pass=%u "
          "last_b_seq=%llu last_b_encoder=%llu last_b_pass=%u "
          "b_reads_a_color=%u b_reads_a_depth=%u "
          "a_reads_b_color=%u a_reads_b_depth=%u "
          "a_color_proof=%u a_depth_proof=%u "
          "b_color_proof=%u b_depth_proof=%u "
          "a_color_touch_distance=%u a_depth_touch_distance=%u "
          "b_color_touch_distance=%u b_depth_touch_distance=%u]\n",
          static_cast<unsigned long long>(frameIndex),
          i + 1,
          static_cast<unsigned long long>(entry.a.color0),
          static_cast<unsigned long long>(entry.a.depth),
          entry.a.sampleCount,
          static_cast<unsigned long long>(entry.b.color0),
          static_cast<unsigned long long>(entry.b.depth),
          entry.b.sampleCount,
          static_cast<unsigned long long>(entry.count),
          static_cast<unsigned long long>(entry.preservationBytes),
          static_cast<unsigned long long>(entry.priorASeq),
          static_cast<unsigned long long>(entry.priorAEncoder),
          entry.priorAPass,
          static_cast<unsigned long long>(entry.firstSeq),
          static_cast<unsigned long long>(entry.firstEncoder),
          entry.firstPass,
          static_cast<unsigned long long>(entry.firstBSeq),
          static_cast<unsigned long long>(entry.firstBEncoder),
          entry.firstBPass,
          static_cast<unsigned long long>(entry.lastSeq),
          static_cast<unsigned long long>(entry.lastEncoder),
          entry.lastPass,
          static_cast<unsigned long long>(entry.lastBSeq),
          static_cast<unsigned long long>(entry.lastBEncoder),
          entry.lastBPass,
          entry.bReadsAColor ? 1u : 0u,
          entry.bReadsADepth ? 1u : 0u,
          entry.aReadsBColor ? 1u : 0u,
          entry.aReadsBDepth ? 1u : 0u,
          static_cast<unsigned>(entry.aColorProof),
          static_cast<unsigned>(entry.aDepthProof),
          static_cast<unsigned>(entry.bColorProof),
          static_cast<unsigned>(entry.bDepthProof),
          entry.aColorTouchDistance,
          entry.aDepthTouchDistance,
          entry.bColorTouchDistance,
          entry.bDepthTouchDistance);
    }
  }

  void noteStart(RenderPassFrameKey key,
                 RenderPassAttachmentFootprint footprint,
                 RenderPassStoreProofSummary proof,
                 std::uint64_t seq,
                 std::uint64_t encoder) {
    if (!key.valid()) {
      return;
    }
    const bool previousPassReadsPreviousColor = currentReadsPreviousColor;
    const bool previousPassReadsPreviousDepth = currentReadsPreviousDepth;
    finalizePendingReentry(previousPassReadsPreviousColor, previousPassReadsPreviousDepth);
    const std::uint32_t currentPassIndex = passIndex++;
    if (last.has_value() && *last != key) {
      const bool sameRt = last->color0 != 0 && last->color0 == key.color0;
      const bool sameDepth = last->depth != 0 && last->depth == key.depth;
      if (!sameRt && sameDepth) {
        perf::countRenderPassTransitionRtChangeSameDepth();
      } else if (sameRt && !sameDepth) {
        perf::countRenderPassTransitionSameRtDepthChange();
      } else if (!sameRt && !sameDepth) {
        perf::countRenderPassTransitionRtDepthChange();
      }
    }
    const auto seenIndex = findSeenIndex(key);
    if (seenIndex.has_value()) {
      if (last.has_value() && *last == key) {
        perf::countRenderPassSameKeyAdjacent();
      } else {
        perf::countRenderPassSameKeyReentry();
        const auto lastPassIndex = lastSeenPassIndex[*seenIndex];
        const std::uint32_t interveningPasses =
            currentPassIndex > lastPassIndex ? currentPassIndex - lastPassIndex - 1u : 0u;
        perf::countRenderPassSameKeyReentryDistance(interveningPasses);
        const std::uint64_t preservationBytes = footprint.totalBytes() * 2u;
        const std::uint64_t priorASeq = lastSeenSeq[*seenIndex];
        const std::uint64_t priorAEncoder = lastSeenEncoder[*seenIndex];
        const std::uint32_t priorAPass = lastSeenPass[*seenIndex];
        if (interveningPasses == 1 && last.has_value()) {
          perf::countRenderPassSameKeyReentryDistance1Shape(
              last->color0 != 0 && last->color0 == key.color0,
              last->depth != 0 && last->depth == key.depth,
              preservationBytes);
          pendingReentry = PendingRenderPassReentryTop{
              .a = key,
              .b = *last,
              .preservationBytes = preservationBytes,
              .priorASeq = priorASeq,
              .priorAEncoder = priorAEncoder,
              .priorAPass = priorAPass,
              .seq = seq,
              .encoder = encoder,
              .pass = currentPassIndex,
              .bSeq = lastSeq,
              .bEncoder = lastEncoder,
              .bPass = lastPass,
              .bReadsAColor = previousPassReadsPreviousColor,
              .bReadsADepth = previousPassReadsPreviousDepth,
              .aProof = proof,
              .bProof = lastProof,
          };
        }
        // A same-key re-entry generally implies the previous pass stored
        // the attachment contents and this pass loads them again. Count
        // that store+load footprint as the preservation budget to attack.
        perf::countRenderPassSameKeyReentryPreservationBytes(preservationBytes);
        perf::countRenderPassSameKeyReentryColorPreservationBytes(
            footprint.color0Bytes * 2u);
        perf::countRenderPassSameKeyReentryDepthPreservationBytes(
            footprint.depthBytes * 2u);
      }
      lastSeenPassIndex[*seenIndex] = currentPassIndex;
      lastSeenSeq[*seenIndex] = seq;
      lastSeenEncoder[*seenIndex] = encoder;
      lastSeenPass[*seenIndex] = currentPassIndex;
    } else {
      remember(key, currentPassIndex, seq, encoder);
    }
    previousKeyForCurrent = last;
    currentReadsPreviousColor = false;
    currentReadsPreviousDepth = false;
    last = key;
    lastSeq = seq;
    lastEncoder = encoder;
    lastPass = currentPassIndex;
    lastProof = proof;
  }

  void noteDrawRead(const core::FlatDrawStateRecord& hot) noexcept {
    if (!previousKeyForCurrent.has_value()) {
      return;
    }
    const auto relation = readRelationToKey(hot, *previousKeyForCurrent);
    currentReadsPreviousColor = currentReadsPreviousColor || relation.color;
    currentReadsPreviousDepth = currentReadsPreviousDepth || relation.depth;
  }
};

bool clearMatchesColorAttachment(const std::optional<ClearDesc>& clear,
                                 std::size_t index,
                                 Handle attachment) {
  return clear.has_value() && clear->clearColor && attachment &&
         clear->colorAttachments[index].handle == attachment;
}

bool clearMatchesDepthStencilAttachment(const std::optional<ClearDesc>& clear,
                                        Handle attachment,
                                        bool clearStencil) {
  if (!clear.has_value() || !attachment) {
    return false;
  }
  const bool requested = clearStencil ? clear->clearStencil : clear->clearDepth;
  return requested && clear->depthStencil.handle == attachment;
}

bool x8ShaderAlphaFillEnabledForDiagnostics() {
  static const bool enabled = [] {
    const char* env = std::getenv("DXMT9_X8_SHADER_ALPHA_FILL");
    return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
  }();
  return enabled;
}

bool isX8TextureFormat(core::Format format) {
  return format == core::Format::X8R8G8B8 ||
         format == core::Format::X8B8G8R8;
}

u32 x8AlphaOneTextureMaskForDraw(core::FlatDrawStateView drawState,
                                 const resources::Pool& pool) {
  if (!x8ShaderAlphaFillEnabledForDiagnostics() || !drawState.hot) {
    return 0;
  }
  const auto& hot = *drawState.hot;
  u32 mask = 0;
  for (u32 stage = 0; stage < core::kMaxFragmentSamplers; ++stage) {
    if ((hot.textureMask & (1u << stage)) == 0u || !hot.textures[stage]) {
      continue;
    }
    const auto* texture = pool.findTexture(hot.textures[stage].value);
    if (texture && isX8TextureFormat(texture->desc.format)) {
      mask |= 1u << stage;
    }
  }
  return mask;
}

u32 primitiveVertexCount(core::PrimitiveType type, u32 primitiveCount) {
  switch (type) {
    case core::PrimitiveType::PointList: return primitiveCount;
    case core::PrimitiveType::LineList: return primitiveCount * 2u;
    case core::PrimitiveType::LineStrip: return primitiveCount + 1u;
    case core::PrimitiveType::TriangleList: return primitiveCount * 3u;
    case core::PrimitiveType::TriangleStrip:
    case core::PrimitiveType::TriangleFan:
      return primitiveCount + 2u;
  }
  return 0u;
}

u64 shaderVariantHashForDraw(core::FlatDrawStateView drawState,
                             const resources::Pool* pool = nullptr,
                             bool fragmentlessDepthOnly = false) {
  if (!drawState.hot || !drawState.hasShaderContext()) {
    return 0;
  }
  const auto& hot = *drawState.hot;
  const auto& shader = drawState.shaderContext();
  u64 hash = shader.vertexShader.hash ^ (shader.pixelShader.hash << 1) ^
             (hot.key.vertexDeclHash << 2) ^ hot.key.renderStateHash ^
             (hot.textureMask << 3) ^ (hot.renderTargetMask << 4);
  hash ^= hot.vertexConstantsHash << 1;
  hash ^= hot.pixelConstantsHash << 2;
  if (pool) {
    hash ^= static_cast<u64>(x8AlphaOneTextureMaskForDraw(drawState, *pool)) << 5;
  }
  if (fragmentlessDepthOnly) {
    hash ^= debug::probeFragmentlessDepthOnlyKeepVSOut()
                ? 0x9e3b7a6c8fb4c521ull
                : 0xf1a974f2b7a25c31ull;
  }
  return hash;
}

u64 shaderSourceAttributionKeyForDraw(
    core::FlatDrawStateView drawState,
    std::optional<bool> forceTextureWhiteOverride = std::nullopt,
    bool fragmentlessDepthOnly = false) {
  if (!drawState.hot || !drawState.hasShaderContext()) {
    return 0;
  }
  const auto& hot = *drawState.hot;
  const auto& shader = drawState.shaderContext();
  auto mix = [](u64 seed, u64 value) {
    seed ^= value + 0x9e3779b97f4a7c15ull + (seed << 6u) + (seed >> 2u);
    return seed;
  };

  u64 hash = shader.vertexShader.hash;
  hash = mix(hash, shader.pixelShader.hash);
  hash = mix(hash, hot.key.vertexDeclHash);
  hash = mix(hash, hot.key.renderStateHash);
  hash = mix(hash, hot.textureMask);
  hash = mix(hash, hot.renderTargetMask);
  hash = mix(hash, hot.clipPlaneMask);
  hash = mix(hash, hot.colorAttachments[0].sampleCount);
  for (const auto textureTypeHash : hot.key.textureStageStateHashes) {
    hash = mix(hash, textureTypeHash);
  }
  for (const auto samplerHash : hot.key.samplerStateHashes) {
    hash = mix(hash, samplerHash);
  }
  if (forceTextureWhiteOverride.has_value()) {
    hash = mix(hash, 0x58f71e9d1a3b4c25ull);
    hash = mix(hash, static_cast<u64>(*forceTextureWhiteOverride));
  }
  if (fragmentlessDepthOnly) {
    hash = mix(hash,
               debug::probeFragmentlessDepthOnlyKeepVSOut()
                   ? 0x9e3b7a6c8fb4c521ull
                   : 0xf1a974f2b7a25c31ull);
  }
  return hash;
}

u32 vsOutLayoutKeyForDraw(core::FlatDrawStateView drawState,
                          bool tileFfpBaseColor,
                          std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                          bool fragmentlessDepthOnly = false) {
  if (!drawState.hot || !drawState.hasShaderContext()) {
    return 0;
  }
  if (fragmentlessDepthOnly &&
      !debug::probeFragmentlessDepthOnlyKeepVSOut()) {
    return shaders::vsoutLayoutKey(shaders::positionOnlyVSOutLayout());
  }
  auto context =
      drawshader::makeShaderSourceContext(drawState.shaderContext(), *drawState.hot);
  context.stripFogAlphaTestForTileBase = tileFfpBaseColor;
  context.stripAlphaTestForDebug = debug::disableAlphaTest();
  context.stripFogForDebug = debug::disableFog();
  context.forceTextureWhiteForDebug =
      forceTextureWhiteOverride.value_or(debug::forceTextureWhite());
  try {
    context.vsOutLayout = drawshader::resolveVSOutLayoutForShaderPair(context);
    return shaders::vsoutLayoutKey(context.vsOutLayout);
  } catch (...) {
    return shaders::vsoutLayoutKey(shaders::fullVSOutLayout());
  }
}

struct ShaderSourceHashes {
  u64 vertex = 0;
  u64 pixel = 0;
};

bool shaderSourceHashAttributionEnabled() {
  static const bool enabled = [] {
    const char* dir = std::getenv("DXMT_DUMP_SHADER_DIR");
    return dir && dir[0] != '\0';
  }();
  return enabled;
}

ShaderSourceHashes shaderSourceHashesForDraw(core::FlatDrawStateView drawState,
                                             bool tileFfpBaseColor,
                                             bool argbufHybridMode,
                                             bool argbufResourceArray,
                                             bool argbufDirectCbufMode,
                                             bool samplerLodBias,
                                             u32 x8AlphaOneTextureMask,
                                             std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                                             bool fragmentlessDepthOnly = false) {
  ShaderSourceHashes hashes{};
  if (!shaderSourceHashAttributionEnabled() || !drawState.hot ||
      !drawState.hasShaderContext()) {
    return hashes;
  }
  auto context =
      drawshader::makeShaderSourceContext(drawState.shaderContext(), *drawState.hot);
  context.stripFogAlphaTestForTileBase = tileFfpBaseColor;
  context.stripAlphaTestForDebug = debug::disableAlphaTest();
  context.stripFogForDebug = debug::disableFog();
  context.forceTextureWhiteForDebug =
      forceTextureWhiteOverride.value_or(debug::forceTextureWhite());
  context.argbufHybridMode = argbufHybridMode;
  context.argbufResourceArray = argbufHybridMode && argbufResourceArray;
  context.argbufDirectCbufMode =
      argbufHybridMode && !context.argbufResourceArray && argbufDirectCbufMode;
  context.samplerLodBias = samplerLodBias;
  context.x8AlphaOneTextureMask = x8AlphaOneTextureMask;
  try {
    if (fragmentlessDepthOnly) {
      context.vsOutLayout = debug::probeFragmentlessDepthOnlyKeepVSOut()
                                 ? drawshader::resolveVSOutLayoutForShaderPair(context)
                                 : shaders::positionOnlyVSOutLayout();
      context.fragmentlessDepthOnly = true;
    } else {
      context.vsOutLayout = drawshader::resolveVSOutLayoutForShaderPair(context);
    }
    const auto vertex = drawshader::makeDrawShaderSource(context, true);
    hashes.vertex = core::hashString(vertex);
    if (!fragmentlessDepthOnly) {
      const auto pixel = drawshader::makeDrawShaderSource(context, false);
      hashes.pixel = core::hashString(pixel);
    }
  } catch (...) {
    hashes = {};
  }
  return hashes;
}

u64 psoHandleBucket(core::PsoHandle handle) noexcept {
  return handle.valid()
             ? (static_cast<u64>(handle.generation) << 32) |
                   static_cast<u64>(handle.slot)
             : 0ull;
}

void recordPsoAttributionForDraw(ActiveEncoderBreakdown* encoderBreakdown,
                                 core::FlatDrawStateView drawState,
                                 const resources::Pool& pool,
                                 core::PsoHandle renderPsoHandle,
                                 bool tileFfpMode,
                                 bool argbufHybridMode,
                                 bool argbufResourceArray,
                                 bool argbufDirectCbufMode,
                                 std::optional<bool> forceTextureWhiteOverride = std::nullopt,
                                 bool fragmentlessDepthOnly = false) {
  if (!encoderBreakdown || !encoderBreakdown->enabled) {
    return;
  }
  const auto variantHash =
      shaderVariantHashForDraw(drawState, &pool, fragmentlessDepthOnly);
  const auto sourceKey =
      shaderSourceAttributionKeyForDraw(drawState, forceTextureWhiteOverride,
                                        fragmentlessDepthOnly);
  u64 vertexShaderHash = 0;
  u64 pixelShaderHash = 0;
  if (drawState.hasShaderContext()) {
    const auto& shader = drawState.shaderContext();
    vertexShaderHash = shader.vertexShader.hash;
    pixelShaderHash = shader.pixelShader.hash;
  }
  u32 vsOutLayoutKey = 0;
  if (!encoderBreakdown->findCachedVsOutLayout(sourceKey, tileFfpMode,
                                               vsOutLayoutKey)) {
    vsOutLayoutKey =
        vsOutLayoutKeyForDraw(drawState, tileFfpMode, forceTextureWhiteOverride,
                              fragmentlessDepthOnly);
    encoderBreakdown->storeCachedVsOutLayout(sourceKey, tileFfpMode,
                                             vsOutLayoutKey);
  }
  const bool samplerLodBias = anySamplerLodBiasNonzero(drawState);
  const u32 x8AlphaOneTextureMask = x8AlphaOneTextureMaskForDraw(drawState, pool);
  u64 vertexShaderSourceHash = 0;
  u64 pixelShaderSourceHash = 0;
  if (!encoderBreakdown->findCachedShaderSourceHashes(
          sourceKey, tileFfpMode, argbufHybridMode,
          argbufHybridMode && argbufResourceArray,
          argbufHybridMode && !argbufResourceArray && argbufDirectCbufMode,
          samplerLodBias,
          x8AlphaOneTextureMask,
          vertexShaderSourceHash, pixelShaderSourceHash)) {
    const auto sourceHashes = shaderSourceHashesForDraw(
        drawState, tileFfpMode, argbufHybridMode, argbufResourceArray,
        argbufDirectCbufMode, samplerLodBias, x8AlphaOneTextureMask,
        forceTextureWhiteOverride, fragmentlessDepthOnly);
    vertexShaderSourceHash = sourceHashes.vertex;
    pixelShaderSourceHash = sourceHashes.pixel;
    encoderBreakdown->storeCachedShaderSourceHashes(
        sourceKey, tileFfpMode, argbufHybridMode,
        argbufHybridMode && argbufResourceArray,
        argbufHybridMode && !argbufResourceArray && argbufDirectCbufMode,
        samplerLodBias,
        x8AlphaOneTextureMask,
        vertexShaderSourceHash, pixelShaderSourceHash);
  }
  encoderBreakdown->recordPsoState(
      psoHandleBucket(renderPsoHandle), variantHash, vsOutLayoutKey,
      vertexShaderHash, pixelShaderHash, vertexShaderSourceHash,
      pixelShaderSourceHash);
}

void countDrawIssue(core::FlatDrawStateView drawState,
                    core::PrimitiveType primitiveType,
                    u32 primitiveCount,
                    u64 vertexCount,
                    bool indexed,
                    bool expandedIndexed,
                    std::size_t userVertexBytes,
                    std::size_t userIndexBytes) {
  if (drawState.hasShaderContext()) {
    const auto& shader = drawState.shaderContext();
    perf::countDrawShaderBucket(shader.vertexShader.hash,
                                shader.pixelShader.hash,
                                shaderVariantHashForDraw(drawState));
  }
  perf::countDrawCall(static_cast<std::uint32_t>(primitiveType),
                      primitiveCount,
                      vertexCount,
                      indexed,
                      expandedIndexed,
                      userVertexBytes,
                      userIndexBytes);
}

std::size_t indexElementSize(IndexType type) {
  return type == IndexType::UInt16 ? 2u : 4u;
}

template <std::size_t Capacity>
u64 estimateVertexCacheMisses(std::span<const u32> indices) {
  std::array<u32, Capacity> cache{};
  std::size_t valid = 0;
  u64 misses = 0;

  for (const u32 index : indices) {
    std::size_t hit = valid;
    for (std::size_t i = 0; i < valid; ++i) {
      if (cache[i] == index) {
        hit = i;
        break;
      }
    }

    if (hit == valid) {
      ++misses;
      hit = std::min(valid, Capacity - 1u);
      if (valid < Capacity) {
        ++valid;
      }
    }

    for (std::size_t i = hit; i > 0; --i) {
      cache[i] = cache[i - 1u];
    }
    cache[0] = index;
  }

  return misses;
}

IndexReuseMeasure measureIndexReuseForDraw(std::span<const u8> indexBytes,
                                           IndexType indexType,
                                           u32 startIndex,
                                           u64 indexCount) {
  IndexReuseMeasure out{.references = indexCount};
  if (indexBytes.empty() || indexCount == 0u) {
    return out;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return out;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return out;
  }

  std::vector<u32> indices;
  indices.reserve(static_cast<std::size_t>(indexCount));
  for (u64 i = 0; i < indexCount; ++i) {
    const std::size_t offset = startByte + static_cast<std::size_t>(i) * elementSize;
    if (indexType == IndexType::UInt16) {
      u16 value = 0;
      std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
      indices.push_back(value);
    } else {
      u32 value = 0;
      std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
      indices.push_back(value);
    }
  }
  if (indices.empty()) {
    return out;
  }
  out.firstIndex = indices.front();
  out.lastIndex = indices.back();
  out.minIndex = std::numeric_limits<u32>::max();
  for (std::size_t i = 0; i < indices.size(); ++i) {
    const u32 index = indices[i];
    out.minIndex = std::min(out.minIndex, index);
    out.maxIndex = std::max(out.maxIndex, index);
    if (i != 0u) {
      const u32 prev = indices[i - 1u];
      const u32 delta = index > prev ? index - prev : prev - index;
      out.adjacentDeltaAbsSum += delta;
      out.adjacentDeltaMax = std::max(out.adjacentDeltaMax, delta);
      if (index < prev) {
        ++out.backwardJumps;
      }
    }
  }
  if ((indices.size() % 3u) == 0u) {
    for (std::size_t i = 0; i < indices.size(); i += 3u) {
      const u32 triMin = std::min({indices[i], indices[i + 1u], indices[i + 2u]});
      const u32 triMax = std::max({indices[i], indices[i + 1u], indices[i + 2u]});
      const u32 span = triMax - triMin + 1u;
      out.triangleIndexSpanSum += span;
      out.triangleIndexSpanMax = std::max(out.triangleIndexSpanMax, span);
    }
  }
  out.cacheMiss16 = estimateVertexCacheMisses<16>(indices);
  out.cacheMiss32 = estimateVertexCacheMisses<32>(indices);
  out.cacheMiss64 = estimateVertexCacheMisses<64>(indices);
  std::sort(indices.begin(), indices.end());
  out.unique = static_cast<u64>(std::unique(indices.begin(), indices.end()) -
                                indices.begin());
  out.available = true;
  return out;
}

IndexReuseMeasure measureIndexCacheMiss32ForDraw(std::span<const u8> indexBytes,
                                                 IndexType indexType,
                                                 u32 startIndex,
                                                 u64 indexCount) {
  IndexReuseMeasure out{.references = indexCount};
  if (indexBytes.empty() || indexCount == 0u) {
    return out;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return out;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return out;
  }

  std::array<u32, 32> cache{};
  std::size_t cacheSize = 0;
  u64 misses = 0;
  for (u64 i = 0; i < indexCount; ++i) {
    const std::size_t offset = startByte + static_cast<std::size_t>(i) * elementSize;
    u32 index = 0;
    if (indexType == IndexType::UInt16) {
      u16 value = 0;
      std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
      index = value;
    } else {
      std::memcpy(&index, indexBytes.data() + offset, sizeof(index));
    }

    std::size_t hit = cacheSize;
    for (std::size_t j = 0; j < cacheSize; ++j) {
      if (cache[j] == index) {
        hit = j;
        break;
      }
    }
    if (hit == cacheSize) {
      ++misses;
      if (cacheSize < cache.size()) {
        ++cacheSize;
      }
      hit = cacheSize - 1u;
    }
    for (std::size_t j = hit; j > 0u; --j) {
      cache[j] = cache[j - 1u];
    }
    cache[0] = index;
  }

  out.cacheMiss32 = misses;
  out.available = true;
  return out;
}

IndexReuseMeasure measureIndexCacheMiss32AndUniqueForDraw(
    std::span<const u8> indexBytes,
    IndexType indexType,
    u32 startIndex,
    u64 indexCount) {
  IndexReuseMeasure out{.references = indexCount};
  if (indexBytes.empty() || indexCount == 0u) {
    return out;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return out;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return out;
  }

  std::array<u32, 32> cache{};
  std::size_t cacheSize = 0;
  u64 misses = 0;
  bool first = true;
  std::array<u8, 65536u> seen16{};
  std::unordered_set<u32> seen32;
  if (indexType == IndexType::UInt32) {
    seen32.reserve(static_cast<std::size_t>(
        std::min<u64>(indexCount, 1024u * 1024u)));
  }

  for (u64 i = 0; i < indexCount; ++i) {
    const std::size_t offset = startByte + static_cast<std::size_t>(i) * elementSize;
    u32 index = 0;
    if (indexType == IndexType::UInt16) {
      u16 value = 0;
      std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
      index = value;
      if (seen16[index] == 0u) {
        seen16[index] = 1u;
        ++out.unique;
      }
    } else {
      std::memcpy(&index, indexBytes.data() + offset, sizeof(index));
      if (seen32.insert(index).second) {
        ++out.unique;
      }
    }

    if (first) {
      out.firstIndex = index;
      out.minIndex = index;
      out.maxIndex = index;
      first = false;
    } else {
      const u32 prev = out.lastIndex;
      const u32 delta = index > prev ? index - prev : prev - index;
      out.adjacentDeltaAbsSum += delta;
      out.adjacentDeltaMax = std::max(out.adjacentDeltaMax, delta);
      if (index < prev) {
        ++out.backwardJumps;
      }
      out.minIndex = std::min(out.minIndex, index);
      out.maxIndex = std::max(out.maxIndex, index);
    }
    out.lastIndex = index;

    std::size_t hit = cacheSize;
    for (std::size_t j = 0; j < cacheSize; ++j) {
      if (cache[j] == index) {
        hit = j;
        break;
      }
    }
    if (hit == cacheSize) {
      ++misses;
      if (cacheSize < cache.size()) {
        ++cacheSize;
      }
      hit = cacheSize - 1u;
    }
    for (std::size_t j = hit; j > 0u; --j) {
      cache[j] = cache[j - 1u];
    }
    cache[0] = index;
  }

  out.cacheMiss32 = misses;
  out.available = true;
  return out;
}

u64 stream0ByteSpanForIndexMeasure(const IndexReuseMeasure& measure,
                                   u64 stream0Stride) {
  if (!measure.available || stream0Stride == 0u) {
    return 0u;
  }
  return static_cast<u64>(measure.maxIndex - measure.minIndex) * stream0Stride;
}

bool indexCacheCandidateMeetsGainGate(const IndexReuseMeasure& original,
                                      const IndexReuseMeasure& candidate,
                                      std::uint32_t minGainPct) {
  if (!original.available || !candidate.available ||
      original.cacheMiss32 == 0u ||
      candidate.cacheMiss32 >= original.cacheMiss32) {
    return false;
  }
  const u64 delta = original.cacheMiss32 - candidate.cacheMiss32;
  return (delta * 100u) / original.cacheMiss32 >= minGainPct;
}

bool indexCacheCandidateCanMeetGainUpperBound(
    const IndexReuseMeasure& original,
    std::uint32_t minGainPct) {
  if (!original.available || original.cacheMiss32 == 0u || original.unique == 0u) {
    return true;
  }
  if (original.unique >= original.cacheMiss32) {
    return false;
  }
  const u64 maxDelta = original.cacheMiss32 - original.unique;
  return (maxDelta * 100u) / original.cacheMiss32 >= minGainPct;
}

bool writeBinaryFile(const std::filesystem::path& path,
                     const u8* data,
                     std::size_t size) {
  if (!data && size != 0u) {
    return false;
  }
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  if (size != 0u) {
    out.write(reinterpret_cast<const char*>(data),
              static_cast<std::streamsize>(size));
  }
  return out.good();
}

bool writeTextFile(const std::filesystem::path& path, const std::string& text) {
  std::ofstream out(path, std::ios::binary);
  if (!out) {
    return false;
  }
  out << text;
  return out.good();
}

bool indexedGeometryDumpShaderMatches(core::FlatDrawStateView drawState) {
  const auto vertexFilter = debug::indexedGeometryDumpVertexShaderHash();
  const auto pixelFilter = debug::indexedGeometryDumpPixelShaderHash();
  if (!vertexFilter.has_value() && !pixelFilter.has_value()) {
    return true;
  }
  if (!drawState.hasShaderContext()) {
    return false;
  }
  const auto& shader = drawState.shaderContext();
  return (!vertexFilter.has_value() || shader.vertexShader.hash == *vertexFilter) &&
         (!pixelFilter.has_value() || shader.pixelShader.hash == *pixelFilter);
}

bool texture0FilterMatches(core::FlatDrawStateView drawState,
                           const resources::Pool& pool,
                           std::optional<u64> texture0Filter,
                           std::optional<u64> texture0WidthFilter,
                           std::optional<u64> texture0HeightFilter,
                           std::optional<u64> texture0FormatFilter) {
  if (!texture0Filter.has_value() &&
      !texture0WidthFilter.has_value() &&
      !texture0HeightFilter.has_value() &&
      !texture0FormatFilter.has_value()) {
    return true;
  }
  if (!drawState.hot || !drawState.hot->textures[0]) {
    return false;
  }
  const auto handle = drawState.hot->textures[0].value;
  if (texture0Filter.has_value() && handle != *texture0Filter) {
    return false;
  }
  if (!texture0WidthFilter.has_value() &&
      !texture0HeightFilter.has_value() &&
      !texture0FormatFilter.has_value()) {
    return true;
  }
  const auto* texture = pool.findTexture(handle);
  if (!texture) {
    return false;
  }
  if (texture0WidthFilter.has_value() &&
      texture->desc.width != *texture0WidthFilter) {
    return false;
  }
  if (texture0HeightFilter.has_value() &&
      texture->desc.height != *texture0HeightFilter) {
    return false;
  }
  if (texture0FormatFilter.has_value() &&
      static_cast<u64>(texture->desc.format) != *texture0FormatFilter) {
    return false;
  }
  return true;
}

bool indexedGeometryDumpTextureMatches(core::FlatDrawStateView drawState,
                                       const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::indexedGeometryDumpTexture0Handle(),
                               debug::indexedGeometryDumpTexture0Width(),
                               debug::indexedGeometryDumpTexture0Height(),
                               debug::indexedGeometryDumpTexture0Format());
}

bool depthFuncAlwaysProbeTextureMatches(core::FlatDrawStateView drawState,
                                        const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::probeDepthFuncAlwaysTexture0Handle(),
                               debug::probeDepthFuncAlwaysTexture0Width(),
                               debug::probeDepthFuncAlwaysTexture0Height(),
                               debug::probeDepthFuncAlwaysTexture0Format());
}

bool forceTextureWhiteProbeTextureMatches(core::FlatDrawStateView drawState,
                                          const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::probeForceTextureWhiteTexture0Handle(),
                               debug::probeForceTextureWhiteTexture0Width(),
                               debug::probeForceTextureWhiteTexture0Height(),
                               debug::probeForceTextureWhiteTexture0Format());
}

bool forceTextureWhiteProbeDrawOrdinalMatches(u64 drawOrdinal) {
  const auto range = debug::probeForceTextureWhiteDrawOrdinalRange();
  const auto list = debug::probeForceTextureWhiteDrawOrdinalList();
  if (!debug::drawOrdinalRangeEnabled(range) && !list.enabled) {
    return true;
  }
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(drawOrdinal, range)) {
    return false;
  }
  return !list.enabled || debug::drawOrdinalListContains(list, drawOrdinal);
}

bool forceTextureWhiteProbeCommandIndexMatches(std::uint32_t commandIndex) {
  const auto range = debug::probeForceTextureWhiteCommandIndexRange();
  const auto list = debug::probeForceTextureWhiteCommandIndexList();
  const auto ordinal = static_cast<u64>(commandIndex);
  if (!debug::drawOrdinalRangeEnabled(range) && !list.enabled) {
    return true;
  }
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(ordinal, range)) {
    return false;
  }
  return !list.enabled || debug::drawOrdinalListContains(list, ordinal);
}

bool forceTextureWhiteProbeCommandDrawIndexMatches(u64 commandDrawIndex) {
  const auto range = debug::probeForceTextureWhiteCommandDrawIndexRange();
  const auto list = debug::probeForceTextureWhiteCommandDrawIndexList();
  if (!debug::drawOrdinalRangeEnabled(range) && !list.enabled) {
    return true;
  }
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(commandDrawIndex, range)) {
    return false;
  }
  return !list.enabled || debug::drawOrdinalListContains(list, commandDrawIndex);
}

bool disableAlphaBlendProbeTextureMatches(core::FlatDrawStateView drawState,
                                          const resources::Pool& pool) {
  return texture0FilterMatches(drawState,
                               pool,
                               debug::probeDisableAlphaBlendTexture0Handle(),
                               debug::probeDisableAlphaBlendTexture0Width(),
                               debug::probeDisableAlphaBlendTexture0Height(),
                               debug::probeDisableAlphaBlendTexture0Format());
}

struct IndexedGeometryStreamPayload {
  u32 stream = 0;
  u32 metalSlot = 0;
  u64 handle = 0;
  u64 offset = 0;
  u64 stride = 0;
  std::span<const u8> bytes{};
};

void appendIndexedGeometryTextureMetadata(std::ostringstream& meta,
                                          core::FlatDrawStateView drawState,
                                          const resources::Pool& pool) {
  if (!drawState.hot) {
    return;
  }
  const auto& hot = *drawState.hot;
  meta << "texture_mask=0x" << std::hex << hot.textureMask << std::dec << "\n";
  for (u32 stage = 0; stage < core::kMaxTextureStages; ++stage) {
    const auto handle = hot.textures[stage];
    if (!handle) {
      continue;
    }
    meta << "texture" << stage << "_handle=0x" << std::hex << handle.value
         << std::dec << "\n"
         << "texture" << stage << "_lod=" << hot.textureLods[stage] << "\n";
    if (const auto* texture = pool.findTexture(handle.value)) {
      meta << "texture" << stage << "_format="
           << static_cast<unsigned>(texture->desc.format) << "\n"
           << "texture" << stage << "_type="
           << static_cast<unsigned>(texture->desc.type) << "\n"
           << "texture" << stage << "_pool="
           << static_cast<unsigned>(texture->desc.pool) << "\n"
           << "texture" << stage << "_usage=0x" << std::hex
           << texture->desc.usage << std::dec << "\n"
           << "texture" << stage << "_width=" << texture->desc.width << "\n"
           << "texture" << stage << "_height=" << texture->desc.height << "\n"
           << "texture" << stage << "_depth=" << texture->desc.depth << "\n"
           << "texture" << stage << "_levels=" << texture->desc.levels << "\n"
           << "texture" << stage << "_has_metal_texture="
           << (texture->texture ? 1 : 0) << "\n"
           << "texture" << stage << "_has_shader_read_texture="
           << (texture->shaderReadTexture ? 1 : 0) << "\n"
           << "texture" << stage << "_has_srgb_shader_read_texture="
           << (texture->srgbShaderReadTexture ? 1 : 0) << "\n";
    } else {
      meta << "texture" << stage << "_missing_record=1\n";
    }
  }
}

void appendIndexedGeometryAttachmentMetadata(std::ostringstream& meta,
                                             core::FlatDrawStateView drawState,
                                             const resources::Pool& pool) {
  if (!drawState.hot) {
    return;
  }
  const auto& hot = *drawState.hot;
  auto appendSurface = [&](std::string_view prefix,
                           core::RenderTargetAttachment attachment) {
    const auto handle = attachment.handle;
    meta << prefix << "_handle=0x" << std::hex << handle.value << std::dec << "\n"
         << prefix << "_level=" << attachment.level << "\n"
         << prefix << "_sample_count=" << attachment.sampleCount << "\n";
    if (!handle) {
      return;
    }
    const auto* surface = pool.findSurface(handle.value);
    if (!surface) {
      meta << prefix << "_missing_surface=1\n";
      return;
    }
    meta << prefix << "_format=" << static_cast<unsigned>(surface->desc.format) << "\n"
         << prefix << "_pool=" << static_cast<unsigned>(surface->desc.pool) << "\n"
         << prefix << "_usage=0x" << std::hex << surface->desc.usage << std::dec << "\n"
         << prefix << "_width=" << surface->desc.width << "\n"
         << prefix << "_height=" << surface->desc.height << "\n"
         << prefix << "_bytes_per_pixel=" << core::bytesPerPixel(surface->desc.format) << "\n"
         << prefix << "_render_target=" << (surface->desc.renderTarget ? 1 : 0) << "\n"
         << prefix << "_depth_stencil=" << (surface->desc.depthStencil ? 1 : 0) << "\n"
         << prefix << "_has_metal_texture=" << (surface->texture ? 1 : 0) << "\n"
         << prefix << "_has_srgb_texture=" << (surface->srgbTexture ? 1 : 0) << "\n"
         << prefix << "_has_resolve_texture=" << (surface->resolveTexture ? 1 : 0) << "\n"
         << prefix << "_alias_texture=0x" << std::hex << surface->aliasTexture.value
         << std::dec << "\n"
         << prefix << "_alias_level=" << surface->level << "\n"
         << prefix << "_alias_slice=" << surface->slice << "\n";
    if (surface->aliasTexture) {
      const auto* texture = pool.findTexture(surface->aliasTexture.value);
      if (texture) {
        meta << prefix << "_alias_texture_format="
             << static_cast<unsigned>(texture->desc.format) << "\n"
             << prefix << "_alias_texture_type="
             << static_cast<unsigned>(texture->desc.type) << "\n"
             << prefix << "_alias_texture_usage=0x" << std::hex
             << texture->desc.usage << std::dec << "\n"
             << prefix << "_alias_texture_width=" << texture->desc.width << "\n"
             << prefix << "_alias_texture_height=" << texture->desc.height << "\n"
             << prefix << "_alias_texture_levels=" << texture->desc.levels << "\n";
      }
    }
  };

  for (u32 index = 0; index < core::kMaxRenderTargets; ++index) {
    std::ostringstream prefix;
    prefix << "attachment_color" << index;
    appendSurface(prefix.str(), hot.colorAttachments[index]);
  }
  appendSurface("attachment_depth", hot.depthStencil);
}

void maybeDumpIndexedGeometryPayload(
    const ActiveEncoderBreakdown* encoderBreakdown,
    core::FlatDrawStateView drawState,
    const resources::Pool& pool,
    std::span<const u8> indexBytes,
    std::span<const u8> vertexBytes,
    const IndexReuseMeasure& indexReuse,
    IndexType indexType,
    u32 startIndex,
    u64 indexCount,
    i32 baseVertexIndex,
    u64 stream0Offset,
    u64 stream0Stride,
    u64 stream0Handle,
    u64 indexBufferHandle,
    u64 vertexShaderHash,
    u64 pixelShaderHash,
    u64 drawOrdinal,
    u64 primitiveCount,
    std::span<const IndexedGeometryStreamPayload> extraStreams = {},
    std::span<const u8> vsConstsBytes = {},
    std::span<const u8> psConstsBytes = {},
    std::span<const u8> ffpVsConstsBytes = {},
    std::span<const u8> ffpPsConstsBytes = {}) {
  const auto dir = debug::indexedGeometryDumpDir();
  if (dir.empty() || !encoderBreakdown || !indexReuse.available) {
    return;
  }
  const std::uint32_t maxDumps = debug::indexedGeometryDumpMaxDraws();
  if (maxDumps == 0u) {
    return;
  }

  std::error_code ec;
  const std::filesystem::path outDir{std::string(dir)};
  std::filesystem::create_directories(outDir, ec);
  if (ec) {
    return;
  }

  const std::size_t indexSize = indexElementSize(indexType);
  const u64 indexStartByte64 = static_cast<u64>(startIndex) * indexSize;
  const u64 indexByteCount64 = indexCount * indexSize;
  bool indexRangeValid =
      indexStartByte64 <= static_cast<u64>(indexBytes.size()) &&
      indexByteCount64 <= static_cast<u64>(indexBytes.size()) - indexStartByte64;
  const std::size_t indexStartByte =
      indexRangeValid ? static_cast<std::size_t>(indexStartByte64) : 0u;
  const std::size_t indexByteCount =
      indexRangeValid ? static_cast<std::size_t>(indexByteCount64) : 0u;

  bool streamRangeValid = false;
  std::size_t streamStartByte = 0u;
  std::size_t streamByteCount = 0u;
  const auto minVertex =
      static_cast<std::int64_t>(baseVertexIndex) +
      static_cast<std::int64_t>(indexReuse.minIndex);
  const auto maxVertex =
      static_cast<std::int64_t>(baseVertexIndex) +
      static_cast<std::int64_t>(indexReuse.maxIndex);
  if (stream0Stride != 0u && minVertex >= 0 && maxVertex >= minVertex) {
    const u64 minVertex64 = static_cast<u64>(minVertex);
    const u64 vertexSpan = static_cast<u64>(maxVertex - minVertex + 1);
    const u64 streamStart64 = stream0Offset + minVertex64 * stream0Stride;
    const u64 streamCount64 = vertexSpan * stream0Stride;
    streamRangeValid =
        streamStart64 <= static_cast<u64>(vertexBytes.size()) &&
        streamCount64 <= static_cast<u64>(vertexBytes.size()) - streamStart64;
    if (streamRangeValid) {
      streamStartByte = static_cast<std::size_t>(streamStart64);
      streamByteCount = static_cast<std::size_t>(streamCount64);
    }
  }
  if (!indexRangeValid || !streamRangeValid) {
    return;
  }

  static std::atomic<std::uint32_t> dumpCount{0u};
  std::uint32_t slot = dumpCount.load(std::memory_order_relaxed);
  while (slot < maxDumps &&
         !dumpCount.compare_exchange_weak(slot,
                                          slot + 1u,
                                          std::memory_order_relaxed)) {
  }
  if (slot >= maxDumps) {
    return;
  }

  const auto seqId = encoderBreakdown->stats.seqId;
  const auto encoderIndex = encoderBreakdown->stats.encoderIndex;
  std::ostringstream stem;
  stem << "seq" << seqId
       << "-enc" << encoderIndex
       << "-draw" << drawOrdinal
       << "-slot" << slot;
  const auto base = outDir / stem.str();

  const bool wroteIndex =
      indexRangeValid &&
      writeBinaryFile(base.string() + ".index.bin",
                      indexBytes.data() + indexStartByte,
                      indexByteCount);
  const bool wroteStream =
      streamRangeValid &&
      writeBinaryFile(base.string() + ".stream0.bin",
                      vertexBytes.data() + streamStartByte,
                      streamByteCount);
  struct ExtraStreamDumpResult {
    const IndexedGeometryStreamPayload* payload = nullptr;
    bool rangeValid = false;
    std::size_t startByte = 0u;
    std::size_t byteCount = 0u;
    bool wrote = false;
  };
  std::array<ExtraStreamDumpResult, core::kMaxStreams - 1u> extraResults{};
  std::size_t extraResultCount = 0u;
  for (const auto& stream : extraStreams) {
    if (stream.stream == 0u || stream.stream >= core::kMaxStreams ||
        stream.stride == 0u || stream.bytes.empty() ||
        extraResultCount >= extraResults.size()) {
      continue;
    }
    auto& result = extraResults[extraResultCount++];
    result.payload = &stream;
    const u64 minVertex64 = static_cast<u64>(minVertex);
    const u64 vertexSpan = static_cast<u64>(maxVertex - minVertex + 1);
    const u64 streamStart64 = stream.offset + minVertex64 * stream.stride;
    const u64 streamCount64 = vertexSpan * stream.stride;
    result.rangeValid =
        streamStart64 <= static_cast<u64>(stream.bytes.size()) &&
        streamCount64 <= static_cast<u64>(stream.bytes.size()) - streamStart64;
    if (result.rangeValid) {
      result.startByte = static_cast<std::size_t>(streamStart64);
      result.byteCount = static_cast<std::size_t>(streamCount64);
      std::ostringstream suffix;
      suffix << ".stream" << stream.stream << ".bin";
      result.wrote = writeBinaryFile(base.string() + suffix.str(),
                                     stream.bytes.data() + result.startByte,
                                     result.byteCount);
    }
  }
  const bool wroteVsConsts =
      !vsConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".vsconsts.bin",
                      vsConstsBytes.data(), vsConstsBytes.size());
  const bool wrotePsConsts =
      !psConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".psconsts.bin",
                      psConstsBytes.data(), psConstsBytes.size());
  const bool wroteFfpVsConsts =
      !ffpVsConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".ffpvs.bin",
                      ffpVsConstsBytes.data(), ffpVsConstsBytes.size());
  const bool wroteFfpPsConsts =
      !ffpPsConstsBytes.empty() &&
      writeBinaryFile(base.string() + ".ffpps.bin",
                      ffpPsConstsBytes.data(), ffpPsConstsBytes.size());

  std::ostringstream meta;
  meta << "seq=" << seqId << "\n"
       << "encoder=" << encoderIndex << "\n"
       << "encoder_draw_index=" << encoderBreakdown->stats.drawCalls << "\n"
       << "draw_ordinal=" << drawOrdinal << "\n"
       << "slot=" << slot << "\n"
       << "primitive_count=" << primitiveCount << "\n"
       << "index_count=" << indexCount << "\n"
       << "index_type=" << (indexType == IndexType::UInt16 ? "uint16" : "uint32")
       << "\n"
       << "start_index=" << startIndex << "\n"
       << "base_vertex=" << baseVertexIndex << "\n"
       << "index_buffer=0x" << std::hex << indexBufferHandle << std::dec << "\n"
       << "stream0_handle=0x" << std::hex << stream0Handle << std::dec << "\n"
       << "vs=0x" << std::hex << vertexShaderHash << std::dec << "\n"
       << "ps=0x" << std::hex << pixelShaderHash << std::dec << "\n"
       << "stream0_offset=" << stream0Offset << "\n"
       << "stream0_stride=" << stream0Stride << "\n"
       << "min_index=" << indexReuse.minIndex << "\n"
       << "max_index=" << indexReuse.maxIndex << "\n"
       << "unique_indices=" << indexReuse.unique << "\n"
       << "cache_miss_64=" << indexReuse.cacheMiss64 << "\n"
       << "index_start_byte=" << indexStartByte64 << "\n"
       << "index_byte_count=" << indexByteCount64 << "\n"
       << "index_range_valid=" << (indexRangeValid ? 1 : 0) << "\n"
       << "stream0_start_byte=" << streamStartByte << "\n"
       << "stream0_byte_count=" << streamByteCount << "\n"
       << "stream0_range_valid=" << (streamRangeValid ? 1 : 0) << "\n"
       << "wrote_index=" << (wroteIndex ? 1 : 0) << "\n"
       << "wrote_stream0=" << (wroteStream ? 1 : 0) << "\n"
       << "stream_payload_count=" << (1u + extraResultCount) << "\n"
       << "vsconsts_byte_count=" << vsConstsBytes.size() << "\n"
       << "psconsts_byte_count=" << psConstsBytes.size() << "\n"
       << "ffpvs_byte_count=" << ffpVsConstsBytes.size() << "\n"
       << "ffpps_byte_count=" << ffpPsConstsBytes.size() << "\n"
       << "wrote_vsconsts=" << (wroteVsConsts ? 1 : 0) << "\n"
       << "wrote_psconsts=" << (wrotePsConsts ? 1 : 0) << "\n"
       << "wrote_ffpvs=" << (wroteFfpVsConsts ? 1 : 0) << "\n"
       << "wrote_ffpps=" << (wroteFfpPsConsts ? 1 : 0) << "\n";
  if (drawState.hasShaderContext()) {
    const auto& vertexDecl = drawState.shaderContext().vertexDecl;
    meta << "vertex_decl_fvf=0x" << std::hex << vertexDecl.fvf << std::dec << "\n"
         << "vertex_decl_hash=0x"
         << std::hex
         << (drawState.hot ? drawState.hot->key.vertexDeclHash : 0ull)
         << std::dec << "\n"
         << "vertex_decl_element_count=" << vertexDecl.elements.size() << "\n";
    for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
      const auto& element = vertexDecl.elements[i];
      meta << "vertex_decl_element" << i
           << "_stream=" << element.stream << "\n"
           << "vertex_decl_element" << i
           << "_offset=" << element.offset << "\n"
           << "vertex_decl_element" << i
           << "_type=" << element.type << "\n"
           << "vertex_decl_element" << i
           << "_method=" << element.method << "\n"
           << "vertex_decl_element" << i
           << "_usage=" << element.usage << "\n"
           << "vertex_decl_element" << i
           << "_usage_index=" << element.usageIndex << "\n";
    }
    for (std::size_t stream = 0; stream < vertexDecl.streams.size(); ++stream) {
      const auto& binding = vertexDecl.streams[stream];
      const auto computedStride =
          computeVertexDeclStreamStride(vertexDecl, static_cast<u32>(stream));
      if (!binding.buffer && binding.offset == 0u &&
          binding.stride == 0u && computedStride == 0u) {
        continue;
      }
      meta << "vertex_decl_stream" << stream
           << "_has_buffer=" << (binding.buffer ? 1 : 0) << "\n"
           << "vertex_decl_stream" << stream
           << "_offset=" << binding.offset << "\n"
           << "vertex_decl_stream" << stream
           << "_stride=" << binding.stride << "\n"
           << "vertex_decl_stream" << stream
           << "_computed_stride=" << computedStride << "\n";
      if (drawState.hot) {
        meta << "hot_stream" << stream
             << "_handle=0x" << std::hex
             << drawState.hot->streamBuffers[stream].value
             << std::dec << "\n"
             << "hot_stream" << stream
             << "_offset=" << drawState.hot->streamOffsets[stream] << "\n"
             << "hot_stream" << stream
             << "_stride=" << drawState.hot->streamStrides[stream] << "\n";
      }
    }
  }
  for (std::size_t i = 0; i < extraResultCount; ++i) {
    const auto& result = extraResults[i];
    const auto& stream = *result.payload;
    meta << "stream" << stream.stream << "_handle=0x" << std::hex
         << stream.handle << std::dec << "\n"
         << "stream" << stream.stream << "_metal_slot=" << stream.metalSlot << "\n"
         << "stream" << stream.stream << "_offset=" << stream.offset << "\n"
         << "stream" << stream.stream << "_stride=" << stream.stride << "\n"
         << "stream" << stream.stream << "_start_byte="
         << (result.rangeValid ? result.startByte : 0u) << "\n"
         << "stream" << stream.stream << "_byte_count="
         << (result.rangeValid ? result.byteCount : 0u) << "\n"
         << "stream" << stream.stream << "_range_valid="
         << (result.rangeValid ? 1 : 0) << "\n"
         << "wrote_stream" << stream.stream << "=" << (result.wrote ? 1 : 0)
         << "\n";
  }
  appendIndexedGeometryTextureMetadata(meta, drawState, pool);
  appendIndexedGeometryAttachmentMetadata(meta, drawState, pool);
  writeTextFile(base.string() + ".meta", meta.str());
}

struct IndexedDrawChunk {
  u32 startPrimitive = 0;
  u32 primitiveCount = 0;
  u64 stream0Span = 0;
};

std::optional<u32> readIndexValue(std::span<const u8> indexBytes,
                                  IndexType indexType,
                                  std::size_t elementIndex) {
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t offset = elementIndex * elementSize;
  if (offset + elementSize > indexBytes.size()) {
    return std::nullopt;
  }
  if (indexType == IndexType::UInt16) {
    u16 value = 0;
    std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
    return value;
  }
  u32 value = 0;
  std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
  return value;
}

u32 effectTraceFloatComponentCount(u32 declType) {
  switch (declType) {
    case kD3DDeclTypeFloat1:
      return 1u;
    case kD3DDeclTypeFloat2:
      return 2u;
    case kD3DDeclTypeFloat3:
      return 3u;
    case kD3DDeclTypeFloat4:
      return 4u;
    default:
      return 0u;
  }
}

struct EffectTraceStream0Layout {
  bool hasPosition = false;
  bool preTransformed = false;
  u32 positionOffset = 0;
  u32 positionComponents = 0;
  bool hasDiffuse = false;
  u32 diffuseOffset = 0;
  bool hasTexcoord0 = false;
  u32 texcoord0Offset = 0;
  u32 texcoord0Components = 0;
};

EffectTraceStream0Layout resolveEffectTraceStream0Layout(
    core::FlatDrawStateView drawState) {
  EffectTraceStream0Layout out{};
  if (!drawState.hasShaderContext()) {
    return out;
  }

  const auto& vertexDecl = drawState.shaderContext().vertexDecl;
  for (const auto& element : vertexDecl.elements) {
    if (element.stream != 0u) {
      continue;
    }
    const u32 floatComponents = effectTraceFloatComponentCount(element.type);
    if ((element.usage == kD3DDeclUsagePosition ||
         element.usage == kD3DDeclUsagePositionT) &&
        element.usageIndex == 0u && floatComponents >= 2u) {
      if (!out.hasPosition || element.usage == kD3DDeclUsagePositionT) {
        out.hasPosition = true;
        out.preTransformed = element.usage == kD3DDeclUsagePositionT;
        out.positionOffset = element.offset;
        out.positionComponents = floatComponents;
      }
    } else if (!out.hasDiffuse &&
               element.usage == kD3DDeclUsageColor &&
               element.usageIndex == 0u &&
               element.type == kD3DDeclTypeD3DColor) {
      out.hasDiffuse = true;
      out.diffuseOffset = element.offset;
    } else if (!out.hasTexcoord0 &&
               element.usage == kD3DDeclUsageTexcoord &&
               element.usageIndex == 0u &&
               floatComponents != 0u) {
      out.hasTexcoord0 = true;
      out.texcoord0Offset = element.offset;
      out.texcoord0Components = floatComponents;
    }
  }
  return out;
}

void traceEffectIndexedGeometry(const ActiveEncoderBreakdown* encoderBreakdown,
                                core::FlatDrawStateView drawState,
                                const resources::Pool& pool,
                                std::span<const u8> indexBytes,
                                std::span<const u8> vertexBytes,
                                IndexType indexType,
                                u32 startIndex,
                                u64 indexCount,
                                i32 baseVertexIndex,
                                u64 stream0Offset,
                                u64 stream0Stride,
                                u64 stream0Handle,
                                u64 indexBufferHandle,
                                u64 seqId,
                                u64 drawOrdinal,
                                std::uint32_t commandIndex,
                                u64 commandDrawIndex,
                                u64 commandDrawCount,
                                core::PrimitiveType primitiveType,
                                u32 primitiveCount,
                                bool fixedFunctionPath,
                                bool preTransformed,
                                u64 vertexShaderHash,
                                u64 pixelShaderHash) {
  if (!effectDrawTraceEnabled() || !effectDrawTraceGeometryEnabled() ||
      !drawState.hot || indexBytes.empty() || vertexBytes.empty() ||
      stream0Stride == 0u) {
    return;
  }

  const auto& hot = *drawState.hot;
  const auto encoderIndex = encoderBreakdown ? encoderBreakdown->stats.encoderIndex : 0ull;
  if (!effectDrawTraceSeqMatches(seqId)) {
    return;
  }
  if (const auto encFilter = effectDrawTraceEncoderFilter();
      encFilter.has_value() && *encFilter != encoderIndex) {
    return;
  }
  const bool alphaBlendEnabled =
      core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  if (!alphaBlendEnabled && !effectDrawTraceIncludeNonAlpha()) {
    return;
  }
  if (hot.textureMask == 0u && !effectDrawTraceIncludeUntextured()) {
    return;
  }
  const bool pointSpriteState =
      core::flatStateOr(hot.renderStates, RS_POINT_SPRITE_ENABLE, 0u) != 0u;
  const bool pointSpriteCandidate =
      pointSpriteState && primitiveType == core::PrimitiveType::PointList;
  if (effectDrawTracePointSpriteOnly() && !pointSpriteCandidate) {
    return;
  }
  if (const auto primitiveTypeFilter = effectDrawTracePrimitiveTypeFilter();
      primitiveTypeFilter.has_value() &&
      static_cast<u64>(primitiveType) != *primitiveTypeFilter) {
    return;
  }

  const resources::TextureRecord* texture0 =
      hot.textures[0] ? pool.findTexture(hot.textures[0].value) : nullptr;
  if (const auto handleFilter = effectDrawTraceTexture0Filter();
      handleFilter.has_value() &&
      (!hot.textures[0] || hot.textures[0].value != *handleFilter)) {
    return;
  }
  if (const auto widthFilter = effectDrawTraceTexture0WidthFilter();
      widthFilter.has_value() && (!texture0 || texture0->desc.width != *widthFilter)) {
    return;
  }
  if (const auto heightFilter = effectDrawTraceTexture0HeightFilter();
      heightFilter.has_value() && (!texture0 || texture0->desc.height != *heightFilter)) {
    return;
  }
  if (const auto formatFilter = effectDrawTraceTexture0FormatFilter();
      formatFilter.has_value() &&
      (!texture0 || static_cast<u64>(texture0->desc.format) != *formatFilter)) {
    return;
  }

  const auto layout = resolveEffectTraceStream0Layout(drawState);
  const auto encoderDrawIndex =
      encoderBreakdown ? encoderBreakdown->stats.drawCalls + 1ull : 0ull;
  if (!layout.hasPosition) {
    std::ostringstream out;
    out << "[dxmt9-effect-geometry"
        << " seq=" << static_cast<unsigned long long>(seqId)
        << " encoder=" << static_cast<unsigned long long>(encoderIndex)
        << " encoder_draw_index=" << static_cast<unsigned long long>(encoderDrawIndex)
        << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
        << " command_index=" << commandIndex
        << " command_draw_index=" << static_cast<unsigned long long>(commandDrawIndex)
        << " command_draw_count=" << static_cast<unsigned long long>(commandDrawCount)
        << " layout=missing-position"
        << " primitive_type=" << static_cast<unsigned>(primitiveType)
        << " primitive_count=" << primitiveCount
        << " index_count=" << static_cast<unsigned long long>(indexCount)
        << " texture0=0x" << std::hex << hot.textures[0].value
        << " vs_hash=0x" << vertexShaderHash
        << " ps_hash=0x" << pixelShaderHash
        << std::dec << ']';
    emitQueueTraceLine(out.str());
    return;
  }

  auto readF32 = [&](u64 absoluteOffset) -> std::optional<float> {
    if (absoluteOffset > static_cast<u64>(vertexBytes.size()) ||
        sizeof(float) > static_cast<u64>(vertexBytes.size()) - absoluteOffset) {
      return std::nullopt;
    }
    float value = 0.0f;
    std::memcpy(&value,
                vertexBytes.data() + static_cast<std::size_t>(absoluteOffset),
                sizeof(value));
    return value;
  };
  auto readU32 = [&](u64 absoluteOffset) -> std::optional<u32> {
    if (absoluteOffset > static_cast<u64>(vertexBytes.size()) ||
        sizeof(u32) > static_cast<u64>(vertexBytes.size()) - absoluteOffset) {
      return std::nullopt;
    }
    u32 value = 0;
    std::memcpy(&value,
                vertexBytes.data() + static_cast<std::size_t>(absoluteOffset),
                sizeof(value));
    return value;
  };

  const u64 maxRefs = effectDrawTraceGeometryMaxRefs();
  const u64 refsToSample = std::min(indexCount, maxRefs);
  u64 readableIndices = 0;
  u64 validVertices = 0;
  u64 invalidRefs = 0;
  float minX = std::numeric_limits<float>::infinity();
  float minY = std::numeric_limits<float>::infinity();
  float minZ = std::numeric_limits<float>::infinity();
  float minW = std::numeric_limits<float>::infinity();
  float maxX = -std::numeric_limits<float>::infinity();
  float maxY = -std::numeric_limits<float>::infinity();
  float maxZ = -std::numeric_limits<float>::infinity();
  float maxW = -std::numeric_limits<float>::infinity();
  float minU = std::numeric_limits<float>::infinity();
  float minV = std::numeric_limits<float>::infinity();
  float maxU = -std::numeric_limits<float>::infinity();
  float maxV = -std::numeric_limits<float>::infinity();
  u32 minAlpha = std::numeric_limits<u32>::max();
  u32 maxAlpha = 0u;
  bool hasUv = false;
  bool hasAlpha = false;
  const bool canProject =
      drawState.hasUniformPayload() &&
      !fixedFunctionPath &&
      !(preTransformed || layout.preTransformed);
  std::optional<VsConsts> projectVsConsts;
  std::optional<PsConsts> projectPsConsts;
  std::optional<FfpVsConsts> projectFfpVsConsts;
  if (canProject) {
    projectVsConsts = buildVsConsts(drawState);
    projectPsConsts = buildPsConsts(drawState);
    projectFfpVsConsts = buildFfpVsConsts(drawState);
  }
  u64 projectedRefs = 0;
  u64 projectedVisibleRefs = 0;
  u64 projectedInsideXyRefs = 0;
  u64 projectedZInsideRefs = 0;
  u64 projectedBehindRefs = 0;
  float minClipX = std::numeric_limits<float>::infinity();
  float minClipY = std::numeric_limits<float>::infinity();
  float minClipZ = std::numeric_limits<float>::infinity();
  float minClipW = std::numeric_limits<float>::infinity();
  float maxClipX = -std::numeric_limits<float>::infinity();
  float maxClipY = -std::numeric_limits<float>::infinity();
  float maxClipZ = -std::numeric_limits<float>::infinity();
  float maxClipW = -std::numeric_limits<float>::infinity();
  float minNdcX = std::numeric_limits<float>::infinity();
  float minNdcY = std::numeric_limits<float>::infinity();
  float minNdcZ = std::numeric_limits<float>::infinity();
  float maxNdcX = -std::numeric_limits<float>::infinity();
  float maxNdcY = -std::numeric_limits<float>::infinity();
  float maxNdcZ = -std::numeric_limits<float>::infinity();
  float minScreenX = std::numeric_limits<float>::infinity();
  float minScreenY = std::numeric_limits<float>::infinity();
  float maxScreenX = -std::numeric_limits<float>::infinity();
  float maxScreenY = -std::numeric_limits<float>::infinity();
  std::ostringstream refs;
  refs << std::setprecision(6);
  u32 refsLogged = 0u;

  for (u64 i = 0; i < refsToSample; ++i) {
    const auto indexValue = readIndexValue(
        indexBytes, indexType, static_cast<std::size_t>(startIndex) +
                                   static_cast<std::size_t>(i));
    if (!indexValue.has_value()) {
      ++invalidRefs;
      continue;
    }
    ++readableIndices;
    const auto effectiveVertex =
        static_cast<std::int64_t>(baseVertexIndex) +
        static_cast<std::int64_t>(*indexValue);
    if (effectiveVertex < 0) {
      ++invalidRefs;
      continue;
    }
    const auto vertexIndex64 = static_cast<u64>(effectiveVertex);
    if (vertexIndex64 >
        (std::numeric_limits<u64>::max() - stream0Offset) / stream0Stride) {
      ++invalidRefs;
      continue;
    }
    const u64 vertexBase = stream0Offset + vertexIndex64 * stream0Stride;
    const auto x = readF32(vertexBase + layout.positionOffset);
    const auto y = readF32(vertexBase + layout.positionOffset + sizeof(float));
    if (!x.has_value() || !y.has_value() ||
        !std::isfinite(*x) || !std::isfinite(*y)) {
      ++invalidRefs;
      continue;
    }
    const auto zValue = layout.positionComponents >= 3u
        ? readF32(vertexBase + layout.positionOffset + sizeof(float) * 2u)
        : std::optional<float>(0.0f);
    const auto wValue = layout.positionComponents >= 4u
        ? readF32(vertexBase + layout.positionOffset + sizeof(float) * 3u)
        : std::optional<float>(1.0f);
    const float z = zValue.value_or(0.0f);
    const float w = wValue.value_or(1.0f);
    if (!std::isfinite(z) || !std::isfinite(w)) {
      ++invalidRefs;
      continue;
    }
    ++validVertices;
    minX = std::min(minX, *x);
    minY = std::min(minY, *y);
    minZ = std::min(minZ, z);
    minW = std::min(minW, w);
    maxX = std::max(maxX, *x);
    maxY = std::max(maxY, *y);
    maxZ = std::max(maxZ, z);
    maxW = std::max(maxW, w);

    if (projectVsConsts && projectFfpVsConsts) {
      const std::array<float, 4> vertex{*x, *y, z, w};
      auto dotConst = [&](std::size_t row) {
        const auto& c = projectVsConsts->vsFloatConst[row];
        return c[0] * vertex[0] + c[1] * vertex[1] +
               c[2] * vertex[2] + c[3] * vertex[3];
      };
      float clipX = dotConst(0);
      float clipY = dotConst(1);
      const float clipZ = dotConst(2);
      const float clipW = dotConst(3);
      clipX += projectFfpVsConsts->halfPixelFixup[0] * clipW;
      clipY += projectFfpVsConsts->halfPixelFixup[1] * clipW;
      if (std::isfinite(clipX) && std::isfinite(clipY) &&
          std::isfinite(clipZ) && std::isfinite(clipW) && clipW != 0.0f) {
        ++projectedRefs;
        minClipX = std::min(minClipX, clipX);
        minClipY = std::min(minClipY, clipY);
        minClipZ = std::min(minClipZ, clipZ);
        minClipW = std::min(minClipW, clipW);
        maxClipX = std::max(maxClipX, clipX);
        maxClipY = std::max(maxClipY, clipY);
        maxClipZ = std::max(maxClipZ, clipZ);
        maxClipW = std::max(maxClipW, clipW);
        if (clipW <= 0.0f) {
          ++projectedBehindRefs;
        }
        const float ndcX = clipX / clipW;
        const float ndcY = clipY / clipW;
        const float ndcZ = clipZ / clipW;
        minNdcX = std::min(minNdcX, ndcX);
        minNdcY = std::min(minNdcY, ndcY);
        minNdcZ = std::min(minNdcZ, ndcZ);
        maxNdcX = std::max(maxNdcX, ndcX);
        maxNdcY = std::max(maxNdcY, ndcY);
        maxNdcZ = std::max(maxNdcZ, ndcZ);
        const bool insideXy =
            ndcX >= -1.0f && ndcX <= 1.0f &&
            ndcY >= -1.0f && ndcY <= 1.0f;
        const bool zInside = ndcZ >= 0.0f && ndcZ <= 1.0f;
        if (insideXy) {
          ++projectedInsideXyRefs;
        }
        if (zInside) {
          ++projectedZInsideRefs;
        }
        if (clipW > 0.0f && insideXy && zInside) {
          ++projectedVisibleRefs;
        }
        const float screenX =
            projectFfpVsConsts->viewportOrigin[0] +
            (ndcX * 0.5f + 0.5f) * projectFfpVsConsts->viewportSize[0];
        const float screenY =
            projectFfpVsConsts->viewportOrigin[1] +
            (0.5f - ndcY * 0.5f) * projectFfpVsConsts->viewportSize[1];
        if (std::isfinite(screenX) && std::isfinite(screenY)) {
          minScreenX = std::min(minScreenX, screenX);
          minScreenY = std::min(minScreenY, screenY);
          maxScreenX = std::max(maxScreenX, screenX);
          maxScreenY = std::max(maxScreenY, screenY);
        }
      }
    }

    std::optional<float> u;
    std::optional<float> v;
    if (layout.hasTexcoord0) {
      u = readF32(vertexBase + layout.texcoord0Offset);
      if (layout.texcoord0Components >= 2u) {
        v = readF32(vertexBase + layout.texcoord0Offset + sizeof(float));
      } else {
        v = 0.0f;
      }
      if (u.has_value() && v.has_value() &&
          std::isfinite(*u) && std::isfinite(*v)) {
        hasUv = true;
        minU = std::min(minU, *u);
        minV = std::min(minV, *v);
        maxU = std::max(maxU, *u);
        maxV = std::max(maxV, *v);
      }
    }

    std::optional<u32> color;
    if (layout.hasDiffuse) {
      color = readU32(vertexBase + layout.diffuseOffset);
      if (color.has_value()) {
        const u32 alpha = (*color >> 24u) & 0xffu;
        hasAlpha = true;
        minAlpha = std::min(minAlpha, alpha);
        maxAlpha = std::max(maxAlpha, alpha);
      }
    }

    if (refsLogged < 6u) {
      refs << " r" << refsLogged << "#" << *indexValue
           << "=(" << *x << "," << *y << "," << z << "," << w << ")";
      if (u.has_value() && v.has_value()) {
        refs << " uv=(" << *u << "," << *v << ")";
      }
      if (color.has_value()) {
        refs << " c=0x" << std::hex << *color << std::dec;
      }
      ++refsLogged;
    }
  }

  std::ostringstream out;
  out << std::setprecision(6)
      << "[dxmt9-effect-geometry"
      << " seq=" << static_cast<unsigned long long>(seqId)
      << " encoder=" << static_cast<unsigned long long>(encoderIndex)
      << " encoder_draw_index=" << static_cast<unsigned long long>(encoderDrawIndex)
      << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
      << " command_index=" << commandIndex
      << " command_draw_index=" << static_cast<unsigned long long>(commandDrawIndex)
      << " command_draw_count=" << static_cast<unsigned long long>(commandDrawCount)
      << " primitive_type=" << static_cast<unsigned>(primitiveType)
      << " primitive_count=" << primitiveCount
      << " index_count=" << static_cast<unsigned long long>(indexCount)
      << " refs_sampled=" << static_cast<unsigned long long>(refsToSample)
      << " readable_indices=" << static_cast<unsigned long long>(readableIndices)
      << " valid_vertices=" << static_cast<unsigned long long>(validVertices)
      << " invalid_refs=" << static_cast<unsigned long long>(invalidRefs)
      << " complete=" << (refsToSample == indexCount ? 1u : 0u)
      << " indexed=1"
      << " ffp=" << (fixedFunctionPath ? 1u : 0u)
      << " pretransformed=" << (preTransformed || layout.preTransformed ? 1u : 0u)
      << " src_blend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
      << " dst_blend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
      << " blend_op=" << core::flatStateOr(hot.renderStates, RS_BLEND_OP, 0u)
      << " depth_enabled="
      << (core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u) != 0u ? 1u : 0u)
      << " depth_write="
      << (core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u ? 1u : 0u)
      << " texture0=0x" << std::hex << hot.textures[0].value
      << " vs_hash=0x" << vertexShaderHash
      << " ps_hash=0x" << pixelShaderHash
      << " stream0_handle=0x" << stream0Handle
      << " index_buffer=0x" << indexBufferHandle
      << std::dec
      << " texture0_width=" << (texture0 ? texture0->desc.width : 0u)
      << " texture0_height=" << (texture0 ? texture0->desc.height : 0u)
      << " texture0_format="
      << (texture0 ? static_cast<u32>(texture0->desc.format) : 0u)
      << " base_vertex=" << baseVertexIndex
      << " start_index=" << startIndex
      << " stream0_offset=" << static_cast<unsigned long long>(stream0Offset)
      << " stream0_stride=" << static_cast<unsigned long long>(stream0Stride)
      << " position_components=" << layout.positionComponents
      << " texcoord0_components=" << layout.texcoord0Components;
  if (validVertices != 0u) {
    out << " pos_min=(" << minX << "," << minY << "," << minZ << "," << minW << ")"
        << " pos_max=(" << maxX << "," << maxY << "," << maxZ << "," << maxW << ")";
  }
  if (hasUv) {
    out << " uv_min=(" << minU << "," << minV << ")"
        << " uv_max=(" << maxU << "," << maxV << ")";
  }
  if (hasAlpha) {
    out << " alpha_min=" << minAlpha
        << " alpha_max=" << maxAlpha;
  }
  if (projectVsConsts && projectFfpVsConsts) {
    out << " project=1"
        << " projected_refs=" << static_cast<unsigned long long>(projectedRefs)
        << " projected_visible_refs="
        << static_cast<unsigned long long>(projectedVisibleRefs)
        << " projected_inside_xy_refs="
        << static_cast<unsigned long long>(projectedInsideXyRefs)
        << " projected_z_inside_refs="
        << static_cast<unsigned long long>(projectedZInsideRefs)
        << " projected_behind_refs="
        << static_cast<unsigned long long>(projectedBehindRefs)
        << " half_pixel=(" << projectFfpVsConsts->halfPixelFixup[0] << ","
        << projectFfpVsConsts->halfPixelFixup[1] << ")"
        << " viewport_origin=(" << projectFfpVsConsts->viewportOrigin[0] << ","
        << projectFfpVsConsts->viewportOrigin[1] << ")"
        << " viewport_size=(" << projectFfpVsConsts->viewportSize[0] << ","
        << projectFfpVsConsts->viewportSize[1] << ")";
    if (projectPsConsts) {
      const auto& ps0 = projectPsConsts->psFloatConst[0];
      out << " ps0=(" << ps0[0] << "," << ps0[1] << "," << ps0[2] << ","
          << ps0[3] << ")";
    }
    if (projectedRefs != 0u) {
      out << " clip_min=(" << minClipX << "," << minClipY << "," << minClipZ
          << "," << minClipW << ")"
          << " clip_max=(" << maxClipX << "," << maxClipY << "," << maxClipZ
          << "," << maxClipW << ")"
          << " ndc_min=(" << minNdcX << "," << minNdcY << "," << minNdcZ
          << ")"
          << " ndc_max=(" << maxNdcX << "," << maxNdcY << "," << maxNdcZ
          << ")"
          << " screen_min=(" << minScreenX << "," << minScreenY << ")"
          << " screen_max=(" << maxScreenX << "," << maxScreenY << ")";
    }
  } else {
    out << " project=0";
  }
  out << refs.str() << ']';
  emitQueueTraceLine(out.str());
}

bool buildIndexedDrawChunks(std::span<const u8> indexBytes,
                            IndexType indexType,
                            u32 startIndex,
                            u32 primitiveCount,
                            u32 primitiveLimit,
                            u64 stream0Stride,
                            u64 stream0SpanLimit,
                            std::vector<IndexedDrawChunk>& chunks) {
  chunks.clear();
  if (primitiveCount == 0u) {
    return false;
  }
  const bool primitiveLimited = primitiveLimit != 0u;
  const bool spanLimited = stream0SpanLimit != 0u && stream0Stride != 0u;
  if (!primitiveLimited && !spanLimited) {
    return false;
  }
  if (spanLimited && indexBytes.empty()) {
    return false;
  }

  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t firstElement = static_cast<std::size_t>(startIndex);
  const std::size_t endElement =
      firstElement + static_cast<std::size_t>(primitiveCount) * 3u;
  if (spanLimited && endElement * elementSize > indexBytes.size()) {
    return false;
  }

  u32 chunkStartPrimitive = 0;
  u32 chunkPrimitiveCount = 0;
  u32 chunkMinIndex = std::numeric_limits<u32>::max();
  u32 chunkMaxIndex = 0;

  auto emitChunk = [&] {
    if (chunkPrimitiveCount == 0u) {
      return;
    }
    const u64 stream0Span =
        chunkMinIndex <= chunkMaxIndex
            ? static_cast<u64>(chunkMaxIndex - chunkMinIndex) * stream0Stride
            : 0u;
    chunks.push_back(IndexedDrawChunk{
        .startPrimitive = chunkStartPrimitive,
        .primitiveCount = chunkPrimitiveCount,
        .stream0Span = stream0Span,
    });
  };

  for (u32 primitive = 0; primitive < primitiveCount; ++primitive) {
    u32 triMin = std::numeric_limits<u32>::max();
    u32 triMax = 0;
    if (spanLimited) {
      const std::size_t triElement =
          firstElement + static_cast<std::size_t>(primitive) * 3u;
      for (std::size_t i = 0; i < 3u; ++i) {
        const auto value = readIndexValue(indexBytes, indexType, triElement + i);
        if (!value.has_value()) {
          chunks.clear();
          return false;
        }
        triMin = std::min(triMin, *value);
        triMax = std::max(triMax, *value);
      }
    }

    bool shouldStartNewChunk = false;
    if (chunkPrimitiveCount != 0u) {
      if (primitiveLimited && chunkPrimitiveCount >= primitiveLimit) {
        shouldStartNewChunk = true;
      }
      if (spanLimited) {
        const u32 nextMin = std::min(chunkMinIndex, triMin);
        const u32 nextMax = std::max(chunkMaxIndex, triMax);
        const u64 nextSpan =
            static_cast<u64>(nextMax - nextMin) * stream0Stride;
        if (nextSpan > stream0SpanLimit) {
          shouldStartNewChunk = true;
        }
      }
    }

    if (shouldStartNewChunk) {
      emitChunk();
      chunkStartPrimitive = primitive;
      chunkPrimitiveCount = 0;
      chunkMinIndex = std::numeric_limits<u32>::max();
      chunkMaxIndex = 0;
    }

    if (chunkPrimitiveCount == 0u) {
      chunkStartPrimitive = primitive;
      if (spanLimited) {
        chunkMinIndex = triMin;
        chunkMaxIndex = triMax;
      }
    } else if (spanLimited) {
      chunkMinIndex = std::min(chunkMinIndex, triMin);
      chunkMaxIndex = std::max(chunkMaxIndex, triMax);
    }
    ++chunkPrimitiveCount;
  }

  emitChunk();
  return chunks.size() > 1u;
}

bool buildReverseTriangleOrderIndexBytes(std::span<const u8> indexBytes,
                                         IndexType indexType,
                                         u32 startIndex,
                                         u64 indexCount,
                                         std::vector<u8>& out) {
  if (indexBytes.empty() || indexCount == 0u || (indexCount % 3u) != 0u) {
    return false;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return false;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return false;
  }

  const std::size_t byteCount = static_cast<std::size_t>(indexCount) * elementSize;
  out.resize(byteCount);
  const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3u);
  const std::size_t triangleBytes = 3u * elementSize;
  for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
    const std::size_t srcTriangle = triangleCount - 1u - triangle;
    std::memcpy(out.data() + triangle * triangleBytes,
                indexBytes.data() + startByte + srcTriangle * triangleBytes,
                triangleBytes);
  }
  return true;
}

bool buildMinIndexSortedTriangleOrderIndexBytes(std::span<const u8> indexBytes,
                                                IndexType indexType,
                                                u32 startIndex,
                                                u64 indexCount,
                                                std::vector<u8>& out) {
  if (indexBytes.empty() || indexCount == 0u || (indexCount % 3u) != 0u) {
    return false;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return false;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return false;
  }

  struct TriangleKey {
    u32 minIndex = 0;
    u32 maxIndex = 0;
    std::size_t triangle = 0;
  };

  const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3u);
  std::vector<TriangleKey> keys;
  keys.reserve(triangleCount);
  for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
    const std::size_t triElement =
        static_cast<std::size_t>(startIndex) + triangle * 3u;
    u32 triMin = std::numeric_limits<u32>::max();
    u32 triMax = 0;
    for (std::size_t i = 0; i < 3u; ++i) {
      const auto value = readIndexValue(indexBytes, indexType, triElement + i);
      if (!value.has_value()) {
        return false;
      }
      triMin = std::min(triMin, *value);
      triMax = std::max(triMax, *value);
    }
    keys.push_back(TriangleKey{
        .minIndex = triMin,
        .maxIndex = triMax,
        .triangle = triangle,
    });
  }

  std::stable_sort(keys.begin(), keys.end(), [](const TriangleKey& a,
                                                const TriangleKey& b) {
    if (a.minIndex != b.minIndex) {
      return a.minIndex < b.minIndex;
    }
    if (a.maxIndex != b.maxIndex) {
      return a.maxIndex < b.maxIndex;
    }
    return a.triangle < b.triangle;
  });

  bool changed = false;
  for (std::size_t triangle = 0; triangle < keys.size(); ++triangle) {
    if (keys[triangle].triangle != triangle) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return false;
  }

  const std::size_t byteCount = static_cast<std::size_t>(indexCount) * elementSize;
  out.resize(byteCount);
  const std::size_t triangleBytes = 3u * elementSize;
  for (std::size_t triangle = 0; triangle < keys.size(); ++triangle) {
    const std::size_t srcTriangle = keys[triangle].triangle;
    std::memcpy(out.data() + triangle * triangleBytes,
                indexBytes.data() + startByte + srcTriangle * triangleBytes,
                triangleBytes);
  }
  return true;
}

void writeIndexValue(std::vector<u8>& out,
                     IndexType indexType,
                     std::size_t elementIndex,
                     u32 value) {
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t offset = elementIndex * elementSize;
  if (indexType == IndexType::UInt16) {
    const u16 v = static_cast<u16>(value);
    std::memcpy(out.data() + offset, &v, sizeof(v));
  } else {
    std::memcpy(out.data() + offset, &value, sizeof(value));
  }
}

bool buildVertexCacheOptimizedTriangleOrderIndexBytes(
    std::span<const u8> indexBytes,
    IndexType indexType,
    u32 startIndex,
    u64 indexCount,
    std::vector<u8>& out,
    std::size_t probeCacheSize = 64u,
    std::size_t candidateFrontierCap = 0u,
    bool candidateLazyFrontier = false,
    bool candidateBucketedSelect = false,
    bool candidateStrictLru = false) {
  if (indexBytes.empty() || indexCount == 0u || (indexCount % 3u) != 0u ||
      probeCacheSize == 0u) {
    return false;
  }
  const std::size_t elementSize = indexElementSize(indexType);
  const std::size_t startByte = static_cast<std::size_t>(startIndex) * elementSize;
  if (startByte > indexBytes.size()) {
    return false;
  }
  const std::size_t maxReadable =
      (indexBytes.size() - startByte) / elementSize;
  if (indexCount > static_cast<u64>(maxReadable)) {
    return false;
  }

  struct Triangle {
    std::array<u32, 3> indices{};
    u32 minIndex = 0;
    u32 maxIndex = 0;
    std::size_t original = 0;
  };

  const std::size_t triangleCount = static_cast<std::size_t>(indexCount / 3u);
  std::vector<Triangle> triangles;
  triangles.reserve(triangleCount);
  u32 minReferencedIndex = std::numeric_limits<u32>::max();
  u32 maxReferencedIndex = 0u;

  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateReadCpuTime);
    for (std::size_t triangle = 0; triangle < triangleCount; ++triangle) {
      Triangle tri{.original = triangle};
      const std::size_t triElement =
          static_cast<std::size_t>(startIndex) + triangle * 3u;
      tri.minIndex = std::numeric_limits<u32>::max();
      for (std::size_t i = 0; i < 3u; ++i) {
        const auto value = readIndexValue(indexBytes, indexType, triElement + i);
        if (!value.has_value()) {
          return false;
        }
        tri.indices[i] = *value;
        tri.minIndex = std::min(tri.minIndex, *value);
        tri.maxIndex = std::max(tri.maxIndex, *value);
        minReferencedIndex = std::min(minReferencedIndex, *value);
        maxReferencedIndex = std::max(maxReferencedIndex, *value);
      }
      triangles.push_back(tri);
    }
  }
  if (triangles.empty()) {
    return false;
  }

  const u64 referencedRange =
      static_cast<u64>(maxReferencedIndex) - minReferencedIndex + 1u;
  const bool useDenseAdjacency = referencedRange <= 131072u;
  std::vector<std::vector<u32>> denseVertexTriangles;
  std::vector<u32> denseRemainingVertexUse;
  std::unordered_map<u32, std::vector<u32>> sparseVertexTriangles;
  std::unordered_map<u32, u32> sparseRemainingVertexUse;

  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateAdjacencyCpuTime);
    if (useDenseAdjacency) {
      const auto range = static_cast<std::size_t>(referencedRange);
      denseVertexTriangles.resize(range);
      denseRemainingVertexUse.assign(range, 0u);
      for (const auto& tri : triangles) {
        for (const u32 index : tri.indices) {
          ++denseRemainingVertexUse[static_cast<std::size_t>(index - minReferencedIndex)];
        }
      }
      for (std::size_t i = 0; i < range; ++i) {
        if (denseRemainingVertexUse[i] != 0u) {
          denseVertexTriangles[i].reserve(denseRemainingVertexUse[i]);
        }
      }
      for (std::size_t triangle = 0; triangle < triangles.size(); ++triangle) {
        for (const u32 index : triangles[triangle].indices) {
          denseVertexTriangles[static_cast<std::size_t>(index - minReferencedIndex)]
              .push_back(static_cast<u32>(triangle));
        }
      }
    } else {
      sparseVertexTriangles.reserve(triangleCount * 3u);
      sparseRemainingVertexUse.reserve(triangleCount * 3u);
      for (std::size_t triangle = 0; triangle < triangles.size(); ++triangle) {
        for (const u32 index : triangles[triangle].indices) {
          sparseVertexTriangles[index].push_back(static_cast<u32>(triangle));
          ++sparseRemainingVertexUse[index];
        }
      }
    }
  }

  std::vector<u32> cache;
  cache.reserve(probeCacheSize);
  std::vector<u32> candidates;
  candidates.reserve(std::min<std::size_t>(triangleCount, 256u));
  std::vector<u8> emitted(triangleCount, 0u);
  std::vector<u8> inCandidates(triangleCount, 0u);
  std::vector<u32> order;
  order.reserve(triangleCount);
  std::size_t nextOriginal = 0;
  const bool useBucketedSelect =
      candidateBucketedSelect && !candidateLazyFrontier;

  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateSelectCpuTime);
    std::uint64_t selectCalls = 0;
    std::uint64_t selectSlots = 0;
    std::uint64_t selectScored = 0;
    std::uint64_t selectSkipped = 0;
    std::uint64_t selectCandidatesMax = 0;
    std::uint64_t frontierDropped = 0;
    std::uint64_t lazyHeapPops = 0;
    std::uint64_t lazyRefreshes = 0;
    std::uint64_t lazyStaleDrops = 0;
    std::uint64_t lazyAccepted = 0;
    std::uint64_t bucketVertexVisits = 0;
    std::uint64_t bucketMoves = 0;
    std::uint64_t bucketSelected = 0;
    constexpr std::size_t kNoCachePosition = std::numeric_limits<std::size_t>::max();
    std::vector<std::size_t> denseCachePositions;
    std::unordered_map<u32, std::size_t> sparseCachePositions;
    if (useDenseAdjacency) {
      denseCachePositions.assign(static_cast<std::size_t>(referencedRange),
                                 kNoCachePosition);
    } else {
      sparseCachePositions.reserve(std::min<std::size_t>(probeCacheSize, 256u));
    }

    auto clearCachePosition = [&](u32 index) {
      if (useDenseAdjacency) {
        if (index < minReferencedIndex) {
          return;
        }
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseCachePositions.size()) {
          denseCachePositions[local] = kNoCachePosition;
        }
        return;
      }
      sparseCachePositions.erase(index);
    };

    auto setCachePosition = [&](u32 index, std::size_t position) {
      if (useDenseAdjacency) {
        if (index < minReferencedIndex) {
          return;
        }
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseCachePositions.size() &&
            denseCachePositions[local] == kNoCachePosition) {
          denseCachePositions[local] = position;
        }
        return;
      }
      sparseCachePositions.try_emplace(index, position);
    };

    auto clearCachePositions = [&]() {
      for (const u32 index : cache) {
        clearCachePosition(index);
      }
    };

    auto rebuildCachePositions = [&]() {
      for (std::size_t i = 0; i < cache.size(); ++i) {
        setCachePosition(cache[i], i);
      }
    };

    auto cachePosition = [&](u32 index) -> std::optional<std::size_t> {
      if (useDenseAdjacency) {
        if (index < minReferencedIndex) {
          return std::nullopt;
        }
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseCachePositions.size() &&
            denseCachePositions[local] != kNoCachePosition) {
          return denseCachePositions[local];
        }
        return std::nullopt;
      }
      auto it = sparseCachePositions.find(index);
      return it != sparseCachePositions.end() ? std::optional<std::size_t>(it->second)
                                              : std::nullopt;
    };

    auto remainingUseFor = [&](u32 index) -> u32 {
      if (useDenseAdjacency) {
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        return local < denseRemainingVertexUse.size()
                   ? denseRemainingVertexUse[local]
                   : 0u;
      }
      auto it = sparseRemainingVertexUse.find(index);
      return it != sparseRemainingVertexUse.end() ? it->second : 0u;
    };

    auto scoreTriangle = [&](const Triangle& tri) -> std::int64_t {
      ++selectScored;
      u32 cachedVertices = 0;
      u32 cacheDistance = 0;
      u32 remainingUse = 0;
      for (const u32 index : tri.indices) {
        if (const auto pos = cachePosition(index)) {
          ++cachedVertices;
          cacheDistance += static_cast<u32>(*pos);
        }
        remainingUse += remainingUseFor(index);
      }
      return static_cast<std::int64_t>(cachedVertices) * 1'000'000ll -
             static_cast<std::int64_t>(cacheDistance) * 1'000ll -
             static_cast<std::int64_t>(remainingUse) * 10ll -
             static_cast<std::int64_t>(tri.minIndex) / 256ll;
    };

    struct CandidateHeapEntry {
      std::int64_t score = 0;
      std::size_t original = 0;
      u32 triangle = 0;
      u32 generation = 0;
      std::uint64_t epoch = 0;
    };

    struct CandidateHeapLess {
      bool operator()(const CandidateHeapEntry& lhs,
                      const CandidateHeapEntry& rhs) const noexcept {
        if (lhs.score != rhs.score) {
          return lhs.score < rhs.score;
        }
        return lhs.original > rhs.original;
      }
    };

    std::priority_queue<CandidateHeapEntry,
                        std::vector<CandidateHeapEntry>,
                        CandidateHeapLess> candidateHeap;
    std::vector<u32> candidateGenerations(
        candidateLazyFrontier ? triangleCount : 0u, 0u);
    std::uint64_t candidateScoreEpoch = 0;
    std::size_t activeCandidateCount = 0;
    constexpr u8 kNoCandidateBucket = 0xffu;
    std::array<std::vector<u32>, 4> candidateBuckets;
    std::vector<u8> candidateCachedVertices(
        useBucketedSelect ? triangleCount : 0u, 0u);
    std::vector<u8> candidateBucket(
        useBucketedSelect ? triangleCount : 0u, kNoCandidateBucket);
    std::vector<std::size_t> candidateBucketSlot(
        useBucketedSelect ? triangleCount : 0u, 0u);

    auto cachedVertexCountFor = [&](const Triangle& tri) -> u8 {
      u8 cachedVertices = 0u;
      for (const u32 index : tri.indices) {
        if (cachePosition(index).has_value()) {
          ++cachedVertices;
        }
      }
      return cachedVertices;
    };

    auto eraseBucketedCandidate = [&](u32 triangle) {
      if (!useBucketedSelect) {
        return;
      }
      const std::size_t t = static_cast<std::size_t>(triangle);
      if (t >= triangleCount) {
        return;
      }
      const u8 bucket = candidateBucket[t];
      if (bucket >= candidateBuckets.size()) {
        return;
      }
      auto& bucketVec = candidateBuckets[bucket];
      const std::size_t slot = candidateBucketSlot[t];
      if (slot >= bucketVec.size() || bucketVec[slot] != triangle) {
        const auto it = std::find(bucketVec.begin(), bucketVec.end(), triangle);
        if (it == bucketVec.end()) {
          candidateBucket[t] = kNoCandidateBucket;
          return;
        }
        const std::size_t found =
            static_cast<std::size_t>(std::distance(bucketVec.begin(), it));
        const u32 replacement = bucketVec.back();
        bucketVec[found] = replacement;
        candidateBucketSlot[static_cast<std::size_t>(replacement)] = found;
        bucketVec.pop_back();
        candidateBucket[t] = kNoCandidateBucket;
        return;
      }
      const u32 replacement = bucketVec.back();
      bucketVec[slot] = replacement;
      candidateBucketSlot[static_cast<std::size_t>(replacement)] = slot;
      bucketVec.pop_back();
      candidateBucket[t] = kNoCandidateBucket;
    };

    auto placeBucketedCandidate = [&](u32 triangle, u8 bucket) {
      if (!useBucketedSelect) {
        return;
      }
      const std::size_t t = static_cast<std::size_t>(triangle);
      if (t >= triangleCount) {
        return;
      }
      bucket = std::min<u8>(bucket, 3u);
      auto& bucketVec = candidateBuckets[bucket];
      candidateBucket[t] = bucket;
      candidateBucketSlot[t] = bucketVec.size();
      candidateCachedVertices[t] = bucket;
      bucketVec.push_back(triangle);
    };

    auto moveBucketedCandidate = [&](u32 triangle, u8 bucket) {
      const std::size_t t = static_cast<std::size_t>(triangle);
      if (!useBucketedSelect || t >= triangleCount) {
        return;
      }
      bucket = std::min<u8>(bucket, 3u);
      if (candidateBucket[t] == bucket) {
        candidateCachedVertices[t] = bucket;
        return;
      }
      eraseBucketedCandidate(triangle);
      placeBucketedCandidate(triangle, bucket);
      ++bucketMoves;
    };

    auto addBucketedCandidate = [&](u32 triangle) {
      const std::size_t t = static_cast<std::size_t>(triangle);
      if (t >= triangleCount) {
        return;
      }
      inCandidates[t] = 1u;
      ++activeCandidateCount;
      selectCandidatesMax = std::max<std::uint64_t>(
          selectCandidatesMax, static_cast<std::uint64_t>(activeCandidateCount));
      placeBucketedCandidate(triangle, cachedVertexCountFor(triangles[t]));
    };

    auto pushLazyCandidate = [&](u32 triangle) {
      const std::size_t t = static_cast<std::size_t>(triangle);
      if (t >= triangleCount) {
        return;
      }
      auto& generation = candidateGenerations[t];
      ++generation;
      const auto& tri = triangles[t];
      candidateHeap.push(CandidateHeapEntry{
          .score = scoreTriangle(tri),
          .original = tri.original,
          .triangle = triangle,
          .generation = generation,
          .epoch = candidateScoreEpoch,
      });
    };

    auto addCandidate = [&](u32 triangle) {
      const std::size_t t = static_cast<std::size_t>(triangle);
      if (t >= triangleCount || emitted[t] || inCandidates[t]) {
        return;
      }
      if (candidateFrontierCap != 0u &&
          ((candidateLazyFrontier || useBucketedSelect)
               ? activeCandidateCount
               : candidates.size()) >=
              candidateFrontierCap) {
        ++frontierDropped;
        return;
      }
      if (useBucketedSelect) {
        addBucketedCandidate(triangle);
        return;
      }
      if (candidateLazyFrontier) {
        inCandidates[t] = 1u;
        ++activeCandidateCount;
        selectCandidatesMax = std::max<std::uint64_t>(
            selectCandidatesMax, static_cast<std::uint64_t>(activeCandidateCount));
        pushLazyCandidate(triangle);
        return;
      }
      candidates.push_back(triangle);
      inCandidates[t] = 1u;
    };

    auto addVertexNeighbors = [&](u32 index) {
      if (useDenseAdjacency) {
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local >= denseVertexTriangles.size()) {
          return;
        }
        for (const u32 triangle : denseVertexTriangles[local]) {
          addCandidate(triangle);
        }
        return;
      }
      auto it = sparseVertexTriangles.find(index);
      if (it == sparseVertexTriangles.end()) {
        return;
      }
      for (const u32 triangle : it->second) {
        addCandidate(triangle);
      }
    };

    auto decrementRemainingUse = [&](u32 index) {
      if (useDenseAdjacency) {
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local < denseRemainingVertexUse.size() &&
            denseRemainingVertexUse[local] != 0u) {
          --denseRemainingVertexUse[local];
        }
        return;
      }
      auto it = sparseRemainingVertexUse.find(index);
      if (it != sparseRemainingVertexUse.end() && it->second != 0u) {
        --it->second;
      }
    };

    auto updateBucketedVertexResidency = [&](u32 index, int delta) {
      if (!useBucketedSelect) {
        return;
      }
      auto visitTriangle = [&](u32 triangle) {
        const std::size_t t = static_cast<std::size_t>(triangle);
        if (t >= triangleCount || emitted[t] || !inCandidates[t]) {
          return;
        }
        ++bucketVertexVisits;
        const int oldCount = candidateCachedVertices[t];
        const int nextCount = std::clamp(oldCount + delta, 0, 3);
        if (nextCount != oldCount) {
          moveBucketedCandidate(triangle, static_cast<u8>(nextCount));
        }
      };
      if (useDenseAdjacency) {
        if (index < minReferencedIndex) {
          return;
        }
        const auto local = static_cast<std::size_t>(index - minReferencedIndex);
        if (local >= denseVertexTriangles.size()) {
          return;
        }
        for (const u32 triangle : denseVertexTriangles[local]) {
          visitTriangle(triangle);
        }
        return;
      }
      auto it = sparseVertexTriangles.find(index);
      if (it == sparseVertexTriangles.end()) {
        return;
      }
      for (const u32 triangle : it->second) {
        visitTriangle(triangle);
      }
    };

    auto chooseBestCandidateLazy = [&]() -> std::optional<u32> {
      ++selectCalls;
      while (!candidateHeap.empty()) {
        const CandidateHeapEntry entry = candidateHeap.top();
        candidateHeap.pop();
        ++lazyHeapPops;
        ++selectSlots;
        const std::size_t triangleIndex =
            static_cast<std::size_t>(entry.triangle);
        if (triangleIndex >= triangleCount ||
            emitted[triangleIndex] ||
            !inCandidates[triangleIndex] ||
            entry.generation != candidateGenerations[triangleIndex]) {
          ++selectSkipped;
          ++lazyStaleDrops;
          continue;
        }
        if (entry.epoch == candidateScoreEpoch) {
          inCandidates[triangleIndex] = 0u;
          if (activeCandidateCount != 0u) {
            --activeCandidateCount;
          }
          ++lazyAccepted;
          return entry.triangle;
        }
        const auto& tri = triangles[triangleIndex];
        const std::int64_t currentScore = scoreTriangle(tri);
        auto& generation = candidateGenerations[triangleIndex];
        ++generation;
        candidateHeap.push(CandidateHeapEntry{
            .score = currentScore,
            .original = tri.original,
            .triangle = entry.triangle,
            .generation = generation,
            .epoch = candidateScoreEpoch,
        });
        ++lazyRefreshes;
      }
      return std::nullopt;
    };

    auto chooseBestCandidateBucketed = [&]() -> std::optional<u32> {
      ++selectCalls;
      selectCandidatesMax = std::max<std::uint64_t>(
          selectCandidatesMax, static_cast<std::uint64_t>(activeCandidateCount));
      for (int bucket = 3; bucket >= 0; --bucket) {
        auto& bucketVec = candidateBuckets[static_cast<std::size_t>(bucket)];
        if (bucketVec.empty()) {
          continue;
        }
        std::optional<std::size_t> bestSlot;
        std::int64_t bestScore = std::numeric_limits<std::int64_t>::min();
        selectSlots += static_cast<std::uint64_t>(bucketVec.size());
        for (std::size_t slot = 0; slot < bucketVec.size(); ++slot) {
          const u32 candidate = bucketVec[slot];
          const std::size_t triangleIndex = static_cast<std::size_t>(candidate);
          if (triangleIndex >= triangleCount ||
              emitted[triangleIndex] ||
              !inCandidates[triangleIndex]) {
            ++selectSkipped;
            continue;
          }
          const auto& tri = triangles[triangleIndex];
          const std::int64_t score = scoreTriangle(tri);
          if (score > bestScore ||
              (score == bestScore &&
               (!bestSlot.has_value() ||
                tri.original < triangles[bucketVec[*bestSlot]].original))) {
            bestScore = score;
            bestSlot = slot;
          }
        }
        if (!bestSlot.has_value()) {
          continue;
        }
        const u32 chosen = bucketVec[*bestSlot];
        eraseBucketedCandidate(chosen);
        inCandidates[static_cast<std::size_t>(chosen)] = 0u;
        if (activeCandidateCount != 0u) {
          --activeCandidateCount;
        }
        ++bucketSelected;
        return chosen;
      }
      return std::nullopt;
    };

    auto chooseBestCandidate = [&]() -> std::optional<u32> {
      if (candidateLazyFrontier) {
        return chooseBestCandidateLazy();
      }
      if (useBucketedSelect) {
        return chooseBestCandidateBucketed();
      }
      std::optional<std::size_t> bestSlot;
      std::int64_t bestScore = std::numeric_limits<std::int64_t>::min();
      ++selectCalls;
      selectSlots += static_cast<std::uint64_t>(candidates.size());
      selectCandidatesMax = std::max<std::uint64_t>(
          selectCandidatesMax, static_cast<std::uint64_t>(candidates.size()));
      for (std::size_t slot = 0; slot < candidates.size(); ++slot) {
        const u32 candidate = candidates[slot];
        const std::size_t triangleIndex = static_cast<std::size_t>(candidate);
        if (triangleIndex >= triangleCount || emitted[triangleIndex]) {
          ++selectSkipped;
          continue;
        }
        const auto& tri = triangles[triangleIndex];
        const std::int64_t score = scoreTriangle(tri);
        if (score > bestScore ||
            (score == bestScore &&
             (!bestSlot.has_value() ||
              tri.original < triangles[candidates[*bestSlot]].original))) {
          bestScore = score;
          bestSlot = slot;
        }
      }
      if (!bestSlot.has_value()) {
        return std::nullopt;
      }
      const u32 chosen = candidates[*bestSlot];
      inCandidates[chosen] = 0u;
      candidates[*bestSlot] = candidates.back();
      candidates.pop_back();
      return chosen;
    };

    auto chooseNextOriginal = [&]() -> std::optional<u32> {
      while (nextOriginal < triangleCount && emitted[nextOriginal]) {
        ++nextOriginal;
      }
      if (nextOriginal >= triangleCount) {
        return std::nullopt;
      }
      return static_cast<u32>(nextOriginal);
    };

    auto touchCacheVertex = [&](u32 index) {
      if (const auto position = cachePosition(index)) {
        clearCachePositions();
        for (std::size_t j = *position; j > 0u; --j) {
          cache[j] = cache[j - 1u];
        }
        cache[0] = index;
        rebuildCachePositions();
        return;
      }
      std::optional<u32> evicted;
      clearCachePositions();
      if (candidateStrictLru) {
        if (cache.size() < probeCacheSize) {
          cache.push_back(0u);
        } else if (!cache.empty()) {
          evicted = cache.back();
        }
        for (std::size_t i = cache.size(); i > 1u; --i) {
          cache[i - 1u] = cache[i - 2u];
        }
      } else {
        if (cache.size() < probeCacheSize) {
          cache.push_back(index);
        } else {
          if (!cache.empty()) {
            evicted = cache.back();
          }
          for (std::size_t i = cache.size() - 1u; i > 0u; --i) {
            cache[i] = cache[i - 1u];
          }
        }
      }
      cache[0] = index;
      rebuildCachePositions();
      if (evicted.has_value()) {
        updateBucketedVertexResidency(*evicted, -1);
      }
      updateBucketedVertexResidency(index, 1);
    };

    while (order.size() < triangleCount) {
      const std::optional<u32> candidate = chooseBestCandidate();
      const std::optional<u32> fallback =
          candidate.has_value() ? candidate : chooseNextOriginal();
      if (!fallback.has_value()) {
        break;
      }
      const u32 chosen = *fallback;
      const std::size_t triangleIndex = static_cast<std::size_t>(chosen);
      if (triangleIndex >= triangleCount || emitted[triangleIndex]) {
        continue;
      }
      emitted[triangleIndex] = 1u;
      if (inCandidates[triangleIndex]) {
        inCandidates[triangleIndex] = 0u;
        if (candidateLazyFrontier && activeCandidateCount != 0u) {
          --activeCandidateCount;
        }
        if (useBucketedSelect) {
          eraseBucketedCandidate(chosen);
          if (activeCandidateCount != 0u) {
            --activeCandidateCount;
          }
        }
      }
      order.push_back(chosen);
      const auto& tri = triangles[triangleIndex];
      for (const u32 index : tri.indices) {
        decrementRemainingUse(index);
        touchCacheVertex(index);
      }
      ++candidateScoreEpoch;
      for (const u32 index : tri.indices) {
        addVertexNeighbors(index);
      }
    }

    perf::countEncodeDrawIndexCacheCandidateSelectVolume(
        selectCalls, selectSlots, selectScored, selectSkipped,
        selectCandidatesMax);
    perf::countEncodeDrawIndexCacheCandidateFrontierDropped(frontierDropped);
    perf::countEncodeDrawIndexCacheCandidateLazyFrontier(
        lazyHeapPops, lazyRefreshes, lazyStaleDrops, lazyAccepted);
    perf::countEncodeDrawIndexCacheCandidateBucketedSelect(
        bucketVertexVisits, bucketMoves, bucketSelected);
  }

  if (order.size() != triangleCount) {
    return false;
  }
  bool changed = false;
  for (std::size_t triangle = 0; triangle < order.size(); ++triangle) {
    if (order[triangle] != triangle) {
      changed = true;
      break;
    }
  }
  if (!changed) {
    return false;
  }

  const std::size_t byteCount = static_cast<std::size_t>(indexCount) * elementSize;
  {
    PerfScope scope(perf::countEncodeDrawIndexCacheCandidateWriteCpuTime);
    out.resize(byteCount);
    for (std::size_t triangle = 0; triangle < order.size(); ++triangle) {
      const auto& tri = triangles[order[triangle]];
      for (std::size_t i = 0; i < 3u; ++i) {
        writeIndexValue(out, indexType, triangle * 3u + i, tri.indices[i]);
      }
    }
  }
  return true;
}

bool renderEncoderSelectionMatches(const ActiveEncoderBreakdown* encoderBreakdown,
                                   debug::RenderEncoderSelector rowSelector,
                                   const debug::RenderEncoderSelectorList& rowSelectors) {
  if (!rowSelector.enabled && !rowSelectors.enabled) {
    return true;
  }
  if (!encoderBreakdown) {
    return false;
  }
  const auto seqId = encoderBreakdown->stats.seqId;
  const auto encoderIndex = encoderBreakdown->stats.encoderIndex;
  return debug::renderEncoderSelectorMatches(rowSelector, seqId, encoderIndex) ||
         debug::renderEncoderSelectorListMatches(rowSelectors, seqId, encoderIndex);
}

bool reverseIndexedTriangleRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeReverseIndexedTrianglesRow(),
                                       debug::probeReverseIndexedTrianglesRows());
}

bool screenBlendIndexOrderRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::optimizeScreenBlendIndexOrderRow(),
                                       debug::optimizeScreenBlendIndexOrderRows());
}

bool splitLargeIndexedDrawRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::splitLargeIndexedDrawRow(),
                                       debug::splitLargeIndexedDrawRows());
}

bool forceExpandIndexedProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeForceExpandIndexedRow(),
                                       debug::probeForceExpandIndexedRows());
}

bool stageStreamIbProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  if (!debug::probeStageStreamIb()) {
    return false;
  }
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeStageStreamIbRow(),
                                       debug::probeStageStreamIbRows());
}

bool indexedTriangleEncoderDrawRangeMatches(
    const ActiveEncoderBreakdown* encoderBreakdown) {
  const auto range = debug::probeIndexedTriangleEncoderDrawRange();
  const auto excludeList = debug::probeIndexedTriangleEncoderDrawExcludeList();
  if (!debug::drawOrdinalRangeEnabled(range) && !excludeList.enabled) {
    return true;
  }
  if (!encoderBreakdown) {
    return false;
  }
  const auto encoderDrawIndex = encoderBreakdown->stats.drawCalls;
  if (debug::drawOrdinalRangeEnabled(range) &&
      debug::shouldSkipDrawOrdinal(encoderDrawIndex, range)) {
    return false;
  }
  return !debug::drawOrdinalListContains(excludeList, encoderDrawIndex);
}

bool scissorRectProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeScissorRectRow(),
                                       debug::probeScissorRectRows());
}

bool forceCullModeProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeForceCullModeRow(),
                                       debug::probeForceCullModeRows());
}

bool forceTextureWhiteProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeForceTextureWhiteRow(),
                                       debug::probeForceTextureWhiteRows());
}

bool disableAlphaBlendProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeDisableAlphaBlendRow(),
                                       debug::probeDisableAlphaBlendRows());
}

bool disableDepthWriteProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeDisableDepthWriteRow(),
                                       debug::probeDisableDepthWriteRows());
}

bool depthFuncAlwaysProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeDepthFuncAlwaysRow(),
                                       debug::probeDepthFuncAlwaysRows());
}

bool fragmentlessDepthOnlyProbeRowMatches(const ActiveEncoderBreakdown* encoderBreakdown) {
  return renderEncoderSelectionMatches(encoderBreakdown,
                                       debug::probeFragmentlessDepthOnlyRow(),
                                       debug::probeFragmentlessDepthOnlyRows());
}

WMTCullMode toWmtCullMode(debug::CullModeOverride mode,
                          WMTCullMode fallback) noexcept {
  switch (mode) {
    case debug::CullModeOverride::None:
      return WMTCullModeNone;
    case debug::CullModeOverride::Front:
      return WMTCullModeFront;
    case debug::CullModeOverride::Back:
      return WMTCullModeBack;
    case debug::CullModeOverride::Disabled:
      return fallback;
  }
  return fallback;
}

bool indexedTriangleOpaqueDepthWriteClass(
    const core::FlatRenderStateSet& renderStates,
    WMTTriangleFillMode fillMode) {
  if (fillMode != WMTTriangleFillModeFill) {
    return false;
  }
  const bool depthEnabled =
      core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
  const auto depthFunc = static_cast<core::CompareFunc>(core::flatStateOr(
      renderStates, RS_Z_FUNC, static_cast<u32>(core::CompareFunc::LessEqual)));
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool alphaTestEnabled =
      core::flatStateOr(renderStates, RS_ALPHA_TEST_ENABLE, 0u) != 0u;
  const bool stencilEnabled =
      core::flatStateOr(renderStates, core::RS_STENCIL_ENABLE, 0u) != 0u;
  const bool clipPlaneEnabled =
      core::flatStateOr(renderStates, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u;
  const bool depthFuncPreservesOpaqueOrder =
      depthFunc == core::CompareFunc::Less ||
      depthFunc == core::CompareFunc::LessEqual;
  return depthWrite && depthFuncPreservesOpaqueOrder &&
         !alphaBlendEnabled && !alphaTestEnabled && !stencilEnabled &&
         !clipPlaneEnabled;
}

bool indexedTriangleBlendEquationMatches(
    const core::FlatRenderStateSet& renderStates,
    core::BlendFactor src,
    core::BlendFactor dst,
    core::BlendOp op) {
  if (core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) == 0u) {
    return false;
  }
  return core::flatStateOr(renderStates,
                           RS_SRC_BLEND,
                           static_cast<u32>(core::BlendFactor::One)) ==
             static_cast<u32>(src) &&
         core::flatStateOr(renderStates,
                           RS_DEST_BLEND,
                           static_cast<u32>(core::BlendFactor::Zero)) ==
             static_cast<u32>(dst) &&
         core::flatStateOr(renderStates,
                           RS_BLEND_OP,
                           static_cast<u32>(core::BlendOp::Add)) ==
             static_cast<u32>(op) &&
         core::flatStateOr(renderStates,
                           RS_SEPARATE_ALPHA_BLEND_ENABLE,
                           0u) == 0u;
}

bool indexedTriangleClassMatches(
    debug::IndexedTriangleClassFilter filter,
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  if (filter == debug::IndexedTriangleClassFilter::Any) {
    return true;
  }

  const bool depthEnabled =
      core::flatStateOr(renderStates, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(renderStates, RS_Z_WRITE_ENABLE, 0u) != 0u;
  const bool alphaBlendEnabled =
      core::flatStateOr(renderStates, RS_ALPHABLEND_ENABLE, 0u) != 0u;
  const bool opaqueDepthWrite =
      indexedTriangleOpaqueDepthWriteClass(renderStates, fillMode);

  switch (filter) {
    case debug::IndexedTriangleClassFilter::Any:
      return true;
    case debug::IndexedTriangleClassFilter::OpaqueDepthWrite:
      return opaqueDepthWrite;
    case debug::IndexedTriangleClassFilter::NonOpaque:
      return !opaqueDepthWrite;
    case debug::IndexedTriangleClassFilter::DepthRead:
      return depthEnabled && !depthWrite;
    case debug::IndexedTriangleClassFilter::AlphaBlend:
      return alphaBlendEnabled;
    case debug::IndexedTriangleClassFilter::NoAlphaBlend:
      return !alphaBlendEnabled;
    case debug::IndexedTriangleClassFilter::ScreenBlend:
      return indexedTriangleBlendEquationMatches(renderStates,
                                                 core::BlendFactor::InvDestColor,
                                                 core::BlendFactor::One,
                                                 core::BlendOp::Add);
    case debug::IndexedTriangleClassFilter::StandardAlphaBlend:
      return indexedTriangleBlendEquationMatches(renderStates,
                                                 core::BlendFactor::SrcAlpha,
                                                 core::BlendFactor::InvSrcAlpha,
                                                 core::BlendOp::Add);
    case debug::IndexedTriangleClassFilter::AdditiveAlphaBlend:
      return indexedTriangleBlendEquationMatches(renderStates,
                                                 core::BlendFactor::SrcAlpha,
                                                 core::BlendFactor::One,
                                                 core::BlendOp::Add);
    case debug::IndexedTriangleClassFilter::Scissor:
      return viewport.scissorEnabled;
    case debug::IndexedTriangleClassFilter::NoScissor:
      return !viewport.scissorEnabled;
    case debug::IndexedTriangleClassFilter::Textured:
      return textureMask != 0u;
    case debug::IndexedTriangleClassFilter::Large4096:
      return primitiveCount >= 4096u;
  }
  return true;
}

bool indexedTriangleClassMatches(
    const debug::IndexedTriangleClassFilterList& filters,
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  for (std::size_t i = 0; i < filters.count; ++i) {
    if (!indexedTriangleClassMatches(filters.filters[i],
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode)) {
      return false;
    }
  }
  return true;
}

bool scissorRectProbeClassMatches(u32 primitiveCount,
                                  u32 textureMask,
                                  const core::FlatRenderStateSet& renderStates,
                                  const core::ViewportScissor& viewport,
                                  WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeScissorRectClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeScissorRectClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool forceCullModeProbeClassMatches(u32 primitiveCount,
                                    u32 textureMask,
                                    const core::FlatRenderStateSet& renderStates,
                                    const core::ViewportScissor& viewport,
                                    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeForceCullModeClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeForceCullModeClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool forceTextureWhiteProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeForceTextureWhiteClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeForceTextureWhiteClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool forceExpandIndexedProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeForceExpandIndexedClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeForceExpandIndexedClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool disableAlphaBlendProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeDisableAlphaBlendClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeDisableAlphaBlendClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool disableDepthWriteProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeDisableDepthWriteClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeDisableDepthWriteClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool depthFuncAlwaysProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeDepthFuncAlwaysClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeDepthFuncAlwaysClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool fragmentlessDepthOnlyProbeClassMatches(
    u32 primitiveCount,
    u32 textureMask,
    const core::FlatRenderStateSet& renderStates,
    const core::ViewportScissor& viewport,
    WMTTriangleFillMode fillMode) {
  return indexedTriangleClassMatches(debug::probeFragmentlessDepthOnlyClassFilter(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode) &&
         indexedTriangleClassMatches(debug::probeFragmentlessDepthOnlyClassFilters(),
                                     primitiveCount,
                                     textureMask,
                                     renderStates,
                                     viewport,
                                     fillMode);
}

bool fragmentlessDepthOnlyStateSafe(const core::FlatDrawStateRecord& hot,
                                    WMTTriangleFillMode fillMode) {
  if (fillMode != WMTTriangleFillModeFill || !hot.depthStencil.handle) {
    return false;
  }
  const auto& rs = hot.renderStates;
  const bool depthEnabled = core::flatStateOr(rs, RS_Z_ENABLE, 0u) != 0u;
  const bool depthWrite =
      depthEnabled && core::flatStateOr(rs, RS_Z_WRITE_ENABLE, 0u) != 0u;
  if (!depthWrite) {
    return false;
  }
  if (core::flatStateOr(rs, RS_ALPHABLEND_ENABLE, 0u) != 0u ||
      core::flatStateOr(rs, RS_ALPHA_TEST_ENABLE, 0u) != 0u ||
      core::flatStateOr(rs, core::RS_STENCIL_ENABLE, 0u) != 0u ||
      core::flatStateOr(rs, core::RS_CLIP_PLANE_ENABLE, 0u) != 0u) {
    return false;
  }
  const u32 adaptiveTessY = core::flatStateOr(rs, core::RS_ADAPTIVETESS_Y, 0u);
  if (adaptiveTessY == core::kFourCcAtoc || adaptiveTessY == core::kFourCcA2M1) {
    return false;
  }

  constexpr std::array<u32, core::kMaxRenderTargets> kColorWriteSlots = {
      core::RS_COLOR_WRITE_ENABLE,
      core::RS_COLOR_WRITE_ENABLE1,
      core::RS_COLOR_WRITE_ENABLE2,
      core::RS_COLOR_WRITE_ENABLE3,
  };
  for (std::size_t i = 0; i < hot.colorAttachments.size(); ++i) {
    if (!hot.colorAttachments[i].handle) {
      continue;
    }
    if (core::flatStateOr(rs, kColorWriteSlots[i], 0xfu) != 0u) {
      return false;
    }
  }
  return true;
}

template <std::size_t MaxEntries>
void overrideFlatStateValue(core::FlatStateSet<MaxEntries>& set,
                            u32 state,
                            u32 value) noexcept {
  auto* first = set.entries.data();
  auto* last = first + (set.count <= MaxEntries ? set.count : MaxEntries);
  auto* hit = std::lower_bound(
      first, last, state,
      [](const core::FlatStateEntry& entry, u32 needle) noexcept {
        return entry.state < needle;
      });
  if (hit != last && hit->state == state) {
    hit->value = value;
    return;
  }
  if (set.count >= MaxEntries) {
    set.overflow = true;
    return;
  }
  auto* insertPos = hit;
  for (auto* it = first + set.count; it != insertPos; --it) {
    *it = *(it - 1);
  }
  *insertPos = core::FlatStateEntry{.state = state, .value = value};
  ++set.count;
}

u32 samplerStateOr(const SamplerSnapshot& snapshot, u32 state, u32 fallback) {
  const auto it = snapshot.states.find(state);
  return it != snapshot.states.end() ? it->second : fallback;
}

u32 samplerStateOr(const core::FlatStateSet<core::kMaxSamplerStates>& states,
                   u32 state,
                   u32 fallback) {
  return core::flatStateOr(states, state, fallback);
}

WMTSamplerAddressMode resolveSamplerAddressMode(u32 value) {
  switch (value) {
    case 1u: return WMTSamplerAddressModeRepeat;
    case 2u: return WMTSamplerAddressModeMirrorRepeat;
    case 5u: return WMTSamplerAddressModeMirrorClampToEdge;
    case 4u: return WMTSamplerAddressModeClampToBorderColor;
    case 3u:
    default: return WMTSamplerAddressModeClampToEdge;
  }
}

WMTSamplerBorderColor resolveSamplerBorderColor(u32 value) {
  switch (value) {
    case 0x00000000u: return WMTSamplerBorderColorTransparentBlack;
    case 0xff000000u: return WMTSamplerBorderColorOpaqueBlack;
    case 0xffffffffu: return WMTSamplerBorderColorOpaqueWhite;
    default: return (value >> 24) == 0u ? WMTSamplerBorderColorTransparentBlack : WMTSamplerBorderColorOpaqueBlack;
  }
}

void appendSamplerTrace(std::ostringstream& out,
                        const core::FlatStateSet<core::kMaxSamplerStates>& states,
                        bool srgbTexture) {
  const auto minFilter = samplerStateOr(states, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(states, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(states, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(states, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(states, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(states, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(states, SAMP_BORDER_COLOR, 0u);
  const auto maxMipLevel = samplerStateOr(states, SAMP_MAX_MIP_LEVEL, 0u);
  out << " addr=(" << addressU << "," << addressV << "," << addressW << ")"
      << " filter=(" << minFilter << "," << magFilter << "," << mipFilter << ")"
      << " border=0x" << std::hex << borderColor << std::dec
      << " maxMip=" << maxMipLevel
      << " srgbTex=" << (srgbTexture ? 1 : 0);
}

}  // namespace

struct EncodeChunkSessionState {
  EncodeChunkSessionStorage storage{};
  core::metalqueue::EncodeSessionSourceList sources{};
};

EncodeChunkSession makeEncodeChunkSession() {
  return EncodeChunkSession(new EncodeChunkSessionState{},
                            EncodeChunkSessionDeleter{});
}

void EncodeChunkSessionDeleter::operator()(
    EncodeChunkSessionState* session) const noexcept {
  delete session;
}

void resetEncodeChunkSession(EncodeChunkSessionState& session) {
  DXMT_ASSERT(!session.storage.activeRenderEncoder);
  DXMT_ASSERT(!session.storage.activeBlitEncoder);
  DXMT_ASSERT(!session.storage.hasActiveRender);
  session.storage = EncodeChunkSessionStorage{};
  session.sources.clear();
}

bool retainEncodeChunkSessionUntilSubmissionComplete(
    EncodeChunkSession session,
    core::metalqueue::QueueSubmissionRecord& record) {
  if (!session) {
    return true;
  }
  std::shared_ptr<EncodeChunkSessionState> retained(
      session.release(), EncodeChunkSessionDeleter{});
  record.retainedPayloads.push_back(std::move(retained));
  return true;
}

bool encodeChunkSessionHasActiveRender(
    const EncodeChunkSessionState& session) noexcept {
  return static_cast<bool>(session.storage.activeRenderEncoder);
}

bool encodeChunkSessionHasDeferredSubmissionPayload(
    const EncodeChunkSessionState& session) noexcept {
  const auto& storage = session.storage;
  return static_cast<bool>(storage.activeRenderEncoder) ||
         static_cast<bool>(storage.activeBlitEncoder) ||
         storage.hasActiveRender ||
         storage.pendingClear.has_value() ||
         !storage.postCommitCallbacks.empty() ||
         !storage.completionCallbacks.empty() ||
         storage.metalCaptureRequest.has_value() ||
         static_cast<bool>(storage.renderEncoderGpuSampleBuffer) ||
         !storage.renderEncoderGpuSamples.empty();
}

bool canAppendEncodeChunkSessionSource(
    const EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept {
  return session.sources.canAppend(source);
}

bool appendEncodeChunkSessionSource(
    EncodeChunkSessionState& session,
    core::metalqueue::QueueCompletionSource source) noexcept {
  return session.sources.append(source);
}

std::span<const core::metalqueue::QueueCompletionSource>
encodeChunkSessionSources(const EncodeChunkSessionState& session) noexcept {
  return session.sources.span();
}

bool publishEncodeChunkSessionSources(
    const EncodeChunkSessionState& sessionState,
    core::metalqueue::QueueSubmissionRecord& record) {
  const auto sessionSources = sessionState.sources.span();
  if (sessionSources.empty()) {
    return true;
  }

  const auto recordSources = record.explicitCompletionSourceSpan();
  if (recordSources.empty()) {
    if (!record.assignFixedCompletionSources(sessionSources)) {
      return false;
    }
    return true;
  }

  if (recordSources.size() != sessionSources.size()) {
    return false;
  }
  for (std::size_t i = 0; i < sessionSources.size(); ++i) {
    const auto& expected = sessionSources[i];
    const auto& actual = recordSources[i];
    if (expected.slotIndex != actual.slotIndex ||
        expected.seqId != actual.seqId ||
        expected.hasPresent != actual.hasPresent ||
        expected.commandBegin != actual.commandBegin ||
        expected.commandCount != actual.commandCount) {
      return false;
    }
  }
  return true;
}

bool finalizeEncodeChunkSessionIntoSubmission(
    EncodeContext& ctx,
    EncodeChunkSessionState& sessionState,
    core::metalqueue::QueueSubmissionRecord& record) {
  auto& storage = sessionState.storage;
  if (!record.commandBuffer) {
    return false;
  }
  if (record.renderEncoderGpuSampleBuffer &&
      storage.renderEncoderGpuSampleBuffer &&
      record.renderEncoderGpuSampleBuffer.handle !=
          storage.renderEncoderGpuSampleBuffer.handle) {
    return false;
  }
  if (record.metalCapture.has_value() &&
      storage.metalCaptureRequest.has_value()) {
    return false;
  }

  auto assertEncoderLifecycleInvariant = [&] {
    DXMT_ASSERT(!(storage.activeRenderEncoder && storage.activeBlitEncoder));
    DXMT_ASSERT(storage.hasActiveRender ==
                static_cast<bool>(storage.activeRenderEncoder));
  };
  auto assertNoActiveEncoder = [&] {
    assertEncoderLifecycleInvariant();
    DXMT_ASSERT(!storage.activeRenderEncoder);
    DXMT_ASSERT(!storage.activeBlitEncoder);
    DXMT_ASSERT(!storage.hasActiveRender);
  };
  auto makeRenderEncoderGpuAttachment = [&](
      core::metalqueue::RenderEncoderGpuPassType passType,
      std::size_t commandIndex,
      std::uint64_t rtHandle,
      std::uint64_t depthHandle,
      std::uint64_t psoHandle = 0) {
    RenderEncoderGpuAttachment result{};
    if (!storage.renderEncoderGpuSampleBuffer ||
        storage.renderEncoderGpuSampleCursor + 1u >=
            storage.requestedRenderEncoderGpuSamples) {
      return result;
    }
    const std::uint32_t startSample =
        storage.renderEncoderGpuSampleCursor++;
    const std::uint32_t endSample =
        storage.renderEncoderGpuSampleCursor++;
    result.attachments[0] = WMTSampleBufferAttachmentInfo{
        .sample_buffer = storage.renderEncoderGpuSampleBuffer.handle,
        .start_of_encoder_sample_index = startSample,
        .end_of_encoder_sample_index = endSample,
    };
    result.sample =
        core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample{
            .startIndex = startSample,
            .endIndex = endSample,
            .passType = passType,
            .seqId = record.seqId,
            .slotIndex =
                record.slotIndex <= std::numeric_limits<std::uint32_t>::max()
                    ? static_cast<std::uint32_t>(record.slotIndex)
                    : std::numeric_limits<std::uint32_t>::max(),
            .commandIndex =
                commandIndex <= std::numeric_limits<std::uint32_t>::max()
                    ? static_cast<std::uint32_t>(commandIndex)
                    : std::numeric_limits<std::uint32_t>::max(),
            .rtHandle = rtHandle,
            .depthHandle = depthHandle,
            .psoHandle = psoHandle,
        };
    result.active = true;
    return result;
  };
  auto recordRenderEncoderGpuAttachment =
      [&](const RenderEncoderGpuAttachment& attachment) {
        if (attachment.active) {
          storage.renderEncoderGpuSamples.push_back(attachment.sample);
        }
      };
  auto flushPendingClear = [&] {
    if (!storage.pendingClear.has_value()) {
      return;
    }
    const auto& clear = *storage.pendingClear;
    const auto sampleAttachment = makeRenderEncoderGpuAttachment(
        core::metalqueue::RenderEncoderGpuPassType::Clear,
        storage.pendingClearCommandIndex,
        clear.colorAttachments[0].handle.value,
        clear.depthStencil.handle.value);
    dxmt9::encoders::encodeClearPass(record.commandBuffer, ctx.pool, clear,
                                     sampleAttachment.span());
    recordRenderEncoderGpuAttachment(sampleAttachment);
    storage.pendingClear.reset();
    storage.pendingClearCommandIndex =
        std::numeric_limits<std::size_t>::max();
  };
  auto flushRender = [&] {
    if (!storage.activeRenderEncoder) {
      return;
    }
    DXMT_ASSERT(storage.hasActiveRender);
    DXMT_ASSERT(!storage.activeBlitEncoder);
    storage.activeRenderEncoder.popDebugGroup();
    storage.activeRenderEncoder.endEncoding();
    maybeEncodeColorAttachmentDump(record.commandBuffer, ctx.device,
                                   storage.activeColorAttachmentDump,
                                   storage.completionCallbacks);
    maybeEncodeDepthAttachmentDump(record.commandBuffer, ctx.device,
                                   storage.activeDepthAttachmentDump,
                                   storage.completionCallbacks);
    maybeEncodeDrawTextureDumps(record.commandBuffer, ctx.device,
                                storage.activeDrawTextureDumps,
                                storage.completionCallbacks);
    if (storage.activeVisibilityScout) {
      enqueueVisibilityScoutCompletion(*storage.activeVisibilityScout,
                                       storage.completionCallbacks);
      storage.activeVisibilityScout.reset();
    }
    perf::countRenderPassEnd(perf::EncoderSplitReason::Final);
    storage.activeEncoderBreakdown.emit(perf::EncoderSplitReason::Final);
    storage.activeStreamIbStaging.begin(false);
    for (auto& handle : storage.activeColorHandles) {
      if (handle) {
        ctx.queue.markColorHandleTouched(handle);
        handle = core::Handle{};
      }
    }
    storage.activeColorAttachmentDump = {};
    storage.activeDepthAttachmentDump = {};
    storage.activeDrawTextureDumps.clear();
    storage.activeRenderEncoderSeq = 0;
    storage.activeRenderEncoderIndex = 0;
    storage.activeRenderEncoder = {};
    storage.hasActiveRender = false;
    storage.activeDrawStateKey.reset();
    storage.activeDrawStateUsesPrefetchedPsoLayout = false;
    storage.textureSamplerShadow.reset();
    assertEncoderLifecycleInvariant();
  };
  auto flushBlit = [&] {
    if (!storage.activeBlitEncoder) {
      return;
    }
    DXMT_ASSERT(!storage.activeRenderEncoder);
    DXMT_ASSERT(!storage.hasActiveRender);
    storage.activeBlitEncoder.endEncoding();
    storage.activeBlitEncoder = {};
    assertEncoderLifecycleInvariant();
  };

  flushPendingClear();
  flushRender();
  flushBlit();
  assertNoActiveEncoder();

  if (storage.metalCaptureRequest.has_value()) {
    record.metalCaptureDevice = WMT::Device{ctx.device.handle};
    record.metalCapture = std::move(storage.metalCaptureRequest);
  }
  if (!publishEncodeChunkSessionSources(sessionState, record)) {
    return false;
  }
  if (!record.renderEncoderGpuSampleBuffer &&
      storage.renderEncoderGpuSampleBuffer) {
    record.renderEncoderGpuSampleBuffer =
        std::move(storage.renderEncoderGpuSampleBuffer);
  }
  for (auto& sample : storage.renderEncoderGpuSamples) {
    record.renderEncoderGpuSamples.push_back(std::move(sample));
  }
  for (auto& callback : storage.postCommitCallbacks) {
    record.postCommitCallbacks.push_back(std::move(callback));
  }
  for (auto& callback : storage.completionCallbacks) {
    record.completionCallbacks.push_back(std::move(callback));
  }
  resetEncodeChunkSession(sessionState);
  return true;
}

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device, bool linear) {
  WMTSamplerInfo info{};
  auto f = linear ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.min_filter = f;
  info.mag_filter = f;
  info.mip_filter = WMTSamplerMipFilterNotMipmapped;
  info.s_address_mode = WMTSamplerAddressModeClampToEdge;
  info.t_address_mode = WMTSamplerAddressModeClampToEdge;
  info.r_address_mode = WMTSamplerAddressModeClampToEdge;
  info.normalized_coords = true;
  // R-BACK-12.22..12.26 (resource-array sub-mode): a sampler placed in an
  // argument buffer must be created with supportArgumentBuffers=YES or its
  // gpuResourceID is invalid (GPU page fault on .sample()). Gated on the
  // opt-in lane so the default direct-bind path is byte-identical.
  info.support_argument_buffers = dxmt9::shaders::argbufResourceArrayEnabled();
  DXMT_ASSERT(device && "makeSampler(linear) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMTSamplerInfo makeSamplerInfo(const SamplerSnapshot& snapshot, float lodMinClamp) {
  const auto minFilter = samplerStateOr(snapshot, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(snapshot, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(snapshot, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(snapshot, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(snapshot, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(snapshot, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(snapshot, SAMP_BORDER_COLOR, 0u);
  const auto maxAnisotropy = samplerStateOr(snapshot, SAMP_MAX_ANISOTROPY, 0u);
  const auto maxMipLevel = samplerStateOr(snapshot, SAMP_MAX_MIP_LEVEL, 0u);
  WMTSamplerInfo info{};
  info.min_filter = minFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.mag_filter = magFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  switch (mipFilter) {
    case 2u: info.mip_filter = WMTSamplerMipFilterLinear; break;
    case 1u: info.mip_filter = WMTSamplerMipFilterNearest; break;
    default: info.mip_filter = WMTSamplerMipFilterNotMipmapped; break;
  }
  info.s_address_mode = resolveSamplerAddressMode(addressU);
  info.t_address_mode = resolveSamplerAddressMode(addressV);
  info.r_address_mode = resolveSamplerAddressMode(addressW);
  if (info.s_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.t_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.r_address_mode == WMTSamplerAddressModeClampToBorderColor) {
    info.border_color = resolveSamplerBorderColor(borderColor);
  }
  // Keep explicit texldl / mip-filter sampling from being clamped to level 0.
  // LOD bias is still intentionally ignored because WMTSamplerInfo has no
  // separate bias field. D3DSAMP_MAXMIPLEVEL clamps the sampled mip from
  // above (most-detailed level), and combines with IDirect3DBaseTexture9::
  // SetLOD (already encoded in `lodMinClamp`) as max — picking the coarser
  // (numerically larger) of the two. Wine d3d9 visual.c maxmip_test
  // (gap_d3d9_wine_test §5.1) is the behavioral oracle.
  info.lod_min_clamp = std::max(lodMinClamp, static_cast<float>(maxMipLevel));
  info.lod_max_clamp = 1e9f;
  info.max_anisotroy = maxAnisotropy;
  info.normalized_coords = true;
  // R-BACK-12.22..12.26 (resource-array sub-mode): samplers that ride the
  // slot-30 argbuf need supportArgumentBuffers=YES for a valid gpuResourceID
  // (else .sample() page-faults). Gated on the opt-in lane; default
  // direct-bind path is byte-identical.
  info.support_argument_buffers = dxmt9::shaders::argbufResourceArrayEnabled();
  return info;
}

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const SamplerSnapshot& snapshot) {
  auto info = makeSamplerInfo(snapshot);
  DXMT_ASSERT(device && "makeSampler(snapshot) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMT::Reference<WMT::SamplerState> makeSampler(WMT::Reference<WMT::Device> device,
                                                const SamplerSnapshot& snapshot,
                                                float lodMinClamp) {
  auto info = makeSamplerInfo(snapshot, lodMinClamp);
  DXMT_ASSERT(device && "makeSampler(snapshot,lod) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMTSamplerInfo makeSamplerInfo(const core::FlatStateSet<core::kMaxSamplerStates>& states,
                               float lodMinClamp) {
  const auto minFilter = samplerStateOr(states, SAMP_MIN_FILTER, 0u);
  const auto magFilter = samplerStateOr(states, SAMP_MAG_FILTER, 0u);
  const auto mipFilter = samplerStateOr(states, SAMP_MIP_FILTER, 0u);
  const auto addressU = samplerStateOr(states, SAMP_ADDRESS_U, 1u);
  const auto addressV = samplerStateOr(states, SAMP_ADDRESS_V, 1u);
  const auto addressW = samplerStateOr(states, SAMP_ADDRESS_W, 1u);
  const auto borderColor = samplerStateOr(states, SAMP_BORDER_COLOR, 0u);
  const auto maxAnisotropy = samplerStateOr(states, SAMP_MAX_ANISOTROPY, 0u);
  const auto maxMipLevel = samplerStateOr(states, SAMP_MAX_MIP_LEVEL, 0u);
  WMTSamplerInfo info{};
  info.min_filter = minFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  info.mag_filter = magFilter == 2u ? WMTSamplerMinMagFilterLinear : WMTSamplerMinMagFilterNearest;
  switch (mipFilter) {
    case 2u: info.mip_filter = WMTSamplerMipFilterLinear; break;
    case 1u: info.mip_filter = WMTSamplerMipFilterNearest; break;
    default: info.mip_filter = WMTSamplerMipFilterNotMipmapped; break;
  }
  info.s_address_mode = resolveSamplerAddressMode(addressU);
  info.t_address_mode = resolveSamplerAddressMode(addressV);
  info.r_address_mode = resolveSamplerAddressMode(addressW);
  if (info.s_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.t_address_mode == WMTSamplerAddressModeClampToBorderColor ||
      info.r_address_mode == WMTSamplerAddressModeClampToBorderColor) {
    info.border_color = resolveSamplerBorderColor(borderColor);
  }
  // See FlatStateSet sibling above: combine D3DSAMP_MAXMIPLEVEL with SetLOD
  // (encoded in `lodMinClamp`) as max — Wine d3d9 visual.c maxmip_test.
  info.lod_min_clamp = std::max(lodMinClamp, static_cast<float>(maxMipLevel));
  info.lod_max_clamp = 1e9f;
  info.max_anisotroy = maxAnisotropy;
  info.normalized_coords = true;
  // R-BACK-12.22..12.26 (resource-array sub-mode): samplers that ride the
  // slot-30 argbuf need supportArgumentBuffers=YES for a valid gpuResourceID
  // (else .sample() page-faults). Gated on the opt-in lane; default
  // direct-bind path is byte-identical.
  info.support_argument_buffers = dxmt9::shaders::argbufResourceArrayEnabled();
  return info;
}

WMT::Reference<WMT::SamplerState> makeSampler(
    WMT::Reference<WMT::Device> device,
    const core::FlatStateSet<core::kMaxSamplerStates>& states) {
  auto info = makeSamplerInfo(states);
  DXMT_ASSERT(device && "makeSampler(FlatStateSet) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

WMT::Reference<WMT::SamplerState> makeSampler(
    WMT::Reference<WMT::Device> device,
    const core::FlatStateSet<core::kMaxSamplerStates>& states,
    float lodMinClamp) {
  auto info = makeSamplerInfo(states, lodMinClamp);
  DXMT_ASSERT(device && "makeSampler(FlatStateSet,lod) called with stale/null Metal device handle");
  return device.newSamplerState(info);
}

// R-BACK-15.7 / spec section 4.2: depth/stencil DontCare-store look-ahead.
// Walks the remaining records in the current chunk starting from
// `startCommandIndex + 1` and returns true when one of two proofs holds:
//
//   1. The next record that touches `depthHandle` is a Clear of that
//      handle (the original G3 simple-form shortcut).
//   2. The depth handle never reappears in the rest of the chunk AND
//      no Present was seen during the walk (H1 broadening, R-BACK-15.7
//      end-of-chunk fall-through). R-BACK-15.9 still applies — no
//      cross-chunk look-ahead — but within the current chunk the depth
//      is provably dead, so DontCare-store is safe.
//
// Any prior live read or surface op on the handle (Readback /
// SurfaceCopy / StretchRect / ColorFill source-or-dest, a DrawRun that
// re-binds the handle as depth target, or a DrawRun that samples it as
// a shadow-map texture) flips the proof to defensive Store.
//
// Public so the G4 render-pass-actions fixture can exercise the
// contract without a Metal device.
perf::RenderPassDepthStoreProof depthStoreProofForLookahead(
    std::span<const RenderPassStoreProofLookaheadSource> sources,
    core::Handle depthHandle,
    std::uint32_t* firstTouchCommandDistance) {
  using Proof = perf::RenderPassDepthStoreProof;
  const auto noTouch = std::numeric_limits<std::uint32_t>::max();
  if (firstTouchCommandDistance) {
    *firstTouchCommandDistance = noTouch;
  }
  auto finish = [&](Proof proof, std::size_t logicalDistance) {
    if (firstTouchCommandDistance) {
      *firstTouchCommandDistance =
          static_cast<std::uint32_t>(
              std::min<std::size_t>(logicalDistance, noTouch - 1u));
    }
    return proof;
  };
  if (!depthHandle) {
    return Proof::BlockNullDepth;
  }
  if (sources.empty()) {
    return Proof::BlockNoLookahead;
  }
  using Kind = core::MetalCommandKind;
  bool sawPresent = false;
  std::size_t logicalDistance = 0;
  for (const auto& source : sources) {
    if (!source.slot) {
      return Proof::BlockNoLookahead;
    }
    const auto& slot = *source.slot;
    const std::size_t firstCommandIndex =
        std::min(source.firstCommandIndex, slot.commandCount());
    const std::size_t commandEndIndex =
        std::min(source.commandEndIndex, slot.commandCount());
    for (std::size_t i = firstCommandIndex; i < commandEndIndex; ++i) {
      ++logicalDistance;
      const auto next = slot.commandAt(i);
      switch (next.kind) {
        case Kind::Clear:
          if (next.clear && next.clear->depthStencil.handle == depthHandle) {
            // R-BACK-15.7: the next op on this depth handle is a clear,
            // so storing tile contents would be wasted bandwidth.
            return finish(Proof::AllowNextClear, logicalDistance);
          }
          break;
        case Kind::DrawRun:
          if (next.drawState.hot) {
            if (next.drawState.hot->depthStencil.handle == depthHandle) {
              // Depth is read by a subsequent draw — must Store.
              return finish(Proof::BlockDrawDepth, logicalDistance);
            }
            // R-BACK-15.7 extension: depth-as-shadow-map sample. Walk the
            // active texture bindings and bail if any matches the depth
            // handle (the depth surface is sampled as a texture by this
            // later draw, so its tile contents must be preserved).
            const auto& textures = next.drawState.hot->textures;
            const std::uint32_t mask = next.drawState.hot->textureMask;
            for (std::size_t s = 0; s < textures.size(); ++s) {
              if ((mask & (1u << s)) == 0) continue;
              if (textures[s] == depthHandle) {
                return finish(Proof::BlockShadowSample, logicalDistance);
              }
            }
          }
          break;
        case Kind::SurfaceCopy:
          if (next.surfaceCopy &&
              (next.surfaceCopy->source == depthHandle ||
               next.surfaceCopy->destination == depthHandle)) {
            return finish(Proof::BlockSurfaceCopy, logicalDistance);
          }
          break;
        case Kind::StretchRect:
          if (next.stretchRect &&
              (next.stretchRect->source == depthHandle ||
               next.stretchRect->destination == depthHandle)) {
            return finish(Proof::BlockStretchRect, logicalDistance);
          }
          break;
        case Kind::Readback:
          // R-BACK-15.15: host-visible read of the depth surface must not
          // be served from a DontCare-stored tile.
          if (next.readback &&
              (next.readback->source == depthHandle ||
               next.readback->destination == depthHandle)) {
            return finish(Proof::BlockReadback, logicalDistance);
          }
          break;
        case Kind::ColorFill:
          if (next.colorFill && next.colorFill->destination == depthHandle) {
            return finish(Proof::BlockColorFill, logicalDistance);
          }
          break;
        case Kind::DepthResolve:
          // R-FORMAT-11: a later RESZ resolve reads the MSAA depth surface as
          // its source (and writes the INTZ destination). If either endpoint is
          // this depth handle its tile contents must survive — force a Store
          // exactly like the StretchRect/Readback depth-touch cases above.
          if (next.depthResolve &&
              (next.depthResolve->msaaDepth == depthHandle ||
               next.depthResolve->intzDest == depthHandle)) {
            return finish(Proof::BlockDepthResolve, logicalDistance);
          }
          break;
        case Kind::Present:
          // R-BACK-15.13: a Present in the selected logical suffix implies the
          // frame may persist depth state across that boundary. Don't return
          // early — a later Clear on the same handle still wins.
          sawPresent = true;
          break;
      }
    }
  }
  // R-BACK-15.7 end-of-chunk fall-through. Default keeps the defensive
  // sawPresent guard: a Present in this chunk implies the frame may
  // persist depth state across the chunk boundary (cross-frame shadow
  // map / depth-test reuse), so we Store. Setting
  // DXMT9_AGGRESSIVE_DEPTH_DONTCARE=1 drops the guard so the look-ahead
  // returns DontCare whenever the depth handle does not reappear in
  // the chunk, even when a Present is present. Empirically this is the
  // SFIV win path (Present-per-chunk pattern zeroes the conservative
  // form). Use only on workloads known not to read depth across
  // frames; depth-as-shadow-map within the same chunk is still
  // protected by the texture-sample scan above.
  static const bool aggressive = []() {
    if (const char* v = std::getenv("DXMT9_AGGRESSIVE_DEPTH_DONTCARE")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  if (aggressive) {
    return Proof::AllowDeadNoPresent;
  }
  return sawPresent ? Proof::BlockPresent : Proof::AllowDeadNoPresent;
}

perf::RenderPassDepthStoreProof depthStoreProofForLookahead(
    const core::ChunkSlot& slot,
    std::size_t startCommandIndex,
    core::Handle depthHandle,
    std::uint32_t* firstTouchCommandDistance) {
  const std::size_t firstCommandIndex =
      startCommandIndex < slot.commandCount()
          ? startCommandIndex + 1u
          : slot.commandCount();
  const RenderPassStoreProofLookaheadSource source{
      .slot = &slot,
      .firstCommandIndex = firstCommandIndex,
      .commandEndIndex = slot.commandCount(),
  };
  return depthStoreProofForLookahead(
      std::span<const RenderPassStoreProofLookaheadSource>(&source, 1u),
      depthHandle,
      firstTouchCommandDistance);
}

bool nextDepthOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle depthHandle) {
  const auto proof =
      depthStoreProofForLookahead(slot, startCommandIndex, depthHandle);
  return proof == perf::RenderPassDepthStoreProof::AllowNextClear ||
         proof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent;
}

perf::RenderPassColorStoreProof colorStoreProofForLookahead(
    std::span<const RenderPassStoreProofLookaheadSource> sources,
    core::Handle colorHandle,
    std::uint32_t* firstTouchCommandDistance) {
  using Proof = perf::RenderPassColorStoreProof;
  const auto noTouch = std::numeric_limits<std::uint32_t>::max();
  if (firstTouchCommandDistance) {
    *firstTouchCommandDistance = noTouch;
  }
  auto finish = [&](Proof proof, std::size_t logicalDistance) {
    if (firstTouchCommandDistance) {
      *firstTouchCommandDistance =
          static_cast<std::uint32_t>(
              std::min<std::size_t>(logicalDistance, noTouch - 1u));
    }
    return proof;
  };
  if (!colorHandle) {
    return Proof::BlockNullColor;
  }
  if (sources.empty()) {
    return Proof::BlockNoLookahead;
  }
  using Kind = core::MetalCommandKind;
  bool sawPresent = false;
  std::size_t logicalDistance = 0;
  for (const auto& source : sources) {
    if (!source.slot) {
      return Proof::BlockNoLookahead;
    }
    const auto& slot = *source.slot;
    const std::size_t firstCommandIndex =
        std::min(source.firstCommandIndex, slot.commandCount());
    const std::size_t commandEndIndex =
        std::min(source.commandEndIndex, slot.commandCount());
    for (std::size_t i = firstCommandIndex; i < commandEndIndex; ++i) {
      ++logicalDistance;
      const auto next = slot.commandAt(i);
      switch (next.kind) {
        case Kind::Clear:
          if (next.clear && next.clear->clearColor) {
            for (const auto& attachment : next.clear->colorAttachments) {
              if (attachment.handle == colorHandle) {
                return finish(Proof::AllowNextClear, logicalDistance);
              }
            }
          }
          break;
        case Kind::DrawRun:
          if (next.drawState.hot) {
            const auto& hot = *next.drawState.hot;
            for (const auto& attachment : hot.colorAttachments) {
              if (attachment.handle == colorHandle) {
                return finish(Proof::BlockDrawTarget, logicalDistance);
              }
            }
            const auto& textures = hot.textures;
            const std::uint32_t mask = hot.textureMask;
            for (std::size_t s = 0; s < textures.size(); ++s) {
              if ((mask & (1u << s)) == 0) continue;
              if (textures[s] == colorHandle) {
                return finish(Proof::BlockTextureSample, logicalDistance);
              }
            }
          }
          break;
        case Kind::SurfaceCopy:
          if (next.surfaceCopy &&
              (next.surfaceCopy->source == colorHandle ||
               next.surfaceCopy->destination == colorHandle)) {
            return finish(Proof::BlockSurfaceCopy, logicalDistance);
          }
          break;
        case Kind::StretchRect:
          if (next.stretchRect &&
              (next.stretchRect->source == colorHandle ||
               next.stretchRect->destination == colorHandle)) {
            return finish(Proof::BlockStretchRect, logicalDistance);
          }
          break;
        case Kind::Readback:
          if (next.readback &&
              (next.readback->source == colorHandle ||
               next.readback->destination == colorHandle)) {
            return finish(Proof::BlockReadback, logicalDistance);
          }
          break;
        case Kind::ColorFill:
          if (next.colorFill && next.colorFill->destination == colorHandle) {
            return finish(Proof::BlockColorFill, logicalDistance);
          }
          break;
        case Kind::Present:
          if (next.present && next.present->presentSource == colorHandle) {
            return finish(Proof::BlockPresent, logicalDistance);
          }
          sawPresent = true;
          break;
        case Kind::DepthResolve:
          break;
      }
    }
  }
  static const bool aggressive = []() {
    if (const char* v = std::getenv("DXMT9_AGGRESSIVE_COLOR_DONTCARE")) {
      return v[0] != '\0' && v[0] != '0';
    }
    return false;
  }();
  if (sawPresent) {
    return Proof::BlockPresent;
  }
  return aggressive ? Proof::AllowDeadNoPresent
                    : Proof::BlockDeadNoPresentDisabled;
}

perf::RenderPassColorStoreProof colorStoreProofForLookahead(
    const core::ChunkSlot& slot,
    std::size_t startCommandIndex,
    core::Handle colorHandle,
    std::uint32_t* firstTouchCommandDistance) {
  const std::size_t firstCommandIndex =
      startCommandIndex < slot.commandCount()
          ? startCommandIndex + 1u
          : slot.commandCount();
  const RenderPassStoreProofLookaheadSource source{
      .slot = &slot,
      .firstCommandIndex = firstCommandIndex,
      .commandEndIndex = slot.commandCount(),
  };
  return colorStoreProofForLookahead(
      std::span<const RenderPassStoreProofLookaheadSource>(&source, 1u),
      colorHandle,
      firstTouchCommandDistance);
}

bool nextColorOperationIsClear(const core::ChunkSlot& slot,
                               std::size_t startCommandIndex,
                               core::Handle colorHandle) {
  return colorStoreProofForLookahead(slot, startCommandIndex, colorHandle) ==
         perf::RenderPassColorStoreProof::AllowNextClear;
}

RenderPassStoreProofSummary renderPassStoreProofSummaryForLookahead(
    EncodeContext& ctx,
    std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    const core::FlatDrawStateRecord& hot) {
  RenderPassStoreProofSummary summary{};
  auto* colorSurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  const bool colorIncluded =
      colorSurface && colorSurface->texture &&
      colorSurface->desc.format != core::Format::NullRt;
  if (colorIncluded) {
    summary.color = !lookaheadSources.empty()
        ? colorStoreProofForLookahead(
              lookaheadSources,
              hot.colorAttachments[0].handle,
              &summary.colorTouchDistance)
        : perf::RenderPassColorStoreProof::BlockNoLookahead;
    if (colorSurface->resolveTexture &&
        (summary.color == perf::RenderPassColorStoreProof::AllowNextClear ||
         summary.color == perf::RenderPassColorStoreProof::AllowDeadNoPresent)) {
      summary.color = perf::RenderPassColorStoreProof::BlockMsaaResolve;
    }
  }

  auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
  if (depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    summary.depth = !lookaheadSources.empty()
        ? depthStoreProofForLookahead(
              lookaheadSources,
              hot.depthStencil.handle,
              &summary.depthTouchDistance)
        : perf::RenderPassDepthStoreProof::BlockNoLookahead;
    if (depthSurface->resolveTexture &&
        (summary.depth == perf::RenderPassDepthStoreProof::AllowNextClear ||
         summary.depth == perf::RenderPassDepthStoreProof::AllowDeadNoPresent)) {
      summary.depth = perf::RenderPassDepthStoreProof::BlockMsaaResolve;
    }
  }
  return summary;
}

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPassWithStoreProofLookahead(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear,
    std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
    WMT::Buffer visibilityBuffer,
    RenderPassActionSummary* actionSummary) {
  const auto& hot = *drawState.hot;
  if (actionSummary) {
    *actionSummary = {};
  }
  auto* primarySurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  // R-FORMAT-12: a D3DFMT_NULL render target is colorless and has no Metal
  // color texture by design. When RT0 is a NULL render target the render
  // pass is depth/stencil-only — proceed (the per-attachment loop below
  // omits every color attachment that has no texture, so the NULL RT
  // contributes no color attachment and the bound depth/stencil becomes
  // the effective target). Only abort when RT0 is genuinely missing, or it
  // is a normal color RT that failed to allocate its texture.
  const ColorlessRenderPassRt0 rt0{
      .surfaceExists = primarySurface != nullptr,
      .hasTexture = primarySurface && static_cast<bool>(primarySurface->texture),
      .isNullRt =
          primarySurface && primarySurface->desc.format == core::Format::NullRt,
  };
  if (!renderPassAdmitsRt0(rt0)) {
    return {};
  }
  WMTRenderPassInfo passInfo{};
  const bool discardAfterPresent = !clear.has_value() && ctx.queue.backBufferDiscardAfterPresent_ &&
                                   hot.colorAttachments[0].handle == ctx.queue.currentBackBuffer_;
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto* surface = ctx.pool.findSurface(hot.colorAttachments[i].handle.value);
    // R-FORMAT-12: omit any color slot whose surface owns no backend texture
    // (a NULL render target is the colorless case). See colorAttachmentIncluded.
    if (!colorAttachmentIncluded(surface != nullptr,
                                 surface && static_cast<bool>(surface->texture))) {
      continue;
    }
    auto& attachment = passInfo.colors[i];
    const bool srgbWrite =
        core::flatStateOr(hot.renderStates, core::RS_SRGB_WRITE_ENABLE, 0u) != 0;
    attachment.texture =
        (srgbWrite && surface->srgbTexture) ? surface->srgbTexture.handle
                                            : surface->texture.handle;
    const bool discardAttachment = discardAfterPresent && i == 0;
    const bool clearAttachment =
        clearMatchesColorAttachment(clear, i, hot.colorAttachments[i].handle);
    // R-BACK-15.4: first-use of a color RT (handle not yet in the
    // queue-local touched set) may DontCare-load. Precedence:
    // Clear > post-present DontCare > first-use DontCare > Load.
    const bool firstUseAttachment =
        !clearAttachment && !discardAttachment &&
        !ctx.queue.isColorHandleTouched(hot.colorAttachments[i].handle);
    attachment.load_action = clearAttachment       ? WMTLoadActionClear
                           : discardAttachment     ? WMTLoadActionDontCare
                           : firstUseAttachment    ? WMTLoadActionDontCare
                                                   : WMTLoadActionLoad;
    auto colorStoreProof = !lookaheadSources.empty()
        ? colorStoreProofForLookahead(lookaheadSources,
                                      hot.colorAttachments[i].handle)
        : perf::RenderPassColorStoreProof::BlockNoLookahead;
    if (surface->resolveTexture &&
        (colorStoreProof == perf::RenderPassColorStoreProof::AllowNextClear ||
         colorStoreProof == perf::RenderPassColorStoreProof::AllowDeadNoPresent)) {
      colorStoreProof = perf::RenderPassColorStoreProof::BlockMsaaResolve;
    }
    perf::countRenderPassColorStoreProof(colorStoreProof);
    const bool colorDontCareStore =
        colorStoreProof == perf::RenderPassColorStoreProof::AllowNextClear ||
        colorStoreProof == perf::RenderPassColorStoreProof::AllowDeadNoPresent;
    attachment.store_action =
        colorDontCareStore ? WMTStoreActionDontCare : WMTStoreActionStore;
    if (surface->resolveTexture) {
      attachment.resolve_texture = surface->resolveTexture.handle;
      attachment.store_action = WMTStoreActionMultisampleResolve;
    }
    if (actionSummary) {
      ++actionSummary->colorAttachmentCount;
      if (i == 0) {
        actionSummary->color0Included = 1;
        actionSummary->color0LoadAction =
            static_cast<std::uint64_t>(attachment.load_action);
        actionSummary->color0StoreAction =
            static_cast<std::uint64_t>(attachment.store_action);
        actionSummary->color0Clear = clearAttachment ? 1u : 0u;
      }
    }
    if (clearAttachment) {
      attachment.clear_color = WMTClearColor{clear->color.r, clear->color.g,
                                             clear->color.b, clear->color.a};
    }
  }

  if (auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
      depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    const bool clearDepth = clearMatchesDepthStencilAttachment(clear, hot.depthStencil.handle, false);
    const bool clearStencil = clearMatchesDepthStencilAttachment(clear, hot.depthStencil.handle, true);
    // R-BACK-15.7 simple form (specs/backend/render-pass-actions/design.md
    // section 4.2): in-chunk look-ahead — if the very next op on this
    // depth handle is a Clear, the about-to-be-stored tile contents are
    // immediately discarded, so we can DontCare-store. R-BACK-15.14:
    // never DontCare an MSAA depth target with an attached resolve.
    const bool hasResolveTarget = static_cast<bool>(depthSurface->resolveTexture);
    auto depthStoreProof = !lookaheadSources.empty()
        ? depthStoreProofForLookahead(lookaheadSources,
                                      hot.depthStencil.handle)
        : perf::RenderPassDepthStoreProof::BlockNoLookahead;
    if (hasResolveTarget &&
        (depthStoreProof == perf::RenderPassDepthStoreProof::AllowNextClear ||
         depthStoreProof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent)) {
      depthStoreProof = perf::RenderPassDepthStoreProof::BlockMsaaResolve;
    }
    perf::countRenderPassDepthStoreProof(depthStoreProof);
    const bool depthDontCareStore =
        !hasResolveTarget &&
        (depthStoreProof == perf::RenderPassDepthStoreProof::AllowNextClear ||
         depthStoreProof == perf::RenderPassDepthStoreProof::AllowDeadNoPresent);
    if (formatHasDepthAspect(depthSurface->desc.format)) {
      passInfo.depth.texture = depthSurface->texture.handle;
      passInfo.depth.load_action = clearDepth ? WMTLoadActionClear : WMTLoadActionLoad;
      passInfo.depth.store_action =
          depthDontCareStore ? WMTStoreActionDontCare : WMTStoreActionStore;
      if (actionSummary) {
        actionSummary->depthIncluded = 1;
        actionSummary->depthLoadAction =
            static_cast<std::uint64_t>(passInfo.depth.load_action);
        actionSummary->depthStoreAction =
            static_cast<std::uint64_t>(passInfo.depth.store_action);
        actionSummary->depthClear = clearDepth ? 1u : 0u;
      }
      if (clearDepth) {
        passInfo.depth.clear_depth = clear->depth;
      }
    }
    if (formatHasStencilAspect(depthSurface->desc.format)) {
      passInfo.stencil.texture = depthSurface->texture.handle;
      passInfo.stencil.load_action = clearStencil ? WMTLoadActionClear : WMTLoadActionLoad;
      // The simple-form shortcut clears the entire depth/stencil surface
      // on the next op, so stencil tile contents are equally discardable.
      passInfo.stencil.store_action =
          depthDontCareStore ? WMTStoreActionDontCare : WMTStoreActionStore;
      if (actionSummary) {
        actionSummary->stencilIncluded = 1;
        actionSummary->stencilLoadAction =
            static_cast<std::uint64_t>(passInfo.stencil.load_action);
        actionSummary->stencilStoreAction =
            static_cast<std::uint64_t>(passInfo.stencil.store_action);
        actionSummary->stencilClear = clearStencil ? 1u : 0u;
      }
      if (clearStencil) {
        passInfo.stencil.clear_stencil = clear->stencil;
      }
    }
  }

  constexpr std::size_t kMaxSampleBufferAttachments =
      sizeof(passInfo.sample_buffer_attachments) /
      sizeof(passInfo.sample_buffer_attachments[0]);
  const auto attachmentCount = std::min<std::size_t>(
      sampleBufferAttachments.size(), kMaxSampleBufferAttachments);
  for (std::size_t i = 0; i < attachmentCount; ++i) {
    passInfo.sample_buffer_attachments[i] = sampleBufferAttachments[i];
  }
  passInfo.num_sample_buffer_attachments =
      static_cast<std::uint8_t>(attachmentCount);
  passInfo.visibility_buffer = visibilityBuffer.handle;

  // R-BACK-15.10/15.11/15.12: emit per-attachment load/store action
  // histograms + tile-preservation byte estimates so scripts/
  // assert_perf_counters.py and the SFIV smoke can see the policy
  // outcome. Counted before encoder open so the counters land even when
  // the encoder fails to open below.
  for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
    auto* surface = ctx.pool.findSurface(hot.colorAttachments[i].handle.value);
    if (!surface || !surface->texture) continue;
    const auto& att = passInfo.colors[i];
    perf::countRenderPassLoadActionColor(static_cast<std::uint32_t>(att.load_action));
    perf::countRenderPassStoreActionColor(static_cast<std::uint32_t>(att.store_action));
    const std::uint64_t pixelBytes =
        static_cast<std::uint64_t>(surface->desc.width) *
        static_cast<std::uint64_t>(surface->desc.height) *
        static_cast<std::uint64_t>(core::bytesPerPixel(surface->desc.format));
    if (att.load_action == WMTLoadActionLoad) {
      perf::countRenderPassTilePreservationBytes(pixelBytes);
      if (actionSummary) {
        actionSummary->colorLoadBytes += pixelBytes;
      }
    }
    if (att.store_action == WMTStoreActionStore ||
        att.store_action == WMTStoreActionMultisampleResolve) {
      perf::countRenderPassTilePreservationBytes(pixelBytes);
      if (actionSummary) {
        actionSummary->colorStoreBytes += pixelBytes;
      }
    }
  }
  if (auto* depthSurface = ctx.pool.findSurface(hot.depthStencil.handle.value);
      depthSurface && depthSurface->texture && depthSurface->desc.depthStencil) {
    const std::uint64_t depthPixelBytes =
        static_cast<std::uint64_t>(depthSurface->desc.width) *
        static_cast<std::uint64_t>(depthSurface->desc.height) *
        static_cast<std::uint64_t>(core::bytesPerPixel(depthSurface->desc.format));
    if (formatHasDepthAspect(depthSurface->desc.format)) {
      perf::countRenderPassLoadActionDepth(
          static_cast<std::uint32_t>(passInfo.depth.load_action));
      perf::countRenderPassStoreActionDepth(
          static_cast<std::uint32_t>(passInfo.depth.store_action));
      if (passInfo.depth.load_action == WMTLoadActionLoad) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
        if (actionSummary) {
          actionSummary->depthLoadBytes += depthPixelBytes;
        }
      }
      if (passInfo.depth.store_action == WMTStoreActionStore ||
          passInfo.depth.store_action == WMTStoreActionMultisampleResolve) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
        if (actionSummary) {
          actionSummary->depthStoreBytes += depthPixelBytes;
        }
      }
    }
    if (formatHasStencilAspect(depthSurface->desc.format)) {
      perf::countRenderPassLoadActionStencil(
          static_cast<std::uint32_t>(passInfo.stencil.load_action));
      perf::countRenderPassStoreActionStencil(
          static_cast<std::uint32_t>(passInfo.stencil.store_action));
      if (passInfo.stencil.load_action == WMTLoadActionLoad) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
        if (actionSummary) {
          actionSummary->stencilLoadBytes += depthPixelBytes;
        }
      }
      if (passInfo.stencil.store_action == WMTStoreActionStore ||
          passInfo.stencil.store_action == WMTStoreActionMultisampleResolve) {
        perf::countRenderPassTilePreservationBytes(depthPixelBytes);
        if (actionSummary) {
          actionSummary->stencilStoreBytes += depthPixelBytes;
        }
      }
    }
  }

  auto encoder = commandBuffer.renderCommandEncoder(passInfo);
  if (!encoder) {
    return {};
  }
  perf::countRenderPassBegin();
  // R-BACK-14.3 — issue `useHeap` once per heap instance that actually
  // backs a resource bound on this encoder. Walking the active draw
  // state's stream/index buffers + sampler textures and consulting each
  // record's `isHeapBacked` flag avoids the over-issue case where every
  // pool heap (including heaps holding resources unrelated to this
  // encoder) was made resident at encoder open. The dedup buffer is
  // sized to the static binding cap (kMaxStreams streams + 1 index
  // buffer + kMaxSamplers texture stages = 33 bindings, all of which
  // share the same handful of heap instances per family) so this stays
  // a fixed-size, allocation-free walk on the encoder-open hot path.
  {
    constexpr std::size_t kMaxBoundHeaps =
        core::kMaxStreams + 1u + core::kMaxSamplers;
    std::array<obj_handle_t, kMaxBoundHeaps> usedHeaps{};
    std::size_t usedHeapCount = 0;
    auto pushHeap = [&](WMT::Heap heap) {
      const obj_handle_t h = heap.handle;
      if (h == 0) return;
      for (std::size_t i = 0; i < usedHeapCount; ++i) {
        if (usedHeaps[i] == h) return;
      }
      if (usedHeapCount < usedHeaps.size()) {
        usedHeaps[usedHeapCount++] = h;
      }
    };
    auto considerBuffer = [&](core::Handle handle) {
      if (!handle) return;
      if (auto* rec = ctx.pool.findBuffer(handle.value); rec && rec->isHeapBacked) {
        pushHeap(rec->heap);
      }
    };
    auto considerTexture = [&](core::Handle handle) {
      if (!handle) return;
      if (auto* rec = ctx.pool.findTexture(handle.value); rec && rec->isHeapBacked) {
        pushHeap(rec->heap);
      }
    };
    considerBuffer(hot.indexBuffer);
    for (const auto& streamHandle : hot.streamBuffers) {
      considerBuffer(streamHandle);
    }
    for (const auto& textureHandle : hot.textures) {
      considerTexture(textureHandle);
    }
    for (std::size_t i = 0; i < usedHeapCount; ++i) {
      encoder.useHeap(WMT::Heap{usedHeaps[i]});
      perf::countUseHeap();
    }
  }
  if (discardAfterPresent) {
    ctx.queue.backBufferDiscardAfterPresent_ = false;
  }
  const auto ffLayout = drawState.hasShaderContext()
      ? decodeFixedFunctionVertexLayout(drawState.shaderContext().vertexDecl)
      : std::optional<dxmt9::ffp::FixedFunctionVertexLayout>{};
  double viewportWidth = static_cast<double>(std::max(1u, hot.viewport.viewport.width));
  double viewportHeight = static_cast<double>(std::max(1u, hot.viewport.viewport.height));
  double viewportOriginX = static_cast<double>(hot.viewport.viewport.x);
  double viewportOriginY = static_cast<double>(hot.viewport.viewport.y);
  if (ffLayout && ffLayout->preTransformed) {
    viewportOriginX = 0.0;
    viewportOriginY = 0.0;
    viewportWidth = static_cast<double>(std::max(1u, primarySurface->desc.width));
    viewportHeight = static_cast<double>(std::max(1u, primarySurface->desc.height));
  }
  WMTViewport vp{viewportOriginX, viewportOriginY, viewportWidth, viewportHeight,
                 static_cast<double>(hot.viewport.viewport.minZ),
                 static_cast<double>(hot.viewport.viewport.maxZ)};
  encoder.setViewport(vp);
  countViewportBind();
  // D3D9 RS_DEPTH_BIAS / RS_SLOPE_SCALE_DEPTH_BIAS are stored as DWORDs but
  // semantically float (see setRasterizerCullMode for the per-draw equivalent).
  // Prologue value is the initial baseline; per-draw rebinds will override.
  const float prologueDepthBias = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_DEPTH_BIAS, 0u));
  const float prologueSlopeScale = std::bit_cast<float>(
      core::flatStateOr(hot.renderStates, core::RS_SLOPE_SCALE_DEPTH_BIAS, 0u));
  encoder.setRasterizerState(WMTTriangleFillModeFill, WMTCullModeNone,
                              WMTDepthClipModeClip, frontFaceWinding(),
                              prologueDepthBias, prologueSlopeScale, 0.0f);
  countRasterizerBind();
  return WMT::Reference<WMT::RenderCommandEncoder>(encoder);
}

WMT::Reference<WMT::RenderCommandEncoder> beginRenderPass(
    EncodeContext& ctx,
    WMT::CommandBuffer& commandBuffer,
    core::FlatDrawStateView drawState,
    const std::optional<ClearDesc>& clear,
    const core::ChunkSlot* lookaheadSlot,
    std::size_t lookaheadStartIndex,
    std::span<const WMTSampleBufferAttachmentInfo> sampleBufferAttachments,
    WMT::Buffer visibilityBuffer,
    RenderPassActionSummary* actionSummary) {
  RenderPassStoreProofLookaheadSource lookaheadSource{};
  std::span<const RenderPassStoreProofLookaheadSource> lookaheadSources{};
  if (lookaheadSlot) {
    const std::size_t firstCommandIndex =
        lookaheadStartIndex < lookaheadSlot->commandCount()
            ? lookaheadStartIndex + 1u
            : lookaheadSlot->commandCount();
    lookaheadSource = RenderPassStoreProofLookaheadSource{
        .slot = lookaheadSlot,
        .firstCommandIndex = firstCommandIndex,
        .commandEndIndex = lookaheadSlot->commandCount(),
    };
    lookaheadSources =
        std::span<const RenderPassStoreProofLookaheadSource>(&lookaheadSource, 1u);
  }
  return beginRenderPassWithStoreProofLookahead(
      ctx, commandBuffer, drawState, clear, lookaheadSources,
      sampleBufferAttachments, visibilityBuffer, actionSummary);
}

std::span<const u8> drawParamVertexBytes(const core::DrawParam& param,
                                         std::span<const u8> arena) {
  if (!param.userVertexRange.empty()) {
    return core::drawRunPayloadBytes(param.userVertexRange, arena);
  }
  return {};
}

std::span<const u8> drawParamIndexBytes(const core::DrawParam& param,
                                        std::span<const u8> arena) {
  if (!param.userIndexRange.empty()) {
    return core::drawRunPayloadBytes(param.userIndexRange, arena);
  }
  return {};
}

bool drawParamBindingOverride(const core::DrawParam& param,
                              std::span<const u8> arena,
                              core::DrawBindingOverride& out) {
  const auto bytes = core::drawRunPayloadBytes(param.bindingOverrideRange, arena);
  if (bytes.size() != sizeof(core::DrawBindingOverride)) {
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return !core::drawBindingOverrideEmpty(out);
}

bool drawParamBindingSnapshot(const core::DrawParam& param,
                              std::span<const u8> arena,
                              core::DrawBindingSnapshot& out) {
  const auto bytes = core::drawRunPayloadBytes(param.bindingSnapshotRange, arena);
  if (bytes.size() != sizeof(core::DrawBindingSnapshot)) {
    return false;
  }
  std::memcpy(&out, bytes.data(), sizeof(out));
  return !core::drawBindingSnapshotEmpty(out);
}

const core::DrawBufferBindingSnapshot* streamBindingSnapshot(
    const core::DrawBindingSnapshot* binding,
    u32 stream) noexcept {
  if (!binding || stream >= core::kMaxStreams ||
      (binding->streamMask & (1u << stream)) == 0u ||
      !binding->streams[stream].snapshot.valid()) {
    return nullptr;
  }
  return &binding->streams[stream].snapshot;
}

const core::DrawBufferBindingSnapshot* indexBindingSnapshot(
    const core::DrawBindingSnapshot* binding) noexcept {
  if (!binding || !binding->indexSnapshotValid ||
      !binding->indexSnapshot.valid()) {
    return nullptr;
  }
  return &binding->indexSnapshot;
}

std::span<const u8> snapshotBufferBytes(
    const core::DrawBufferBindingSnapshot* snapshot) noexcept {
  if (!snapshot || snapshot->contentsAddress == 0 || snapshot->byteSize == 0) {
    return {};
  }
  return std::span<const u8>(
      reinterpret_cast<const u8*>(
          static_cast<std::uintptr_t>(snapshot->contentsAddress)),
      static_cast<std::size_t>(snapshot->byteSize));
}

void applyDrawBindingOverride(core::FlatDrawStateRecord& hot,
                              core::DrawShaderLayoutContext* shaderLayout,
                              const core::DrawBindingOverride& binding) {
  for (u32 stream = 0; stream < core::kMaxStreams; ++stream) {
    if ((binding.streamMask & (1u << stream)) == 0) {
      continue;
    }
    const bool bufferHandleChanged =
        hot.streamBuffers[stream] != binding.streams[stream].buffer;
    hot.streamBuffers[stream] = binding.streams[stream].buffer;
    hot.streamOffsets[stream] = binding.streams[stream].offset;
    hot.streamStrides[stream] = binding.streams[stream].stride;
    hot.key.streamBuffers[stream] = hot.streamBuffers[stream];
    hot.key.streamOffsets[stream] = hot.streamOffsets[stream];
    hot.key.streamStrides[stream] = hot.streamStrides[stream];
    if (shaderLayout) {
      auto& streamBinding = shaderLayout->vertexDecl.streams[stream];
      if (bufferHandleChanged) {
        streamBinding.buffer.reset();
      }
      streamBinding.offset = binding.streams[stream].offset;
      streamBinding.stride = binding.streams[stream].stride;
    }
  }
  hot.streamMask = 0;
  for (u32 stream = 0; stream < core::kMaxStreams; ++stream) {
    if (hot.streamBuffers[stream]) {
      hot.streamMask |= 1u << stream;
    }
  }
  hot.key.streamMask = hot.streamMask;
  if (binding.indexBufferValid) {
    hot.indexBuffer = binding.indexBuffer;
    hot.key.indexBuffer = binding.indexBuffer;
  }
}

bool drawUsesFixedFunctionPath(core::FlatDrawStateView drawState, bool hasFfpLayout) {
  if (!drawState.hasShaderContext()) {
    return hasFfpLayout;
  }
  return drawState.shaderContext().vertexShader.kind == core::ShaderRef::Kind::FixedFunctionVertex;
}

bool encodeDraw(EncodeContext& ctx,
                 WMT::CommandBuffer& commandBuffer,
                 WMT::RenderCommandEncoder& encoder,
                 core::FlatDrawStateView drawState,
                 u64 seqId,
                 bool skipBaseStateBind,
                 const PreUploadedDrawData* preUploaded,
                 const core::DrawParam* paramOverride,
                 std::span<const u8> paramPayloadArena,
                 const core::DrawBindingSnapshot* bindingSnapshot,
                 uniform::DirtyState* dirty,
                 bool tileFfpMode,
                 bool argbufHybridMode,
                 bool argbufResourceArray,
                 bool argbufDirectCbufMode,
                 bool reopenArgbufHybrid,
                 bool argbufVsPayloadSourceChanged,
                 bool argbufPsPayloadSourceChanged,
                 bool bindingOverridePrefetchedPsoCompatible,
                 core::PsoHandle renderPsoHandle,
                 core::PsoHandle tilePsoHandle,
                 core::DepthStencilHandle depthStencilHandle,
                 TextureSamplerBindShadow* textureSamplerShadow,
                 std::uint32_t commandIndex,
                 u64 commandDrawIndex,
                 u64 commandDrawCount,
                 ActiveEncoderBreakdown* encoderBreakdown,
                 ArgbufCbufCache* argbufCbufCache,
                 StreamIbStagingCache* streamIbStagingCache,
                 VisibilityScoutPass* visibilityScout) {
  // Hot per-draw entry. Per codebase_conventions.rules.md, no heap allocation
  // is permitted on this path; the guard is debug-only and asserts this when
  // DXMT_DEBUG_NO_PER_DRAW_ALLOC=1 is set in env. See dxmt9_debug_alloc_guard.
  DXMT_DEBUG_NO_HEAP_ALLOC_SCOPE("encodeDraw");
  PerfScope scope(perf::countEncodeDrawCpuTime);
  emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                              commandDrawCount, "enter");
  // M3 — per-draw Instruments interval. os_signpost_id_generate gives each
  // call a unique paired id so overlapping work surfaces correctly in
  // Instruments. No-op when no consumer is recording (~5 ns).
  os_log_t signpostLog = dxmt9::signposts::log();
  os_signpost_id_t drawSignpost = os_signpost_id_generate(signpostLog);
  os_signpost_interval_begin(signpostLog, drawSignpost, "draw",
                             "seq=%llu",
                             static_cast<unsigned long long>(seqId));
  struct DrawSignpostScope {
    os_log_t log;
    os_signpost_id_t id;
    ~DrawSignpostScope() {
      os_signpost_interval_end(log, id, "draw");
    }
  } drawSignpostScope{signpostLog, drawSignpost};
  // M2: per-draw debug group, paired via DebugGroupScope's dtor on
  // every return path (including early-return failures below).
  // primitiveCount may be zero pre-paramOverride; that's OK — captures
  // see whatever is encoded.
  const auto drawDebugPrimCount = paramOverride ? paramOverride->primitiveCount : 0u;
  std::optional<DebugGroupScope> drawDebugGroup;
  if (!suppressRecordedMetalCalls(ctx)) {
    drawDebugGroup.emplace(
        WMT::CommandEncoder{encoder.handle},
        makeLabelStringFmt("Draw[seq=%llu,prim=%u]",
            static_cast<unsigned long long>(seqId), drawDebugPrimCount));
  }
  (void)commandBuffer;
  const auto& hot = *drawState.hot;
  const auto& shader = drawState.shaderContext();
  const auto& vertexDecl = shader.vertexDecl;
  const auto* debug = drawState.hasDebugSnapshot() ? &drawState.debugSnapshot() : nullptr;
  const ParamView pv = paramOverride
      ? ParamView{paramOverride->primitiveType,
                  paramOverride->primitiveCount,
                  paramOverride->startVertex,
                  paramOverride->baseVertexIndex,
                  paramOverride->startIndex,
                  paramOverride->indexType,
                  paramOverride->indexed,
                  drawParamVertexBytes(*paramOverride, paramPayloadArena),
                  drawParamIndexBytes(*paramOverride, paramPayloadArena)}
      : ParamView{debug ? debug->primitiveType : core::PrimitiveType::TriangleList,
                  debug ? debug->primitiveCount : 0u,
                  debug ? debug->startVertex : 0u,
                  debug ? debug->baseVertexIndex : 0,
                  debug ? debug->startIndex : 0u,
                  debug ? debug->indexType : IndexType::UInt16,
                  false,
                  {},
                  {}};
  const bool traceEncode = debug::shouldTraceEncode(hot, seqId) ||
                           colorAttachmentAliasesTracedTexture(ctx.pool, hot);
  const bool effectiveArgbufDirectCbufMode =
      argbufHybridMode && !argbufResourceArray && argbufDirectCbufMode;
  const bool argbufTableMode =
      argbufHybridMode && !effectiveArgbufDirectCbufMode;
  const bool directCbufBindings = !argbufTableMode;
  if (debug::skipAllDraws()) {
    if (queueTraceEnabled() || traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip all draws seq=" << static_cast<unsigned long long>(seqId)
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value);
      emitQueueTraceLine(out.str());
    }
    return false;
  }
  if (debug::shouldSkipDrawSeq(seqId)) {
    if (queueTraceEnabled() || traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
          << " reason=seq-range"
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value);
      emitQueueTraceLine(out.str());
    }
    return false;
  }
  const u64 drawOrdinal = debug::nextDrawOrdinal();
  traceRenderTargetWriteForTexture(ctx.pool, hot, seqId, drawOrdinal);
  if (debug::shouldSkipDrawOrdinal(drawOrdinal)) {
    if (queueTraceEnabled() || traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
          << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
          << " reason=ordinal-range"
          << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value);
      emitQueueTraceLine(out.str());
    }
    return false;
  }
  if (!encoder) {
    if (traceEncode) {
      emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) +
                         " ordinal=" + std::to_string(drawOrdinal) +
                         " skipped reason=no-encoder");
    }
    return false;
  }
  const bool depthStateProbeRequested =
      debug::probeDisableDepthWrite() || debug::probeDepthFuncAlways();
  std::optional<dxmt9::ffp::FixedFunctionVertexLayout> ffLayout;
  bool fixedFunctionPath = false;
  {
    PerfScope fvfDecodeScope(perf::countEncodeDrawFvfDecodeCpuTime);
    ffLayout = decodeFixedFunctionVertexLayout(vertexDecl);
    fixedFunctionPath = drawUsesFixedFunctionPath(drawState, static_cast<bool>(ffLayout));
  }
  emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                              commandDrawCount, "after-fvf-decode");
  const u32 primitiveCount = std::max<u32>(1, pv.primitiveCount);
  const uint64_t vertexCount =
      static_cast<uint64_t>(std::max(1u, primitiveVertexCount(pv.primitiveType, primitiveCount)));
  const bool indexedDraw = pv.indexed && (hot.indexBuffer || !pv.userIndexData.empty());
  const auto primitiveType = toPrimitiveType(pv.primitiveType);
  const bool preTransformed = ffLayout && ffLayout->preTransformed;
  const auto fillMode = triangleFillModeFromRenderState(hot.renderStates);
  const bool indexedTriangleDraw =
      indexedDraw && pv.primitiveType == core::PrimitiveType::TriangleList;
  const bool disableDepthWriteProbeApplied =
      debug::probeDisableDepthWrite() &&
      indexedTriangleDraw &&
      disableDepthWriteProbeRowMatches(encoderBreakdown) &&
      disableDepthWriteProbeClassMatches(primitiveCount,
                                         hot.textureMask,
                                         hot.renderStates,
                                         hot.viewport,
                                         fillMode);
  const bool disableAlphaBlendProbeApplied =
      debug::probeDisableAlphaBlend() &&
      indexedTriangleDraw &&
      disableAlphaBlendProbeRowMatches(encoderBreakdown) &&
      indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
      disableAlphaBlendProbeTextureMatches(drawState, ctx.pool) &&
      disableAlphaBlendProbeClassMatches(primitiveCount,
                                         hot.textureMask,
                                         hot.renderStates,
                                         hot.viewport,
                                         fillMode);
  const bool depthFuncAlwaysProbeApplied =
      debug::probeDepthFuncAlways() &&
      indexedTriangleDraw &&
      depthFuncAlwaysProbeRowMatches(encoderBreakdown) &&
      indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
      depthFuncAlwaysProbeTextureMatches(drawState, ctx.pool) &&
      depthFuncAlwaysProbeClassMatches(primitiveCount,
                                       hot.textureMask,
                                       hot.renderStates,
                                       hot.viewport,
                                       fillMode);
  const bool forceTextureWhiteProbeApplied =
      debug::probeForceTextureWhite() &&
      indexedTriangleDraw &&
      forceTextureWhiteProbeRowMatches(encoderBreakdown) &&
      indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
      forceTextureWhiteProbeTextureMatches(drawState, ctx.pool) &&
      forceTextureWhiteProbeDrawOrdinalMatches(drawOrdinal) &&
      forceTextureWhiteProbeCommandIndexMatches(commandIndex) &&
      forceTextureWhiteProbeCommandDrawIndexMatches(commandDrawIndex) &&
      forceTextureWhiteProbeClassMatches(primitiveCount,
                                         hot.textureMask,
                                         hot.renderStates,
                                         hot.viewport,
                                         fillMode);
  const bool fragmentlessDepthOnlyProbeApplied =
      debug::probeFragmentlessDepthOnly() &&
      indexedTriangleDraw &&
      fragmentlessDepthOnlyProbeRowMatches(encoderBreakdown) &&
      fragmentlessDepthOnlyProbeClassMatches(primitiveCount,
                                             hot.textureMask,
                                             hot.renderStates,
                                             hot.viewport,
                                             fillMode) &&
      fragmentlessDepthOnlyStateSafe(hot, fillMode);
  const std::optional<bool> forceTextureWhiteOverride =
      forceTextureWhiteProbeApplied ? std::optional<bool>{true} : std::nullopt;
  const bool hasPerDrawBindingOverride =
      paramOverride && !paramOverride->bindingOverrideRange.empty();
  const bool psoPrefetchBypassProbe =
      disableAlphaBlendProbeApplied || forceTextureWhiteProbeApplied ||
      fragmentlessDepthOnlyProbeApplied;
  const bool bypassPrefetchedPsoHandle =
      psoPrefetchBypassProbe ||
      (hasPerDrawBindingOverride && !bindingOverridePrefetchedPsoCompatible);
  const bool effectiveSkipBaseStateBind =
      skipBaseStateBind && !depthStateProbeRequested && !forceTextureWhiteProbeApplied;
  if (!effectiveSkipBaseStateBind && !suppressBaseStateLookup(ctx)) {
    const bool psoPrefetchHandleAvailable = renderPsoHandle.valid();
    perf::countEncodeDrawPsoPrefetch(
        psoPrefetchHandleAvailable,
        psoPrefetchHandleAvailable && !bypassPrefetchedPsoHandle,
        hasPerDrawBindingOverride,
        bindingOverridePrefetchedPsoCompatible,
        psoPrefetchBypassProbe);
  }
  if (effectiveSkipBaseStateBind) {
    recordPsoAttributionForDraw(encoderBreakdown, drawState, ctx.pool, renderPsoHandle,
                                tileFfpMode, argbufHybridMode,
                                argbufResourceArray, argbufDirectCbufMode,
                                std::nullopt,
                                fragmentlessDepthOnlyProbeApplied);
  }
  if (encoderBreakdown) {
    if (disableAlphaBlendProbeApplied) {
      ++encoderBreakdown->stats.probeDisableAlphaBlendDraws;
    }
    if (disableDepthWriteProbeApplied) {
      ++encoderBreakdown->stats.probeDisableDepthWriteDraws;
    }
    if (depthFuncAlwaysProbeApplied) {
      ++encoderBreakdown->stats.probeDepthFuncAlwaysDraws;
    }
    if (forceTextureWhiteProbeApplied) {
      ++encoderBreakdown->stats.probeForceTextureWhiteDraws;
    }
    if (fragmentlessDepthOnlyProbeApplied) {
      ++encoderBreakdown->stats.probeFragmentlessDepthOnlyDraws;
      encoderBreakdown->stats.probeFragmentlessDepthOnlyPrimitives += primitiveCount;
      encoderBreakdown->stats.probeFragmentlessDepthOnlyVertices += vertexCount;
    }
  }
  core::FlatDrawStateRecord alphaBlendProbeHot{};
  core::FlatDrawStateView pipelineDrawState = drawState;
  if (disableAlphaBlendProbeApplied) {
    alphaBlendProbeHot = hot;
    overrideFlatStateValue(alphaBlendProbeHot.renderStates,
                           RS_ALPHABLEND_ENABLE,
                           0u);
    pipelineDrawState.hot = &alphaBlendProbeHot;
  }
  // Phase 3-E: pipeline lookup + depth state + setRenderPipelineState
  // are BaseDrawState-only and survive across iterations of a
  // Kind::DrawRun on the Metal render encoder. Skip on iter 2..N.
  if (!effectiveSkipBaseStateBind) {
    emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                commandDrawCount, "before-pipeline-lookup");
    PerfScope pipelineLookupScope(perf::countEncodeDrawPipelineLookupCpuTime);
    auto depthKey = makeDepthStencilKey(drawState);
    if (disableDepthWriteProbeApplied) {
      depthKey.depthWrite = false;
    }
    if (depthFuncAlwaysProbeApplied) {
      depthKey.depthFunc = static_cast<u32>(core::CompareFunc::Always);
    }
    const std::uint8_t stencilRef = state::computeStencilRef(drawState);
    emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                commandDrawCount, "after-pipeline-key");
    // R-BACK-13.3: pass `tileFfpMode` through so the cache returns the
    // tile-stage MTLRenderPipelineState (built via
    // newRenderPipelineStateWithTileDescriptor) when the selector chose
    // Tile, and the standard fragment PSO otherwise. The variant key
    // already records this bit, so the two variants land in distinct
    // cache entries.
    // R-BACK-12.22..12.26: pass `argbufHybridMode` through as a real
    // PSO/source variant. Stage 1 uses direct slot 0/3 bindings; Stage 2
    // emits the slot-30 ArgbufLayout prelude and reads cbuf/texture/sampler
    // state through the argument buffer.
    WMT::Reference<WMT::RenderPipelineState> pipelineRef;
    WMT::RenderPipelineState pipeline{};
    const pipeline::HandleLookupContext renderPsoLookup{
        .chunkSeqId = seqId,
        .commandIndex = commandIndex,
        .role = tileFfpMode ? "tile-base-render-pso" : "render-pso",
    };
    if (suppressBaseStateLookup(ctx)) {
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "before-pipeline-recorder");
      pipeline = ctx.drawRecorder->renderPipelineState;
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "after-pipeline-recorder");
    } else if (tileFfpMode) {
      // R-BACK-13.1 two-stage tile-FFP encode: the render command encoder
      // first rasterizes the geometry with the BASE-COLOUR fragment PSO
      // (fog / alpha-test / A2C stripped), and only afterwards runs the tile
      // kernel over the imageblock. So in tile mode the PSO bound here via
      // setRenderPipelineState is the base-colour render PSO, NOT the tile
      // PSO. The tile PSO is fetched + dispatched after drawPrimitives below.
      if (renderPsoHandle.valid() && !bypassPrefetchedPsoHandle) {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-handle-tile-base");
        pipelineRef =
            ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                            renderPsoLookup).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-handle-tile-base");
      } else {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-build-tile-base");
        pipelineRef =
            ctx.cache.getOrBuildTileFfpBaseColorPipelineForState(
                ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                ctx.shaderArchive, ctx.shaderArchivePath,
                forceTextureWhiteOverride).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-build-tile-base");
      }
      pipeline = WMT::RenderPipelineState{pipelineRef.handle};
    } else {
      if (renderPsoHandle.valid() && !bypassPrefetchedPsoHandle) {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-handle-render");
        pipelineRef =
            ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                            renderPsoLookup).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-handle-render");
      } else {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "before-pipeline-build-render");
        pipelineRef =
            ctx.cache.getOrBuildDrawPipelineForState(
                ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                ctx.shaderArchive, ctx.shaderArchivePath, tileFfpMode,
                argbufHybridMode, argbufResourceArray,
                effectiveArgbufDirectCbufMode,
                disableAlphaBlendProbeApplied,
                forceTextureWhiteOverride,
                fragmentlessDepthOnlyProbeApplied).get();
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount,
                                    "after-pipeline-build-render");
      }
      pipeline = WMT::RenderPipelineState{pipelineRef.handle};
    }
    emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                commandDrawCount, "after-pipeline-lookup");
    if (!pipeline) {
      perf::countDrawSkippedNoPipeline();
      static std::mutex logMutex;
      static std::unordered_set<std::uint64_t> loggedNoPipelineKeys;
      const std::uint64_t noPipelineKey =
          hot.key.vertexShaderHash ^ (hot.key.pixelShaderHash << 1u) ^
          (static_cast<std::uint64_t>(tileFfpMode) << 2u) ^
          (static_cast<std::uint64_t>(argbufHybridMode) << 3u) ^
          (static_cast<std::uint64_t>(argbufResourceArray) << 4u) ^
          (static_cast<std::uint64_t>(effectiveArgbufDirectCbufMode) << 5u);
      bool shouldLog = false;
      {
        std::lock_guard lock(logMutex);
        shouldLog = loggedNoPipelineKeys.size() < 64u &&
                    loggedNoPipelineKeys.insert(noPipelineKey).second;
      }
      if (shouldLog) {
        util::logf(util::LogLevel::Error, "dxmt9-encode",
                   "draw skipped: no render pipeline seq=%llu command=%u vs=0x%llx ps=0x%llx tile=%u argbuf=%u resource_array=%u argbuf_direct_cbuf=%u rt0=0x%llx ds=0x%llx",
                   static_cast<unsigned long long>(seqId),
                   commandIndex,
                   static_cast<unsigned long long>(hot.key.vertexShaderHash),
                   static_cast<unsigned long long>(hot.key.pixelShaderHash),
                   tileFfpMode ? 1u : 0u,
                   argbufHybridMode ? 1u : 0u,
                   argbufResourceArray ? 1u : 0u,
                   effectiveArgbufDirectCbufMode ? 1u : 0u,
                   static_cast<unsigned long long>(hot.colorAttachments[0].handle.value),
                   static_cast<unsigned long long>(hot.depthStencil.handle.value));
      }
      if (traceEncode) {
        std::ostringstream out;
        out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
            << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
            << " skipped reason=no-pipeline"
            << " rt0=" << static_cast<unsigned long long>(hot.colorAttachments[0].handle.value)
            << " ds=" << static_cast<unsigned long long>(hot.depthStencil.handle.value)
            << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
            << " fvf=0x" << std::hex << vertexDecl.fvf << std::dec
            << " alphaBlend="
            << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
            << " colorWrite="
            << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu);
        emitQueueTraceLine(out.str());
      }
      return false;
    }
    WMT::Reference<WMT::DepthStencilState> depthStateRef;
    WMT::DepthStencilState depthState{};
    if (suppressBaseStateLookup(ctx)) {
      depthState = ctx.drawRecorder->depthStencilState;
    } else {
      DXMT_ASSERT(ctx.device && "depthStencilStateFor called with stale/null Metal device handle");
      const pipeline::HandleLookupContext depthLookup{
          .chunkSeqId = seqId,
          .commandIndex = commandIndex,
          .role = "depth-stencil",
      };
      depthStateRef =
          depthStencilHandle.valid()
              ? ctx.cache.depthStencilStateForHandle(depthStencilHandle,
                                                     depthLookup)
              : WMT::Reference<WMT::DepthStencilState>{};
      if (!depthStateRef) {
        depthStateRef = ctx.cache.depthStencilStateFor(ctx.device, depthKey);
      }
      depthState = WMT::DepthStencilState{depthStateRef.handle};
    }
    if (depthState) {
      // P0-3: propagate D3DRS_STENCILREF through to Metal. D3D9 has only
      // one stencil ref slot (Wine `wined3d_device_apply_stencil_ref`),
      // so the same byte applies to front and back faces — WMT's
      // `setStencilReferenceValue` mirrors that.
      const bool depthUnchanged =
          textureSamplerShadow &&
          textureSamplerShadowMatches(textureSamplerShadow->depthStencil,
                                      stencilRef, depthState.handle);
      if (!depthUnchanged) {
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount, "before-set-depth-state");
        recordedSetDepthStencilState(ctx, encoder, depthState, stencilRef);
        emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                    commandDrawCount, "after-set-depth-state");
        if (textureSamplerShadow) {
          textureSamplerShadowStore(textureSamplerShadow->depthStencil,
                                    stencilRef, depthState.handle);
        }
        countDepthStateBind();
      } else {
        countDepthStateBindSkipped();
      }
    }
    if (!disableAlphaBlendProbeApplied &&
        blendFactorNeedsConstantColor(hot.renderStates)) {
      const auto factor = decodeD3DBlendFactor(
          core::flatStateOr(hot.renderStates, RS_BLEND_FACTOR, 0xffffffffu));
      recordedSetBlendColorAndStencilRef(
          ctx, encoder, factor[0], factor[1], factor[2], factor[3], stencilRef);
    }
    // M1: label the pipeline with the shader-variant hash so frame
    // captures show "pso_h<hash>" instead of an anonymous pipeline. When
    // encoder breakdown is active, also retain the pair-local VSOut layout
    // key in both the log and label so Xcode's VS buffer-write counters can
    // be tied back to a concrete stage-in shape.
    {
      const auto variantHash =
          shaderVariantHashForDraw(drawState, &ctx.pool,
                                   fragmentlessDepthOnlyProbeApplied);
      const bool recordPsoBreakdown = encoderBreakdown && encoderBreakdown->enabled;
      u32 vsOutLayoutKey = 0;
      if (recordPsoBreakdown) {
        recordPsoAttributionForDraw(encoderBreakdown, drawState, ctx.pool, renderPsoHandle,
                                    tileFfpMode, argbufHybridMode,
                                    argbufResourceArray,
                                    argbufDirectCbufMode,
                                    forceTextureWhiteOverride,
                                    fragmentlessDepthOnlyProbeApplied);
        vsOutLayoutKey = encoderBreakdown->stats.vsOutLayoutLast;
      }
      if (variantHash != 0 && !suppressRecordedMetalCalls(ctx)) {
        WMT::RenderPipelineState psoView{pipeline.handle};
        if (recordPsoBreakdown) {
          psoView.setLabel(makeLabelStringFmt("pso_h%016llx_vso0x%03x",
              static_cast<unsigned long long>(variantHash),
              static_cast<unsigned>(vsOutLayoutKey)));
        } else {
          psoView.setLabel(makeLabelStringFmt("pso_h%016llx",
              static_cast<unsigned long long>(variantHash)));
        }
      }
    }
    // R-BACK-13.1: in BOTH the portable and the tile-FFP base-colour cases
    // the PSO bound here is an ordinary render pipeline, so the geometry
    // draw below rasterizes into the imageblock. The tile-FFP imageblock
    // post-pass (setTileRenderPipelineState + dispatchThreadsPerTile) is
    // deferred until AFTER drawPrimitives — see emitTileFfpPostPass below.
    const bool pipelineUnchanged =
        textureSamplerShadow &&
        bindShadowMatches(textureSamplerShadow->renderPipeline, pipeline.handle);
    if (!pipelineUnchanged) {
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "before-set-pipeline");
      recordedSetRenderPipelineState(ctx, encoder, pipeline);
      emitEncodeProgressDrawStage(seqId, commandIndex, commandDrawIndex,
                                  commandDrawCount, "after-set-pipeline");
      if (textureSamplerShadow) {
        bindShadowStore(textureSamplerShadow->renderPipeline, pipeline.handle);
      }
      countPipelineBind();
    } else {
      countPipelineBindSkipped();
    }
  }
  auto uploadTransientBuffer = [&](const void* data, std::size_t len, std::size_t alignment) {
    return ctx.queue.uploadTransientBufferWithCompletedSeqId(
        std::span<const std::byte>(reinterpret_cast<const std::byte*>(data), len),
        alignment, seqId, ctx.transientCompletedSeqId);
  };
  auto setVertexBufferCached = [&](WMT::Buffer buffer, u64 offset, std::uint8_t index) {
    if (textureSamplerShadow && index < textureSamplerShadow->vertexBuffers.size() &&
        bufferBindShadowMatches(textureSamplerShadow->vertexBuffers[index],
                                buffer.handle, offset)) {
      // Cache hit: the shadow already records this (buffer, offset) at
      // this binding slot — skip the Metal call and count the saved
      // bind. Mirrors the texture/sampler skip pattern.
      countVertexBufferBindSkipped();
      return false;
    }
    recordedSetVertexBuffer(ctx, encoder, buffer, offset, index);
    if (textureSamplerShadow && index < textureSamplerShadow->vertexBuffers.size()) {
      bufferBindShadowStore(textureSamplerShadow->vertexBuffers[index],
                            buffer.handle, offset);
    }
    return true;
  };
  // R-BACK-13.1 two-stage tile-FFP encode — tile post-pass.
  //
  // After the base-colour geometry draw rasterizes into the imageblock, a
  // tile kernel (makeFfpTilePixelSource) applies the D3D9 fog / alpha-test /
  // A2C over the imageblock value. The kernel reads `FfpPsConsts` at tile
  // buffer slot 3, so the tile PSO is built in its NON-argbuf form
  // (argbufHybridMode=false) and the FfpPsConsts struct is bound to the TILE
  // stage via setTileBuffer (the render-stage fragment-buffer binding does
  // not feed the tile stage).
  //
  // Metal API note (validated on Apple M1 / AGXG13GFamilyRenderContext): a
  // tile render-pipeline state — even though it is built via
  // newRenderPipelineStateWithTileDescriptor: — is bound with the ordinary
  // `setRenderPipelineState:`, NOT `setTileRenderPipelineState:` (which the
  // M1 render encoder does not respond to and throws an unrecognized-selector
  // NSException for). This matches design.md §13.5. Because that overwrites
  // the render PSO, we rebind the base-colour PSO after the dispatch so any
  // subsequent draw in a DrawRun (which skips the base-state bind) still has
  // its base-colour pipeline current.
  //
  // We fetch both PSOs on every tile-mode draw — the cache lookup is a hit
  // after the first build — so each draw in a DrawRun gets its own post-pass.
  WMT::Reference<WMT::RenderPipelineState> tileFfpPsoRef;
  WMT::RenderPipelineState tileFfpPso{};
  WMT::Reference<WMT::RenderPipelineState> tileFfpBasePsoRef;
  WMT::RenderPipelineState tileFfpBasePso{};
  if (tileFfpMode && !suppressBaseStateLookup(ctx)) {
    const pipeline::HandleLookupContext tileLookup{
        .chunkSeqId = seqId,
        .commandIndex = commandIndex,
        .role = "tile-pso",
    };
    const pipeline::HandleLookupContext tileBaseLookup{
        .chunkSeqId = seqId,
        .commandIndex = commandIndex,
        .role = "tile-base-render-pso",
    };
    tileFfpPsoRef =
        tilePsoHandle.valid() && !bypassPrefetchedPsoHandle
            ? ctx.cache.drawPipelineForHandle(tilePsoHandle,
                                              tileLookup).get()
            : ctx.cache.getOrBuildDrawPipelineForState(
                  ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                  ctx.shaderArchive, ctx.shaderArchivePath,
                  /*tileFfpMode=*/true, /*argbufHybridMode=*/false,
                  /*argbufResourceArray=*/false,
                  /*argbufDirectCbufMode=*/false,
                  disableAlphaBlendProbeApplied,
                  forceTextureWhiteOverride).get();
    tileFfpPso = WMT::RenderPipelineState{tileFfpPsoRef.handle};
    tileFfpBasePsoRef =
        renderPsoHandle.valid() && !bypassPrefetchedPsoHandle
            ? ctx.cache.drawPipelineForHandle(renderPsoHandle,
                                              tileBaseLookup).get()
            : ctx.cache.getOrBuildTileFfpBaseColorPipelineForState(
                  ctx.device, ctx.limits, ctx.pool, pipelineDrawState,
                  ctx.shaderArchive, ctx.shaderArchivePath,
                  forceTextureWhiteOverride).get();
    tileFfpBasePso = WMT::RenderPipelineState{tileFfpBasePsoRef.handle};
  }
  // Called immediately after a successful geometry draw (every `return true`
  // path below) when this draw runs the tile-FFP path. No-op otherwise.
  auto emitTileFfpPostPass = [&]() {
    if (!tileFfpMode || !tileFfpPso || suppressRecordedMetalCalls(ctx)) {
      return;
    }
    // Bind FfpPsConsts to the tile-stage buffer table (slot 3). The tile
    // kernel reads alphaRef / alphaTestFunc / fogStart / fogEnd / fogColor
    // / fogMode from here; without this the kernel would read zeroes and
    // leave the base colour unfogged.
    FfpPsConsts ffpPs = buildFfpPsConsts(drawState);
    auto slice = uploadTransientBuffer(&ffpPs, sizeof(FfpPsConsts), alignof(FfpPsConsts));
    if (slice) {
      encoder.setTileBuffer(slice.buffer, slice.offset, 3);
    }
    // R-BACK-13.5: bind the tile-stage variant via setRenderPipelineState
    // (see the API note above) and dispatch one tile thread per imageblock
    // lane. tileWidth/tileHeight come from the bound attachment shape; fall
    // back to 16x16 (typical Apple GPU tile) when Metal reports 0.
    encoder.setRenderPipelineState(tileFfpPso);
    uint64_t tileW = encoder.tileWidth();
    uint64_t tileH = encoder.tileHeight();
    if (tileW == 0u || tileH == 0u) {
      tileW = 16u;
      tileH = 16u;
    }
    encoder.dispatchThreadsPerTile(WMTSize{tileW, tileH, 1u});
    // Restore the base-colour render PSO so a subsequent DrawRun draw (which
    // skips the base-state bind) rasterizes with the correct pipeline.
    if (tileFfpBasePso) {
      encoder.setRenderPipelineState(tileFfpBasePso);
    }
  };
  // Per-frequency UBO bind sequence (R-BACK-12.5/12.8). Each category
  // sub-allocates from the existing transient slab pool, builds its
  // struct via the A2 transform, binds to its slot, and clears the
  // dirty bit. Stale (non-dirty) categories rely on the previous
  // draw's sticky binding on the same Metal render encoder.
  uniform::DirtyState scratchDirty;
  uniform::markAllDirty(scratchDirty);
  uniform::DirtyState* dirtyPtr = dirty ? dirty : &scratchDirty;
  const auto& shaderUsage = drawState.shaderContext();
  // R-BACK-12.22..12.26 (resource-array sub-mode) — when this pass runs the
  // resource-array lane the constant-buffer entries AND the texture/sampler
  // arrays share the SAME argument buffer, so every argbuf write
  // (updateDirtyArgbufRegions / pointFfpVsAtSlice / populateResourceBindings)
  // must target the resource-array encoder. Otherwise (constants-only Stage 2)
  // the constants-only encoder is used, byte-identical to before. Resolved
  // once here so the constant + resource write paths can't diverge.
  const bool useResourceArrayArgbuf =
      argbufResourceArray && argbufTableMode &&
      ctx.queue.resourceArrayEncoderResource().initialized();
  auto& argbufEncoderForDraw = useResourceArrayArgbuf
                                   ? ctx.queue.resourceArrayEncoderResource()
                                   : ctx.queue.argbufEncoderResource();
  const auto argbufConsumedBits = static_cast<std::uint16_t>(
      uniform::kVsAny | uniform::kPsAny | uniform::kFfpVsAny | uniform::kFfpPsAny);
  const u64 argbufPayloadHash =
      drawState.hasUniformPayload() ? drawState.uniformPayload().hash : 0;
  // R-BACK-12.22..12.26 (argbuf lifetime across draws). Reserve a FRESH
  // argbuf per draw and rebind slot 30 so each draw's argbuf is
  // self-contained. This applies to BOTH Stage 2 lanes:
  //
  //   * resource-array lane: texture/sampler arrays write the gpuResourceID
  //     INLINE into the argbuf slot, so a second draw that changes a texture
  //     would overwrite the first draw's slot before the GPU consumed it.
  //
  //   * constants-only lane: the cbuf DATA goes to a fresh transient slab per
  //     dirty draw (uploadTransientBuffer), but the argbuf descriptor table
  //     itself is anchored once per openArgbuf and re-pointed IN PLACE by
  //     updateDirtyArgbufRegions. The GPU reads the descriptor table at
  //     execution time, so multiple draws in one render pass that share a
  //     single descriptor table all observe the LAST pointer written
  //     (last-write-wins on constants — dxut-simple overlay PassMix bug).
  //     Re-opening per draw gives each draw its own descriptor table.
  //
  // A fresh table still needs all four cbuf entries. When an encoder-local
  // cache has slices for the same uniform payload and no pending cbuf dirty
  // bits, point the fresh table at those slices. Otherwise force all cbuf
  // categories dirty so the mirror below repopulates the table.
  //
  // `reopenArgbufHybrid` is the caller's optimisation gate (encodeChunk):
  // the resource-array lane always reopens (texture/sampler inline writes),
  // while the constants-only lane reopens only when this draw's uniform
  // payload differs from the previous draw on the same encoder. When false
  // we leave slot 30 bound to the prior draw's table — correct because its
  // pointers still describe the unchanged constants — and the dirty mirror
  // below is a no-op (the prior draw already consumed the const dirty bits).
  const bool argbufCbufProbeSplit =
      argbufTableMode && argbufCbufProbeSplitPerfEnabled();
  const bool argbufReopenSplit =
      argbufTableMode && argbufReopenSplitPerfEnabled();
  const bool argbufCbufDirtyIdentityProbe =
      argbufTableMode && argbufCbufDirtyIdentityPerfEnabled();
  const bool encoderBreakdownCbufContent =
      encoderBreakdown && encoderBreakdown->enabled &&
      encoderBreakdownCbufContentEnabled();
  if (argbufTableMode && reopenArgbufHybrid) {
    PerfScope argbufSetupScope(perf::countEncodeDrawArgbufSetupCpuTime);
    PerfScope argbufOpenScope(perf::countEncodeDrawArgbufOpenCpuTime);
    const auto populated = [&]() {
      PerfScope argbufOpenCallScope(
          perf::countEncodeDrawArgbufOpenCallCpuTime);
      return dxmt9::argbuf_hybrid::openArgbufWithCompletedSeqId(
          ctx.queue, argbufEncoderForDraw, seqId,
          ctx.transientCompletedSeqId);
    }();
    if (populated && !suppressRecordedMetalCalls(ctx)) {
      PerfScope argbufReopenPostScope(
          perf::countEncodeDrawArgbufReopenPostCpuTime);
      bool tableUnchanged = false;
      {
        PerfScope tableProbeScope(
            argbufReopenSplit
                ? perf::countEncodeDrawArgbufReopenTableProbeCpuTime
                : nullptr);
        tableUnchanged =
            textureSamplerShadow &&
            dxmt9::shaders::kArgbufHybridBindSlot <
                textureSamplerShadow->vertexBuffers.size() &&
            bufferBindShadowMatches(
                textureSamplerShadow
                    ->vertexBuffers[dxmt9::shaders::kArgbufHybridBindSlot],
                populated.storage.handle, populated.offset);
      }
      if (!tableUnchanged) {
        PerfScope tableBindScope(perf::countEncodeDrawArgbufTableBindCpuTime);
        encoder.setVertexBuffer(populated.storage, populated.offset,
                                dxmt9::shaders::kArgbufHybridBindSlot);
        encoder.setFragmentBuffer(populated.storage, populated.offset,
                                  dxmt9::shaders::kArgbufHybridBindSlot);
        perf::countEncodeDrawArgbufTableBindCalls(1u);
        if (textureSamplerShadow) {
          PerfScope tableShadowStoreScope(
              argbufReopenSplit
                  ? perf::countEncodeDrawArgbufReopenTableShadowStoreCpuTime
                  : nullptr);
          if (dxmt9::shaders::kArgbufHybridBindSlot <
              textureSamplerShadow->vertexBuffers.size()) {
            bufferBindShadowStore(
                textureSamplerShadow->vertexBuffers[dxmt9::shaders::kArgbufHybridBindSlot],
                populated.storage.handle, populated.offset);
          }
        }
      } else {
        perf::countEncodeDrawArgbufTableBindSkipped(1u);
      }
      {
        PerfScope byteAccountScope(
            argbufReopenSplit
                ? perf::countEncodeDrawArgbufReopenByteAccountCpuTime
                : nullptr);
        perf::countArgbufHybridBytes(populated.length);
        if (encoderBreakdown) {
          encoderBreakdown->addArgbufTableBytes(populated.length);
        }
      }
      bool canRepointCachedCbufs = false;
      {
        PerfScope cbufCacheProbeScope(
            argbufReopenSplit
                ? perf::countEncodeDrawArgbufReopenCbufCacheProbeCpuTime
                : nullptr);
        canRepointCachedCbufs =
            argbufCbufCache &&
            argbufCbufCache->matches(argbufPayloadHash) &&
            !uniform::anyDirty(*dirtyPtr, argbufConsumedBits);
      }
      if (canRepointCachedCbufs) {
        perf::countEncodeDrawArgbufCbufReopenFullRepointCalls(1u);
        {
          PerfScope fullRepointScope(
              perf::countEncodeDrawArgbufCbufFullRepointCpuTime);
          for (u32 i = 0; i < argbufCbufCache->bindings.entries.size(); ++i) {
            dxmt9::argbuf_hybrid::pointConstantBufferBinding(
                argbufEncoderForDraw, i, argbufCbufCache->bindings.entries[i],
                nullptr, encoder);
          }
        }
      } else {
        auto forceDirty = [&](std::uint16_t mask) {
          PerfScope forceDirtyScope(
              argbufReopenSplit
                  ? perf::countEncodeDrawArgbufReopenCbufForceDirtyCpuTime
                  : nullptr);
          dirtyPtr->mask = static_cast<std::uint16_t>(dirtyPtr->mask | mask);
        };
        auto pointCachedBinding = [&](u32 argbufIndex) -> bool {
          if (!argbufCbufCache || !argbufCbufCache->hasBinding(argbufIndex)) {
            return false;
          }
          const auto binding = argbufCbufCache->binding(argbufIndex);
          {
            PerfScope cachedRepointScope(
                argbufCbufProbeSplit
                    ? perf::countEncodeDrawArgbufCbufCachedRepointCpuTime
                    : nullptr);
            PerfScope cachedRepointStageScope(
                argbufCbufProbeSplit
                    ? argbufCbufCachedRepointCpuRecorder(argbufIndex)
                    : nullptr);
            dxmt9::argbuf_hybrid::pointConstantBufferBinding(
                argbufEncoderForDraw, argbufIndex, binding, nullptr, encoder);
          }
          perf::countEncodeDrawArgbufCbufCachedRepointCalls(1u);
          perf::countEncodeDrawArgbufCbufCachedRepointBytes(binding.bytes);
          if (argbufCbufProbeSplit) {
            countArgbufCbufCachedRepointStage(argbufIndex, binding.bytes);
          }
          return true;
        };
        bool hasAnyCbufDirty = false;
        {
          PerfScope dirtyScanScope(
              argbufReopenSplit
                  ? perf::countEncodeDrawArgbufReopenCbufDirtyScanCpuTime
                  : nullptr);
          hasAnyCbufDirty =
              uniform::anyDirty(*dirtyPtr, argbufConsumedBits);
        }
        if (!hasAnyCbufDirty) {
          // Payload hash drift without matching dirty bits is not trusted as
          // a whole-payload decision. Probe each cbuf category by its current
          // draw-state identity instead; only unmatched categories upload.
          perf::countEncodeDrawArgbufCbufReopenNoDirtyHashMismatch(1u);
          perf::countEncodeDrawArgbufCbufContentProbeCalls(1u);
          ArgbufCbufIdentityProbe vsProbe{};
          ArgbufCbufIdentityProbe psProbe{};
          ArgbufCbufIdentityProbe ffpPsProbe{};
          {
            PerfScope probeScope(
                argbufCbufProbeSplit
                    ? perf::countEncodeDrawArgbufCbufContentProbeCpuTime
                    : nullptr);
            if (!argbufVsPayloadSourceChanged) {
              PerfScope vsProbeScope(
                  argbufCbufProbeSplit
                      ? argbufCbufContentProbeCpuRecorder(
                            dxmt9::argbuf_hybrid::kConstantBufferVsIndex)
                      : nullptr);
              const auto vsPlan =
                  uniform::makeVsConstantUploadPlan(
                      *dirtyPtr, shaderUsage.vertexConstantUsage);
              const auto vsBytes = uniform::vsConstantUploadBytes(vsPlan);
              vsProbe = ArgbufCbufIdentityProbe{
                  .bytes = vsBytes,
                  .hash = makeArgbufCbufIdentityHash(
                      0x76735f636275665full,
                      drawStateVertexCbufSourceHash(drawState),
                      vsBytes),
              };
            }

            if (!argbufPsPayloadSourceChanged) {
              PerfScope psProbeScope(
                  argbufCbufProbeSplit
                      ? argbufCbufContentProbeCpuRecorder(
                            dxmt9::argbuf_hybrid::kConstantBufferPsIndex)
                      : nullptr);
              const auto psPlan =
                  uniform::makePsConstantUploadPlan(
                      *dirtyPtr, shaderUsage.pixelConstantUsage);
              const auto psBytes = uniform::psConstantUploadBytes(psPlan);
              psProbe = ArgbufCbufIdentityProbe{
                  .bytes = psBytes,
                  .hash = makeArgbufCbufIdentityHash(
                      0x70735f636275665full,
                      drawStatePixelCbufSourceHash(drawState),
                      psBytes),
              };
            }

            {
              PerfScope ffpPsProbeScope(
                  argbufCbufProbeSplit
                      ? argbufCbufContentProbeCpuRecorder(
                            dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex)
                      : nullptr);
              ffpPsProbe = ArgbufCbufIdentityProbe{
                  .bytes = sizeof(FfpPsConsts),
                  .hash = makeArgbufFfpPsIdentityHash(
                      drawState, sizeof(FfpPsConsts)),
              };
            }
          }

          auto pointCachedByIdentity =
              [&](std::uint16_t mask,
                  u32 argbufIndex,
                  ArgbufCbufIdentityProbe probe,
                  void (*countHit)(std::uint64_t),
                  void (*countMiss)(std::uint64_t)) {
                if (argbufCbufCache &&
                    argbufCbufCache->hasMatchingIdentity(
                        argbufIndex, probe.hash, probe.bytes) &&
                    pointCachedBinding(argbufIndex)) {
                  countHit(1u);
                  return;
                }
                countMiss(1u);
                forceDirty(mask);
              };

          if (argbufVsPayloadSourceChanged) {
            perf::countEncodeDrawArgbufCbufContentProbeVsMisses(1u);
            forceDirty(uniform::kVsAny);
          } else {
            pointCachedByIdentity(
                uniform::kVsAny,
                dxmt9::argbuf_hybrid::kConstantBufferVsIndex,
                vsProbe,
                perf::countEncodeDrawArgbufCbufContentProbeVsHits,
                perf::countEncodeDrawArgbufCbufContentProbeVsMisses);
          }
          if (argbufPsPayloadSourceChanged) {
            perf::countEncodeDrawArgbufCbufContentProbePsMisses(1u);
            forceDirty(uniform::kPsAny);
          } else {
            pointCachedByIdentity(
                uniform::kPsAny,
                dxmt9::argbuf_hybrid::kConstantBufferPsIndex,
                psProbe,
                perf::countEncodeDrawArgbufCbufContentProbePsHits,
                perf::countEncodeDrawArgbufCbufContentProbePsMisses);
          }
          // FFPVS stays on the deferred path because pre-transformed viewport
          // handling may patch the host bytes later in this draw.
          forceDirty(uniform::kFfpVsAny);
          pointCachedByIdentity(
              uniform::kFfpPsAny,
              dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex,
              ffpPsProbe,
              perf::countEncodeDrawArgbufCbufContentProbeFfpPsHits,
              perf::countEncodeDrawArgbufCbufContentProbeFfpPsMisses);
        } else {
          perf::countEncodeDrawArgbufCbufReopenPartialCandidates(1u);
          {
            PerfScope dirtyScanScope(
                argbufReopenSplit
                    ? perf::countEncodeDrawArgbufReopenCbufDirtyScanCpuTime
                    : nullptr);
            if (uniform::anyDirty(*dirtyPtr, uniform::kVsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyVs(1u);
            }
            if (uniform::anyDirty(*dirtyPtr, uniform::kPsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyPs(1u);
            }
            if (uniform::anyDirty(*dirtyPtr, uniform::kFfpVsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyFfpVs(1u);
            }
            if (uniform::anyDirty(*dirtyPtr, uniform::kFfpPsAny)) {
              perf::countEncodeDrawArgbufCbufReopenDirtyFfpPs(1u);
            }
          }
          auto repointCleanOrForceDirty = [&](std::uint16_t mask, u32 argbufIndex) {
            bool dirty = false;
            {
              PerfScope dirtyScanScope(
                  argbufReopenSplit
                      ? perf::countEncodeDrawArgbufReopenCbufDirtyScanCpuTime
                      : nullptr);
              dirty = uniform::anyDirty(*dirtyPtr, mask);
            }
            if (dirty) {
              return;
            }
            if (!pointCachedBinding(argbufIndex)) {
              forceDirty(mask);
            }
          };

          repointCleanOrForceDirty(
              uniform::kVsAny,
              dxmt9::argbuf_hybrid::kConstantBufferVsIndex);
          repointCleanOrForceDirty(
              uniform::kPsAny,
              dxmt9::argbuf_hybrid::kConstantBufferPsIndex);
          repointCleanOrForceDirty(
              uniform::kFfpVsAny,
              dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex);
          repointCleanOrForceDirty(
              uniform::kFfpPsAny,
              dxmt9::argbuf_hybrid::kConstantBufferFfpPsIndex);
        }
      }
    }
  }
  // R-BACK-12.24 — Stage 2 argbuf dirty mirror.
  //
  // When the encoder is on the argbuf-hybrid path AND any per-frequency
  // bit is dirty, mirror the dirty VS/PS/FFPPS regions into the argbuf so
  // the cbuf entries point at fresh transient slabs. FFPVS is deferred until
  // after the pre-transformed viewport override below; that lets the final
  // host bytes reuse a cached stable slice instead of re-uploading an
  // unchanged block on every draw.
  if (argbufTableMode) {
    PerfScope argbufSetupScope(perf::countEncodeDrawArgbufSetupCpuTime);
    auto dirtyForArgbuf = *dirtyPtr;
    uniform::clearBits(dirtyForArgbuf, uniform::kFfpVsAny);
    const auto cbufUpdateMask = static_cast<std::uint16_t>(
        argbufConsumedBits & ~uniform::kFfpVsAny);
    perf::countEncodeDrawArgbufCbufUpdateCalls(1u);
    if (!uniform::anyDirty(dirtyForArgbuf, cbufUpdateMask)) {
      perf::countEncodeDrawArgbufCbufUpdateSkippedClean(1u);
    } else {
      perf::countEncodeDrawArgbufCbufUpdateDirtyCalls(1u);
      PerfScope argbufCbufUpdateScope(
          perf::countEncodeDrawArgbufCbufUpdateCpuTime);
      if (argbufCbufDirtyIdentityProbe &&
          uniform::anyDirty(dirtyForArgbuf, uniform::kVsAny)) {
        perf::countEncodeDrawArgbufCbufDirtyVsIdentityProbeCalls(1u);
        const auto vsPlan =
            uniform::makeVsConstantUploadPlan(
                dirtyForArgbuf, shaderUsage.vertexConstantUsage);
        const auto vsBytes = uniform::vsConstantUploadBytes(vsPlan);
        const auto vsIdentityHash =
            makeArgbufCbufIdentityHash(
                0x76735f636275665full,
                drawStateVertexCbufSourceHash(drawState),
                vsBytes);
        if (!argbufCbufCache ||
            !argbufCbufCache->hasBinding(
                dxmt9::argbuf_hybrid::kConstantBufferVsIndex)) {
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityNoCache(1u);
        } else if (argbufCbufCache->hasMatchingIdentity(
                       dxmt9::argbuf_hybrid::kConstantBufferVsIndex,
                       vsIdentityHash,
                       vsBytes)) {
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityHits(1u);
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityHitBytes(vsBytes);
        } else {
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityMisses(1u);
          perf::countEncodeDrawArgbufCbufDirtyVsIdentityMissBytes(vsBytes);
        }
      }
      dxmt9::argbuf_hybrid::ConstantBufferBindings writtenCbufBindings{};
      dxmt9::argbuf_hybrid::ConstantBufferUploadObserver cbufUploadObserver{};
      if (encoderBreakdownCbufContent) {
        cbufUploadObserver.userdata = encoderBreakdown;
        cbufUploadObserver.upload = recordArgbufCbufUploadForBreakdown;
      }
      if (encoderBreakdown && encoderBreakdown->enabled &&
          uniform::anyDirty(dirtyForArgbuf, uniform::kVsAny)) {
        encoderBreakdown->recordVsUploadPlan(
            dirtyForArgbuf, shaderUsage.vertexConstantUsage,
            uniform::makeVsConstantUploadPlan(
                dirtyForArgbuf, shaderUsage.vertexConstantUsage));
      }
      const auto bytes = dxmt9::argbuf_hybrid::updateDirtyArgbufRegions(
          ctx.queue, argbufEncoderForDraw, drawState, dirtyForArgbuf,
          shaderUsage.vertexConstantUsage, shaderUsage.pixelConstantUsage, seqId,
          nullptr, encoder, &writtenCbufBindings,
          cbufUploadObserver.upload ? &cbufUploadObserver : nullptr);
      if (bytes != 0) {
        stampArgbufCbufBindingIdentities(writtenCbufBindings, drawState);
        perf::countEncodeDrawArgbufCbufUpdateWriteCalls(1u);
        perf::countArgbufHybridBytes(bytes);
        if (encoderBreakdown) {
          encoderBreakdown->addArgbufCbufBindings(writtenCbufBindings);
        }
        if (argbufCbufCache) {
          PerfScope cacheMergeScope(
              perf::countEncodeDrawArgbufCbufCacheMergeCpuTime);
          argbufCbufCache->merge(argbufPayloadHash, writtenCbufBindings);
        }
      }
    }
    uniform::clearBits(*dirtyPtr, cbufUpdateMask);
  }
  {
    PerfScope uniformBuildScope(perf::countEncodeDrawUniformBuildCpuTime);
    if (directCbufBindings && uniform::anyDirty(*dirtyPtr, uniform::kVsAny)) {
      VsConsts vs = buildVsConsts(drawState);
      const auto plan =
          uniform::makeVsConstantUploadPlan(*dirtyPtr, shaderUsage.vertexConstantUsage);
      const auto bytes = static_cast<std::size_t>(uniform::vsConstantUploadBytes(plan));
      auto slice = uploadTransientBuffer(&vs, bytes, alignof(VsConsts));
      if (slice) {
        if (setVertexBufferCached(slice.buffer, slice.offset, 0)) {
          countUniformBufferBinds(1);
        }
        perf::countUniformVsConsts(bytes);
        uniform::clearBits(*dirtyPtr, uniform::kVsAny);
      }
    }
    if (directCbufBindings && uniform::anyDirty(*dirtyPtr, uniform::kPsAny)) {
      PsConsts ps = buildPsConsts(drawState);
      const auto plan =
          uniform::makePsConstantUploadPlan(*dirtyPtr, shaderUsage.pixelConstantUsage);
      const auto bytes = static_cast<std::size_t>(uniform::psConstantUploadBytes(plan));
      auto slice = uploadTransientBuffer(&ps, bytes, alignof(PsConsts));
      if (slice) {
        if (!suppressRecordedMetalCalls(ctx)) {
          encoder.setFragmentBuffer(slice.buffer, slice.offset, 0);
        }
        countUniformBufferBinds(1);
        perf::countUniformPsConsts(bytes);
        uniform::clearBits(*dirtyPtr, uniform::kPsAny);
      }
    }
    if (directCbufBindings && uniform::anyDirty(*dirtyPtr, uniform::kFfpPsAny)) {
      FfpPsConsts ffpPs = buildFfpPsConsts(drawState);
      auto slice = uploadTransientBuffer(&ffpPs, sizeof(FfpPsConsts), alignof(FfpPsConsts));
      if (slice) {
        if (!suppressRecordedMetalCalls(ctx)) {
          encoder.setFragmentBuffer(slice.buffer, slice.offset, 3);
        }
        countUniformBufferBinds(1);
        perf::countUniformFfpPs(sizeof(FfpPsConsts));
        uniform::clearBits(*dirtyPtr, uniform::kFfpPsAny);
      }
    }
  }
  // FfpVsConsts: every VS shader (FFP or otherwise) declares
  // [[buffer(3)]] FfpVsConsts because halfPixelFixup, clipPlanes, and
  // viewport metadata live there. Build the host copy lazily; the
  // FFP preTransformed path overrides viewportOrigin/Size below before
  // upload+bind via bindFfpVsIfDirty.
  std::optional<FfpVsConsts> ffpVs;
  auto ensureFfpVs = [&] {
    if (!ffpVs) ffpVs = buildFfpVsConsts(drawState);
    return &*ffpVs;
  };
  bool ffpVsBound = false;
  auto bindFfpVsIfDirty = [&] {
    if (ffpVsBound || !uniform::anyDirty(*dirtyPtr, uniform::kFfpVsAny)) {
      return;
    }
    auto* host = ensureFfpVs();
    if (argbufTableMode && argbufCbufCache &&
        argbufCbufCache->hasMatchingFfpVs(*host)) {
      dxmt9::argbuf_hybrid::pointConstantBufferBinding(
          argbufEncoderForDraw,
          dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
          argbufCbufCache->ffpVsBinding(), nullptr, encoder);
      argbufCbufCache->promotePayloadHash(argbufPayloadHash);
      uniform::clearBits(*dirtyPtr, uniform::kFfpVsAny);
      ffpVsBound = true;
      return;
    }
    auto slice = uploadTransientBuffer(host, sizeof(FfpVsConsts), alignof(FfpVsConsts));
    if (slice) {
      if (argbufTableMode) {
        dxmt9::argbuf_hybrid::pointFfpVsAtSlice(
            argbufEncoderForDraw, slice.buffer, slice.offset,
            nullptr, encoder);
        perf::countArgbufHybridBytes(sizeof(FfpVsConsts));
        if (encoderBreakdown) {
          encoderBreakdown->addArgbufCbufBytes(
              dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
              sizeof(FfpVsConsts));
        }
        if (encoderBreakdownCbufContent) {
          encoderBreakdown->recordArgbufCbufUploadContent(
              dxmt9::argbuf_hybrid::kConstantBufferFfpVsIndex,
              host, sizeof(FfpVsConsts), sizeof(FfpVsConsts));
        }
        if (argbufCbufCache) {
          argbufCbufCache->storeFfpVs(
              argbufPayloadHash, *host, slice.buffer, slice.offset,
              sizeof(FfpVsConsts));
        }
      } else {
        if (setVertexBufferCached(slice.buffer, slice.offset, 3)) {
          countUniformBufferBinds(1);
        }
        perf::countUniformFfpVs(sizeof(FfpVsConsts));
      }
      uniform::clearBits(*dirtyPtr, uniform::kFfpVsAny);
      ffpVsBound = true;
    }
  };
  bool expandedIndexedDraw = false;
  const bool scissorDisabled = debug::disableScissor();
  const u32 cullState = core::flatStateOr(
      hot.renderStates, RS_CULL_MODE, static_cast<u32>(core::CullMode::Ccw));
  const auto requestedCullMode = (preTransformed || debug::disableCull())
                                     ? WMTCullModeNone
                                     : static_cast<WMTCullMode>(toCullMode(cullState));
  WMTCullMode effectiveCullMode = applyDebugCullOverride(requestedCullMode);
  const auto forceCullModeProbe = debug::probeForceCullMode();
  if (forceCullModeProbe != debug::CullModeOverride::Disabled &&
      indexedDraw &&
      pv.primitiveType == core::PrimitiveType::TriangleList &&
      forceCullModeProbeRowMatches(encoderBreakdown) &&
      forceCullModeProbeClassMatches(primitiveCount,
                                     hot.textureMask,
                                     hot.renderStates,
                                     hot.viewport,
                                     fillMode)) {
    effectiveCullMode = toWmtCullMode(forceCullModeProbe, effectiveCullMode);
  }
  core::ViewportScissor effectiveViewport = hot.viewport;
  const auto scissorRectOverride = debug::probeScissorRectOverride();
  bool scissorRectProbeConsidered = false;
  bool scissorRectProbeEligible = false;
  bool scissorRectProbeApplied = false;
  if (scissorRectOverride.enabled &&
      !scissorDisabled &&
      indexedDraw &&
      pv.primitiveType == core::PrimitiveType::TriangleList &&
      hot.viewport.scissorEnabled &&
      scissorRectProbeRowMatches(encoderBreakdown)) {
    scissorRectProbeConsidered = true;
    scissorRectProbeEligible =
        scissorRectProbeClassMatches(primitiveCount,
                                     hot.textureMask,
                                     hot.renderStates,
                                     hot.viewport,
                                     fillMode);
    if (scissorRectProbeEligible) {
      effectiveViewport.scissorEnabled = true;
      effectiveViewport.scissor = scissorRectOverride.rect;
      scissorRectProbeApplied = true;
    }
  }
  const auto* bindingPacketSurface =
      ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
  const bool bindingPacketHasRasterTarget =
      bindingPacketSurface && bindingPacketSurface->texture;
  const DrawBindingPacketPlan* bindingPacketPtr = nullptr;
  {
    PerfScope bindingPacketScope(perf::countEncodeDrawBindingPacketCpuTime);
    DrawBindingPacketPlan bindingPacketPlan{};
    {
      PerfScope bindingPacketPlanScope(
          perf::countEncodeDrawBindingPacketPlanCpuTime);
      if (bindingPacketPlanSplitPerfEnabled()) {
        {
          PerfScope fragmentScope(
              perf::countEncodeDrawBindingPacketPlanFragmentCpuTime);
          bindingPacketPlan.fragmentTextureSamplers =
              makeFragmentTextureSamplerBindings(
                  hot,
                  &drawState.shaderContext().pixelShader);
        }
        {
          PerfScope vertexScope(
              perf::countEncodeDrawBindingPacketPlanVertexCpuTime);
          bindingPacketPlan.vertexTextureSamplers =
              makeVertexTextureSamplerBindings(hot);
        }
        {
          PerfScope extraStreamScope(
              perf::countEncodeDrawBindingPacketPlanExtraStreamCpuTime);
          bindingPacketPlan.extraStreams =
              makeProgrammableVsExtraStreamBindings(vertexDecl, hot, pv);
        }
        {
          PerfScope rasterScope(
              perf::countEncodeDrawBindingPacketPlanRasterCpuTime);
          bindingPacketPlan.raster =
              makeEncoderRasterStatePlan(
                  hot,
                  bindingPacketHasRasterTarget
                      ? bindingPacketSurface->desc.width
                      : 1u,
                  bindingPacketHasRasterTarget
                      ? bindingPacketSurface->desc.height
                      : 1u,
                  ffLayout && ffLayout->preTransformed,
                  scissorDisabled,
                  debug::disableCull(),
                  &effectiveViewport);
        }
      } else {
        bindingPacketPlan = makeDrawBindingPacketPlan(
            vertexDecl,
            hot,
            pv,
            bindingPacketHasRasterTarget ? bindingPacketSurface->desc.width : 1u,
            bindingPacketHasRasterTarget ? bindingPacketSurface->desc.height : 1u,
            ffLayout && ffLayout->preTransformed,
            scissorDisabled,
            debug::disableCull(),
            &drawState.shaderContext().pixelShader,
            &effectiveViewport);
      }
    }
    {
      PerfScope bindingPacketCacheScope(
          perf::countEncodeDrawBindingPacketCacheCpuTime);
      DrawBindingPacketCacheStats bindingPacketCacheStats{};
      bindingPacketPtr =
          &cacheDrawBindingPacket(
              gDrawBindingPacketCache,
              bindingPacketPlan,
              &bindingPacketCacheStats);
      perf::countEncodeDrawBindingPacketCacheKeyCpuTime(
          bindingPacketCacheStats.keyCpuNs);
      perf::countEncodeDrawBindingPacketCacheHashCpuTime(
          bindingPacketCacheStats.hashCpuNs);
      perf::countEncodeDrawBindingPacketCacheProbeCpuTime(
          bindingPacketCacheStats.probeCpuNs);
      perf::countEncodeDrawBindingPacketCacheStoreCpuTime(
          bindingPacketCacheStats.storeCpuNs);
      perf::countEncodeDrawBindingPacketCacheHits(
          bindingPacketCacheStats.hits);
      perf::countEncodeDrawBindingPacketCacheMisses(
          bindingPacketCacheStats.misses);
      perf::countEncodeDrawBindingPacketCacheCollisions(
          bindingPacketCacheStats.collisions);
    }
    if (encoderBreakdown) {
      PerfScope bindingPacketTextureRecordScope(
          perf::countEncodeDrawBindingPacketTextureRecordCpuTime);
      for (const auto& binding : bindingPacketPtr->fragmentTextureSamplers) {
        const auto* texture = ctx.pool.findTexture(binding.texture.value);
        encoderBreakdown->recordFragmentTextureBinding(binding.stage,
                                                       binding.texture,
                                                       texture);
      }
    }
  }
  const auto& bindingPacket = *bindingPacketPtr;
  // Apply FFP preTransformed viewport override to the FfpVs host copy
  // before any bindFfpVsIfDirty call uploads it (R-BACK-12.5). The
  // override values come from run-stable sources (ffLayout +
  // targetSurface->desc); only mark dirty when the host copy actually
  // differs to avoid re-uploading the slab every draw.
  if (ffLayout && ffLayout->preTransformed) {
    if (auto* targetSurface = ctx.pool.findSurface(hot.colorAttachments[0].handle.value);
        targetSurface) {
      auto* host = ensureFfpVs();
      const std::array<f32, 2> wantOrigin{0.0f, 0.0f};
      const std::array<f32, 2> wantSize{
          static_cast<f32>(std::max(1u, targetSurface->desc.width)),
          static_cast<f32>(std::max(1u, targetSurface->desc.height))};
      if (host->viewportOrigin != wantOrigin || host->viewportSize != wantSize) {
        host->viewportOrigin = wantOrigin;
        host->viewportSize = wantSize;
        uniform::setBit(*dirtyPtr, uniform::DirtyBit::FfpVsViewport);
      }
    }
  }
  bindFfpVsIfDirty();
  // Phase 3-E: viewport / scissor / cull are BaseDrawState-only.
  const bool streamBindPhaseSplitPerf = streamBindPhaseSplitPerfEnabled();
  if (!effectiveSkipBaseStateBind) {
    PerfScope streamBindViewportScope(perf::countEncodeDrawStreamBindCpuTime);
    PerfScope streamBindRasterPhaseScope(
        streamBindPhaseSplitPerf
            ? perf::countEncodeDrawStreamBindRasterPhaseCpuTime
            : nullptr);
    PerfScope rasterStateScope(perf::countEncodeDrawRasterStateCpuTime);
    perf::countEncodeDrawStreamBindRasterPhaseCalls(1u);
    if (bindingPacketHasRasterTarget) {
      // 2026-06-05 — viewport / scissor per-draw shadow cache was tested
      // (commit 5eef5d4) and reverted: bind diversity on GT1 is high
      // enough that the cache hit rate is essentially zero, while the
      // per-draw equality comparisons added +12.7% encode_chunk_cpu_ms.
      // See docs/perfomance/present-pacing/
      // present-pacing-bind-cache-work-a.01.md.
      recordedSetViewport(ctx, encoder, bindingPacket.raster.viewport);
      countViewportBind();
      recordedSetScissorRect(ctx, encoder, bindingPacket.raster.scissor);
      countScissorBind();
      setRasterizerCullMode(ctx, encoder, hot.renderStates, effectiveCullMode);
    }
  }
  static std::atomic<int> ffTraceRemaining{debug::fixedFunctionTraceBudget()};
  CommandQueue::TransientBufferSlice transientVertexBuffer;
  std::span<const u8> vertexBytes;
  const resources::BufferRecord* stream0Record = nullptr;
  WMT::Buffer vertexBuffer{};
  uint64_t vertexBufferOffset = 0;
  bool stream0Staged = false;
  auto makeTransientBuffer = [&](const void* data, std::size_t len) {
    return uploadTransientBuffer(data, len, 16);
  };
  auto makeTransientVertexBuffer = [&](const void* data, std::size_t len,
                                       ActiveEncoderBreakdown::TransientVertexSource source) {
    auto slice = makeTransientBuffer(data, len);
    if (slice && encoderBreakdown) {
      encoderBreakdown->addTransientVertexBytes(static_cast<u64>(len), source);
    }
    return slice;
  };
  auto makeTransientIndexBuffer = [&](const void* data, std::size_t len,
                                      ActiveEncoderBreakdown::TransientIndexSource source) {
    auto slice = makeTransientBuffer(data, len);
    if (slice && encoderBreakdown) {
      encoderBreakdown->addTransientIndexBytes(static_cast<u64>(len), source);
    }
    return slice;
  };
  const auto* stream0Snapshot =
      streamBindingSnapshot(bindingSnapshot, 0u);
  const auto* indexSnapshot =
      indexBindingSnapshot(bindingSnapshot);
  {
    PerfScope fvfDecodeBytesScope(perf::countEncodeDrawFvfDecodeCpuTime);
    if (!pv.userVertexData.empty()) {
      // Phase 5-B: prefer pre-batched UP vertex slice when the
      // DrawRun handler did the bulk upload; otherwise fall back
      // to the per-draw upload.
      if (preUploaded && preUploaded->vertex) {
        transientVertexBuffer = preUploaded->vertex;
      } else {
        transientVertexBuffer = makeTransientVertexBuffer(
            pv.userVertexData.data(),
            pv.userVertexData.size(),
            ActiveEncoderBreakdown::TransientVertexSource::User);
      }
      if (transientVertexBuffer) {
        vertexBuffer = transientVertexBuffer.buffer;
        vertexBufferOffset = transientVertexBuffer.offset + hot.streamOffsets[0];
        vertexBytes = pv.userVertexData;
      }
    } else if (hot.streamBuffers[0]) {
      if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[0].value);
          buffer && (buffer->buffer || (stream0Snapshot && stream0Snapshot->valid()))) {
        stream0Record = buffer;
        vertexBuffer = WMT::Buffer{stream0Snapshot
            ? stream0Snapshot->metalHandle
            : buffer->buffer.handle};
        vertexBufferOffset = hot.streamOffsets[0];
        if (traceEncode) {
          std::ostringstream trace;
          trace << "[dxmt9-encode-stream] seq=" << static_cast<unsigned long long>(seqId)
                << " stream=0 slot=1 handle="
                << static_cast<unsigned long long>(hot.streamBuffers[0].value)
                << " liveMetal=0x" << std::hex
                << static_cast<unsigned long long>(buffer->buffer.handle)
                << std::dec
                << " boundMetal=0x" << std::hex
                << static_cast<unsigned long long>(vertexBuffer.handle)
                << std::dec
                << " snapshot=" << (stream0Snapshot ? 1 : 0)
                << " offset=" << vertexBufferOffset
                << " stride=" << hot.streamStrides[0]
                << " shadowBytes=" << buffer->shadow.size()
                << " contents=" << (buffer->contents ? 1 : 0);
          emitQueueTraceLine(trace.str());
        }
        if (const auto bytes = snapshotBufferBytes(stream0Snapshot);
            !bytes.empty()) {
          vertexBytes = bytes;
        } else if (!buffer->shadow.empty()) {
          vertexBytes = buffer->shadow;
        } else if (buffer->contents) {
          vertexBytes = std::span<const u8>(static_cast<const u8*>(buffer->contents),
                                            static_cast<std::size_t>(buffer->desc.size));
        }
      } else if (vertexDecl.streams[0].buffer) {
        const auto bytes = vertexDecl.streams[0].buffer->bytes();
        if (!bytes.empty()) {
          transientVertexBuffer = makeTransientVertexBuffer(
              bytes.data(), bytes.size(),
              ActiveEncoderBreakdown::TransientVertexSource::DeclFallback);
          if (transientVertexBuffer) {
            vertexBuffer = transientVertexBuffer.buffer;
            vertexBufferOffset = transientVertexBuffer.offset + hot.streamOffsets[0];
            vertexBytes = bytes;
          }
        }
      }
    }
  }
  if (streamIbStagingActive(streamIbStagingCache) &&
      indexedDraw &&
      pv.userVertexData.empty() &&
      !stream0Snapshot &&
      stream0Record &&
      stream0Record->buffer &&
      vertexBuffer) {
    if (auto staged = streamIbStagingCache->findOrStage(
            ctx, seqId, hot.streamBuffers[0].value, stream0Record,
            encoderBreakdown, /*indexBuffer=*/false)) {
      vertexBuffer = staged.buffer;
      vertexBufferOffset = staged.offset + hot.streamOffsets[0];
      stream0Staged = true;
    }
  }
  if (traceEncode && !ffLayout && !vertexBytes.empty() && !vertexDecl.elements.empty()) {
    auto readF32 = [&](std::size_t absoluteOffset) {
      float value = 0.0f;
      if (absoluteOffset + sizeof(float) <= vertexBytes.size()) {
        std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(float));
      }
      return value;
    };
    auto readU32 = [&](std::size_t absoluteOffset) {
      u32 value = 0;
      if (absoluteOffset + sizeof(u32) <= vertexBytes.size()) {
        std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(u32));
      }
      return value;
    };

    std::optional<u32> positionOffset;
    std::optional<u32> colorOffset;
    std::optional<u32> texcoord0Offset;
    for (const auto& element : vertexDecl.elements) {
      if (!positionOffset && element.usage == kD3DDeclUsagePosition && element.usageIndex == 0 &&
          element.type == kD3DDeclTypeFloat4) {
        positionOffset = element.offset;
      } else if (!colorOffset && element.usage == kD3DDeclUsageColor && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeD3DColor) {
        colorOffset = element.offset;
      } else if (!texcoord0Offset && element.usage == kD3DDeclUsageTexcoord && element.usageIndex == 0 &&
                 element.type == kD3DDeclTypeFloat4) {
        texcoord0Offset = element.offset;
      }
    }

    if (positionOffset && texcoord0Offset) {
      const std::size_t stride = static_cast<std::size_t>(computeVertexDeclStride(vertexDecl));
      const std::size_t streamBase = static_cast<std::size_t>(hot.streamOffsets[0]);
      std::ostringstream trace;
      trace << "[dxmt9-encode-verts] seq=" << static_cast<unsigned long long>(seqId)
            << " startVertex=" << pv.startVertex
            << " baseVertex=" << pv.baseVertexIndex
            << " stride=" << stride
            << " bytes=" << vertexBytes.size();
      const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 6u);
      for (u32 i = 0; i < tracedVertexCount; ++i) {
        const std::size_t base = streamBase +
                            static_cast<std::size_t>(pv.startVertex + i) * stride;
        trace << " v" << i << "=("
              << readF32(base + *positionOffset + 0) << ","
              << readF32(base + *positionOffset + 4) << ","
              << readF32(base + *positionOffset + 8) << ","
              << readF32(base + *positionOffset + 12) << ")";
        if (colorOffset) {
          trace << " c=0x" << std::hex << readU32(base + *colorOffset) << std::dec;
        }
        trace << " uv=("
              << readF32(base + *texcoord0Offset + 0) << ","
              << readF32(base + *texcoord0Offset + 4) << ","
              << readF32(base + *texcoord0Offset + 8) << ","
              << readF32(base + *texcoord0Offset + 12) << ")";
      }
      emitQueueTraceLine(trace.str());
    }
  }
  // DrawVolatile field state — derived per draw and pushed via
  // setVertexBytes(slot=5) right before drawPrimitives.
  u32 drawVertexStreamOffset = 0;
  u32 drawVertexStreamStride = 0;
  i32 drawVertexBaseIndex = 0;
  // Branch on fixedFunctionPath (which respects the bound shader context),
  // not just ffLayout. A user-bound programmable VS with an FFP-decodable
  // vertex declaration must take the programmable path; gating on ffLayout
  // alone would force such draws through the FFP setup below.
  if (fixedFunctionPath && ffLayout) {
    if (!vertexBuffer) {
      if (traceEncode) {
        emitQueueTraceLine("[dxmt9-encode] seq=" + std::to_string(seqId) +
                           " ordinal=" + std::to_string(drawOrdinal) +
                           " skipped reason=no-vertex-buffer");
      }
      return false;
    }
    {
      PerfScope uniformBuildFfScope(perf::countEncodeDrawUniformBuildCpuTime);
      drawVertexStreamOffset = 0;
      drawVertexStreamStride =
          vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride;
      if (!indexedDraw && drawVertexStreamStride != 0u) {
        vertexBufferOffset += static_cast<uint64_t>(pv.startVertex) *
                              static_cast<uint64_t>(drawVertexStreamStride);
        drawVertexBaseIndex = 0;
      } else {
        drawVertexBaseIndex = indexedDraw ? pv.baseVertexIndex : static_cast<i32>(pv.startVertex);
      }
    }
    {
      PerfScope streamBindFfScope(perf::countEncodeDrawStreamBindCpuTime);
      PerfScope streamBindFfpStreamScope(
          streamBindPhaseSplitPerf
              ? perf::countEncodeDrawStreamBindFfpStreamCpuTime
              : nullptr);
      PerfScope vertexStreamBindFfScope(perf::countEncodeDrawVertexStreamBindCpuTime);
      perf::countEncodeDrawStreamBindFfpStreamCalls(1u);
      if (encoderBreakdown) {
        if (stream0Record) {
          encoderBreakdown->recordStreamResource(0, hot.streamBuffers[0].value,
                                                 stream0Record->desc);
        }
        const u64 stream0StateHandle =
            stream0Staged ? vertexBuffer.handle : hot.streamBuffers[0].value;
        const u64 stream0StateOffset =
            stream0Staged ? vertexBufferOffset : hot.streamOffsets[0];
        encoderBreakdown->recordStreamState(
            0, stream0StateHandle, stream0StateOffset, drawVertexStreamStride);
      }
      if (setVertexBufferCached(vertexBuffer, vertexBufferOffset, 1)) {
        countVertexBufferBind();
        if (encoderBreakdown) {
          encoderBreakdown->recordStreamMetalBind(0);
        }
      }
    }

    const u64 ffTraceTex0 = debug::fixedFunctionTraceTextureHandle();
    const bool forceTrace =
        ffTraceTex0 != 0 && hot.textures[0] && hot.textures[0].value == ffTraceTex0;
    if ((forceTrace || ffTraceRemaining.load(std::memory_order_relaxed) > 0) && !vertexBytes.empty()) {
      bool shouldTrace = forceTrace;
      if (!shouldTrace) {
        int expected = ffTraceRemaining.load(std::memory_order_relaxed);
        while (expected > 0 &&
               !ffTraceRemaining.compare_exchange_weak(expected, expected - 1, std::memory_order_relaxed)) {
        }
        shouldTrace = expected > 0;
      }
      if (shouldTrace) {
        std::ostringstream trace;
        const auto stageStateValue = [&](u32 key, u32 fallback) -> u32 {
          return core::flatStateOr(hot.textureStageStates[0], key, fallback);
        };
        const auto stageStateValueAt = [&](std::size_t stageIndex, u32 key, u32 fallback) -> u32 {
          if (stageIndex >= hot.textureStageStates.size()) {
            return fallback;
          }
          return core::flatStateOr(hot.textureStageStates[stageIndex], key, fallback);
        };
        trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
              << " fvf=0x" << std::hex << vertexDecl.fvf << std::dec
              << " ffLayout=1"
              << " preT=" << (ffLayout->preTransformed ? 1 : 0)
              << " startVertex=" << pv.startVertex
              << " baseVertex=" << pv.baseVertexIndex
              << " startIndex=" << pv.startIndex
              << " primCount=" << pv.primitiveCount
              << " stride=" << drawVertexStreamStride
              << " viewport=(" << ensureFfpVs()->viewportOrigin[0] << "," << ensureFfpVs()->viewportOrigin[1]
              << " " << ensureFfpVs()->viewportSize[0] << "x" << ensureFfpVs()->viewportSize[1] << ")"
              << " zEnable=" << core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u)
              << " zFunc=" << core::flatStateOr(hot.renderStates, RS_Z_FUNC, 0u)
              << " alphaTest="
              << core::flatStateOr(hot.renderStates, RS_ALPHA_TEST_ENABLE, 0u)
              << " alphaFunc="
              << core::flatStateOr(hot.renderStates, RS_ALPHA_FUNC,
                                   static_cast<u32>(CompareFunc::Always))
              << " alphaRef="
              << core::flatStateOr(hot.renderStates, RS_ALPHA_REF, 0u)
              << " alphaBlend="
              << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
              << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
              << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
              << " tci0=0x" << std::hex
              << stageStateValue(TSS_TEXCOORD_INDEX, 0u)
              << std::dec
              << " ttff0=0x" << std::hex
              << stageStateValue(TSS_TEXTURE_TRANSFORM_FLAGS, 0u)
              << std::dec
              << " colorOp0=" << stageStateValue(TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable))
              << " colorArg10=" << stageStateValue(TSS_COLOR_ARG1, 0u)
              << " colorArg20=" << stageStateValue(TSS_COLOR_ARG2, 0u)
              << " alphaOp0=" << stageStateValue(TSS_ALPHA_OP, static_cast<u32>(TextureOp::Disable))
              << " alphaArg10=" << stageStateValue(TSS_ALPHA_ARG1, 0u)
              << " alphaArg20=" << stageStateValue(TSS_ALPHA_ARG2, 0u)
              << " colorOp1=" << stageStateValueAt(1, TSS_COLOR_OP, static_cast<u32>(TextureOp::Disable))
              << " colorArg11=" << stageStateValueAt(1, TSS_COLOR_ARG1, 0u)
              << " colorArg21=" << stageStateValueAt(1, TSS_COLOR_ARG2, 0u)
              << " alphaOp1=" << stageStateValueAt(1, TSS_ALPHA_OP, static_cast<u32>(TextureOp::Disable))
              << " alphaArg11=" << stageStateValueAt(1, TSS_ALPHA_ARG1, 0u)
              << " alphaArg21=" << stageStateValueAt(1, TSS_ALPHA_ARG2, 0u)
              << " elems=" << vertexDecl.elements.size()
              << " tfactor=0x"
              << std::hex
              << core::flatStateOr(hot.renderStates, RS_TEXTURE_FACTOR, 0u)
              << std::dec;
        const auto& texM0 = ensureFfpVs()->ffpTextureTransforms[0];
        trace << " texM0=["
              << texM0[0][0] << "," << texM0[0][1] << ","
              << texM0[0][2] << "," << texM0[0][3] << ";"
              << texM0[1][0] << "," << texM0[1][1] << ","
              << texM0[1][2] << "," << texM0[1][3] << ";"
              << texM0[2][0] << "," << texM0[2][1] << ","
              << texM0[2][2] << "," << texM0[2][3] << ";"
              << texM0[3][0] << "," << texM0[3][1] << ","
              << texM0[3][2] << "," << texM0[3][3] << "]";
        for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
          const auto& e = vertexDecl.elements[i];
          trace << " e" << i << "={s=" << e.stream
                << ",off=" << e.offset
                << ",type=" << e.type
                << ",usage=" << e.usage
                << ",idx=" << e.usageIndex
                << "}";
        }

        auto readF32 = [&](std::size_t absoluteOffset) {
          float value = 0.0f;
          if (absoluteOffset + sizeof(float) <= vertexBytes.size()) {
            std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(float));
          }
          return value;
        };
        auto readU32 = [&](std::size_t absoluteOffset) {
          u32 value = 0;
          if (absoluteOffset + sizeof(u32) <= vertexBytes.size()) {
            std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(u32));
          }
          return value;
        };

        const std::size_t stride = static_cast<std::size_t>(
            drawVertexStreamStride ? drawVertexStreamStride : ffLayout->stride);
        const u32 tracedVertexCount = std::min<u32>(static_cast<u32>(vertexCount), 24u);
        for (u32 i = 0; i < tracedVertexCount; ++i) {
          const std::size_t base = static_cast<std::size_t>(hot.streamOffsets[0]) +
                                   static_cast<std::size_t>(pv.baseVertexIndex + static_cast<int>(i)) *
                                       stride;
          trace << " v" << i << "=("
                << readF32(base + ffLayout->positionOffset + 0) << ","
                << readF32(base + ffLayout->positionOffset + 4) << ","
                << readF32(base + ffLayout->positionOffset + 8) << ","
                << readF32(base + ffLayout->positionOffset + 12) << ")";
          if (ffLayout->hasDiffuse) {
            const u32 rgba = readU32(base + ffLayout->diffuseOffset);
            trace << " c" << i << "=0x" << std::hex << rgba << std::dec;
          }
          if (ffLayout->hasTexcoord[0]) {
            trace << " uv" << i << "=("
                  << readF32(base + ffLayout->texcoordOffset[0] + 0) << ","
                  << readF32(base + ffLayout->texcoordOffset[0] + 4) << ")";
          }
        }

        if (hot.indexBuffer || !pv.userIndexData.empty()) {
          const auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
          std::span<const u8> indexBytes;
          if (!pv.userIndexData.empty()) {
            indexBytes = pv.userIndexData;
          } else if (indexRecord && !indexRecord->shadow.empty()) {
            indexBytes = indexRecord->shadow;
          } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
            indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                             static_cast<std::size_t>(indexRecord->desc.size));
          }
          if (!indexBytes.empty()) {
            trace << " idx=";
            const std::size_t start = static_cast<std::size_t>(pv.startIndex) * indexElementSize(pv.indexType);
            const u32 tracedIndexCount =
                std::min<u32>(primitiveCount * 3u, 36u);
            for (u32 i = 0; i < tracedIndexCount; ++i) {
              if (i) {
                trace << ",";
              }
              if (pv.indexType == IndexType::UInt16 &&
                  start + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                u16 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u16),
                            sizeof(u16));
                trace << index;
              } else if (pv.indexType == IndexType::UInt32 &&
                         start + static_cast<std::size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
                u32 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u32),
                            sizeof(u32));
                trace << index;
              } else {
                trace << '?';
              }
            }
            trace << " ref=";
            const u32 tracedRefs = std::min<u32>(12u, tracedIndexCount);
            for (u32 i = 0; i < tracedRefs; ++i) {
              u32 vertexIndex = 0;
              bool haveIndex = false;
              if (pv.indexType == IndexType::UInt16 &&
                  start + static_cast<std::size_t>(i + 1) * sizeof(u16) <= indexBytes.size()) {
                u16 index = 0;
                std::memcpy(&index, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u16),
                            sizeof(u16));
                vertexIndex = static_cast<u32>(index);
                haveIndex = true;
              } else if (pv.indexType == IndexType::UInt32 &&
                         start + static_cast<std::size_t>(i + 1) * sizeof(u32) <= indexBytes.size()) {
                std::memcpy(&vertexIndex, indexBytes.data() + start + static_cast<std::size_t>(i) * sizeof(u32),
                            sizeof(u32));
                haveIndex = true;
              }
              if (!haveIndex) {
                break;
              }
              const std::size_t refBase =
                  static_cast<std::size_t>(hot.streamOffsets[0]) +
                  static_cast<std::size_t>(pv.baseVertexIndex + static_cast<int>(vertexIndex)) * stride;
              trace << " r" << i << "#" << vertexIndex << "=("
                    << readF32(refBase + ffLayout->positionOffset + 0) << ","
                    << readF32(refBase + ffLayout->positionOffset + 4) << ","
                    << readF32(refBase + ffLayout->positionOffset + 8) << ","
                    << readF32(refBase + ffLayout->positionOffset + 12) << ")";
              if (ffLayout->hasTexcoord[0]) {
                trace << " uv=("
                      << readF32(refBase + ffLayout->texcoordOffset[0] + 0) << ","
                      << readF32(refBase + ffLayout->texcoordOffset[0] + 4) << ")";
              }
              if (ffLayout->hasDiffuse) {
                const u32 rgba = readU32(refBase + ffLayout->diffuseOffset);
                trace << " c=0x" << std::hex << rgba << std::dec;
              }
            }
          }
        }
        trace << " tex0=";
        if (hot.textures[0]) {
          trace << static_cast<unsigned long long>(hot.textures[0].value);
        } else {
          trace << 0;
        }
        trace << " tex1=";
        if (hot.textures.size() > 1 && hot.textures[1]) {
          trace << static_cast<unsigned long long>(hot.textures[1].value);
        } else {
          trace << 0;
        }
        emitQueueTraceLine(trace.str());
      }
    }
  }
  if (vertexBuffer && !fixedFunctionPath) {
    const u64 ffTraceTex0 = debug::fixedFunctionTraceTextureHandle();
    const bool forceTrace =
        ffTraceTex0 != 0 && hot.textures[0] && hot.textures[0].value == ffTraceTex0;
    if (forceTrace) {
      std::ostringstream trace;
      trace << "[dxmt9-ffp] seq=" << static_cast<unsigned long long>(seqId)
            << " fvf=0x" << std::hex << vertexDecl.fvf << std::dec
            << " ffLayout=" << (ffLayout ? 1 : 0)
            << " baseVertex=" << pv.baseVertexIndex
            << " startIndex=" << pv.startIndex
            << " primCount=" << pv.primitiveCount
            << " stride="
            << (ffLayout ? (vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride)
                         : computeVertexDeclStride(vertexDecl))
            << " elems=" << vertexDecl.elements.size();
      for (std::size_t i = 0; i < vertexDecl.elements.size(); ++i) {
        const auto& e = vertexDecl.elements[i];
        trace << " e" << i << "={s=" << e.stream
              << ",off=" << e.offset
              << ",type=" << e.type
              << ",usage=" << e.usage
              << ",idx=" << e.usageIndex
              << "}";
      }
      if (ffLayout && !vertexBytes.empty()) {
        auto readF32 = [&](std::size_t absoluteOffset) {
          float value = 0.0f;
          if (absoluteOffset + sizeof(float) <= vertexBytes.size()) {
            std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(float));
          }
          return value;
        };
        auto readU32 = [&](std::size_t absoluteOffset) {
          u32 value = 0;
          if (absoluteOffset + sizeof(u32) <= vertexBytes.size()) {
            std::memcpy(&value, vertexBytes.data() + absoluteOffset, sizeof(u32));
          }
          return value;
        };
        auto appendVertexRef = [&](std::string_view prefix, u32 ordinal, u32 vertexIndex) {
          const int effectiveVertexIndex =
              pv.baseVertexIndex + static_cast<int>(vertexIndex);
          if (effectiveVertexIndex < 0) {
            return;
          }
          const std::size_t refBase =
              static_cast<std::size_t>(hot.streamOffsets[0]) +
              static_cast<std::size_t>(effectiveVertexIndex) *
                  static_cast<std::size_t>(
                      drawVertexStreamStride ? drawVertexStreamStride : ffLayout->stride);
          trace << " " << prefix << ordinal << "#" << vertexIndex << "=("
                << readF32(refBase + ffLayout->positionOffset + 0) << ","
                << readF32(refBase + ffLayout->positionOffset + 4) << ","
                << readF32(refBase + ffLayout->positionOffset + 8) << ")";
          if (ffLayout->hasDiffuse) {
            trace << " c=0x" << std::hex
                  << readU32(refBase + ffLayout->diffuseOffset)
                  << std::dec;
          }
          if (ffLayout->hasTexcoord[0]) {
            trace << " uv=("
                  << readF32(refBase + ffLayout->texcoordOffset[0] + 0)
                  << ","
                  << readF32(refBase + ffLayout->texcoordOffset[0] + 4)
                  << ")";
          }
        };
        std::span<const u8> indexBytes;
        if (!pv.userIndexData.empty()) {
          indexBytes = pv.userIndexData;
        } else if (hot.indexBuffer) {
          const auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
          if (indexRecord && !indexRecord->shadow.empty()) {
            indexBytes = indexRecord->shadow;
          } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
            indexBytes = std::span<const u8>(
                static_cast<const u8*>(indexRecord->contents),
                static_cast<std::size_t>(indexRecord->desc.size));
          }
        }
        if (indexedDraw && !indexBytes.empty()) {
          const std::size_t indexStart =
              static_cast<std::size_t>(pv.startIndex) *
              indexElementSize(pv.indexType);
          const u32 tracedIndexCount = std::min<u32>(primitiveCount * 3u, 36u);
          trace << " idx=";
          for (u32 i = 0; i < tracedIndexCount; ++i) {
            if (i != 0u) {
              trace << ",";
            }
            std::optional<u32> vertexIndex;
            const std::size_t offset =
                indexStart + static_cast<std::size_t>(i) *
                indexElementSize(pv.indexType);
            if (pv.indexType == IndexType::UInt16 &&
                offset + sizeof(u16) <= indexBytes.size()) {
              u16 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            } else if (pv.indexType == IndexType::UInt32 &&
                       offset + sizeof(u32) <= indexBytes.size()) {
              u32 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            }
            if (vertexIndex.has_value()) {
              trace << *vertexIndex;
            } else {
              trace << '?';
            }
          }
          const u32 tracedRefs = std::min<u32>(12u, tracedIndexCount);
          for (u32 i = 0; i < tracedRefs; ++i) {
            const std::size_t offset =
                indexStart + static_cast<std::size_t>(i) *
                indexElementSize(pv.indexType);
            std::optional<u32> vertexIndex;
            if (pv.indexType == IndexType::UInt16 &&
                offset + sizeof(u16) <= indexBytes.size()) {
              u16 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            } else if (pv.indexType == IndexType::UInt32 &&
                       offset + sizeof(u32) <= indexBytes.size()) {
              u32 value = 0;
              std::memcpy(&value, indexBytes.data() + offset, sizeof(value));
              vertexIndex = value;
            }
            if (!vertexIndex.has_value()) {
              break;
            }
            appendVertexRef("r", i, *vertexIndex);
          }
        } else {
          const u32 tracedVertexCount =
              std::min<u32>(static_cast<u32>(vertexCount), 12u);
          for (u32 i = 0; i < tracedVertexCount; ++i) {
            appendVertexRef("v", i, pv.startVertex + i);
          }
        }
      }
      emitQueueTraceLine(trace.str());
    }
    {
      PerfScope uniformBuildVsScope(perf::countEncodeDrawUniformBuildCpuTime);
      drawVertexStreamOffset = 0;
      drawVertexStreamStride =
          ffLayout ? (vertexDecl.streams[0].stride ? vertexDecl.streams[0].stride : ffLayout->stride)
                   : computeVertexDeclStride(vertexDecl);
      if (!indexedDraw && drawVertexStreamStride != 0u) {
        vertexBufferOffset += static_cast<uint64_t>(pv.startVertex) *
                              static_cast<uint64_t>(drawVertexStreamStride);
        drawVertexBaseIndex = 0;
      } else {
        drawVertexBaseIndex = indexedDraw ? pv.baseVertexIndex : static_cast<i32>(pv.startVertex);
      }
    }
    {
      PerfScope streamBindVsScope(perf::countEncodeDrawStreamBindCpuTime);
      PerfScope streamBindShaderStreamScope(
          streamBindPhaseSplitPerf
              ? perf::countEncodeDrawStreamBindShaderStreamCpuTime
              : nullptr);
      PerfScope vertexStreamBindVsScope(perf::countEncodeDrawVertexStreamBindCpuTime);
      perf::countEncodeDrawStreamBindShaderStreamCalls(1u);
      if (encoderBreakdown) {
        if (stream0Record) {
          encoderBreakdown->recordStreamResource(0, hot.streamBuffers[0].value,
                                                 stream0Record->desc);
        }
        const u64 stream0StateHandle =
            stream0Staged ? vertexBuffer.handle : hot.streamBuffers[0].value;
        const u64 stream0StateOffset =
            stream0Staged ? vertexBufferOffset : hot.streamOffsets[0];
        encoderBreakdown->recordStreamState(
            0, stream0StateHandle, stream0StateOffset, drawVertexStreamStride);
      }
      if (setVertexBufferCached(vertexBuffer, vertexBufferOffset, 1)) {
        countVertexBufferBind();
        if (encoderBreakdown) {
          encoderBreakdown->recordStreamMetalBind(0);
        }
      }
      for (const auto& streamBinding : bindingPacket.extraStreams) {
        WMT::Buffer extraVertexBuffer{};
        uint64_t extraVertexBufferOffset = streamBinding.offset;
        const u32 stream = streamBinding.stream;
        uint64_t liveMetalHandle = 0;
        std::size_t shadowBytes = 0;
        bool hasContents = false;
        bool usedDeclBytes = false;
        bool extraStaged = false;
        const resources::BufferRecord* extraRecord = nullptr;
        const auto* extraSnapshot =
            streamBindingSnapshot(bindingSnapshot, stream);
        if (hot.streamBuffers[stream]) {
          if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
              buffer && (buffer->buffer || (extraSnapshot && extraSnapshot->valid()))) {
            extraRecord = buffer;
            extraVertexBuffer = WMT::Buffer{extraSnapshot
                ? extraSnapshot->metalHandle
                : buffer->buffer.handle};
            liveMetalHandle = buffer->buffer.handle;
            shadowBytes = buffer->shadow.size();
            hasContents = buffer->contents != nullptr;
          }
        }
        if (!extraVertexBuffer && vertexDecl.streams[stream].buffer) {
          const auto bytes = vertexDecl.streams[stream].buffer->bytes();
          if (!bytes.empty()) {
            if (auto slice = makeTransientVertexBuffer(
                    bytes.data(), bytes.size(),
                    ActiveEncoderBreakdown::TransientVertexSource::DeclFallback)) {
              extraVertexBuffer = slice.buffer;
              extraVertexBufferOffset += slice.offset;
              usedDeclBytes = true;
            }
          }
        }
        if (streamIbStagingActive(streamIbStagingCache) &&
            indexedDraw &&
            !extraSnapshot &&
            extraRecord &&
            extraRecord->buffer &&
            extraVertexBuffer) {
          if (auto staged = streamIbStagingCache->findOrStage(
                  ctx, seqId, hot.streamBuffers[stream].value, extraRecord,
                  encoderBreakdown, /*indexBuffer=*/false)) {
            extraVertexBuffer = staged.buffer;
            extraVertexBufferOffset = staged.offset + streamBinding.offset;
            extraStaged = true;
          }
        }
        if (traceEncode) {
          std::ostringstream trace;
          trace << "[dxmt9-encode-stream] seq=" << static_cast<unsigned long long>(seqId)
                << " stream=" << stream
                << " slot=" << streamBinding.metalSlot
                << " handle="
                << static_cast<unsigned long long>(hot.streamBuffers[stream].value)
                << " liveMetal=0x" << std::hex
                << static_cast<unsigned long long>(liveMetalHandle)
                << " boundMetal=0x"
                << static_cast<unsigned long long>(extraVertexBuffer.handle)
                << std::dec
                << " snapshot=" << (extraSnapshot ? 1 : 0)
                << " offset=" << extraVertexBufferOffset
                << " stride=" << streamBinding.stride
                << " declFallback=" << (usedDeclBytes ? 1 : 0)
                << " shadowBytes=" << shadowBytes
                << " contents=" << (hasContents ? 1 : 0)
                << " bound=" << (extraVertexBuffer ? 1 : 0);
          emitQueueTraceLine(trace.str());
        }
        if (extraVertexBuffer) {
          if (encoderBreakdown) {
            if (extraRecord) {
              encoderBreakdown->recordStreamResource(
                  stream, hot.streamBuffers[stream].value, extraRecord->desc);
            }
            const u64 extraStateHandle =
                extraStaged ? extraVertexBuffer.handle : hot.streamBuffers[stream].value;
            const u64 extraStateOffset =
                extraStaged ? extraVertexBufferOffset : hot.streamOffsets[stream];
            encoderBreakdown->recordStreamState(
                stream, extraStateHandle, extraStateOffset,
                streamBinding.stride);
          }
          if (setVertexBufferCached(extraVertexBuffer, extraVertexBufferOffset,
                                    streamBinding.metalSlot)) {
            countVertexBufferBind();
            if (encoderBreakdown) {
              encoderBreakdown->recordStreamMetalBind(stream);
            }
          }
        }
      }
    }
  }
  // Phase 3-E: texture / sampler binding is BaseDrawState-only.
  // R-BACK-12.24 — texture/sampler resources travel on the direct render
  // encoder lane (the validated Stage 1 binding path) regardless of
  // whether the constant argbuf hybrid is active.
  if (!effectiveSkipBaseStateBind) {
    PerfScope streamBindTexScope(perf::countEncodeDrawStreamBindCpuTime);
    PerfScope streamBindTexturePhaseScope(
        streamBindPhaseSplitPerf
            ? perf::countEncodeDrawStreamBindTexturePhaseCpuTime
            : nullptr);
    PerfScope textureSamplerBindScope(perf::countEncodeDrawTextureSamplerBindCpuTime);
    perf::countEncodeDrawStreamBindTexturePhaseCalls(1u);
    const bool samplerSupportsArgumentBuffers = dxmt9::shaders::argbufResourceArrayEnabled();
    const bool textureSamplerDirectSplitPerf = textureSamplerDirectSplitPerfEnabled();
    auto resolveFragmentSamplerState =
        [&](const core::FlatStateSet<core::kMaxSamplerStates>& samplerStates,
            u32 textureLod) -> std::pair<WMT::Reference<WMT::SamplerState>, WMT::SamplerState> {
      perf::countEncodeDrawTextureSamplerSamplerLookupCalls(1u);
      PerfScope samplerLookupScope(
          perf::countEncodeDrawTextureSamplerSamplerLookupCpuTime);
      if (suppressBaseStateLookup(ctx)) {
        return {WMT::Reference<WMT::SamplerState>{}, ctx.drawRecorder->fragmentSamplerState};
      }
      auto samplerRef = ctx.cache.samplerStateFor(
          ctx.device, samplerStates,
          static_cast<float>(textureLod),
          samplerSupportsArgumentBuffers);
      return {samplerRef, WMT::SamplerState{samplerRef.handle}};
    };
    if (!bindingPacket.fragmentTextureSamplers.empty()) {
      struct ResolvedFragmentTextureSamplerBinding {
        u32 stage = 0;
        core::Handle textureHandle{};
        u32 textureLod = 0;
        const resources::TextureRecord* textureRecord = nullptr;
        WMT::Texture texture{};
        u64 samplerStateHash = 0;
        core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
        bool srgbTexture = false;
        WMT::Reference<WMT::SamplerState> samplerRef{};
        WMT::SamplerState sampler{};
      };
      std::array<ResolvedFragmentTextureSamplerBinding, core::kMaxSamplers>
          resolvedFragmentBindings{};
      std::size_t resolvedFragmentBindingCount = 0;
      {
        PerfScope fragmentResolveScope(
            perf::countEncodeDrawTextureSamplerFragmentResolveCpuTime);
        perf::countEncodeDrawTextureSamplerFragmentResolveCalls(1u);
        for (const auto& binding : bindingPacket.fragmentTextureSamplers) {
          const auto stage = binding.stage;
          const auto textureHandle = binding.texture;
          if (const u64 skipped = debug::skippedTextureHandle();
              skipped != 0ull && textureHandle.value == skipped) {
            if (traceEncode || debug::shouldTraceTexture(textureHandle)) {
              std::ostringstream out;
              out << "[dxmt9-debug] skip draw seq=" << static_cast<unsigned long long>(seqId)
                  << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
                  << " tex" << stage << "=" << static_cast<unsigned long long>(textureHandle.value);
              emitQueueTraceLine(out.str());
            }
            return false;
          }
          auto& resolved = resolvedFragmentBindings[resolvedFragmentBindingCount++];
          resolved.stage = stage;
          resolved.textureHandle = textureHandle;
          resolved.textureLod = binding.textureLod;
          resolved.samplerStateHash = binding.samplerStateHash;
          resolved.samplerStates = binding.samplerStates;
          const bool srgbTexture =
              core::flatStateOr(hot.samplerStates[stage], core::SAMP_SRGB_TEXTURE, 0u) != 0;
          resolved.srgbTexture = srgbTexture;
          if (textureSamplerDirectSplitPerf) {
            perf::countEncodeDrawTextureSamplerFragmentResolveTextureCalls(1u);
          }
          PerfScope fragmentResolveTextureScope(
              textureSamplerDirectSplitPerf
                  ? perf::countEncodeDrawTextureSamplerFragmentResolveTextureCpuTime
                  : nullptr);
          if (auto* texture = ctx.pool.findTexture(textureHandle.value); texture && texture->texture) {
            resolved.textureRecord = texture;
            resolved.texture = resources::textureForShaderRead(*texture, srgbTexture);
          }
        }
      }

      // R-BACK-12.22..12.26 (resource-array sub-mode) — when this pass runs
      // the resource-array lane, fragment-stage textures/samplers travel
      // through the slot-30 argbuf (writes + useResource residency) instead
      // of the direct lane. Only the FFP s0..s7 fragment stages (<
      // kArgbufResourceArrayStageCount) ride the argbuf; any higher stage
      // stays direct. The argbuf texture array is homogeneously
      // texture2d<float>, so this MUST match the emitter's
      // pixelResourceArrayEligible decision: every used stage < 8 AND every
      // bound texture is 2D. A cube/volume binding (or a stage >= 8) forces
      // the WHOLE draw back onto the direct lane — the emitter emitted the
      // constants-only-hybrid form with direct [[texture(N)]] params for
      // exactly this shader, so a split would leave those params unbound.
      // Trace lines are kept identical to the direct path so capture
      // diffing is unchanged.
      bool fragmentResourceArrayEligible = useResourceArrayArgbuf;
      if (fragmentResourceArrayEligible) {
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          const auto& binding = resolvedFragmentBindings[i];
          if (binding.stage >= dxmt9::shaders::kArgbufResourceArrayStageCount) {
            fragmentResourceArrayEligible = false;
            break;
          }
          if (binding.textureRecord &&
              binding.textureRecord->desc.type != core::TextureType::TwoD &&
              binding.textureRecord->desc.type != core::TextureType::Array2D) {
            fragmentResourceArrayEligible = false;
            break;
          }
        }
      }
      const bool useResourceArrayLane = fragmentResourceArrayEligible;
      if (useResourceArrayLane) {
        PerfScope fragmentResourceArrayScope(
            perf::countEncodeDrawTextureSamplerFragmentResourceArrayCpuTime);
        perf::countEncodeDrawTextureSamplerFragmentResourceArrayCalls(1u);
        std::array<dxmt9::argbuf_hybrid::ResourceArrayBinding,
                   dxmt9::shaders::kArgbufResourceArrayStageCount>
            argbufBindings{};
        std::size_t argbufBindingCount = 0;
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          auto& binding = resolvedFragmentBindings[i];
          if (binding.stage >= dxmt9::shaders::kArgbufResourceArrayStageCount) {
            continue;
          }
          if ((traceEncode || debug::shouldTraceTexture(binding.textureHandle)) &&
              binding.textureRecord) {
            std::ostringstream out;
            out << "[dxmt9-texture] bind stage=" << binding.stage
                << " handle=0x" << std::hex << binding.textureHandle.value << std::dec
                << " format=" << static_cast<unsigned>(binding.textureRecord->desc.format)
                << " size=" << binding.textureRecord->desc.width << "x"
                << binding.textureRecord->desc.height
                << " levels=" << binding.textureRecord->desc.levels
                << " lod=" << binding.textureLod;
            appendSamplerTrace(out, binding.samplerStates, binding.srgbTexture);
            emitTextureTraceLine(out.str());
          }
          auto& slot = argbufBindings[argbufBindingCount++];
          slot.stage = binding.stage;
          slot.texture = binding.texture;
          auto [samplerRef, sampler] =
              resolveFragmentSamplerState(binding.samplerStates, binding.textureLod);
          binding.samplerRef = samplerRef;
          binding.sampler = sampler;
          slot.sampler = binding.sampler;
          if (binding.samplerRef) {
            ctx.queue.retainSamplerForSeq(binding.samplerRef, seqId);
          }
          if (binding.texture) countTextureBind();
          if (binding.sampler) countSamplerBind();
        }
        if (!suppressRecordedMetalCalls(ctx)) {
          dxmt9::argbuf_hybrid::populateResourceBindings(
              argbufEncoderForDraw,
              std::span<const dxmt9::argbuf_hybrid::ResourceArrayBinding>(
                  argbufBindings.data(), argbufBindingCount),
              /*recorder=*/nullptr, encoder);
        }
      } else {
        PerfScope fragmentDirectScope(
            perf::countEncodeDrawTextureSamplerFragmentDirectCpuTime);
        perf::countEncodeDrawTextureSamplerFragmentDirectCalls(1u);
        for (std::size_t i = 0; i < resolvedFragmentBindingCount; ++i) {
          auto& binding = resolvedFragmentBindings[i];
          if (binding.texture) {
            if (textureSamplerDirectSplitPerf) {
              perf::countEncodeDrawTextureSamplerFragmentDirectTextureCalls(1u);
            }
            PerfScope fragmentDirectTextureScope(
                textureSamplerDirectSplitPerf
                    ? perf::countEncodeDrawTextureSamplerFragmentDirectTextureCpuTime
                    : nullptr);
            bool skipTextureBind = false;
            if (textureSamplerShadow && binding.stage < core::kMaxSamplers) {
              auto& slot = textureSamplerShadow->fragmentTextures[binding.stage];
              const auto hash = textureSamplerShadowHash(
                  kFragmentTextureShadowTag,
                  static_cast<std::uint8_t>(binding.stage),
                  binding.texture.handle);
              skipTextureBind = textureSamplerShadowMatches(
                  slot, hash, binding.texture.handle);
              if (!skipTextureBind) {
                textureSamplerShadowStore(slot, hash, binding.texture.handle);
              }
            }
            if (skipTextureBind) {
              countTextureBindSkipped();
            } else {
              if ((traceEncode || debug::shouldTraceTexture(binding.textureHandle)) &&
                  binding.textureRecord) {
                std::ostringstream out;
                out << "[dxmt9-texture] bind stage=" << binding.stage
                    << " handle=0x" << std::hex << binding.textureHandle.value << std::dec
                    << " format=" << static_cast<unsigned>(binding.textureRecord->desc.format)
                    << " size=" << binding.textureRecord->desc.width << "x"
                    << binding.textureRecord->desc.height
                    << " levels=" << binding.textureRecord->desc.levels
                    << " lod=" << binding.textureLod;
                appendSamplerTrace(out, binding.samplerStates, binding.srgbTexture);
                emitTextureTraceLine(out.str());
              }
              {
                if (textureSamplerDirectSplitPerf) {
                  perf::countEncodeDrawTextureSamplerFragmentDirectTextureSetCalls(1u);
                }
                PerfScope fragmentDirectTextureSetScope(
                    textureSamplerDirectSplitPerf
                        ? perf::countEncodeDrawTextureSamplerFragmentDirectTextureSetCpuTime
                        : nullptr);
                recordedSetFragmentTexture(ctx, encoder, binding.texture,
                                           static_cast<std::uint8_t>(binding.stage));
              }
              countTextureBind();
            }
          }
          {
            if (textureSamplerDirectSplitPerf) {
              perf::countEncodeDrawTextureSamplerFragmentDirectSamplerCalls(1u);
            }
            PerfScope fragmentDirectSamplerScope(
                textureSamplerDirectSplitPerf
                    ? perf::countEncodeDrawTextureSamplerFragmentDirectSamplerCpuTime
                    : nullptr);
            bool skipSamplerBind = false;
            const auto samplerHash = samplerBindShadowHash(
                kFragmentSamplerShadowTag,
                static_cast<std::uint8_t>(binding.stage),
                binding.samplerStateHash,
                binding.textureLod,
                samplerSupportsArgumentBuffers);
            if (textureSamplerShadow && binding.stage < core::kMaxSamplers) {
              auto& slot = textureSamplerShadow->fragmentSamplers[binding.stage];
              skipSamplerBind = samplerBindShadowMatches(
                  slot, samplerHash, binding.samplerStates, binding.textureLod,
                  samplerSupportsArgumentBuffers);
            }
            if (skipSamplerBind) {
              perf::countEncodeDrawTextureSamplerSamplerLookupSkippedPrehandle(1u);
              countSamplerBindSkipped();
            } else {
              auto [samplerRef, sampler] =
                  resolveFragmentSamplerState(binding.samplerStates, binding.textureLod);
              if (sampler) {
                if (textureSamplerShadow && binding.stage < core::kMaxSamplers) {
                  auto& slot = textureSamplerShadow->fragmentSamplers[binding.stage];
                  samplerBindShadowStore(
                      slot, samplerHash, binding.samplerStates, binding.textureLod,
                      samplerSupportsArgumentBuffers, sampler.handle);
                }
                binding.samplerRef = samplerRef;
                binding.sampler = sampler;
                {
                  if (textureSamplerDirectSplitPerf) {
                    perf::countEncodeDrawTextureSamplerFragmentDirectSamplerSetCalls(1u);
                  }
                  PerfScope fragmentDirectSamplerSetScope(
                      textureSamplerDirectSplitPerf
                          ? perf::countEncodeDrawTextureSamplerFragmentDirectSamplerSetCpuTime
                          : nullptr);
                  recordedSetFragmentSamplerState(ctx, encoder, binding.sampler,
                                                  static_cast<std::uint8_t>(binding.stage));
                }
                countSamplerBind();
              }
            }
          }
        }
      }
    }
    // D3DSAMP_MIPMAPLODBIAS (gap_d3d9 B.3): the per-sampler mip LOD bias is
    // applied at sample time in MSL via `texture.sample(..., bias(b))` —
    // Metal samplers have no LOD-bias field. PSO-variant gated: the fragment
    // shader only declares `constant SamplerLodBias& samplerLodBias
    // [[buffer(4)]]` when the active variant's `samplerLodBias` bit is set, so
    // the slot-4 upload + bind happens on the SAME predicate the PSO key /
    // emitters read — anySamplerLodBiasNonzero(drawState). This keeps emit and
    // bind in lockstep: a declared-but-unbound reference is a Metal error, and
    // a bound-but-undeclared slot is wasted. The common no-bias draw skips both
    // the 32-byte upload and the bind entirely. Bound on the same direct
    // resource lane as textures/samplers, so it is consistent under the
    // argbuf-hybrid path too (textures/samplers also stay direct there).
    if (anySamplerLodBiasNonzero(drawState)) {
      PerfScope lodBiasScope(perf::countEncodeDrawTextureSamplerLodBiasCpuTime);
      perf::countEncodeDrawTextureSamplerLodBiasCalls(1u);
      SamplerLodBias lodBias = buildSamplerLodBias(drawState);
      auto slice = uploadTransientBuffer(&lodBias, sizeof(lodBias), alignof(SamplerLodBias));
      if (slice && !suppressRecordedMetalCalls(ctx)) {
        encoder.setFragmentBuffer(slice.buffer, slice.offset, 4);
        countUniformBufferBinds(1);
      }
    }
    if (!bindingPacket.vertexTextureSamplers.empty()) {
      struct ResolvedVertexTextureSamplerBinding {
        u32 stage = 0;
        core::Handle textureHandle{};
        u32 textureLod = 0;
        const resources::TextureRecord* textureRecord = nullptr;
        WMT::Texture texture{};
        u64 samplerStateHash = 0;
        core::FlatStateSet<core::kMaxSamplerStates> samplerStates{};
        WMT::Reference<WMT::SamplerState> samplerRef{};
        WMT::SamplerState sampler{};
      };
      std::array<ResolvedVertexTextureSamplerBinding, core::kMaxVertexTextureSamplers>
          resolvedVertexBindings{};
      std::size_t resolvedVertexBindingCount = 0;
      {
        PerfScope vertexResolveScope(
            perf::countEncodeDrawTextureSamplerVertexResolveCpuTime);
        perf::countEncodeDrawTextureSamplerVertexResolveCalls(1u);
        for (const auto& binding : bindingPacket.vertexTextureSamplers) {
          const auto stage = binding.stage;
          const auto textureHandle = binding.texture;
          auto& resolved = resolvedVertexBindings[resolvedVertexBindingCount++];
          resolved.stage = stage;
          resolved.textureHandle = textureHandle;
          resolved.textureLod = binding.textureLod;
          resolved.samplerStateHash = binding.samplerStateHash;
          resolved.samplerStates = binding.samplerStates;
          if (auto* texture = ctx.pool.findTexture(textureHandle.value); texture && texture->texture) {
            const u32 textureSlot = core::kVertexTextureSampler0 + stage;
            const bool srgbTexture =
                core::flatStateOr(hot.samplerStates[textureSlot], core::SAMP_SRGB_TEXTURE, 0u) != 0;
            resolved.textureRecord = texture;
            resolved.texture = resources::textureForShaderRead(*texture, srgbTexture);
          }
          if (suppressBaseStateLookup(ctx)) {
            resolved.sampler = ctx.drawRecorder->fragmentSamplerState;
          } else {
            resolved.samplerRef = ctx.cache.samplerStateFor(
                ctx.device, binding.samplerStates,
                static_cast<float>(binding.textureLod),
                dxmt9::shaders::argbufResourceArrayEnabled());
            resolved.sampler = WMT::SamplerState{resolved.samplerRef.handle};
          }
        }
      }

      {
        PerfScope vertexDirectScope(
            perf::countEncodeDrawTextureSamplerVertexDirectCpuTime);
        perf::countEncodeDrawTextureSamplerVertexDirectCalls(1u);
        for (std::size_t i = 0; i < resolvedVertexBindingCount; ++i) {
          const auto& binding = resolvedVertexBindings[i];
          if (binding.texture) {
            bool skipTextureBind = false;
            if (textureSamplerShadow && binding.stage < core::kMaxVertexTextureSamplers) {
              auto& slot = textureSamplerShadow->vertexTextures[binding.stage];
              const auto hash = textureSamplerShadowHash(
                  kVertexTextureShadowTag,
                  static_cast<std::uint8_t>(binding.stage),
                  binding.texture.handle);
              skipTextureBind = textureSamplerShadowMatches(
                  slot, hash, binding.texture.handle);
              if (!skipTextureBind) {
                textureSamplerShadowStore(slot, hash, binding.texture.handle);
              }
            }
            if (skipTextureBind) {
              countTextureBindSkipped();
            } else {
              if ((traceEncode || debug::shouldTraceTexture(binding.textureHandle)) &&
                  binding.textureRecord) {
                std::ostringstream out;
                out << "[dxmt9-texture] bind vertex stage=" << binding.stage
                    << " handle=0x" << std::hex << binding.textureHandle.value << std::dec
                    << " format=" << static_cast<unsigned>(binding.textureRecord->desc.format)
                    << " size=" << binding.textureRecord->desc.width << "x"
                    << binding.textureRecord->desc.height
                    << " levels=" << binding.textureRecord->desc.levels
                    << " lod=" << binding.textureLod;
                emitTextureTraceLine(out.str());
              }
              recordedSetVertexTexture(ctx, encoder, binding.texture,
                                       static_cast<std::uint8_t>(binding.stage));
              countTextureBind();
            }
          }
          if (binding.sampler) {
            bool skipSamplerBind = false;
            const auto samplerHash = samplerBindShadowHash(
                kVertexSamplerShadowTag,
                static_cast<std::uint8_t>(binding.stage),
                binding.samplerStateHash,
                binding.textureLod,
                samplerSupportsArgumentBuffers);
            if (textureSamplerShadow && binding.stage < core::kMaxVertexTextureSamplers) {
              auto& slot = textureSamplerShadow->vertexSamplers[binding.stage];
              skipSamplerBind = samplerBindShadowMatches(
                  slot, samplerHash, binding.samplerStates, binding.textureLod,
                  samplerSupportsArgumentBuffers);
              if (!skipSamplerBind) {
                samplerBindShadowStore(
                    slot, samplerHash, binding.samplerStates, binding.textureLod,
                    samplerSupportsArgumentBuffers, binding.sampler.handle);
              }
            }
            if (skipSamplerBind) {
              countSamplerBindSkipped();
            } else {
              recordedSetVertexSamplerState(ctx, encoder, binding.sampler,
                                            static_cast<std::uint8_t>(binding.stage));
              countSamplerBind();
            }
          }
        }
      }
    }
  }
  if (traceEncode) {
    std::ostringstream out;
    out << "[dxmt9-encode] seq=" << static_cast<unsigned long long>(seqId)
        << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
        << " draw rt0=" << static_cast<unsigned long long>(hot.colorAttachments[0].handle.value)
        << " ds=" << static_cast<unsigned long long>(hot.depthStencil.handle.value)
        << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
        << " tex1=" << static_cast<unsigned long long>(hot.textures[1].value)
        << " tex2=" << static_cast<unsigned long long>(hot.textures[2].value)
        << " tex3=" << static_cast<unsigned long long>(hot.textures[3].value)
        << " tex4=" << static_cast<unsigned long long>(hot.textures[4].value)
        << " tex5=" << static_cast<unsigned long long>(hot.textures[5].value)
        << " textureMask=0x" << std::hex << hot.textureMask << std::dec
        << " vsHash=" << static_cast<unsigned long long>(drawState.shaderContext().vertexShader.hash)
        << " psHash=" << static_cast<unsigned long long>(drawState.shaderContext().pixelShader.hash)
        << " ffLayout=" << (ffLayout ? 1 : 0)
        << " preT=" << (preTransformed ? 1 : 0)
        << " indexed=" << (indexedDraw ? 1 : 0)
        << " primType=" << static_cast<unsigned>(pv.primitiveType)
        << " primCount=" << pv.primitiveCount
        << " vertexCount=" << static_cast<unsigned long long>(vertexCount)
        << " vertexStreamStride=" << drawVertexStreamStride
        << " vertexBufferOffset=" << vertexBufferOffset
        << " vertexStreamOffset=" << drawVertexStreamOffset
        << " vertexBaseIndex=" << drawVertexBaseIndex
        << " colorWrite="
        << core::flatStateOr(hot.renderStates, RS_COLOR_WRITE_ENABLE, 0xfu)
        << " zEnable=" << core::flatStateOr(hot.renderStates, RS_Z_ENABLE, 0u)
        << " zWrite=" << core::flatStateOr(hot.renderStates, RS_Z_WRITE_ENABLE, 0u)
        << " zFunc=" << core::flatStateOr(hot.renderStates, RS_Z_FUNC, 0u)
        << " cullState=" << cullState
        << " cullRequested=" << static_cast<unsigned>(requestedCullMode)
        << " cullEffective=" << static_cast<unsigned>(effectiveCullMode)
        << " scissor=" << (effectiveViewport.scissorEnabled ? 1 : 0)
        << " scissorRect=" << effectiveViewport.scissor.left << ","
        << effectiveViewport.scissor.top << "-" << effectiveViewport.scissor.right
        << "," << effectiveViewport.scissor.bottom
        << " alphaBlend="
        << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
        << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
        << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
        << " forceVisible=" << (debug::forceVisibleDraw() ? 1 : 0);
    emitQueueTraceLine(out.str());
  }
  if (indexedDraw) {
    const bool texture0R32FCube = [&] {
      if (!hot.textures[0]) {
        return false;
      }
      const auto* texture = ctx.pool.findTexture(hot.textures[0].value);
      return texture && texture->desc.format == core::Format::R32F &&
             texture->desc.type == core::TextureType::Cube;
    }();
    const bool autoExpandIndexed =
        shouldAutoExpandIndexedDraw(hot.renderStates,
                                    hot.textureMask,
                                    fixedFunctionPath,
                                    ffLayout.has_value(),
                                    texture0R32FCube);
    const bool probeForceExpandIndexedApplied =
        debug::probeForceExpandIndexed() &&
        indexedTriangleDraw &&
        forceExpandIndexedProbeRowMatches(encoderBreakdown) &&
        indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
        forceExpandIndexedProbeClassMatches(primitiveCount,
                                            hot.textureMask,
                                            hot.renderStates,
                                            effectiveViewport,
                                            fillMode);
    const bool forceExpandIndexed =
        debug::forceExpandIndexed() ||
        probeForceExpandIndexedApplied ||
        (autoExpandIndexed && !debug::disableAutoExpandIndexed());
    if (traceEncode) {
      std::ostringstream out;
      out << "[dxmt9-expand-policy] seq=" << static_cast<unsigned long long>(seqId)
          << " ordinal=" << static_cast<unsigned long long>(drawOrdinal)
          << " auto=" << (autoExpandIndexed ? 1 : 0)
          << " env=" << (debug::forceExpandIndexed() ? 1 : 0)
          << " probe=" << (probeForceExpandIndexedApplied ? 1 : 0)
          << " disabled=" << (debug::disableAutoExpandIndexed() ? 1 : 0)
          << " active=" << (forceExpandIndexed ? 1 : 0)
          << " ff=" << (fixedFunctionPath ? 1 : 0)
          << " ffLayout=" << (ffLayout ? 1 : 0)
          << " r32fCube=" << (texture0R32FCube ? 1 : 0)
          << " textureMask=0x" << std::hex << hot.textureMask << std::dec
          << " alphaBlend=" << core::flatStateOr(hot.renderStates, RS_ALPHABLEND_ENABLE, 0u)
          << " srcBlend=" << core::flatStateOr(hot.renderStates, RS_SRC_BLEND, 0u)
          << " dstBlend=" << core::flatStateOr(hot.renderStates, RS_DEST_BLEND, 0u)
          << " primCount=" << pv.primitiveCount
          << " vertexCount=" << static_cast<unsigned long long>(vertexCount);
      emitQueueTraceLine(out.str());
    }
    if (forceExpandIndexed) {
      PerfScope fvfDecodeExpandedScope(perf::countEncodeDrawFvfDecodeCpuTime);
      std::span<const u8> indexBytes;
      if (!pv.userIndexData.empty()) {
        indexBytes = pv.userIndexData;
      } else {
        auto* indexRecord = ctx.pool.findBuffer(hot.indexBuffer.value);
        if (const auto bytes = snapshotBufferBytes(indexSnapshot);
            !bytes.empty()) {
          indexBytes = bytes;
        } else if (indexRecord && !indexRecord->shadow.empty()) {
          indexBytes = indexRecord->shadow;
        } else if (indexRecord && indexRecord->buffer && indexRecord->contents) {
          indexBytes = std::span<const u8>(static_cast<const u8*>(indexRecord->contents),
                                           static_cast<std::size_t>(indexRecord->desc.size));
        }
      }
      const std::size_t stride = static_cast<std::size_t>(
          ffLayout ? (drawVertexStreamStride ? drawVertexStreamStride : ffLayout->stride)
                   : computeVertexDeclStride(vertexDecl));
      const std::size_t streamBase = static_cast<std::size_t>(hot.streamOffsets[0]);
      if (traceEncode) {
        std::ostringstream out;
        out << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
            << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
            << " ff=" << (ffLayout ? 1 : 0)
            << " vertexBytes=" << vertexBytes.size()
            << " indexBytes=" << indexBytes.size()
            << " stride=" << stride
            << " startIndex=" << pv.startIndex
            << " baseVertex=" << pv.baseVertexIndex;
        emitQueueTraceLine(out.str());
      }

      if (!vertexBytes.empty() && !indexBytes.empty() && stride != 0) {
        struct ExpandedExtraStream {
          u32 stream = 0;
          u32 metalSlot = 0;
          CommandQueue::TransientBufferSlice slice;
        };
        std::vector<ExpandedExtraStream> expandedExtraStreams;
        std::vector<u8> expandedVertices;
        auto resolveStreamBytes = [&](u32 stream) -> std::span<const u8> {
          if (const auto bytes =
                  snapshotBufferBytes(streamBindingSnapshot(bindingSnapshot, stream));
              !bytes.empty()) {
            return bytes;
          }
          if (hot.streamBuffers[stream]) {
            if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
                buffer) {
              if (!buffer->shadow.empty()) {
                return buffer->shadow;
              }
              if (buffer->contents) {
                return std::span<const u8>(static_cast<const u8*>(buffer->contents),
                                           static_cast<std::size_t>(buffer->desc.size));
              }
            }
          }
          if (vertexDecl.streams[stream].buffer) {
            return vertexDecl.streams[stream].buffer->bytes();
          }
          return {};
        };
        bool expansionComplete =
            expandIndexedStreamToFlatVertexBytes(vertexBytes,
                                                 indexBytes,
                                                 pv.indexType,
                                                 pv.startIndex,
                                                 pv.baseVertexIndex,
                                                 vertexCount,
                                                 streamBase,
                                                 stride,
                                                 expandedVertices);
        if (expansionComplete) {
          for (const auto& streamBinding : bindingPacket.extraStreams) {
            std::vector<u8> expandedStream;
            const auto sourceBytes = resolveStreamBytes(streamBinding.stream);
            if (!expandIndexedStreamToFlatVertexBytes(
                    sourceBytes,
                    indexBytes,
                    pv.indexType,
                    pv.startIndex,
                    pv.baseVertexIndex,
                    vertexCount,
                    static_cast<std::size_t>(hot.streamOffsets[streamBinding.stream]),
                    static_cast<std::size_t>(streamBinding.stride),
                    expandedStream)) {
              expansionComplete = false;
              break;
            }
            auto slice = makeTransientVertexBuffer(
                expandedStream.data(), expandedStream.size(),
                ActiveEncoderBreakdown::TransientVertexSource::ExpandedExtra);
            if (!slice) {
              expansionComplete = false;
              break;
            }
            expandedExtraStreams.push_back(ExpandedExtraStream{
                .stream = streamBinding.stream,
                .metalSlot = streamBinding.metalSlot,
                .slice = slice,
            });
          }
        }
        if (expansionComplete) {
          transientVertexBuffer = makeTransientVertexBuffer(
              expandedVertices.data(), expandedVertices.size(),
              ActiveEncoderBreakdown::TransientVertexSource::ExpandedMain);
        }
        if (transientVertexBuffer) {
          if (setVertexBufferCached(transientVertexBuffer.buffer,
                                    transientVertexBuffer.offset, 1)) {
            countVertexBufferBind();
            if (encoderBreakdown) {
              encoderBreakdown->recordStreamMetalBind(0);
            }
          }
          for (const auto& stream : expandedExtraStreams) {
            if (setVertexBufferCached(stream.slice.buffer, stream.slice.offset,
                                      stream.metalSlot)) {
              countVertexBufferBind();
              if (encoderBreakdown) {
                encoderBreakdown->recordStreamMetalBind(stream.stream);
              }
            }
          }
          if (ffLayout && ffLayout->preTransformed && vertexCount >= 6 && hot.textures[0]) {
            const bool traceExpanded = [] {
              const char* env = std::getenv("DXMT_TRACE_FVF_EXPANDED");
              return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
            }();
            if (traceExpanded) {
              auto readExpandedF32 = [&](std::size_t absoluteOffset) {
                float value = 0.0f;
                if (absoluteOffset + sizeof(float) <= expandedVertices.size()) {
                  std::memcpy(&value, expandedVertices.data() + absoluteOffset, sizeof(float));
                }
                return value;
              };
              std::ostringstream trace;
              trace << "[dxmt9-expanded] seq=" << static_cast<unsigned long long>(seqId)
                    << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
                    << " stride=" << stride;
              for (uint64_t i = 0; i < std::min<uint64_t>(vertexCount, 6); ++i) {
                const std::size_t base = static_cast<std::size_t>(i) * stride;
                trace << " v" << i << "=("
                      << readExpandedF32(base + ffLayout->positionOffset + 0) << ","
                      << readExpandedF32(base + ffLayout->positionOffset + 4) << ","
                      << readExpandedF32(base + ffLayout->positionOffset + 8) << ","
                      << readExpandedF32(base + ffLayout->positionOffset + 12) << ")";
                if (ffLayout->hasTexcoord[0]) {
                  trace << " uv" << i << "=("
                        << readExpandedF32(base + ffLayout->texcoordOffset[0] + 0) << ","
                        << readExpandedF32(base + ffLayout->texcoordOffset[0] + 4) << ")";
                }
              }
              emitQueueTraceLine(trace.str());
            }
          }
          vertexBytes = std::span<const u8>(expandedVertices.data(), expandedVertices.size());
          drawVertexStreamOffset = 0;
          drawVertexBaseIndex = 0;
          expandedIndexedDraw = true;
        }
      }

      if (traceEncode) {
        std::ostringstream resultTrace;
        resultTrace << "[dxmt9-expanded-check] seq=" << static_cast<unsigned long long>(seqId)
                    << " tex0=" << static_cast<unsigned long long>(hot.textures[0].value)
                    << " expanded=" << (expandedIndexedDraw ? 1 : 0);
        emitQueueTraceLine(resultTrace.str());
      }
    }
    const bool nativeBaseVertexRequested =
        indexedDraw && !expandedIndexedDraw && debug::useNativeMetalBaseVertex();
    const bool nativeBaseVertexUsed =
        nativeBaseVertexRequested && pv.baseVertexIndex >= 0;
    if (nativeBaseVertexUsed) {
      drawVertexBaseIndex = 0;
    }
    auto pushDrawVolatile = [&] {
      const DrawVolatile vol = buildDrawVolatile(drawVertexBaseIndex, drawVertexStreamOffset,
                                                  drawVertexStreamStride);
      recordedSetVertexBytes(ctx, encoder, &vol, sizeof(DrawVolatile), 5);
      perf::countUniformVolatilePush();
      if (encoderBreakdown) {
        encoderBreakdown->addSetVertexBytes(sizeof(DrawVolatile), 5);
      }
    };
    auto recordEncoderDrawIssue = [&](bool indexed, bool expanded) {
      if (!encoderBreakdown) {
        return;
      }
      encoderBreakdown->recordTileFfpCoverage(
          dxmt9::pipeline::classifyTileFfpForPass(
              drawState, ctx.pool.supportsApple3()),
          tileFfpMode,
          primitiveCount,
          vertexCount);
      encoderBreakdown->recordDrawIssue(
          pv.primitiveType,
          primitiveCount,
          vertexCount,
          indexed,
          expanded,
          fixedFunctionPath,
          preTransformed,
          hot.textureMask,
          drawVertexStreamStride,
          drawVertexBaseIndex,
          drawVertexStreamOffset,
          pv.baseVertexIndex,
          nativeBaseVertexRequested,
          nativeBaseVertexUsed,
          pv.startIndex,
          pv.indexType,
          hot.renderStates,
          effectiveViewport,
          effectiveCullMode,
          fillMode);
    };
    traceEffectDraw(encoderBreakdown,
                    hot,
                    ctx.pool,
                    seqId,
                    drawOrdinal,
                    commandIndex,
                    commandDrawIndex,
                    commandDrawCount,
                    pv.primitiveType,
                    primitiveCount,
                    vertexCount,
                    indexedDraw,
                    fixedFunctionPath,
                    preTransformed,
                    drawState.hasShaderContext()
                        ? drawState.shaderContext().vertexShader.hash
                        : 0u,
                    drawState.hasShaderContext()
                        ? drawState.shaderContext().pixelShader.hash
                        : 0u);
    if (expandedIndexedDraw) {
      if (encoderBreakdown && indexedDraw) {
        encoderBreakdown->recordIndexBufferState(hot.indexBuffer.value);
      }
      recordEncoderDrawIssue(true, true);
      recordDrawGeometryDiagnostics(drawState,
                                    pv,
                                    seqId,
                                    vertexCount,
                                    transientVertexBuffer.offset,
                                    drawVertexStreamOffset,
                                    drawVertexStreamStride,
                                    true,
                                    false,
                                    !pv.userVertexData.empty() || !pv.userIndexData.empty(),
                                    true,
                                    fixedFunctionPath);
      countDrawIssue(drawState,
                     pv.primitiveType,
                     primitiveCount,
                     vertexCount,
                     true,
                     true,
                     pv.userVertexData.size(),
                     pv.userIndexData.size());
      pushDrawVolatile();
      {
        const bool issueSplit = drawIssueSplitPerfEnabled();
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        PerfScope issuePathScope(
            issueSplit ? perf::countEncodeDrawIssueExpandedIndexedCpuTime
                       : nullptr);
        std::optional<std::uint32_t> visibilityResult;
        if (visibilityScout) {
          PerfScope visibilityScope(
              issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                         : nullptr);
          visibilityResult = beginVisibilityScoutDraw(
              visibilityScout, encoder,
              makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                            effectiveViewport, primitiveType,
                                            pv, drawOrdinal, commandIndex,
                                            primitiveCount, vertexCount,
                                            /*indexed=*/true,
                                            /*expandedIndexed=*/true,
                                            /*splitChunk=*/0,
                                            effectiveCullMode, fillMode));
        }
        {
          PerfScope metalScope(
              issueSplit ? perf::countEncodeDrawIssueMetalCpuTime : nullptr);
          recordedDrawPrimitives(ctx, encoder, primitiveType, 0, (uint64_t)vertexCount);
        }
        if (visibilityScout) {
          PerfScope visibilityScope(
              issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                         : nullptr);
          endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
        }
      }
      // R-BACK-13.1: run the tile-FFP imageblock post-pass after this draw.
      emitTileFfpPostPass();
      return true;
    }
    CommandQueue::TransientBufferSlice transientIndexBuffer;
    WMT::Buffer indexBuffer{};
    const resources::BufferRecord* indexBufferRecord = nullptr;
    std::span<const u8> indexBytesForReuse;
    u32 indexReuseStartIndex = pv.startIndex;
    std::vector<u8> probeReorderedIndexBytes;
    uint64_t indexBufferOffset = static_cast<uint64_t>(pv.startIndex) * indexElementSize(pv.indexType);
    u32 splitPrimitiveLimit = 0u;
    u64 splitStream0SpanLimit = 0u;
    u32 splitMaxChunksPerDraw = 0u;
    std::vector<IndexedDrawChunk> splitChunks;
    u64 splitChunkStream0SpanMax = 0u;
    bool splitWouldApply = false;
    bool indexReorderApplied = false;
    const bool encoderBreakdownActive =
        encoderBreakdown && encoderBreakdown->enabled;
    const bool streamIbStagingEnabled =
        streamIbStagingActive(streamIbStagingCache);
    const bool indexedDiagnosticsEnabled =
        debug::indexedTriangleDiagnosticsEnabled() ||
        (effectDrawTraceEnabled() && effectDrawTraceGeometryEnabled());
    const bool needIndexBytesForDiagnostics =
        encoderBreakdownActive || indexedDiagnosticsEnabled;
    {
      PerfScope streamBindIndexScope(perf::countEncodeDrawStreamBindCpuTime);
      PerfScope streamBindIndexPhaseScope(
          streamBindPhaseSplitPerf
              ? perf::countEncodeDrawStreamBindIndexPhaseCpuTime
              : nullptr);
      PerfScope indexSetupScope(perf::countEncodeDrawIndexSetupCpuTime);
      perf::countEncodeDrawStreamBindIndexPhaseCalls(1u);
      {
        PerfScope indexSourceResolveScope(
            perf::countEncodeDrawIndexSourceResolveCpuTime);
        if (!pv.userIndexData.empty()) {
          indexBytesForReuse = pv.userIndexData;
          // Phase 5-B: prefer pre-batched UP index slice from DrawRun
          // bulk upload; fall back to per-draw upload otherwise.
          if (preUploaded && preUploaded->index) {
            transientIndexBuffer = preUploaded->index;
          } else {
            transientIndexBuffer = makeTransientIndexBuffer(
                pv.userIndexData.data(), pv.userIndexData.size(),
                ActiveEncoderBreakdown::TransientIndexSource::User);
          }
          if (transientIndexBuffer) {
            indexBuffer = transientIndexBuffer.buffer;
            indexBufferOffset += transientIndexBuffer.offset;
          }
        } else {
          auto* buffer = ctx.pool.findBuffer(hot.indexBuffer.value);
          indexBufferRecord = buffer;
          if (buffer && (buffer->buffer || (indexSnapshot && indexSnapshot->valid()))) {
            indexBuffer = WMT::Buffer{indexSnapshot
                ? indexSnapshot->metalHandle
                : buffer->buffer.handle};
            if (needIndexBytesForDiagnostics) {
              if (const auto bytes = snapshotBufferBytes(indexSnapshot);
                  !bytes.empty()) {
                indexBytesForReuse = bytes;
              } else if (!buffer->shadow.empty()) {
                indexBytesForReuse = buffer->shadow;
              } else if (buffer->contents) {
                indexBytesForReuse = std::span<const u8>(
                    static_cast<const u8*>(buffer->contents),
                    static_cast<std::size_t>(buffer->desc.size));
              }
            }
          } else if (buffer && !buffer->shadow.empty()) {
            indexBytesForReuse = buffer->shadow;
            transientIndexBuffer = makeTransientIndexBuffer(
                buffer->shadow.data(), buffer->shadow.size(),
                ActiveEncoderBreakdown::TransientIndexSource::ShadowFallback);
            if (transientIndexBuffer) {
              indexBuffer = transientIndexBuffer.buffer;
              indexBufferOffset += transientIndexBuffer.offset;
            }
          }
        }
      }
      const bool defaultIndexedFastPath =
          !needIndexBytesForDiagnostics && !streamIbStagingEnabled;
      if (defaultIndexedFastPath) {
        if (indexBuffer) {
          countIndexBufferBind();
        }
      } else {
      const std::span<const u8> originalIndexBytesForReuse = indexBytesForReuse;
      const u32 originalIndexReuseStartIndex = indexReuseStartIndex;
      const char* effectiveIndexSource = "original";
      u64 effectiveIndexOffset = indexBufferOffset;
      u64 effectiveIndexBytes = indexBytesForReuse.size();
      u64 effectiveIndexBufferHandle = hot.indexBuffer.value;
      const u64 reverseStream0SpanMin =
          debug::probeReverseIndexedTrianglesStream0SpanMin();
      const u64 optimizeStream0SpanMin =
          debug::optimizeScreenBlendIndexOrderStream0SpanMin();
      splitPrimitiveLimit = debug::splitLargeIndexedDrawPrimitiveLimit();
      splitStream0SpanLimit = debug::splitLargeIndexedDrawStream0SpanMax();
      splitMaxChunksPerDraw = debug::splitLargeIndexedDrawMaxChunksPerDraw();
      const bool triangleList =
          pv.primitiveType == core::PrimitiveType::TriangleList;
      // When a seq filter is active, keep expensive indexed diagnostics inside
      // the selected frame. Otherwise measurement can slow 3DMark05 enough to
      // change which semantic workload a frame/encoder row represents.
      const bool indexedDiagnosticSeqScopeActive =
          !perf::encoderBreakdownSeqFilterActive() || encoderBreakdownActive;
      const bool reverseAllIndexedTriangles = debug::probeReverseIndexedTriangles();
      const bool reverseOpaqueIndexedTriangles =
          debug::probeReverseOpaqueIndexedTriangles();
      const bool reverseNonOpaqueIndexedTriangles =
          debug::probeReverseNonOpaqueIndexedTriangles();
      const bool sortIndexedTrianglesByMinIndex =
          debug::probeSortIndexedTrianglesByMinIndex();
      const bool optimizeIndexedTrianglesVertexCache =
          debug::probeOptimizeIndexedTrianglesVertexCache();
      const bool applyIndexCacheOptCandidateProbe =
          debug::probeApplyIndexCacheOptCandidate() &&
          triangleList;
      const bool optimizeOpaqueDepthIndexCache =
          debug::optimizeOpaqueDepthIndexCache() &&
          triangleList;
      const bool optimizeScreenBlendIndexCache =
          debug::optimizeScreenBlendIndexCache() &&
          triangleList;
      const bool reverseTriangleProbeRequested =
          (reverseAllIndexedTriangles || reverseOpaqueIndexedTriangles ||
           reverseNonOpaqueIndexedTriangles || sortIndexedTrianglesByMinIndex ||
           optimizeIndexedTrianglesVertexCache ||
           applyIndexCacheOptCandidateProbe) &&
          triangleList;
      const bool reverseTriangleProbeScopeMatches =
          reverseTriangleProbeRequested &&
          indexedDiagnosticSeqScopeActive &&
          reverseIndexedTriangleRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown);
      const bool optimizeScreenBlendIndexOrderRequested =
          debug::optimizeScreenBlendIndexOrder() &&
          triangleList;
      const bool optimizeScreenBlendIndexOrderScopeMatches =
          optimizeScreenBlendIndexOrderRequested &&
          indexedDiagnosticSeqScopeActive &&
          screenBlendIndexOrderRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown);
      const bool splitConsidered =
          (splitPrimitiveLimit != 0u || splitStream0SpanLimit != 0u) &&
          triangleList &&
          indexedDiagnosticSeqScopeActive &&
          splitLargeIndexedDrawRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown);
      const bool dumpIndexedGeometryRequested =
          !debug::indexedGeometryDumpDir().empty() &&
          debug::indexedGeometryDumpMaxDraws() != 0u &&
          triangleList;
      const bool dumpIndexedGeometryScopeMatches =
          dumpIndexedGeometryRequested &&
          indexedDiagnosticSeqScopeActive &&
          reverseIndexedTriangleRowMatches(encoderBreakdown) &&
          indexedTriangleEncoderDrawRangeMatches(encoderBreakdown) &&
          indexedGeometryDumpShaderMatches(drawState) &&
          indexedGeometryDumpTextureMatches(drawState, ctx.pool);
      const bool opaqueDepthWritingEligible =
          shouldOptimizeOpaqueDepthIndexOrder(
              hot.renderStates,
              fillMode,
              debug::probeDisableDepthWrite());
      const bool applyProbeCacheOptCandidateSafetyEligible =
          opaqueDepthWritingEligible ||
          debug::probeApplyIndexCacheOptCandidateUnsafeNonOpaque();
      const bool optimizeOpaqueDepthIndexCacheScopeMatches =
          optimizeOpaqueDepthIndexCache &&
          opaqueDepthWritingEligible;
      const bool optimizeScreenBlendIndexCacheScopeMatches =
          optimizeScreenBlendIndexCache &&
          shouldOptimizeScreenBlendIndexOrder(hot.renderStates);
      const bool applyProbeCacheOptCandidateScopeMatches =
          applyIndexCacheOptCandidateProbe &&
          reverseTriangleProbeScopeMatches &&
          applyProbeCacheOptCandidateSafetyEligible;
      const bool stableOriginalIndexBufferForCandidate =
          pv.userIndexData.empty() && indexBufferRecord &&
          indexBufferRecord->buffer &&
          !indexSnapshot;
      const bool reverseTriangleClassEligibleNoSpan =
          indexedTriangleClassMatches(
              debug::probeReverseIndexedTrianglesClassFilter(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode) &&
          indexedTriangleClassMatches(
              debug::probeReverseIndexedTrianglesClassFilters(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode);
      const bool diagnosticCacheOptCandidatePreEligible =
          applyProbeCacheOptCandidateScopeMatches &&
          stableOriginalIndexBufferForCandidate &&
          reverseTriangleClassEligibleNoSpan;
      const bool productionCacheOptCandidatePreEligible =
          (optimizeOpaqueDepthIndexCacheScopeMatches ||
           optimizeScreenBlendIndexCacheScopeMatches) &&
          stableOriginalIndexBufferForCandidate;
      const bool applyCacheOptCandidatePreEligible =
          diagnosticCacheOptCandidatePreEligible ||
          productionCacheOptCandidatePreEligible;
      resources::ReorderedIndexBufferCacheKey cacheOptReorderKey{};
      if (stableOriginalIndexBufferForCandidate) {
        cacheOptReorderKey.startIndex = originalIndexReuseStartIndex;
        cacheOptReorderKey.indexCount = vertexCount;
        cacheOptReorderKey.indexType = pv.indexType;
        cacheOptReorderKey.order = resources::ReorderedIndexOrder::VertexCacheLru32;
        cacheOptReorderKey.cacheSize = 32u;
      }
      resources::ReorderedIndexBufferLookup cacheOptPrelookup{};
      const bool cacheOptPrelookupEligible =
          (optimizeOpaqueDepthIndexCacheScopeMatches ||
           optimizeScreenBlendIndexCacheScopeMatches) &&
          stableOriginalIndexBufferForCandidate;
      if (cacheOptPrelookupEligible) {
        PerfScope indexCacheLookupScope(
            perf::countEncodeDrawIndexCacheLookupCpuTime);
        cacheOptPrelookup = ctx.queue.findReorderedIndexBuffer(
            hot.indexBuffer,
            cacheOptReorderKey,
            seqId);
      }
      const bool cacheOptPrelookupPositive =
          cacheOptPrelookup.hit && cacheOptPrelookup.buffer;
      const bool cacheOptPrelookupRejected =
          cacheOptPrelookup.hit && cacheOptPrelookup.rejected;
      if (cacheOptPrelookupEligible && cacheOptPrelookup.hit) {
        perf::countReorderedIndexCacheLookup(
            cacheOptPrelookupPositive,
            cacheOptPrelookupRejected,
            false,
            0u);
      }
      if (cacheOptPrelookupEligible && encoderBreakdown && cacheOptPrelookup.hit) {
        encoderBreakdown->recordReorderedIndexCacheLookup(
            cacheOptPrelookupPositive,
            cacheOptPrelookupRejected,
            false,
            0u);
      }
      const bool explicitMeasureCacheOptCandidate =
          debug::measureIndexCacheOptCandidate() && encoderBreakdownActive;
      const bool measureProductionCacheOptPrelookup =
          cacheOptPrelookupPositive &&
          encoderBreakdownActive &&
          perf::encoderBreakdownSeqFilterActive();
      const bool measureCacheOptCandidate =
          triangleList &&
          (explicitMeasureCacheOptCandidate ||
           measureProductionCacheOptPrelookup ||
           (applyCacheOptCandidatePreEligible &&
            !cacheOptPrelookupPositive &&
            !cacheOptPrelookupRejected));
      const bool cacheOptFullReuseMeasureRequired =
          explicitMeasureCacheOptCandidate ||
          measureProductionCacheOptPrelookup ||
          encoderBreakdownActive ||
          debug::measureIndexReuse() ||
          reverseTriangleProbeScopeMatches ||
          optimizeScreenBlendIndexOrderScopeMatches ||
          splitConsidered ||
          dumpIndexedGeometryScopeMatches;
      const bool cacheOptFastLru32Measure =
          measureCacheOptCandidate && !cacheOptFullReuseMeasureRequired;
      const bool measureProbeIndexLocality =
          (debug::measureIndexReuse() && encoderBreakdownActive) ||
          (reverseTriangleProbeScopeMatches && reverseStream0SpanMin != 0u) ||
          (optimizeScreenBlendIndexOrderScopeMatches && optimizeStream0SpanMin != 0u) ||
          (splitConsidered && splitStream0SpanLimit != 0u) ||
          dumpIndexedGeometryScopeMatches ||
          measureCacheOptCandidate;
      const u64 stream0StrideForProbe =
          encoderBreakdownActive ? encoderBreakdown->stats.streams[0].lastStride
                                 : static_cast<u64>(hot.streamStrides[0]);
      u32 cacheOptMinGainPct =
          debug::probeApplyIndexCacheOptCandidateMinGainPct();
      if (optimizeOpaqueDepthIndexCacheScopeMatches) {
        cacheOptMinGainPct =
            debug::optimizeOpaqueDepthIndexCacheMinGainPct();
      } else if (optimizeScreenBlendIndexCacheScopeMatches) {
        cacheOptMinGainPct =
            debug::optimizeScreenBlendIndexCacheMinGainPct();
      }
      IndexReuseMeasure originalIndexReuseForProbe{.references = vertexCount};
      if (measureProbeIndexLocality) {
        if (measureCacheOptCandidate) {
          PerfScope indexCacheCandidateScope(
              perf::countEncodeDrawIndexCacheCandidateCpuTime);
          PerfScope indexCacheOriginalMeasureScope(
              perf::countEncodeDrawIndexCacheOriginalMeasureCpuTime);
          if (cacheOptFastLru32Measure &&
              debug::indexCacheCandidateUpperBoundGate()) {
            originalIndexReuseForProbe =
                measureIndexCacheMiss32AndUniqueForDraw(
                    originalIndexBytesForReuse,
                    pv.indexType,
                    originalIndexReuseStartIndex,
                    vertexCount);
          } else {
            originalIndexReuseForProbe = cacheOptFastLru32Measure
                ? measureIndexCacheMiss32ForDraw(originalIndexBytesForReuse,
                                                 pv.indexType,
                                                 originalIndexReuseStartIndex,
                                                 vertexCount)
                : measureIndexReuseForDraw(originalIndexBytesForReuse,
                                           pv.indexType,
                                           originalIndexReuseStartIndex,
                                           vertexCount);
          }
        } else {
          originalIndexReuseForProbe =
              measureIndexReuseForDraw(originalIndexBytesForReuse,
                                       pv.indexType,
                                       originalIndexReuseStartIndex,
                                       vertexCount);
        }
      }
      traceEffectIndexedGeometry(
          encoderBreakdown,
          drawState,
          ctx.pool,
          originalIndexBytesForReuse,
          vertexBytes,
          pv.indexType,
          originalIndexReuseStartIndex,
          vertexCount,
          pv.baseVertexIndex,
          hot.streamOffsets[0],
          drawVertexStreamStride,
          hot.streamBuffers[0].value,
          hot.indexBuffer.value,
          seqId,
          drawOrdinal,
          commandIndex,
          commandDrawIndex,
          commandDrawCount,
          pv.primitiveType,
          primitiveCount,
          fixedFunctionPath,
          preTransformed,
          drawState.hasShaderContext()
              ? drawState.shaderContext().vertexShader.hash
              : 0u,
          drawState.hasShaderContext()
              ? drawState.shaderContext().pixelShader.hash
              : 0u);
      std::vector<u8> cacheOptCandidateIndexBytes;
      IndexReuseMeasure cacheOptCandidateReuse{.references = vertexCount};
      bool cacheOptCandidateBuilt = false;
      bool cacheOptCandidateGatePassed = false;
      if (measureCacheOptCandidate) {
        PerfScope indexCacheCandidateScope(
            perf::countEncodeDrawIndexCacheCandidateCpuTime);
        if (originalIndexReuseForProbe.available) {
          const bool upperBoundRejected =
              debug::indexCacheCandidateUpperBoundGate() &&
              !indexCacheCandidateCanMeetGainUpperBound(
                  originalIndexReuseForProbe,
                  cacheOptMinGainPct);
          if (upperBoundRejected) {
            perf::countEncodeDrawIndexCacheCandidateUpperBoundRejected(1u);
          } else {
            PerfScope indexCacheCandidateBuildScope(
                perf::countEncodeDrawIndexCacheCandidateBuildCpuTime);
            cacheOptCandidateBuilt =
                buildVertexCacheOptimizedTriangleOrderIndexBytes(
                    originalIndexBytesForReuse,
                    pv.indexType,
                    originalIndexReuseStartIndex,
                    vertexCount,
                    cacheOptCandidateIndexBytes,
                    32u,
                    debug::indexCacheCandidateFrontierCap(),
                    debug::indexCacheCandidateLazyFrontier(),
                    debug::indexCacheCandidateBucketedSelect(),
                    debug::indexCacheCandidateStrictLru());
          }
        }
        if (cacheOptCandidateBuilt) {
          PerfScope indexCacheCandidateMeasureScope(
              perf::countEncodeDrawIndexCacheCandidateMeasureCpuTime);
          cacheOptCandidateReuse = cacheOptFastLru32Measure
              ? measureIndexCacheMiss32ForDraw(cacheOptCandidateIndexBytes,
                                               pv.indexType,
                                               0,
                                               vertexCount)
              : measureIndexReuseForDraw(cacheOptCandidateIndexBytes,
                                         pv.indexType,
                                         0,
                                         vertexCount);
        }
        {
          PerfScope indexCacheGateScope(
              perf::countEncodeDrawIndexCacheGateCpuTime);
          cacheOptCandidateGatePassed =
              indexCacheCandidateMeetsGainGate(
                  originalIndexReuseForProbe,
                  cacheOptCandidateReuse,
                  cacheOptMinGainPct);
        }
        const bool cacheOptCandidateAvailable =
            originalIndexReuseForProbe.available && cacheOptCandidateReuse.available;
        perf::countIndexedCacheOptCandidate(
            cacheOptCandidateAvailable,
            static_cast<u64>(cacheOptCandidateIndexBytes.size()),
            originalIndexReuseForProbe.cacheMiss16,
            originalIndexReuseForProbe.cacheMiss32,
            originalIndexReuseForProbe.cacheMiss64,
            cacheOptCandidateReuse.cacheMiss16,
            cacheOptCandidateReuse.cacheMiss32,
            cacheOptCandidateReuse.cacheMiss64);
        if (cacheOptCandidateAvailable) {
          perf::countIndexedCacheOptCandidateGate(
              cacheOptCandidateGatePassed,
              primitiveCount,
              optimizeOpaqueDepthIndexCacheScopeMatches,
              optimizeScreenBlendIndexCacheScopeMatches);
        }
        if (encoderBreakdownActive) {
          encoderBreakdown->recordIndexedCacheOptCandidate(
              originalIndexReuseForProbe,
              cacheOptCandidateReuse,
              static_cast<u64>(cacheOptCandidateIndexBytes.size()));
          if (cacheOptCandidateAvailable) {
            encoderBreakdown->recordIndexedCacheOptCandidateGate(
                cacheOptCandidateGatePassed,
                primitiveCount,
                optimizeOpaqueDepthIndexCacheScopeMatches,
                optimizeScreenBlendIndexCacheScopeMatches);
          }
        }
        if ((optimizeOpaqueDepthIndexCacheScopeMatches ||
             optimizeScreenBlendIndexCacheScopeMatches) &&
            applyCacheOptCandidatePreEligible &&
            !explicitMeasureCacheOptCandidate &&
            !cacheOptCandidateGatePassed) {
          ctx.queue.rememberRejectedReorderedIndexBuffer(
              hot.indexBuffer,
              cacheOptReorderKey,
              seqId);
          perf::countReorderedIndexCacheLookup(false, false, false, 0u);
          if (encoderBreakdown) {
            encoderBreakdown->recordReorderedIndexCacheLookup(false, false, false, 0u);
          }
        }
      }
      auto stream0SpanFilterMatches = [&](u64 minSpan) {
        if (minSpan == 0u) {
          return true;
        }
        return stream0ByteSpanForIndexMeasure(originalIndexReuseForProbe,
                                              stream0StrideForProbe) >= minSpan;
      };
      bool probeConsidered = false;
      bool probeEligible = false;
      bool probeApplied = false;
      bool optimizedConsidered = false;
      bool optimizedEligible = false;
      bool optimizedApplied = false;
      const bool splitEligible =
          splitConsidered &&
          indexedTriangleClassMatches(
              debug::splitLargeIndexedDrawClassFilter(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode) &&
          indexedTriangleClassMatches(
              debug::splitLargeIndexedDrawClassFilters(),
              primitiveCount,
              hot.textureMask,
              hot.renderStates,
              effectiveViewport,
              fillMode);
      if (splitEligible &&
          buildIndexedDrawChunks(originalIndexBytesForReuse,
                                 pv.indexType,
                                 originalIndexReuseStartIndex,
                                 primitiveCount,
                                 splitPrimitiveLimit,
                                 stream0StrideForProbe,
                                 splitStream0SpanLimit,
                                 splitChunks)) {
        for (const auto& chunk : splitChunks) {
          splitChunkStream0SpanMax =
              std::max(splitChunkStream0SpanMax, chunk.stream0Span);
        }
        splitWouldApply =
            splitMaxChunksPerDraw == 0u ||
            splitChunks.size() <= static_cast<std::size_t>(splitMaxChunksPerDraw);
      }
      const bool triangleOrderMutationScopeMatches =
          reverseTriangleProbeScopeMatches ||
          optimizeOpaqueDepthIndexCacheScopeMatches ||
          optimizeScreenBlendIndexCacheScopeMatches;
      if (triangleOrderMutationScopeMatches) {
        probeConsidered = true;
        const bool classEligible =
            reverseTriangleClassEligibleNoSpan &&
            stream0SpanFilterMatches(reverseStream0SpanMin);
        const bool cacheOptCandidateClassEligible =
            productionCacheOptCandidatePreEligible ||
            (diagnosticCacheOptCandidatePreEligible && classEligible);
        const bool applyCacheOptCandidateEligible =
            cacheOptCandidateClassEligible &&
            (cacheOptPrelookupPositive ||
             (cacheOptCandidateBuilt &&
              cacheOptCandidateGatePassed));
        probeEligible =
            classEligible &&
            (applyCacheOptCandidateEligible ||
             (reverseTriangleProbeScopeMatches &&
              (optimizeIndexedTrianglesVertexCache ||
               sortIndexedTrianglesByMinIndex || reverseAllIndexedTriangles ||
               (reverseOpaqueIndexedTriangles && opaqueDepthWritingEligible) ||
               (reverseNonOpaqueIndexedTriangles && !opaqueDepthWritingEligible))));
        bool probeIndexBytesBuilt = false;
        if (probeEligible) {
          if (applyCacheOptCandidateEligible) {
            if (cacheOptPrelookupPositive) {
              indexBuffer = cacheOptPrelookup.buffer;
              indexBufferOffset = 0;
              effectiveIndexSource = "cached-reordered-prelookup";
              effectiveIndexOffset = 0;
              effectiveIndexBufferHandle = indexBuffer.handle;
              effectiveIndexBytes = cacheOptPrelookup.byteCount;
              probeApplied = true;
            } else {
              probeReorderedIndexBytes = cacheOptCandidateIndexBytes;
              probeIndexBytesBuilt = !probeReorderedIndexBytes.empty();
            }
          } else if (optimizeIndexedTrianglesVertexCache) {
            probeIndexBytesBuilt =
                buildVertexCacheOptimizedTriangleOrderIndexBytes(
                    indexBytesForReuse,
                    pv.indexType,
                    pv.startIndex,
                    vertexCount,
                    probeReorderedIndexBytes);
          } else if (sortIndexedTrianglesByMinIndex) {
            probeIndexBytesBuilt =
                buildMinIndexSortedTriangleOrderIndexBytes(
                    indexBytesForReuse,
                    pv.indexType,
                    pv.startIndex,
                    vertexCount,
                    probeReorderedIndexBytes);
          } else {
            probeIndexBytesBuilt =
                buildReverseTriangleOrderIndexBytes(indexBytesForReuse,
                                                    pv.indexType,
                                                    pv.startIndex,
                                                    vertexCount,
                                                    probeReorderedIndexBytes);
          }
        }
        if (probeIndexBytesBuilt) {
          if (applyCacheOptCandidateEligible) {
            PerfScope indexCacheApplyScope(
                perf::countEncodeDrawIndexCacheApplyCpuTime);
            const auto cached = ctx.queue.getOrCreateReorderedIndexBuffer(
                hot.indexBuffer,
                cacheOptReorderKey,
                std::span<const u8>(probeReorderedIndexBytes.data(),
                                    probeReorderedIndexBytes.size()),
                seqId);
            perf::countReorderedIndexCacheLookup(
                cached.hit,
                false,
                cached.created,
                cached.created ? cached.byteCount : 0u);
            if (encoderBreakdown) {
              encoderBreakdown->recordReorderedIndexCacheLookup(
                  cached.hit,
                  false,
                  cached.created,
                  cached.created ? cached.byteCount : 0u);
            }
            if (cached.buffer) {
              indexBuffer = cached.buffer;
              indexBufferOffset = 0;
              indexBytesForReuse =
                  std::span<const u8>(probeReorderedIndexBytes.data(),
                                      probeReorderedIndexBytes.size());
              indexReuseStartIndex = 0;
              effectiveIndexSource =
                  cached.created ? "cached-reordered-created"
                                 : "cached-reordered-hit";
              effectiveIndexOffset = 0;
              effectiveIndexBufferHandle = indexBuffer.handle;
              effectiveIndexBytes = cached.byteCount;
              probeApplied = true;
            }
          } else {
            transientIndexBuffer = makeTransientIndexBuffer(
                probeReorderedIndexBytes.data(), probeReorderedIndexBytes.size(),
                ActiveEncoderBreakdown::TransientIndexSource::ProbeReorder);
            if (transientIndexBuffer) {
              indexBuffer = transientIndexBuffer.buffer;
              indexBufferOffset = transientIndexBuffer.offset;
              indexBytesForReuse =
                  std::span<const u8>(probeReorderedIndexBytes.data(),
                                      probeReorderedIndexBytes.size());
              indexReuseStartIndex = 0;
              effectiveIndexSource = "transient-reordered";
              effectiveIndexOffset = transientIndexBuffer.offset;
              effectiveIndexBufferHandle = indexBuffer.handle;
              effectiveIndexBytes = probeReorderedIndexBytes.size();
              probeApplied = true;
            }
          }
        }
      }
      if (!probeApplied && optimizeScreenBlendIndexOrderScopeMatches) {
        optimizedConsidered = true;
        optimizedEligible =
            shouldOptimizeScreenBlendIndexOrder(hot.renderStates) &&
            indexedTriangleClassMatches(
                debug::optimizeScreenBlendIndexOrderClassFilter(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            indexedTriangleClassMatches(
                debug::optimizeScreenBlendIndexOrderClassFilters(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            stream0SpanFilterMatches(optimizeStream0SpanMin);
        if (optimizedEligible &&
            buildReverseTriangleOrderIndexBytes(indexBytesForReuse,
                                                pv.indexType,
                                                pv.startIndex,
                                                vertexCount,
                                                probeReorderedIndexBytes)) {
          transientIndexBuffer = makeTransientIndexBuffer(
              probeReorderedIndexBytes.data(), probeReorderedIndexBytes.size(),
              ActiveEncoderBreakdown::TransientIndexSource::OptimizedOrder);
          if (transientIndexBuffer) {
            indexBuffer = transientIndexBuffer.buffer;
            indexBufferOffset = transientIndexBuffer.offset;
            indexBytesForReuse = std::span<const u8>(probeReorderedIndexBytes.data(),
                                                     probeReorderedIndexBytes.size());
            indexReuseStartIndex = 0;
            effectiveIndexSource = "transient-optimized-order";
            effectiveIndexOffset = transientIndexBuffer.offset;
            effectiveIndexBufferHandle = indexBuffer.handle;
            effectiveIndexBytes = probeReorderedIndexBytes.size();
            optimizedApplied = true;
          }
        }
      }
      if (streamIbStagingEnabled &&
          !probeApplied &&
          !optimizedApplied &&
          pv.userIndexData.empty() &&
          indexBufferRecord &&
          indexBufferRecord->buffer &&
          indexBuffer) {
        if (auto staged = streamIbStagingCache->findOrStage(
                ctx, seqId, hot.indexBuffer.value, indexBufferRecord,
                encoderBreakdown, /*indexBuffer=*/true)) {
          indexBuffer = staged.buffer;
          indexBufferOffset = staged.offset +
                              static_cast<uint64_t>(pv.startIndex) *
                                  indexElementSize(pv.indexType);
          effectiveIndexSource = "staged-original";
          effectiveIndexOffset = indexBufferOffset;
          effectiveIndexBufferHandle = indexBuffer.handle;
          effectiveIndexBytes = indexBytesForReuse.size();
        }
      }
      const bool emitMeasureOnlyIndexedDraw =
          debug::measureIndexReuse() &&
          encoderBreakdownActive &&
          triangleList;
      bool dumpIndexedGeometryEligible = false;
      if (dumpIndexedGeometryScopeMatches) {
        dumpIndexedGeometryEligible =
            indexedTriangleClassMatches(
                debug::probeReverseIndexedTrianglesClassFilter(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            indexedTriangleClassMatches(
                debug::probeReverseIndexedTrianglesClassFilters(),
                primitiveCount,
                hot.textureMask,
                hot.renderStates,
                effectiveViewport,
                fillMode) &&
            stream0SpanFilterMatches(reverseStream0SpanMin);
      }
      if (encoderBreakdownActive &&
          (probeConsidered || optimizedConsidered || scissorRectProbeConsidered ||
           splitConsidered || emitMeasureOnlyIndexedDraw ||
           dumpIndexedGeometryEligible)) {
        const auto& stream0 = encoderBreakdown->stats.streams[0];
        const u64 reorderedIndexByteCount =
            !probeReorderedIndexBytes.empty()
                ? static_cast<u64>(probeReorderedIndexBytes.size())
                : cacheOptPrelookupPositive ? cacheOptPrelookup.byteCount : 0u;
        const u64 reorderBytes =
            (probeApplied || optimizedApplied) ? reorderedIndexByteCount : 0u;
        const bool productionCacheOptOnly =
            (optimizeOpaqueDepthIndexCacheScopeMatches ||
             optimizeScreenBlendIndexCacheScopeMatches) &&
            !reverseTriangleProbeScopeMatches &&
            !explicitMeasureCacheOptCandidate &&
            !splitConsidered &&
            !emitMeasureOnlyIndexedDraw &&
            !dumpIndexedGeometryEligible &&
            !scissorRectProbeConsidered &&
            !optimizedConsidered;
        const bool emitIndexedProbeDrawLine =
            !productionCacheOptOnly || perf::encoderBreakdownSeqFilterActive();
        const auto originalIndexReuse =
            measureProbeIndexLocality ? originalIndexReuseForProbe
                                      : IndexReuseMeasure{.references = vertexCount};
        if (emitIndexedProbeDrawLine) {
          const bool useCacheOptCandidateReuseForEffective =
              probeApplied && cacheOptPrelookupPositive && cacheOptCandidateBuilt;
          const auto effectiveIndexReuse =
              useCacheOptCandidateReuseForEffective
                  ? cacheOptCandidateReuse
                  : measureProbeIndexLocality
                  ? measureIndexReuseForDraw(indexBytesForReuse,
                                             pv.indexType,
                                             indexReuseStartIndex,
                                             vertexCount)
                  : IndexReuseMeasure{.references = vertexCount};
          const auto streamExtraBindings =
              encoderBreakdown->streamExtraBindingsSummary();
          encoderBreakdown->emitIndexedOrderProbeDraw(
              probeEligible,
              probeApplied,
              optimizedEligible,
              optimizedApplied,
              scissorRectProbeEligible,
              scissorRectProbeApplied,
              disableAlphaBlendProbeApplied,
              disableDepthWriteProbeApplied,
              depthFuncAlwaysProbeApplied,
              fragmentlessDepthOnlyProbeApplied,
              splitEligible,
              splitWouldApply,
              static_cast<u32>(std::min<std::size_t>(
                  splitChunks.size(),
                  std::numeric_limits<u32>::max())),
              splitMaxChunksPerDraw,
              splitStream0SpanLimit,
              splitChunkStream0SpanMax,
              splitEligible ? static_cast<u64>(primitiveCount) : 0u,
              reorderBytes,
              originalIndexReuse,
              effectiveIndexReuse,
              cacheOptCandidateReuse,
              cacheOptCandidateBuilt,
              cacheOptCandidateGatePassed,
              drawOrdinal,
              commandIndex,
              pv.primitiveType,
              primitiveCount,
              vertexCount,
              hot.textureMask,
              hot.textures,
              hot.renderStates,
              effectiveViewport,
              effectiveCullMode,
              fillMode,
              pv.baseVertexIndex,
              pv.startIndex,
              pv.indexType,
              effectiveIndexBufferHandle,
              effectiveIndexSource,
              effectiveIndexOffset,
              effectiveIndexBytes,
              stream0.lastHandle,
              stream0.lastOffset,
              stream0.lastStride,
              streamExtraBindings.c_str(),
              hot.vertexConstantsHash,
              hot.pixelConstantsHash,
              drawState.hasUniformPayload() ? drawState.uniformPayload().hash : 0ull,
              hot.viewport.scissor);
        }
        if (probeConsidered) {
          encoderBreakdown->recordIndexedOrderProbe(
              probeApplied,
              probeApplied ? reorderedIndexByteCount : 0u);
        }
        if (optimizedConsidered) {
          encoderBreakdown->recordIndexedOrderOptimization(
              optimizedApplied,
              optimizedApplied ? static_cast<u64>(probeReorderedIndexBytes.size()) : 0u);
        }
        if (scissorRectProbeConsidered) {
          encoderBreakdown->recordScissorRectProbe(scissorRectProbeApplied,
                                                   hot.viewport.scissor,
                                                   effectiveViewport.scissor);
        }
        if (dumpIndexedGeometryEligible) {
          std::array<IndexedGeometryStreamPayload, core::kMaxStreams - 1u>
              dumpExtraStreams{};
          std::size_t dumpExtraStreamCount = 0u;
          auto resolveDumpStreamBytes = [&](u32 stream) -> std::span<const u8> {
            if (const auto bytes =
                    snapshotBufferBytes(streamBindingSnapshot(bindingSnapshot, stream));
                !bytes.empty()) {
              return bytes;
            }
            if (hot.streamBuffers[stream]) {
              if (auto* buffer = ctx.pool.findBuffer(hot.streamBuffers[stream].value);
                  buffer) {
                if (!buffer->shadow.empty()) {
                  return buffer->shadow;
                }
                if (buffer->contents) {
                  return std::span<const u8>(
                      static_cast<const u8*>(buffer->contents),
                      static_cast<std::size_t>(buffer->desc.size));
                }
              }
            }
            if (vertexDecl.streams[stream].buffer) {
              return vertexDecl.streams[stream].buffer->bytes();
            }
            return {};
          };
          for (const auto& streamBinding : bindingPacket.extraStreams) {
            if (dumpExtraStreamCount >= dumpExtraStreams.size()) {
              break;
            }
            const auto streamBytes = resolveDumpStreamBytes(streamBinding.stream);
            if (streamBytes.empty()) {
              continue;
            }
            dumpExtraStreams[dumpExtraStreamCount++] = IndexedGeometryStreamPayload{
                .stream = streamBinding.stream,
                .metalSlot = streamBinding.metalSlot,
                .handle = hot.streamBuffers[streamBinding.stream].value,
                .offset = streamBinding.offset,
                .stride = streamBinding.stride,
                .bytes = streamBytes,
            };
          }
          std::optional<VsConsts> dumpVsConsts;
          std::optional<PsConsts> dumpPsConsts;
          std::optional<FfpPsConsts> dumpFfpPsConsts;
          std::span<const u8> dumpVsConstsBytes;
          std::span<const u8> dumpPsConstsBytes;
          std::span<const u8> dumpFfpVsConstsBytes;
          std::span<const u8> dumpFfpPsConstsBytes;
          if (debug::indexedGeometryDumpCbufs()) {
            dumpVsConsts = buildVsConsts(drawState);
            dumpPsConsts = buildPsConsts(drawState);
            dumpFfpPsConsts = buildFfpPsConsts(drawState);
            const auto* dumpFfpVsConsts = ensureFfpVs();
            dumpVsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(&*dumpVsConsts),
                sizeof(VsConsts));
            dumpPsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(&*dumpPsConsts),
                sizeof(PsConsts));
            dumpFfpVsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(dumpFfpVsConsts),
                sizeof(FfpVsConsts));
            dumpFfpPsConstsBytes = std::span<const u8>(
                reinterpret_cast<const u8*>(&*dumpFfpPsConsts),
                sizeof(FfpPsConsts));
          }
          maybeDumpIndexedGeometryPayload(
              encoderBreakdown,
              drawState,
              ctx.pool,
              originalIndexBytesForReuse,
              vertexBytes,
              originalIndexReuse,
              pv.indexType,
              originalIndexReuseStartIndex,
              vertexCount,
              pv.baseVertexIndex,
              hot.streamOffsets[0],
              drawVertexStreamStride,
              hot.streamBuffers[0].value,
              hot.indexBuffer.value,
              drawState.hasShaderContext()
                  ? drawState.shaderContext().vertexShader.hash
                  : 0u,
              drawState.hasShaderContext()
                  ? drawState.shaderContext().pixelShader.hash
                  : 0u,
              drawOrdinal,
              primitiveCount,
              std::span<const IndexedGeometryStreamPayload>(
                  dumpExtraStreams.data(), dumpExtraStreamCount),
              dumpVsConstsBytes,
              dumpPsConstsBytes,
              dumpFfpVsConstsBytes,
              dumpFfpPsConstsBytes);
        }
      }
      indexReorderApplied = probeApplied || optimizedApplied;
      if (indexBuffer) {
        if (encoderBreakdown) {
          if (indexBufferRecord) {
            encoderBreakdown->recordIndexBufferResource(
                hot.indexBuffer.value, indexBufferRecord->desc);
          }
          encoderBreakdown->recordIndexBufferState(effectiveIndexBufferHandle);
        }
        countIndexBufferBind();
        if (encoderBreakdown) {
          encoderBreakdown->recordIndexBufferMetalBind();
        }
      }
      }
    }
    if (indexBuffer) {
      if (encoderBreakdown && encoderBreakdown->enabled &&
          debug::measureIndexReuse()) {
        encoderBreakdown->recordIndexedVertexReuse(
            measureIndexReuseForDraw(indexBytesForReuse,
                                     pv.indexType,
                                     indexReuseStartIndex,
                                     vertexCount));
      }
      const bool upDraw = !pv.userVertexData.empty() || !pv.userIndexData.empty();
      recordEncoderDrawIssue(true, false);
      recordDrawGeometryDiagnostics(drawState,
                                    pv,
                                    seqId,
                                    vertexCount,
                                    vertexBufferOffset,
                                    drawVertexStreamOffset,
                                    drawVertexStreamStride,
                                    true,
                                    !upDraw,
                                    upDraw,
                                    false,
                                    fixedFunctionPath);
      countDrawIssue(drawState,
                     pv.primitiveType,
                     primitiveCount,
                     vertexCount,
                     true,
                     false,
                     pv.userVertexData.size(),
                     pv.userIndexData.size());
      pushDrawVolatile();
      {
        const bool issueSplit = drawIssueSplitPerfEnabled();
        PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
        const i32 metalBaseVertex = nativeBaseVertexUsed ? pv.baseVertexIndex : 0;
        const auto metalIndexType = toIndexType(pv.indexType);
        const bool submitSplitIndexed =
            splitWouldApply && !indexReorderApplied;
        PerfScope issuePathScope(
            issueSplit
                ? (submitSplitIndexed
                       ? perf::countEncodeDrawIssueSplitIndexedCpuTime
                       : perf::countEncodeDrawIssueIndexedCpuTime)
                : nullptr);
        if (submitSplitIndexed) {
          const u64 indexSize = indexElementSize(pv.indexType);
          for (const auto& chunk : splitChunks) {
            const u32 primitivesEmitted = chunk.startPrimitive;
            const u32 chunkPrimitives = chunk.primitiveCount;
            if (chunkPrimitives == 0u) {
              continue;
            }
            const u64 chunkIndexOffset =
                indexBufferOffset + static_cast<u64>(primitivesEmitted) * 3u * indexSize;
            std::optional<std::uint32_t> visibilityResult;
            if (visibilityScout) {
              PerfScope visibilityScope(
                  issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                             : nullptr);
              visibilityResult = beginVisibilityScoutDraw(
                  visibilityScout, encoder,
                  makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                                effectiveViewport, primitiveType,
                                                pv, drawOrdinal, commandIndex,
                                                chunkPrimitives,
                                                static_cast<u64>(chunkPrimitives) * 3u,
                                                /*indexed=*/true,
                                                /*expandedIndexed=*/false,
                                                chunk.startPrimitive,
                                                effectiveCullMode, fillMode));
            }
            {
              PerfScope metalScope(
                  issueSplit ? perf::countEncodeDrawIssueMetalCpuTime
                             : nullptr);
              recordedDrawIndexedPrimitives(ctx, encoder, primitiveType,
                                            metalIndexType,
                                            static_cast<u64>(chunkPrimitives) * 3u,
                                            indexBuffer, chunkIndexOffset, 1,
                                            metalBaseVertex, 0);
            }
            if (visibilityScout) {
              PerfScope visibilityScope(
                  issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                             : nullptr);
              endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
            }
          }
          if (encoderBreakdown) {
            encoderBreakdown->recordSplitLargeIndexedDraw(
                primitiveCount,
                splitPrimitiveLimit,
                splitStream0SpanLimit,
                splitChunkStream0SpanMax,
                static_cast<u32>(splitChunks.size()));
          }
        } else {
          std::optional<std::uint32_t> visibilityResult;
          if (visibilityScout) {
            PerfScope visibilityScope(
                issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                           : nullptr);
            visibilityResult = beginVisibilityScoutDraw(
                visibilityScout, encoder,
                makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                              effectiveViewport, primitiveType,
                                              pv, drawOrdinal, commandIndex,
                                              primitiveCount, vertexCount,
                                              /*indexed=*/true,
                                              /*expandedIndexed=*/false,
                                              /*splitChunk=*/0,
                                              effectiveCullMode, fillMode));
          }
          {
            PerfScope metalScope(
                issueSplit ? perf::countEncodeDrawIssueMetalCpuTime : nullptr);
            recordedDrawIndexedPrimitives(ctx, encoder, primitiveType,
                                          metalIndexType,
                                          (uint64_t)vertexCount, indexBuffer,
                                          indexBufferOffset, 1, metalBaseVertex, 0);
          }
          if (visibilityScout) {
            PerfScope visibilityScope(
                issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime
                           : nullptr);
            endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
          }
        }
      }
      // R-BACK-13.1: run the tile-FFP imageblock post-pass after this draw.
      emitTileFfpPostPass();
      return true;
    }
  }
  const bool upDraw = !pv.userVertexData.empty();
  if (encoderBreakdown) {
    encoderBreakdown->recordTileFfpCoverage(
        dxmt9::pipeline::classifyTileFfpForPass(
            drawState, ctx.pool.supportsApple3()),
        tileFfpMode,
        primitiveCount,
        vertexCount);
    encoderBreakdown->recordDrawIssue(
        pv.primitiveType,
        primitiveCount,
        vertexCount,
        false,
        false,
        fixedFunctionPath,
        preTransformed,
        hot.textureMask,
        drawVertexStreamStride,
        drawVertexBaseIndex,
        drawVertexStreamOffset,
        pv.baseVertexIndex,
        false,
        false,
        pv.startIndex,
        pv.indexType,
        hot.renderStates,
        effectiveViewport,
        effectiveCullMode,
        fillMode);
  }
  recordDrawGeometryDiagnostics(drawState,
                                pv,
                                seqId,
                                vertexCount,
                                vertexBufferOffset,
                                drawVertexStreamOffset,
                                drawVertexStreamStride,
                                false,
                                !upDraw,
                                upDraw,
                                false,
                                fixedFunctionPath);
  countDrawIssue(drawState,
                 pv.primitiveType,
                 primitiveCount,
                 vertexCount,
                 false,
                 false,
                 pv.userVertexData.size(),
                 pv.userIndexData.size());
  {
    const DrawVolatile vol = buildDrawVolatile(drawVertexBaseIndex, drawVertexStreamOffset,
                                                drawVertexStreamStride);
    recordedSetVertexBytes(ctx, encoder, &vol, sizeof(DrawVolatile), 5);
    perf::countUniformVolatilePush();
    if (encoderBreakdown) {
      encoderBreakdown->addSetVertexBytes(sizeof(DrawVolatile), 5);
    }
    const bool issueSplit = drawIssueSplitPerfEnabled();
    PerfScope issueScope(perf::countEncodeDrawIssueCpuTime);
    PerfScope issuePathScope(
        issueSplit ? perf::countEncodeDrawIssueNonIndexedCpuTime : nullptr);
    std::optional<std::uint32_t> visibilityResult;
    if (visibilityScout) {
      PerfScope visibilityScope(
          issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime : nullptr);
      visibilityResult = beginVisibilityScoutDraw(
          visibilityScout, encoder,
          makeVisibilityScoutDrawRecord(*visibilityScout, drawState,
                                        effectiveViewport, primitiveType, pv,
                                        drawOrdinal, commandIndex,
                                        primitiveCount, vertexCount,
                                        /*indexed=*/false,
                                        /*expandedIndexed=*/false,
                                        /*splitChunk=*/0,
                                        effectiveCullMode, fillMode));
    }
    {
      PerfScope metalScope(
          issueSplit ? perf::countEncodeDrawIssueMetalCpuTime : nullptr);
      recordedDrawPrimitives(ctx, encoder, primitiveType, 0, (uint64_t)vertexCount);
    }
    if (visibilityScout) {
      PerfScope visibilityScope(
          issueSplit ? perf::countEncodeDrawIssueVisibilityCpuTime : nullptr);
      endVisibilityScoutDraw(visibilityScout, encoder, visibilityResult);
    }
  }
  // R-BACK-13.1: run the tile-FFP imageblock post-pass after this draw.
  emitTileFfpPostPass();
  return true;
}

bool encodeDraw(EncodeContext& ctx,
                WMT::CommandBuffer& commandBuffer,
                WMT::RenderCommandEncoder& encoder,
                core::FlatDrawStateView drawState,
                u64 seqId,
                bool skipBaseStateBind,
                const PreUploadedDrawData* preUploaded,
                const core::DrawParam* paramOverride,
                std::span<const u8> paramPayloadArena,
                uniform::DirtyState* dirty,
                bool tileFfpMode,
                bool argbufHybridMode,
                bool argbufResourceArray,
                bool argbufDirectCbufMode,
                bool reopenArgbufHybrid,
                TextureSamplerBindShadow* textureSamplerShadow,
                std::uint32_t commandIndex,
                const core::DrawBindingSnapshot* bindingSnapshot) {
  return encodeDraw(ctx, commandBuffer, encoder, drawState, seqId,
                    skipBaseStateBind, preUploaded, paramOverride,
                    paramPayloadArena, bindingSnapshot, dirty, tileFfpMode,
                    argbufHybridMode, argbufResourceArray,
                    argbufDirectCbufMode, reopenArgbufHybrid,
                    /*argbufVsPayloadSourceChanged=*/false,
                    /*argbufPsPayloadSourceChanged=*/false,
                    /*bindingOverridePrefetchedPsoCompatible=*/false,
                    core::PsoHandle{}, core::PsoHandle{}, core::DepthStencilHandle{},
                    textureSamplerShadow, commandIndex, 0u, 0u, nullptr, nullptr,
                    nullptr, nullptr);
}

std::optional<core::metalqueue::QueueSubmissionRecord> encodeChunk(
    EncodeContext& ctx,
    std::size_t slotIndex,
    const core::ChunkSlot& slot,
    EncodeChunkOptions options) {
  @autoreleasepool {
  PerfScope scope(perf::countEncodeChunkCpuTime);
  if (!ctx.device || !ctx.queue.valid()) {
    return std::nullopt;
  }
  const EncodeChunkReplayRange replayRange =
      encodeChunkReplayRange(slotIndex, slot, options);
  if (!replayRange.valid) {
    return std::nullopt;
  }

  const bool traceEncodeProgress = traceEncodeProgressForSeq(slot.seqId);
  auto traceEncodeStage = [&](const char* stage) {
    if (!traceEncodeProgress) {
      return;
    }
    std::ostringstream out;
    out << "[dxmt9-encode-progress]"
        << " stage=" << stage
        << " seq=" << static_cast<unsigned long long>(slot.seqId)
        << " slot=" << slotIndex
        << " commands=" << slot.commandCount()
        << " command_begin=" << replayRange.commandBegin
        << " command_end=" << replayRange.commandEnd
        << " draw_only=" << (slot.drawOnlyCommandStream() ? 1 : 0);
    emitQueueTraceLine(out.str());
  };
  auto traceEncodeCommand = [&](const char* phase,
                                std::size_t commandIndex,
                                core::MetalCommandKind kind,
                                const core::MetalCommandView& command) {
    if (!traceEncodeProgress) {
      return;
    }
    std::ostringstream out;
    out << "[dxmt9-encode-progress]"
        << " stage=command." << phase
        << " seq=" << static_cast<unsigned long long>(slot.seqId)
        << " slot=" << slotIndex
        << " command=" << commandIndex
        << " kind=" << metalCommandKindName(kind);
    if (kind == core::MetalCommandKind::DrawRun) {
      out << " draws=" << command.drawParams.size();
      if (command.drawRunRecord) {
        out << " first_param=" << command.drawRunRecord->firstParam
            << " param_count=" << command.drawRunRecord->paramCount
            << " state_index=" << command.drawRunRecord->stateIndex;
      }
    }
    emitQueueTraceLine(out.str());
  };

  traceEncodeStage("begin");

  // M3 — Metal frame capture: ask the controller whether this chunk is
  // the first chunk of the target frame. If so, start capture BEFORE
  // `newCommandBuffer()` so Apple's MTLCaptureManager records every CB
  // we create. Capture stays open across every chunk of the target
  // frame; `notePresentChunkForCapture` later returns the request when
  // the target frame's Present chunk is encoded, and that request is
  // attached to the record so the queue's commit closes the capture.
  std::optional<core::metalcapture::MetalCaptureRequest> earlyCaptureRequest =
      ctx.queue.metalCaptureForChunkBegin(slot.seqId);
  bool captureAlreadyStartedAtChunkBegin = false;
  if (earlyCaptureRequest.has_value()) {
    traceEncodeStage("before-start-capture");
    captureAlreadyStartedAtChunkBegin =
        core::metalcapture::startMetalCapture(WMT::Device{ctx.device.handle},
                                               *earlyCaptureRequest);
    traceEncodeStage(captureAlreadyStartedAtChunkBegin
                         ? "after-start-capture-ok"
                         : "after-start-capture-failed");
  }

  const bool injectedCommandBuffer = options.hasInjectedCommandBuffer();
  traceEncodeStage(injectedCommandBuffer ? "before-use-injected-command-buffer"
                                         : "before-new-command-buffer");
  auto commandBuffer = injectedCommandBuffer
      ? std::move(options.commandBuffer)
      : ctx.queue.newCommandBuffer();
  if (!commandBuffer) {
    traceEncodeStage(injectedCommandBuffer ? "injected-command-buffer-null"
                                           : "new-command-buffer-null");
    if (captureAlreadyStartedAtChunkBegin && earlyCaptureRequest.has_value()) {
      core::metalcapture::stopMetalCapture(*earlyCaptureRequest);
    }
    return std::nullopt;
  }
  traceEncodeStage(injectedCommandBuffer ? "after-use-injected-command-buffer"
                                         : "after-new-command-buffer");
  bool commandBufferHasWork = false;
  // One-shot session for the current chunk. It remains local and still closes
  // the render encoder at function exit; making the owner explicit is the
  // prerequisite for a later opt-in render-pass carry path.
  EncodeChunkSessionStorage localSession =
      makeEncodeChunkSessionStorage(ctx.dirty);
  EncodeChunkSessionStorage& session =
      options.session ? options.session->storage : localSession;
  if (options.session) {
    initializeEncodeChunkSessionStorage(session, ctx.dirty);
  }
  const bool deferSessionFinalization =
      options.deferSessionFinalization && options.session != nullptr;
  auto& activeRenderEncoder = session.activeRenderEncoder;
  auto& activeBlitEncoder = session.activeBlitEncoder;
  auto& postCommitCallbacks = session.postCommitCallbacks;
  auto& completionCallbacks = session.completionCallbacks;
  auto& metalCaptureRequest = session.metalCaptureRequest;
  auto& activeKey = session.activeKey;
  auto& activeWriteHazard = session.activeWriteHazard;
  auto& hasActiveRender = session.hasActiveRender;
  auto& activePassUsesTileFfp = session.activePassUsesTileFfp;
  auto& activePassUsesArgbufHybrid = session.activePassUsesArgbufHybrid;
  auto& activePassUsesArgbufResourceArray =
      session.activePassUsesArgbufResourceArray;
  auto& activePassUsesArgbufDirectCbuf = session.activePassUsesArgbufDirectCbuf;
  [[maybe_unused]] auto& activeArgbufStorage = session.activeArgbufStorage;
  [[maybe_unused]] auto& activeArgbufOffset = session.activeArgbufOffset;
  auto& activeDrawStateKey = session.activeDrawStateKey;
  auto& activeDrawStateUsesPrefetchedPsoLayout =
      session.activeDrawStateUsesPrefetchedPsoLayout;
  auto& pendingClear = session.pendingClear;
  auto& pendingClearCommandIndex = session.pendingClearCommandIndex;
  auto& activeColorHandles = session.activeColorHandles;
  auto& activeColorAttachmentDump = session.activeColorAttachmentDump;
  auto& activeDepthAttachmentDump = session.activeDepthAttachmentDump;
  auto& activeDrawTextureDumps = session.activeDrawTextureDumps;
  auto& activeRenderEncoderSeq = session.activeRenderEncoderSeq;
  auto& activeRenderEncoderIndex = session.activeRenderEncoderIndex;
  auto& uniformDirty = session.uniformDirty;
  auto& lastArgbufPayloadHash = session.lastArgbufPayloadHash;
  auto& lastArgbufPayloadDeltaKey = session.lastArgbufPayloadDeltaKey;
  auto& lastArgbufPayloadDeltaComponentKey =
      session.lastArgbufPayloadDeltaComponentKey;
  auto& lastArgbufPayloadDeltaPayload = session.lastArgbufPayloadDeltaPayload;
  auto& argbufCbufCache = session.argbufCbufCache;
  auto& activeStreamIbStaging = session.activeStreamIbStaging;
  auto& textureSamplerShadow = session.textureSamplerShadow;
  auto& activeEncoderBreakdown = session.activeEncoderBreakdown;
  auto& activeVisibilityScout = session.activeVisibilityScout;
  auto& renderEncoderIndex = session.renderEncoderIndex;
  auto& renderEncoderGpuSampleBuffer =
      session.renderEncoderGpuSampleBuffer;
  auto& renderEncoderGpuSamples = session.renderEncoderGpuSamples;
  auto& renderEncoderGpuSampleCursor =
      session.renderEncoderGpuSampleCursor;
  auto& requestedRenderEncoderGpuSamples =
      session.requestedRenderEncoderGpuSamples;

  if (options.session) {
    perf::countEncodeSessionCarrySourceEntry(
        static_cast<bool>(activeRenderEncoder),
        static_cast<bool>(activeBlitEncoder),
        pendingClear.has_value());
  }
  const bool sessionSourceEntryActiveRender =
      options.session && static_cast<bool>(activeRenderEncoder);
  bool sessionSourceFirstDrawDecisionRecorded = false;
  bool sessionSourceActiveRenderClosedBeforeFirstDraw = false;
  auto recordSessionSourceActiveRenderClosedBeforeFirstDraw =
      [&](perf::EncoderSplitReason reason) {
        if (!sessionSourceEntryActiveRender ||
            sessionSourceFirstDrawDecisionRecorded ||
            sessionSourceActiveRenderClosedBeforeFirstDraw) {
          return;
        }
        sessionSourceActiveRenderClosedBeforeFirstDraw = true;
        perf::countEncodeSessionCarryActiveEntryLostActiveBeforeFirstDraw(
            reason);
      };
  auto recordSessionSourceFirstDrawDecision =
      [&](RenderPassEntryDecision decision) {
        if (!options.session || sessionSourceFirstDrawDecisionRecorded) {
          return;
        }
        sessionSourceFirstDrawDecisionRecorded = true;
        switch (decision) {
        case RenderPassEntryDecision::ContinueActive:
          perf::countEncodeSessionCarryFirstDrawContinueActive();
          if (sessionSourceEntryActiveRender) {
            perf::countEncodeSessionCarryActiveEntryFirstDrawContinueActive();
          }
          break;
        case RenderPassEntryDecision::BeginPass:
          perf::countEncodeSessionCarryFirstDrawBeginPass();
          if (sessionSourceEntryActiveRender) {
            perf::countEncodeSessionCarryActiveEntryFirstDrawBeginPass();
          }
          break;
        case RenderPassEntryDecision::SplitRenderTargetChange:
          perf::countEncodeSessionCarryFirstDrawSplitRenderTargetChange();
          if (sessionSourceEntryActiveRender) {
            perf::countEncodeSessionCarryActiveEntryFirstDrawSplitRenderTargetChange();
          }
          break;
        case RenderPassEntryDecision::SplitHazard:
          perf::countEncodeSessionCarryFirstDrawSplitHazard();
          if (sessionSourceEntryActiveRender) {
            perf::countEncodeSessionCarryActiveEntryFirstDrawSplitHazard();
          }
          break;
        case RenderPassEntryDecision::SplitTileMidPassIneligible:
          perf::countEncodeSessionCarryFirstDrawSplitOther();
          if (sessionSourceEntryActiveRender) {
            perf::countEncodeSessionCarryActiveEntryFirstDrawSplitOther();
          }
          break;
        }
      };

  traceEncodeStage("before-gpu-sampling-setup");
  initializeEncodeChunkSessionGpuSamplingStorage(
      session, WMT::Device{ctx.device.handle},
      options.sessionLookaheadSources.empty()
          ? replayRange.commandCount()
          : sessionGpuSamplingCommandCount(slot,
                                           replayRange.commandCount(),
                                           options.sessionLookaheadSources));
  traceEncodeStage("after-gpu-sampling-setup");

  auto makeRenderEncoderGpuAttachment = [&](
      core::metalqueue::RenderEncoderGpuPassType passType,
      std::size_t commandIndex,
      std::uint64_t rtHandle,
      std::uint64_t depthHandle,
      std::uint64_t psoHandle = 0) {
    RenderEncoderGpuAttachment result{};
    if (!renderEncoderGpuSampleBuffer ||
        renderEncoderGpuSampleCursor + 1u >= requestedRenderEncoderGpuSamples) {
      return result;
    }
    const std::uint32_t startSample = renderEncoderGpuSampleCursor++;
    const std::uint32_t endSample = renderEncoderGpuSampleCursor++;
    result.attachments[0] = WMTSampleBufferAttachmentInfo{
        .sample_buffer = renderEncoderGpuSampleBuffer.handle,
        .start_of_encoder_sample_index = startSample,
        .end_of_encoder_sample_index = endSample,
    };
    result.sample =
        core::metalqueue::QueueSubmissionRecord::RenderEncoderGpuSample{
            .startIndex = startSample,
            .endIndex = endSample,
            .passType = passType,
            .seqId = slot.seqId,
            .slotIndex = slotIndex <= std::numeric_limits<std::uint32_t>::max()
                ? static_cast<std::uint32_t>(slotIndex)
                : std::numeric_limits<std::uint32_t>::max(),
            .commandIndex = commandIndex <= std::numeric_limits<std::uint32_t>::max()
                ? static_cast<std::uint32_t>(commandIndex)
                : std::numeric_limits<std::uint32_t>::max(),
            .rtHandle = rtHandle,
            .depthHandle = depthHandle,
            .psoHandle = psoHandle,
        };
    result.active = true;
    return result;
  };
  auto recordRenderEncoderGpuAttachment =
      [&](const RenderEncoderGpuAttachment& attachment) {
        if (attachment.active) {
          renderEncoderGpuSamples.push_back(attachment.sample);
        }
      };

  // Chunk's GPU seqId — feeds every transient-buffer reservation in this
  // chunk so the slab is retained until the matching command buffer
  // completes. R-BACK-12.24 argbuf populator threads this through to
  // `reserveTransientBuffer` / `uploadTransientBuffer`.
  const u64 encodeChunkSeqId = slot.seqId;

  // R-BACK-12.22..12.26 — constants-only argbuf reopen gate. Tracks the
  // uniform payload hash last written into the active encoder's argbuf
  // descriptor table. A DrawRun whose payload matches reuses that table
  // (no fresh reservation, no rebind); a changed payload forces a fresh
  // table so draws can't observe last-write-wins on a shared table. Reset
  // whenever a new encoder opens (its argbuf table starts empty).
  auto hashArgbufPayloadComponentPrefix =
      [](const auto& values, u16 count) {
        const auto clamped =
            std::min<std::size_t>(count, values.size());
        const auto bytes = std::as_bytes(std::span(
            values.data(), clamped));
        u64 hash = drawBindingPacketHashMix(
            0x8fc6d3f8f0b19c45ull, static_cast<u64>(clamped));
        hash = drawBindingPacketHashMix(hash, core::hashBytes(bytes));
        return hash;
      };
  auto makeArgbufPayloadDeltaKey =
      [](core::FlatDrawStateView drawState) {
        const auto& payload = drawState.uniformPayload();
        return ArgbufPayloadDeltaKey{
            .hash = payload.hash,
            .vertexConstantsHash = drawStateVertexCbufSourceHash(drawState),
            .pixelConstantsHash = drawStatePixelCbufSourceHash(drawState),
        };
      };
  auto makeArgbufPayloadDeltaComponentKey =
      [&](const core::DrawUniformPayload& payload) {
        return ArgbufPayloadDeltaComponentKey{
            .vsFloatHash = hashArgbufPayloadComponentPrefix(
                payload.vsConst.float4, payload.vertexFloatConstantCount),
            .vsIntHash = hashArgbufPayloadComponentPrefix(
                payload.vsConst.int4, payload.vertexIntConstantCount),
            .vsBoolHash = hashArgbufPayloadComponentPrefix(
                payload.vsConst.bools, payload.vertexBoolConstantCount),
            .psFloatHash = hashArgbufPayloadComponentPrefix(
                payload.psConst.float4, payload.pixelFloatConstantCount),
            .psIntHash = hashArgbufPayloadComponentPrefix(
                payload.psConst.int4, payload.pixelIntConstantCount),
            .psBoolHash = hashArgbufPayloadComponentPrefix(
                payload.psConst.bools, payload.pixelBoolConstantCount),
        };
      };
  auto markDirectCbufVsPayloadDirty =
      [](uniform::DirtyState& dirty,
         const core::DrawUniformPayload& payload) {
        bool marked = false;
        if (payload.vertexFloatConstantCount != 0) {
          uniform::applyConstantSetVsF(
              dirty, 0u, payload.vertexFloatConstantCount);
          marked = true;
        }
        if (payload.vertexIntConstantCount != 0) {
          uniform::applyConstantSetVsI(
              dirty, 0u, payload.vertexIntConstantCount);
          marked = true;
        }
        if (payload.vertexBoolConstantCount != 0) {
          uniform::applyConstantSetVsB(
              dirty, 0u, payload.vertexBoolConstantCount);
          marked = true;
        }
        if (!marked) {
          uniform::setBit(dirty, uniform::DirtyBit::VsF);
        }
      };
  auto markDirectCbufPsPayloadDirty =
      [](uniform::DirtyState& dirty,
         const core::DrawUniformPayload& payload) {
        bool marked = false;
        if (payload.pixelFloatConstantCount != 0) {
          uniform::applyConstantSetPsF(
              dirty, 0u, payload.pixelFloatConstantCount);
          marked = true;
        }
        if (payload.pixelIntConstantCount != 0) {
          uniform::applyConstantSetPsI(
              dirty, 0u, payload.pixelIntConstantCount);
          marked = true;
        }
        if (payload.pixelBoolConstantCount != 0) {
          uniform::applyConstantSetPsB(
              dirty, 0u, payload.pixelBoolConstantCount);
          marked = true;
        }
        if (!marked) {
          uniform::setBit(dirty, uniform::DirtyBit::PsF);
        }
      };
  struct ArgbufPayloadChangedPrefixStats {
    u64 changed = 0;
    u64 prefix = 0;
    u64 span = 0;
    bool fullPrefix = false;
  };
  auto measureArgbufPayloadChangedPrefix =
      [](const auto& previousValues, u16 previousCount,
         const auto& currentValues, u16 currentCount) {
        const auto previousClamped =
            std::min<std::size_t>(previousCount, previousValues.size());
        const auto currentClamped =
            std::min<std::size_t>(currentCount, currentValues.size());
        const auto count = std::max(previousClamped, currentClamped);
        std::size_t firstChanged = count;
        std::size_t lastChanged = 0;
        for (std::size_t i = 0; i < count; ++i) {
          if (i >= previousClamped || i >= currentClamped ||
              std::memcmp(&previousValues[i], &currentValues[i],
                          sizeof(previousValues[i])) != 0) {
            firstChanged = std::min(firstChanged, i);
            lastChanged = i;
          }
        }
        if (firstChanged == count) {
          return ArgbufPayloadChangedPrefixStats{
              .changed = 0,
              .prefix = static_cast<u64>(count),
              .span = 0,
              .fullPrefix = false,
          };
        }
        const auto changed = lastChanged - firstChanged + 1u;
        u64 changedCount = 0;
        for (std::size_t i = firstChanged; i <= lastChanged; ++i) {
          if (i >= previousClamped || i >= currentClamped ||
              std::memcmp(&previousValues[i], &currentValues[i],
                          sizeof(previousValues[i])) != 0) {
            ++changedCount;
          }
        }
        return ArgbufPayloadChangedPrefixStats{
            .changed = changedCount,
            .prefix = static_cast<u64>(count),
            .span = static_cast<u64>(changed),
            .fullPrefix = changedCount == static_cast<u64>(count),
        };
      };
  auto recordArgbufPayloadChangedVsFloatRegBucket = [](u64 changedRegs) {
    if (changedRegs <= 1u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe1Sum(
          changedRegs);
    } else if (changedRegs <= 4u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe4Sum(
          changedRegs);
    } else if (changedRegs <= 16u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe16Sum(
          changedRegs);
    } else if (changedRegs <= 64u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsLe64Sum(
          changedRegs);
    } else {
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsGt64Sum(
          changedRegs);
    }
  };
  auto recordArgbufPayloadChangedPsFloatRegBucket = [](u64 changedRegs) {
    if (changedRegs <= 1u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe1Sum(
          changedRegs);
    } else if (changedRegs <= 4u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe4Sum(
          changedRegs);
    } else if (changedRegs <= 16u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe16Sum(
          changedRegs);
    } else if (changedRegs <= 64u) {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsLe64Sum(
          changedRegs);
    } else {
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64(1u);
      perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsGt64Sum(
          changedRegs);
    }
  };
  struct ArgbufPayloadDeltaSourceBucket {
    bool valid = false;
    u64 vsHash = 0;
    u64 psHash = 0;
    u64 prefixRegs = 0;
    u64 rows = 0;
    u64 changedRegs = 0;
    u64 spanRegs = 0;
    u64 fullPrefixRows = 0;
    u64 fullPrefixRegs = 0;
  };
  struct ArgbufPayloadDeltaSourceAttribution {
    std::array<ArgbufPayloadDeltaSourceBucket, 128> buckets{};
    u64 overflowRows = 0;
    u64 overflowChangedRegs = 0;

    void record(core::FlatDrawStateView drawState,
                const ArgbufPayloadChangedPrefixStats& stats) noexcept {
      if (stats.changed == 0) {
        return;
      }
      u64 vsHash = 0;
      u64 psHash = 0;
      if (drawState.hasShaderContext()) {
        const auto& shader = drawState.shaderContext();
        vsHash = shader.vertexShader.hash;
        psHash = shader.pixelShader.hash;
      }
      ArgbufPayloadDeltaSourceBucket* target = nullptr;
      for (auto& bucket : buckets) {
        if (bucket.valid && bucket.vsHash == vsHash &&
            bucket.psHash == psHash && bucket.prefixRegs == stats.prefix) {
          target = &bucket;
          break;
        }
        if (!bucket.valid && !target) {
          target = &bucket;
        }
      }
      if (!target) {
        ++overflowRows;
        overflowChangedRegs += stats.changed;
        return;
      }
      if (!target->valid) {
        target->valid = true;
        target->vsHash = vsHash;
        target->psHash = psHash;
        target->prefixRegs = stats.prefix;
      }
      ++target->rows;
      target->changedRegs += stats.changed;
      target->spanRegs += stats.span;
      if (stats.fullPrefix) {
        ++target->fullPrefixRows;
        target->fullPrefixRegs += stats.changed;
      }
    }

    void emit(u64 seqId) const {
      if (overflowRows != 0 || overflowChangedRegs != 0) {
        std::fprintf(
            stderr,
            "[dxmt9-perf-argbuf-payload-delta-source seq=%llu "
            "overflow=1 rows=%llu changed_regs=%llu]\n",
            static_cast<unsigned long long>(seqId),
            static_cast<unsigned long long>(overflowRows),
            static_cast<unsigned long long>(overflowChangedRegs));
      }
      for (const auto& bucket : buckets) {
        if (!bucket.valid || bucket.rows == 0) {
          continue;
        }
        std::fprintf(
            stderr,
            "[dxmt9-perf-argbuf-payload-delta-source seq=%llu "
            "overflow=0 vs_hash=0x%llx ps_hash=0x%llx prefix_regs=%llu "
            "rows=%llu changed_regs=%llu span_regs=%llu "
            "full_prefix_rows=%llu full_prefix_regs=%llu]\n",
            static_cast<unsigned long long>(seqId),
            static_cast<unsigned long long>(bucket.vsHash),
            static_cast<unsigned long long>(bucket.psHash),
            static_cast<unsigned long long>(bucket.prefixRegs),
            static_cast<unsigned long long>(bucket.rows),
            static_cast<unsigned long long>(bucket.changedRegs),
            static_cast<unsigned long long>(bucket.spanRegs),
            static_cast<unsigned long long>(bucket.fullPrefixRows),
            static_cast<unsigned long long>(bucket.fullPrefixRegs));
      }
    }
  };
  const bool argbufPayloadDeltaSourcePerf =
      argbufPayloadDeltaSourcePerfEnabled();
  ArgbufPayloadDeltaSourceAttribution argbufPayloadDeltaSourceAttribution;
  static thread_local RenderPassFrameTracker renderPassFrameTracker;
  auto traceRenderPassProgress = [&](const char* stage,
                                     std::size_t commandIndex,
                                     bool hasClear) {
    if (!traceEncodeProgress) {
      return;
    }
    std::ostringstream out;
    out << "[dxmt9-encode-progress]"
        << " stage=renderpass." << stage
        << " seq=" << static_cast<unsigned long long>(slot.seqId)
        << " slot=" << slotIndex
        << " command=" << commandIndex
        << " encoder=" << static_cast<unsigned long long>(renderEncoderIndex)
        << " clear=" << (hasClear ? 1 : 0);
    emitQueueTraceLine(out.str());
  };

  // TLA+: EncoderLifecycle variable binding:
  // activeKind  := activeRenderEncoder ? "Render" : activeBlitEncoder ? "Blit" : "None"
  // activeRT    := activeKey while activeRenderEncoder is live; NoRT otherwise.
  // hazardFlag  := exact overlap between current attachments and next draw reads, consumed immediately by a split.
  // opCount     := progress through the slot commandHeaders replay loop below.
  // The current blit helpers open and end short-lived encoders internally, so
  // activeBlitEncoder is normally None but remains the local binding for a
  // future chunk-scoped blit encoder.
  auto assertEncoderLifecycleInvariant = [&] {
    DXMT_ASSERT(!(activeRenderEncoder && activeBlitEncoder));
    DXMT_ASSERT(hasActiveRender == static_cast<bool>(activeRenderEncoder));
  };

  auto assertNoActiveEncoder = [&] {
    assertEncoderLifecycleInvariant();
    DXMT_ASSERT(!activeRenderEncoder);
    DXMT_ASSERT(!activeBlitEncoder);
    DXMT_ASSERT(!hasActiveRender);
  };

  auto flushRender = [&](perf::EncoderSplitReason reason = perf::EncoderSplitReason::Final) {
    if (activeRenderEncoder) {
      recordSessionSourceActiveRenderClosedBeforeFirstDraw(reason);
      // TLA+: EncoderLifecycle / EndEncoder(Render)
      DXMT_ASSERT(hasActiveRender);
      DXMT_ASSERT(!activeBlitEncoder);
      // M2: pop the render-pass debug group pushed in startRenderPass.
      // Must happen before endEncoding — the encoder rejects further
      // commands once endEncoding fires.
      activeRenderEncoder.popDebugGroup();
      activeRenderEncoder.endEncoding();
      maybeEncodeColorAttachmentDump(commandBuffer, ctx.device,
                                     activeColorAttachmentDump,
                                     completionCallbacks);
      maybeEncodeDepthAttachmentDump(commandBuffer, ctx.device,
                                     activeDepthAttachmentDump,
                                     completionCallbacks);
      maybeEncodeDrawTextureDumps(commandBuffer, ctx.device,
                                  activeDrawTextureDumps,
                                  completionCallbacks);
      if (activeVisibilityScout) {
        enqueueVisibilityScoutCompletion(*activeVisibilityScout,
                                         completionCallbacks);
        activeVisibilityScout.reset();
      }
      perf::countRenderPassEnd(reason);
      activeEncoderBreakdown.emit(reason);
      activeStreamIbStaging.begin(false);
      // R-BACK-15.4: color attachments stored on this pass become "touched"
      // on the queue so the next pass on the same handle Loads instead of
      // DontCare-loads. The current color DontCare-store proof only fires
      // when the next touch is a Clear, whose load action ignores this
      // touched bit, so conservatively marking every active color handle
      // keeps the touched-set contract simple without losing that win.
      for (auto& handle : activeColorHandles) {
        if (handle) {
          ctx.queue.markColorHandleTouched(handle);
          handle = core::Handle{};
        }
      }
      activeColorAttachmentDump = {};
      activeDepthAttachmentDump = {};
      activeDrawTextureDumps.clear();
      activeRenderEncoderSeq = 0;
      activeRenderEncoderIndex = 0;
      activeRenderEncoder = {};
      hasActiveRender = false;
      activeDrawStateKey.reset();
      activeDrawStateUsesPrefetchedPsoLayout = false;
      textureSamplerShadow.reset();
      assertEncoderLifecycleInvariant();
    }
  };

  auto flushBlit = [&] {
    if (activeBlitEncoder) {
      // TLA+: EncoderLifecycle / EndEncoder(Blit)
      DXMT_ASSERT(!activeRenderEncoder);
      DXMT_ASSERT(!hasActiveRender);
      activeBlitEncoder.endEncoding();
      activeBlitEncoder = {};
      assertEncoderLifecycleInvariant();
    }
  };

  auto startRenderPass = [&](core::FlatDrawStateView drawState,
                             const std::optional<core::ClearDesc>& clear,
                             std::size_t lookaheadStartIndex,
                             core::PsoHandle renderPsoHandle) {
    traceRenderPassProgress("begin", lookaheadStartIndex, clear.has_value());
    // TLA+: EncoderLifecycle / BeginRender(rt)
    // Callers split through None before opening a new render encoder.
    // R-BACK-15.7: pass the slot + current command index so beginRenderPass
    // can run the depth/stencil DontCare-store look-ahead over the
    // remaining records.
    assertNoActiveEncoder();
    const auto sampleAttachment = makeRenderEncoderGpuAttachment(
        core::metalqueue::RenderEncoderGpuPassType::Draw,
        lookaheadStartIndex,
        drawState.hot->colorAttachments[0].handle.value,
        drawState.hot->depthStencil.handle.value,
        psoHandleBucket(renderPsoHandle));
    const u64 openedRenderEncoderIndex = renderEncoderIndex;
    activeVisibilityScout =
        makeVisibilityScoutPass(ctx.device, encodeChunkSeqId,
                                openedRenderEncoderIndex);
    const WMT::Buffer visibilityBuffer{
        activeVisibilityScout ? activeVisibilityScout->buffer.handle
                              : NULL_OBJECT_HANDLE};
    traceRenderPassProgress("before-begin-render-pass", lookaheadStartIndex,
                            clear.has_value());
    std::array<RenderPassStoreProofLookaheadSource,
               core::metalqueue::kMaxEncodeSessionSources>
        storeProofLookaheadStorage{};
    std::size_t storeProofLookaheadCount = 0;
    auto appendStoreProofLookahead = [&](const core::ChunkSlot* sourceSlot,
                                         std::size_t firstCommandIndex,
                                         std::size_t commandEndIndex) {
      if (!sourceSlot ||
          storeProofLookaheadCount >= storeProofLookaheadStorage.size()) {
        return;
      }
      const std::size_t slotCommandCount = sourceSlot->commandCount();
      const std::size_t clampedFirst =
          std::min(firstCommandIndex, slotCommandCount);
      const std::size_t clampedEnd =
          std::min(commandEndIndex, slotCommandCount);
      storeProofLookaheadStorage[storeProofLookaheadCount++] =
          RenderPassStoreProofLookaheadSource{
              .slot = sourceSlot,
              .firstCommandIndex = clampedFirst,
              .commandEndIndex = clampedEnd,
          };
    };
    auto firstCommandAfter = [](const core::ChunkSlot& sourceSlot,
                                std::size_t commandIndex) {
      return commandIndex < sourceSlot.commandCount()
          ? commandIndex + 1u
          : sourceSlot.commandCount();
    };
    const bool selectedSessionLookaheadStartsHere =
        !options.sessionLookaheadSources.empty() &&
        (options.sessionSource.has_value()
             ? readySlotSnapshotMatchesCompletionSource(
                   options.sessionLookaheadSources.front(),
                   *options.sessionSource,
                   slotIndex,
                   slot)
             : readySlotSnapshotMatchesReplayRange(
                   options.sessionLookaheadSources.front(),
                   slotIndex,
                   slot,
                   replayRange));
    bool selectedSessionLookaheadValid = selectedSessionLookaheadStartsHere;
    if (selectedSessionLookaheadValid) {
      for (const auto& source : options.sessionLookaheadSources) {
        if (!source.slot ||
            source.commandBegin > source.slot->commandCount() ||
            source.commandCount >
                source.slot->commandCount() - source.commandBegin) {
          selectedSessionLookaheadValid = false;
          break;
        }
      }
      if (selectedSessionLookaheadValid) {
        const auto& firstSource = options.sessionLookaheadSources.front();
        const std::size_t firstSourceEnd =
            firstSource.commandBegin + firstSource.commandCount;
        const std::size_t nextCommand =
            firstCommandAfter(*firstSource.slot, lookaheadStartIndex);
        selectedSessionLookaheadValid =
            lookaheadStartIndex >= firstSource.commandBegin &&
            lookaheadStartIndex < firstSourceEnd &&
            nextCommand <= firstSourceEnd;
      }
    }
    if (selectedSessionLookaheadValid) {
      for (std::size_t i = 0; i < options.sessionLookaheadSources.size(); ++i) {
        const auto& source = options.sessionLookaheadSources[i];
        const auto* sourceSlot = source.slot;
        const std::size_t sourceEnd = source.commandBegin + source.commandCount;
        appendStoreProofLookahead(
            sourceSlot,
            i == 0u ? firstCommandAfter(*sourceSlot, lookaheadStartIndex)
                    : source.commandBegin,
            sourceEnd);
      }
    }
    if (storeProofLookaheadCount == 0 &&
        useSourceLocalStoreProofLookahead(options.session != nullptr,
                                         deferSessionFinalization)) {
      appendStoreProofLookahead(&slot, firstCommandAfter(slot, lookaheadStartIndex),
                                slot.commandCount());
    }
    const std::span<const RenderPassStoreProofLookaheadSource>
        storeProofLookaheadSources(storeProofLookaheadStorage.data(),
                                   storeProofLookaheadCount);
    RenderPassActionSummary renderPassActions{};
    activeRenderEncoder = beginRenderPassWithStoreProofLookahead(
        ctx, commandBuffer, drawState, clear, storeProofLookaheadSources,
        sampleAttachment.span(), visibilityBuffer, &renderPassActions);
    traceRenderPassProgress(activeRenderEncoder
                                ? "after-begin-render-pass-ok"
                                : "after-begin-render-pass-null",
                            lookaheadStartIndex, clear.has_value());
    hasActiveRender = static_cast<bool>(activeRenderEncoder);
    if (!hasActiveRender) {
      activeVisibilityScout.reset();
    }
    if (hasActiveRender) {
      const auto storeProof = renderPassStoreProofSummaryForLookahead(
          ctx, storeProofLookaheadSources, *drawState.hot);
      renderPassFrameTracker.noteStart(
          makeRenderPassFrameKey(*drawState.hot),
          estimateRenderPassAttachmentFootprintBytes(ctx, *drawState.hot),
          storeProof,
          encodeChunkSeqId,
          openedRenderEncoderIndex);
      activeEncoderBreakdown.begin(
          encodeChunkSeqId, openedRenderEncoderIndex,
          drawState.hot->colorAttachments[0].handle.value,
          drawState.hot->depthStencil.handle.value);
      activeStreamIbStaging.begin(
          stageStreamIbProbeRowMatches(&activeEncoderBreakdown));
      activeEncoderBreakdown.recordAttachmentMetadata(ctx.pool, *drawState.hot);
      activeEncoderBreakdown.recordRenderPassActions(renderPassActions);
      ++renderEncoderIndex;
      recordRenderEncoderGpuAttachment(sampleAttachment);
    } else {
      activeStreamIbStaging.begin(false);
    }
    activeKey = makeAttachmentKey(*drawState.hot);
    activeWriteHazard = makeAttachmentHazard(*drawState.hot);
    activeDrawStateKey.reset();
    activeDrawStateUsesPrefetchedPsoLayout = false;
    textureSamplerShadow.reset();
    // R-BACK-13.1 — per-pass tile-shader FFP selector. Eligibility is
    // computed once at encoder open; the choice is sticky for the pass.
    // Counters: each opened pass bumps exactly one of
    // tileFfpPassCount / portableFfpPassCount, plus the by-reason
    // breakdown when the precision/unsupported_state path forced a
    // fallback. R-BACK-13.5: gpu_family is recorded but only via the
    // dedicated tileFfpFallbackGpuFamily counter, not the pass count.
    {
      const auto selection =
          dxmt9::pipeline::selectTileFfpForPass(drawState, ctx.pool.supportsApple3());
      activePassUsesTileFfp = selection.decision == dxmt9::pipeline::TileFfpDecision::Tile;
      if (activePassUsesTileFfp) {
        perf::countTileFfpPass();
      } else {
        perf::countPortableFfpPass();
        switch (selection.reason) {
          case dxmt9::pipeline::TileFfpFallbackReason::GpuFamily:
            perf::countTileFfpFallbackGpuFamily();
            break;
          case dxmt9::pipeline::TileFfpFallbackReason::Precision:
            perf::countTileFfpFallbackPrecision();
            break;
          case dxmt9::pipeline::TileFfpFallbackReason::UnsupportedState:
            perf::countTileFfpFallbackUnsupportedState();
            break;
          case dxmt9::pipeline::TileFfpFallbackReason::None:
          case dxmt9::pipeline::TileFfpFallbackReason::NotFfp:
            // No fallback class is bumped: NotFfp means the pass never
            // had an FFP key to translate, GpuFamily is its own counter,
            // None is the eligible case (already on tile path).
            break;
        }
      }
    }
    // R-BACK-15.4: capture color attachment handles so flushRender can
    // mark them touched on the queue once the encoder closes.
    for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
      activeColorHandles[i] = drawState.hot->colorAttachments[i].handle;
    }
    activeColorAttachmentDump = {};
    activeDepthAttachmentDump = {};
    activeDrawTextureDumps.clear();
    activeRenderEncoderSeq = encodeChunkSeqId;
    activeRenderEncoderIndex = openedRenderEncoderIndex;
    if (drawTextureDumpPassMatches(activeRenderEncoderSeq,
                                   activeRenderEncoderIndex)) {
      activeDrawTextureDumps.reserve(
          drawTextureDumpConfig().handles.size());
    }
    if (auto* depthSurface =
            ctx.pool.findSurface(drawState.hot->depthStencil.handle.value);
        activeRenderEncoder && depthSurface && depthSurface->texture &&
        depthSurface->desc.depthStencil) {
      activeDepthAttachmentDump.handle = drawState.hot->depthStencil.handle;
      activeDepthAttachmentDump.texture = depthSurface->texture;
      activeDepthAttachmentDump.format = depthSurface->desc.format;
      activeDepthAttachmentDump.metalPixelFormat =
          toPixelFormat(depthSurface->desc.format, ctx.limits);
      activeDepthAttachmentDump.width = std::max(1u, depthSurface->desc.width);
      activeDepthAttachmentDump.height = std::max(1u, depthSurface->desc.height);
      activeDepthAttachmentDump.seq = encodeChunkSeqId;
      activeDepthAttachmentDump.enc = openedRenderEncoderIndex;
      activeDepthAttachmentDump.hasDepth =
          formatHasDepthAspect(depthSurface->desc.format);
      activeDepthAttachmentDump.hasStencil =
          formatHasStencilAspect(depthSurface->desc.format);
    }
    if (activeRenderEncoder && colorAttachmentDumpConfig().enabled) {
      const auto& colorDumpConfig = colorAttachmentDumpConfig();
      for (std::size_t i = 0; i < core::kMaxRenderTargets; ++i) {
        const auto colorHandle = drawState.hot->colorAttachments[i].handle;
        if (!colorHandle) {
          continue;
        }
        if (colorDumpConfig.index.has_value() &&
            *colorDumpConfig.index != i) {
          continue;
        }
        if (colorDumpConfig.handle.has_value() &&
            *colorDumpConfig.handle != colorHandle.value) {
          continue;
        }
        auto* colorSurface = ctx.pool.findSurface(colorHandle.value);
        if (!colorSurface || !colorSurface->texture ||
            !colorSurface->desc.renderTarget) {
          continue;
        }
        activeColorAttachmentDump.handle = colorHandle;
        activeColorAttachmentDump.texture = colorSurface->texture;
        activeColorAttachmentDump.format = colorSurface->desc.format;
        activeColorAttachmentDump.metalPixelFormat =
            toPixelFormat(colorSurface->desc.format, ctx.limits);
        activeColorAttachmentDump.width = std::max(1u, colorSurface->desc.width);
        activeColorAttachmentDump.height = std::max(1u, colorSurface->desc.height);
        activeColorAttachmentDump.index = static_cast<u32>(i);
        activeColorAttachmentDump.seq = encodeChunkSeqId;
        activeColorAttachmentDump.enc = openedRenderEncoderIndex;
        break;
      }
    }
    // M2: push a debug group identifying the render pass attachments.
    // Paired with the popDebugGroup() at the head of flushRender.
    //
    // Also set the encoder label with the same string. The debug group is
    // visible in Xcode's frame capture (.gputrace), but xctrace's
    // metal-application-encoders-list schema reports only the encoder
    // label, so without setLabel xctrace shows the Metal default
    // "Render Command N" and per-pass GPU time cannot be attributed to
    // an RT in text-based analysis.
    if (activeRenderEncoder) {
      traceRenderPassProgress("before-label", lookaheadStartIndex,
                              clear.has_value());
      const auto rt0 = static_cast<unsigned long long>(
          drawState.hot->colorAttachments[0].handle.value);
      const auto depth = static_cast<unsigned long long>(
          drawState.hot->depthStencil.handle.value);
      auto passLabel = makeLabelStringFmt(
          "RenderPass[seq=%llu,enc=%llu,rt=0x%llx,depth=0x%llx]",
          static_cast<unsigned long long>(encodeChunkSeqId),
          static_cast<unsigned long long>(openedRenderEncoderIndex), rt0,
          depth);
      activeRenderEncoder.setLabel(passLabel);
      activeRenderEncoder.pushDebugGroup(passLabel);
      traceRenderPassProgress("after-label", lookaheadStartIndex,
                              clear.has_value());
    }
    // R-BACK-12.22 / 12.24 / 12.25 — Stage 2 argbuf-hybrid per-encoder
    // populator. The selector reads the cached capability bool on the
    // pool. When the gate holds AND the queue-owned encoder resource
    // initialized successfully, the populator reserves the argbuf
    // storage from the transient ring, points the queue's
    // MTLArgumentEncoder at it, writes the four per-frequency cbuf
    // entries + the texture/sampler descriptors, and binds slot 30
    // (vertex + fragment) of the active render encoder. Stage 2 PSOs
    // read through this slot-30 argbuf; encodeDraw skips the direct
    // Stage 1 slot 0 / slot 3 and texture/sampler binds in this mode.
    //
    // When the gate fails (any non-Apple-Silicon device) `openArgbuf`
    // returns an empty handle and we fall through to the Stage 1
    // counter; no slot-30 bind is issued.
    activePassUsesArgbufHybrid = false;
    activePassUsesArgbufResourceArray = false;
    activePassUsesArgbufDirectCbuf = false;
    activeArgbufStorage = {};
    activeArgbufOffset = 0;
    {
      traceRenderPassProgress("before-argbuf-select", lookaheadStartIndex,
                              clear.has_value());
      const auto argbufDecision = dxmt9::pipeline::selectArgbufHybridForPass(
          drawState, ctx.pool.argbufHybridEnabled());
      traceRenderPassProgress("after-argbuf-select", lookaheadStartIndex,
                              clear.has_value());
      if (argbufDecision == dxmt9::pipeline::ArgbufHybridDecision::Stage2) {
        perf::countArgbufHybridEncoder();
        // R-BACK-12.22..12.26 (resource-array sub-mode) — pick the
        // resource-array encoder (20-entry table, larger encodedLength) when
        // the lane is active for the queue; otherwise the constants-only
        // encoder. Both anchor onto a fresh transient slab; the only delta is
        // the reservation size and whether texture/sampler slots are written.
        const bool resourceArrayLane = ctx.queue.resourceArrayLaneActive() &&
            ctx.queue.resourceArrayEncoderResource().initialized();
        const bool directCbufLane =
            !resourceArrayLane && dxmt9::pipeline::argbufDirectCbufEnabled();
        if (directCbufLane) {
          activePassUsesArgbufHybrid = true;
          activePassUsesArgbufDirectCbuf = true;
          traceRenderPassProgress("argbuf-direct-cbuf", lookaheadStartIndex,
                                  clear.has_value());
        } else {
          auto& encoderResource = resourceArrayLane
                                      ? ctx.queue.resourceArrayEncoderResource()
                                      : ctx.queue.argbufEncoderResource();
          traceRenderPassProgress("before-argbuf-open", lookaheadStartIndex,
                                  clear.has_value());
          const auto populated = dxmt9::argbuf_hybrid::openArgbufWithCompletedSeqId(
              ctx.queue, encoderResource, encodeChunkSeqId,
              ctx.transientCompletedSeqId);
          traceRenderPassProgress(populated ? "after-argbuf-open-ok"
                                            : "after-argbuf-open-empty",
                                  lookaheadStartIndex, clear.has_value());
          if (populated) {
            // Constant-buffer entries (VsConsts/PsConsts/FfpVsConsts/
            // FfpPsConsts) are populated lazily from encodeDraw's dirty
            // path on the first draw. Texture/sampler resources remain on
            // the direct fragment binding lane for texture-bound Stage 2
            // draws, so encoder open only binds the argbuf storage.
            // Bind slot 30 — vertex + fragment. The render encoder reads
            // from this single argbuf for the duration of the pass; the
            // slot-30 bind is the only argbuf-related bind on the encoder
            // (per design.md §11.2; setVertexBytes(slot=5) / vertex stream
            // slot 1 stay direct).
            traceRenderPassProgress("before-argbuf-bind", lookaheadStartIndex,
                                    clear.has_value());
            activeRenderEncoder.setVertexBuffer(populated.storage,
                                                populated.offset,
                                                dxmt9::shaders::kArgbufHybridBindSlot);
            activeRenderEncoder.setFragmentBuffer(populated.storage,
                                                  populated.offset,
                                                  dxmt9::shaders::kArgbufHybridBindSlot);
            traceRenderPassProgress("after-argbuf-bind", lookaheadStartIndex,
                                    clear.has_value());
            // R-BACK-12.25 — upload accounting. `populated.length` is the
            // argbuf descriptor-table size (matches the encoder's reported
            // encodedLength); per-frequency cbuf bytes are bumped by
            // updateDirtyArgbufRegions on the first draw.
            perf::countArgbufHybridBytes(populated.length);
            activeEncoderBreakdown.addArgbufTableBytes(populated.length);
            activePassUsesArgbufHybrid = true;
            activePassUsesArgbufResourceArray = resourceArrayLane;
            activePassUsesArgbufDirectCbuf = false;
            activeArgbufStorage = populated.storage;
            activeArgbufOffset = populated.offset;
          } else {
            // Selector chose Stage 2 but the encoder resource didn't init
            // (sentinel-null device, test fixture, or transient ring
            // exhaustion). R-BACK-12.22 sentence 2: never mid-pass switch
            // — the pass commits to Stage 1 for its lifetime. Fallback
            // counter bumps so a regression that turns this from "rare"
            // into "common" surfaces.
            perf::countArgbufHybridFallback();
          }
        }
      } else {
        perf::countStage1Encoder();
        // Stage 1 byte total so the regression test in design.md §11.5
        // can compare Stage 2's expected savings. Bytes scale with the
        // four per-frequency UBOs the encoder may upload (worst-case,
        // dirty-mask all set on encoder open). Stage 2's counter bumps
        // with the argbuf encodedLength when the runtime activates
        // it; both remain comparable per-encoder.
        perf::countStage1Bytes(sizeof(VsConsts) + sizeof(PsConsts) +
                                sizeof(FfpVsConsts) + sizeof(FfpPsConsts));
      }
    }
    // R-BACK-12.12: a fresh Metal render encoder loses any prior
    // sticky bindings — every uniform category must rebind on the
    // first draw of the new encoder.
    uniform::markAllDirty(uniformDirty);
    // The fresh encoder's argbuf table (opened above) is empty, so the
    // first draw of this pass must reopen + populate regardless of its
    // payload hash.
    lastArgbufPayloadHash.reset();
    lastArgbufPayloadDeltaKey.reset();
    lastArgbufPayloadDeltaComponentKey.reset();
    lastArgbufPayloadDeltaPayload.reset();
    argbufCbufCache.reset();
    assertEncoderLifecycleInvariant();
    traceRenderPassProgress("end", lookaheadStartIndex, clear.has_value());
  };

  auto assertHelperEncoderPrecondition = [&] {
    // TLA+: EncoderLifecycle / BeginBlit
    // Blit-style helpers own any Metal encoder they open and end it before
    // returning; encodeChunk must have ended its active encoder first.
    assertNoActiveEncoder();
  };

  auto flushPendingClear = [&] {
    if (!pendingClear.has_value()) return;
    const auto& clear = *pendingClear;
    const auto sampleAttachment = makeRenderEncoderGpuAttachment(
        core::metalqueue::RenderEncoderGpuPassType::Clear,
        pendingClearCommandIndex,
        clear.colorAttachments[0].handle.value,
        clear.depthStencil.handle.value);
    dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, clear,
                                     sampleAttachment.span());
    recordRenderEncoderGpuAttachment(sampleAttachment);
    commandBufferHasWork = true;
    pendingClear.reset();
    pendingClearCommandIndex = std::numeric_limits<std::size_t>::max();
  };

  auto finalizeEncodeChunkSessionForReturn = [&] {
    traceEncodeStage("before-final-flush-pending-clear");
    flushPendingClear();
    traceEncodeStage("after-final-flush-pending-clear");
    traceEncodeStage("before-final-flush-render");
    flushRender(perf::EncoderSplitReason::Final);
    traceEncodeStage("after-final-flush-render");
    traceEncodeStage("before-final-flush-blit");
    flushBlit();
    traceEncodeStage("after-final-flush-blit");
    traceEncodeStage("before-final-assert-no-active-encoder");
    assertNoActiveEncoder();
    traceEncodeStage("after-final-assert-no-active-encoder");
  };

  // Deferred-upload fence: flush any pending staging->private blits via
  // the queue-owned ResourceInitializer, then wait for its SharedEvent
  // signal before any draw samples those resources. A stale event value
  // from an earlier flush is not a new dependency for this command buffer;
  // waiting for it again would force-close a carried render encoder for no
  // resource-ordering benefit.
  traceEncodeStage("before-initializer-flush");
  const auto initializerFlush = ctx.queue.flushInitializerUploads();
  traceEncodeStage("after-initializer-flush");
  if (initializerFlush.didFlush &&
      initializerFlush.event &&
      initializerFlush.value > 0) {
    if (activeRenderEncoder || activeBlitEncoder || pendingClear.has_value()) {
      traceEncodeStage("before-initializer-wait-finalize-session");
      if (options.session) {
        perf::countEncodeSessionCarryForcedFinalizeInitializerWait(
            static_cast<bool>(activeRenderEncoder),
            static_cast<bool>(activeBlitEncoder),
            pendingClear.has_value());
      }
      finalizeEncodeChunkSessionForReturn();
      traceEncodeStage("after-initializer-wait-finalize-session");
    }
    traceEncodeStage("before-initializer-wait");
    commandBuffer.encodeWaitForEvent(initializerFlush.event, initializerFlush.value);
    traceEncodeStage("after-initializer-wait");
    commandBufferHasWork = true;
  }

  auto splitBeforeBlockingPresent = [&] {
    if (injectedCommandBuffer ||
        options.disablePresentAcquireSplit ||
        !splitPresentBeforeAcquireEnabled() ||
        !commandBufferHasWork) {
      return;
    }
    auto presentCommandBuffer = ctx.queue.newCommandBuffer();
    if (!presentCommandBuffer) {
      return;
    }
    const auto commitStarted = std::chrono::steady_clock::now();
    commandBuffer.commit();
    perf::countCommandBufferCommitCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()));
    commandBuffer = std::move(presentCommandBuffer);
    commandBufferHasWork = false;
  };

  // R-BACK-2.29..2.32 — mid-chunk MTLCommandBuffer split. Mirrors
  // splitBeforeBlockingPresent exactly: open the next sub-CB on the same
  // queue, commit the current one (timing the commit), swap, and reset the
  // hasWork bit. CRITICAL invariants:
  //   * Must NEVER be called while an encoder is active. The natural call
  //     site after flushRender(non-Final) already satisfies this — flushRender
  //     ends the active render encoder. Helper-encoder paths
  //     (SurfaceCopy/StretchRect/Readback/ColorFill) own and end their own
  //     short-lived encoders, so calling splitMidChunk after they return is
  //     also safe. Callers must ensure flushBlit() has run if a blit encoder
  //     could be open.
  //   * Must NEVER be called between the present record's encoder open and
  //     the chain tail. The Present arm flushes + calls
  //     splitBeforeBlockingPresent already; do NOT add another split there.
  // Sub-CB completion order is guaranteed by Metal's same-queue in-order
  // submission (R-BACK-2.32). Per-chunk commits (mid + final) are folded
  // into chunkSubCBCountMax via updateMax at chunk exit so the table
  // surfaces both total mid-chunk commits and the worst-case chain length.
  std::uint64_t perChunkSubCBCount = 0;
  auto splitMidChunk = [&] {
    if ((injectedCommandBuffer &&
         !options.allowInjectedCommandBufferMidChunkCommits) ||
        !commandBufferHasWork) {
      return;
    }
    auto next = ctx.queue.newCommandBuffer();
    if (!next) return;
    const auto commitStarted = std::chrono::steady_clock::now();
    commandBuffer.commit();
    perf::countCommandBufferCommitCpuTime(static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - commitStarted).count()));
    perf::countSubCommandBufferCommit();
    ++perChunkSubCBCount;
    if (options.session) {
      ++session.committedSubCommandBuffers;
    }
    commandBuffer = std::move(next);
    commandBufferHasWork = false;
  };

  const bool injectedCommandBufferCanSplit =
      !injectedCommandBuffer ||
      options.allowInjectedCommandBufferMidChunkCommits;
  const auto commitPolicy =
      options.disableMidChunkCommits || !injectedCommandBufferCanSplit
          ? MidChunkCommitPolicy::Off
          : midChunkCommitPolicy();
  const std::uint32_t splitNRecords = midChunkCommitNRecords();
  const std::uint32_t splitChainCap = midChunkCommitCapPerRenderPass();
  std::uint32_t recordsSinceLastSplit = 0;
  // R-BACK-2.33 — splitMidChunkUnderCap wraps splitMidChunk so callers
  // do not need to repeat the cap check at every split site. cap=0
  // disables the cap (unbounded chain) for diagnostic comparison.
  // perChunkSubCBCount counts mid-chunk commits issued by this encodeChunk
  // call. A carried EncodeSession also tracks committedSubCommandBuffers
  // across source boundaries so the cap applies to the logical coalesced
  // session rather than resetting for each source.
  auto splitMidChunkUnderCap = [&] {
    const std::uint64_t committedForCap =
        options.session ? session.committedSubCommandBuffers
                        : perChunkSubCBCount;
    if (splitChainCap > 0 && committedForCap + 1 >= splitChainCap) {
      perf::countSubCommandBufferSplitSuppressedByCap();
      return;
    }
    splitMidChunk();
  };

  core::DrawUniformPayloadMaterializeCache paramUniformPayloadCache;

  using Kind = core::MetalCommandKind;
  auto encodeDrawRunCommand = [&](std::size_t commandIndex,
                                  const core::MetalCommandView& command) {
    paramUniformPayloadCache.reset();
    if (!command.drawState.hot || !command.drawState.shaderLayout ||
        !command.drawRunRecord ||
        core::drawRunDrawCount(command) == 0) {
      traceEncodeCommand("drawrun.skip-invalid", commandIndex, Kind::DrawRun,
                         command);
      return;
    }
    traceEncodeCommand("drawrun.enter", commandIndex, Kind::DrawRun, command);
    // The compact uniform materializer overwrites every field before returning
    // this scratch pointer; avoid a full DrawUniformPayload zero-fill here.
    traceEncodeCommand("drawrun.before-command-uniform", commandIndex,
                       Kind::DrawRun, command);
    core::DrawUniformPayload commandUniformScratch;
    const auto* commandUniformPayload = core::drawRunUniformPayloadForHandle(
        command, command.drawRunRecord->uniformHandle, commandUniformScratch,
        perf::DrawUniformPayloadMaterializeSite::DrawEncoderCommand);
    if (!commandUniformPayload) {
      traceEncodeCommand("drawrun.skip-no-command-uniform", commandIndex,
                         Kind::DrawRun, command);
      return;
    }
    traceEncodeCommand("drawrun.after-command-uniform", commandIndex,
                       Kind::DrawRun, command);
    auto stateView = command.drawState;
    stateView.uniforms = commandUniformPayload;
    const auto& hot = *stateView.hot;
    const auto drawItems =
        command.drawItems.empty() ? command.drawParams : command.drawItems;
    const core::PsoHandle renderPsoHandle =
        command.drawRunRecord ? command.drawRunRecord->renderPsoHandle
                              : core::PsoHandle{};
    const core::PsoHandle tilePsoHandle =
        command.drawRunRecord ? command.drawRunRecord->tilePsoHandle
                              : core::PsoHandle{};
    const core::DepthStencilHandle depthStencilHandle =
        command.drawRunRecord ? command.drawRunRecord->depthStencilHandle
                              : core::DepthStencilHandle{};
    // Compact draw-run: state bound from base ONCE (render-pass +
    // resource-binding decisions key off base.rts), then loop over
    // per-DrawParam emits. FlatDrawStateKey is the hot-path decision
    // object for skipping base-state rebinding across compatible
    // Draw/DrawRun records on the same Metal render encoder.
    traceEncodeCommand("drawrun.before-flush-blit", commandIndex,
                       Kind::DrawRun, command);
    flushBlit();
    traceEncodeCommand("drawrun.after-flush-blit", commandIndex,
                       Kind::DrawRun, command);
    assertEncoderLifecycleInvariant();
    const auto drawKey = makeAttachmentKey(hot);
    const auto drawReadHazard = makeDrawReadHazard(stateView);
    auto hasExactRenderHazard = [&] {
      const bool bloomOverlap = activeWriteHazard.bloomOverlaps(drawReadHazard);
      const bool exactOverlap = activeWriteHazard.exactOverlaps(drawReadHazard);
      perf::countHazardProbe(bloomOverlap, exactOverlap);
      return exactOverlap;
    };
    // R-BACK-13.6 — mid-pass eligibility. When the active encoder is on
    // the tile path and the next draw's state has become ineligible
    // (e.g. alpha-test reference flipped out of [0,1], fog mode flipped
    // to Exp/Exp2), force a render-pass split so the next encoder opens
    // on the portable path. A pass that opened on the portable path
    // stays portable regardless (portable handles every state).
    auto tileMidPassIneligible = [&]() {
      if (!hasActiveRender || !activePassUsesTileFfp) return false;
      const auto sel =
          dxmt9::pipeline::selectTileFfpForPass(stateView, ctx.pool.supportsApple3());
      return sel.decision != dxmt9::pipeline::TileFfpDecision::Tile;
    };
    if (pendingClear.has_value()) {
      const auto clearKey = makeAttachmentKey(*pendingClear);
      const auto clearHazard = makeAttachmentHazard(*pendingClear);
      if (clearKey == drawKey && !clearHazard.exactOverlaps(drawReadHazard)) {
        recordSessionSourceFirstDrawDecision(RenderPassEntryDecision::BeginPass);
        startRenderPass(stateView, pendingClear, commandIndex, renderPsoHandle);
        pendingClear.reset();
        pendingClearCommandIndex = std::numeric_limits<std::size_t>::max();
      } else {
        flushPendingClear();
        const bool renderTargetChanged = hasActiveRender && activeKey != drawKey;
        const bool hazardDetected =
            hasActiveRender && !renderTargetChanged && hasExactRenderHazard();
        const bool tileResplit =
            hasActiveRender && !renderTargetChanged && !hazardDetected &&
            tileMidPassIneligible();
        if (tileResplit) {
          // R-BACK-13.6: tile path can't host this draw; fall back
          // to portable for a fresh encoder. The split is a real
          // change of pipeline kind (not a Bloom false positive),
          // so it does not violate R-BACK-2.28's no-false-positive
          // policy.
          perf::countTileFfpMidPassResplit();
          perf::countTileFfpFallbackMidPassIneligible();
        }
        const auto entryDecision = classifyRenderPassEntry(
            hasActiveRender,
            !renderTargetChanged,
            hazardDetected,
            tileResplit);
        recordSessionSourceFirstDrawDecision(entryDecision);
        if (entryDecision != RenderPassEntryDecision::ContinueActive) {
          if (entryDecision ==
              RenderPassEntryDecision::SplitRenderTargetChange) {
            // TLA+: EncoderLifecycle / RenderTargetChange(newRT)
            DXMT_ASSERT(hasActiveRender);
          }
          if (entryDecision == RenderPassEntryDecision::SplitHazard) {
            // TLA+: EncoderLifecycle / HazardDetected
            DXMT_ASSERT(hasActiveRender);
            DXMT_ASSERT(activeKey == drawKey);
          }
          const auto splitReason = renderPassEntrySplitReason(
              entryDecision, perf::EncoderSplitReason::ClearBarrier);
          flushRender(splitReason);
          // R-BACK-2.29..2.32 — per-render-pass policy commits the
          // current sub-CB at every non-Final flushRender. Encoder is
          // already ended by flushRender, so the splitMidChunk
          // invariant (no active encoder) holds. Skip when policy
          // is off so the default 1 CB/chunk behavior is preserved.
          if (commitPolicy == MidChunkCommitPolicy::PerRenderPass) {
            splitMidChunkUnderCap();
          }
          startRenderPass(stateView, std::nullopt, commandIndex, renderPsoHandle);
        } else {
          // TLA+: EncoderLifecycle / MergeRenderDraw(rt)
          DXMT_ASSERT(hasActiveRender);
          DXMT_ASSERT(activeKey == drawKey);
          DXMT_ASSERT(!activeWriteHazard.exactOverlaps(drawReadHazard));
        }
      }
    } else {
      const bool renderTargetChanged = hasActiveRender && activeKey != drawKey;
      const bool hazardDetected =
          hasActiveRender && !renderTargetChanged && hasExactRenderHazard();
      const bool tileResplit =
          hasActiveRender && !renderTargetChanged && !hazardDetected &&
          tileMidPassIneligible();
      if (tileResplit) {
        // R-BACK-13.6 — see twin call site above.
        perf::countTileFfpMidPassResplit();
        perf::countTileFfpFallbackMidPassIneligible();
      }
      const auto entryDecision = classifyRenderPassEntry(
          hasActiveRender,
          !renderTargetChanged,
          hazardDetected,
          tileResplit);
      recordSessionSourceFirstDrawDecision(entryDecision);
      if (entryDecision != RenderPassEntryDecision::ContinueActive) {
        if (entryDecision ==
            RenderPassEntryDecision::SplitRenderTargetChange) {
          // TLA+: EncoderLifecycle / RenderTargetChange(newRT)
          DXMT_ASSERT(hasActiveRender);
        }
        if (entryDecision == RenderPassEntryDecision::SplitHazard) {
          // TLA+: EncoderLifecycle / HazardDetected
          DXMT_ASSERT(hasActiveRender);
          DXMT_ASSERT(activeKey == drawKey);
        }
        const auto splitReason = renderPassEntrySplitReason(
            entryDecision, perf::EncoderSplitReason::Final);
        flushRender(splitReason);
        // R-BACK-2.29..2.32 — see twin call site above. The split
        // reason here can be Final when neither RT-change nor hazard
        // forced the flush, but per-render-pass policy still
        // commits to start a new sub-CB before the next pass opens.
        if (commitPolicy == MidChunkCommitPolicy::PerRenderPass) {
          splitMidChunkUnderCap();
        }
        startRenderPass(stateView, std::nullopt, commandIndex, renderPsoHandle);
      } else {
        // TLA+: EncoderLifecycle / MergeRenderDraw(rt)
        DXMT_ASSERT(hasActiveRender);
        DXMT_ASSERT(activeKey == drawKey);
        DXMT_ASSERT(!activeWriteHazard.exactOverlaps(drawReadHazard));
      }
    }
    if (hasActiveRender && activeKey == drawKey) {
      renderPassFrameTracker.noteDrawRead(*stateView.hot);
    }
    // Phase 3-E: bind BaseDrawState ONCE on iter 0, then issue-only
    // path on iters 1..N — the Metal render encoder retains
    // pipeline / depth / viewport / scissor / cull / texture /
    // sampler state across draw calls.
    //
    // Phase 5-B: pre-scan for UP vertex/index payloads + batch-
    // upload them all in ONE uploadTransientBufferBatch call
    // (single TransientResourceArena acquire, single completedSeqId
    // snapshot, single reclaim pass for the whole run). Per-draw
    // pre-resolved slices are handed to encodeDraw via
    // PreUploadedDrawData.
    //
    // Layout of the batch payload vector (interleaved per draw):
    //   [0]   = draw 0 vertex (empty if no UP)
    //   [1]   = draw 0 index  (empty if no UP)
    //   [2]   = draw 1 vertex
    //   [3]   = draw 1 index
    //   …
    // Returned slices use the same indexing.
    const std::size_t drawCount = drawItems.size();
    const auto recordPayloadArena = core::drawRunPayloadBytes(command);
    bool anyUpData = false;
    bool hasUpPayloadRanges = false;
    traceEncodeCommand("drawrun.before-up-prescan", commandIndex,
                       Kind::DrawRun, command);
    for (const auto& param : drawItems) {
      if (!param.userVertexRange.empty() || !param.userIndexRange.empty()) {
        hasUpPayloadRanges = true;
        break;
      }
    }
    traceEncodeCommand("drawrun.after-up-prescan", commandIndex,
                       Kind::DrawRun, command);
    std::vector<CommandQueue::TransientBufferSlice> upSlices;
    if (hasUpPayloadRanges) {
      traceEncodeCommand("drawrun.before-up-upload", commandIndex,
                         Kind::DrawRun, command);
      std::vector<std::span<const std::byte>> upPayloads;
      upPayloads.reserve(drawCount * 2);
      for (const auto& param : drawItems) {
        const auto vertexBytes = drawParamVertexBytes(param, recordPayloadArena);
        if (!vertexBytes.empty()) anyUpData = true;
        upPayloads.emplace_back(reinterpret_cast<const std::byte*>(vertexBytes.data()),
                                vertexBytes.size());
        const auto indexBytes = drawParamIndexBytes(param, recordPayloadArena);
        if (!indexBytes.empty()) anyUpData = true;
        upPayloads.emplace_back(reinterpret_cast<const std::byte*>(indexBytes.data()),
                                indexBytes.size());
      }
      if (anyUpData) {
        upSlices = ctx.queue.uploadTransientBufferBatchWithCompletedSeqId(
            upPayloads, /*alignment=*/16, slot.seqId,
            ctx.transientCompletedSeqId);
        if (!upSlices.empty()) {
          for (const auto& param : drawItems) {
            const auto vertexBytes = drawParamVertexBytes(param, recordPayloadArena);
            const auto indexBytes = drawParamIndexBytes(param, recordPayloadArena);
            activeEncoderBreakdown.addTransientVertexBytes(
                static_cast<u64>(vertexBytes.size()),
                ActiveEncoderBreakdown::TransientVertexSource::Preupload);
            activeEncoderBreakdown.addTransientIndexBytes(
                static_cast<u64>(indexBytes.size()),
                ActiveEncoderBreakdown::TransientIndexSource::Preupload);
          }
        }
      }
      traceEncodeCommand("drawrun.after-up-upload", commandIndex,
                         Kind::DrawRun, command);
    }

    // encodeDraw receives the per-draw fields through DrawParam while
    // all base state is read from the canonical hot/shader view.
    // Per-frequency UBOs (VsConsts/PsConsts/FfpVsConsts/FfpPsConsts)
    // bind only on dirty (R-BACK-12.5/12.8); DrawVolatile is pushed
    // via setVertexBytes per draw with no slab traffic.
    for (std::size_t i = 0; i < drawCount; ++i) {
      traceEncodeCommand("drawrun.draw-begin", commandIndex,
                         Kind::DrawRun, command);
      const auto& param = drawItems[i];
      const bool usesCommandUniform =
          !param.uniformHandle.valid() ||
          param.uniformHandle == command.drawRunRecord->uniformHandle;
      const auto* drawUniformPayload = usesCommandUniform
          ? commandUniformPayload
          : paramUniformPayloadCache.payloadForParam(
                command, param,
                perf::DrawUniformPayloadMaterializeSite::DrawEncoderParam);
      if (!drawUniformPayload) {
        traceEncodeCommand("drawrun.draw-skip-no-uniform", commandIndex,
                           Kind::DrawRun, command);
        continue;
      }
      PreUploadedDrawData preData{};
      if (i * 2u + 1u < upSlices.size()) {
        preData.vertex = upSlices[i * 2u];
        preData.index = upSlices[i * 2u + 1u];
      }
      core::DrawBindingOverride bindingOverride{};
      core::DrawBindingSnapshot bindingSnapshot{};
      core::FlatDrawStateRecord overrideHot{};
      core::DrawShaderLayoutContext overrideShaderLayout{};
      auto drawStateView = stateView;
      bool hasBindingOverride =
          drawParamBindingOverride(param, recordPayloadArena, bindingOverride);
      const bool hasBindingSnapshot =
          drawParamBindingSnapshot(param, recordPayloadArena, bindingSnapshot);
      if (hasBindingOverride) {
        overrideHot = hot;
        if (drawStateView.shaderLayout) {
          overrideShaderLayout = *drawStateView.shaderLayout;
          applyDrawBindingOverride(overrideHot, &overrideShaderLayout, bindingOverride);
          drawStateView.shaderLayout = &overrideShaderLayout;
        } else {
          applyDrawBindingOverride(overrideHot, nullptr, bindingOverride);
        }
        drawStateView.hot = &overrideHot;
      }
      drawStateView.uniforms = drawUniformPayload;
      const auto drawArgbufPayloadDeltaKey =
          makeArgbufPayloadDeltaKey(drawStateView);
      const u64 drawArgbufPayloadHash = drawUniformPayload->hash;
      const bool argbufPayloadChanged =
          !lastArgbufPayloadHash.has_value() ||
          *lastArgbufPayloadHash != drawArgbufPayloadHash;
      const bool activePassUsesArgbufTable =
          activePassUsesArgbufHybrid && !activePassUsesArgbufDirectCbuf;
      const bool reopenArgbuf =
          activePassUsesArgbufTable &&
          (activePassUsesArgbufResourceArray || argbufPayloadChanged);
      const bool argbufVsPayloadSourceChanged =
          lastArgbufPayloadDeltaKey.has_value() &&
          lastArgbufPayloadDeltaKey->vertexConstantsHash !=
              drawArgbufPayloadDeltaKey.vertexConstantsHash;
      const bool argbufPsPayloadSourceChanged =
          lastArgbufPayloadDeltaKey.has_value() &&
          lastArgbufPayloadDeltaKey->pixelConstantsHash !=
              drawArgbufPayloadDeltaKey.pixelConstantsHash;
      if (activePassUsesArgbufDirectCbuf) {
        if (argbufVsPayloadSourceChanged) {
          markDirectCbufVsPayloadDirty(uniformDirty, *drawUniformPayload);
        }
        if (argbufPsPayloadSourceChanged) {
          markDirectCbufPsPayloadDirty(uniformDirty, *drawUniformPayload);
        }
      }
      const bool argbufPayloadDeltaPerf =
          activePassUsesArgbufHybrid && argbufPayloadDeltaPerfEnabled();
      std::optional<ArgbufPayloadDeltaComponentKey>
          drawArgbufPayloadDeltaComponentKey;
      if (argbufPayloadDeltaPerf) {
        drawArgbufPayloadDeltaComponentKey =
            makeArgbufPayloadDeltaComponentKey(*drawUniformPayload);
        perf::countEncodeDrawArgbufPayloadDeltaProbeCalls(1u);
        const bool cbufOnlyReopen =
            reopenArgbuf && !activePassUsesArgbufResourceArray;
        if (activePassUsesArgbufResourceArray && reopenArgbuf) {
          perf::countEncodeDrawArgbufPayloadDeltaReopenResourceArray(1u);
        }
        if (cbufOnlyReopen) {
          perf::countEncodeDrawArgbufPayloadDeltaReopenCbufOnly(1u);
        }
        if (!lastArgbufPayloadDeltaKey.has_value()) {
          perf::countEncodeDrawArgbufPayloadDeltaFirst(1u);
          if (reopenArgbuf) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenFirst(1u);
          }
          if (cbufOnlyReopen) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyFirst(1u);
          }
        } else if (lastArgbufPayloadDeltaKey->hash ==
                   drawArgbufPayloadDeltaKey.hash) {
          perf::countEncodeDrawArgbufPayloadDeltaSame(1u);
          if (reopenArgbuf) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenPayloadSame(1u);
          }
        } else {
          perf::countEncodeDrawArgbufPayloadDeltaChanged(1u);
          if (reopenArgbuf) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenPayloadChanged(1u);
          }
          if (cbufOnlyReopen) {
            perf::countEncodeDrawArgbufPayloadDeltaReopenCbufOnlyPayloadChanged(
                1u);
          }
          const bool vsChanged =
              lastArgbufPayloadDeltaKey->vertexConstantsHash !=
              drawArgbufPayloadDeltaKey.vertexConstantsHash;
          const bool psChanged =
              lastArgbufPayloadDeltaKey->pixelConstantsHash !=
              drawArgbufPayloadDeltaKey.pixelConstantsHash;
          if (vsChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedVs(1u);
          }
          if (psChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedPs(1u);
          }
          if (vsChanged && psChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedVsPs(1u);
          }
          if (!vsChanged && !psChanged) {
            perf::countEncodeDrawArgbufPayloadDeltaChangedNonConstOnly(1u);
          }
          if (lastArgbufPayloadDeltaComponentKey.has_value() &&
              drawArgbufPayloadDeltaComponentKey.has_value()) {
            const auto& lastComponents =
                *lastArgbufPayloadDeltaComponentKey;
            const auto& drawComponents =
                *drawArgbufPayloadDeltaComponentKey;
            if (vsChanged) {
              if (lastComponents.vsFloatHash != drawComponents.vsFloatHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloat(1u);
                if (lastArgbufPayloadDeltaPayload.has_value()) {
                  const auto stats =
                      measureArgbufPayloadChangedPrefix(
                          lastArgbufPayloadDeltaPayload->vsConst.float4,
                          lastArgbufPayloadDeltaPayload->vertexFloatConstantCount,
                          drawUniformPayload->vsConst.float4,
                          drawUniformPayload->vertexFloatConstantCount);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegs(
                      stats.changed);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatRegsMax(
                      stats.changed);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegs(
                      stats.prefix);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatPrefixRegsMax(
                      stats.prefix);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegs(
                      stats.span);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatSpanRegsMax(
                      stats.span);
                  if (stats.fullPrefix) {
                    perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefix(
                        1u);
                    perf::countEncodeDrawArgbufPayloadDeltaChangedVsFloatFullPrefixRegs(
                        stats.changed);
                  }
                  if (argbufPayloadDeltaSourcePerf) {
                    argbufPayloadDeltaSourceAttribution.record(
                        drawStateView, stats);
                  }
                  recordArgbufPayloadChangedVsFloatRegBucket(stats.changed);
                }
              }
              if (lastComponents.vsIntHash != drawComponents.vsIntHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedVsInt(1u);
              }
              if (lastComponents.vsBoolHash != drawComponents.vsBoolHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedVsBool(1u);
              }
            }
            if (psChanged) {
              if (lastComponents.psFloatHash != drawComponents.psFloatHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloat(1u);
                if (lastArgbufPayloadDeltaPayload.has_value()) {
                  const auto stats =
                      measureArgbufPayloadChangedPrefix(
                          lastArgbufPayloadDeltaPayload->psConst.float4,
                          lastArgbufPayloadDeltaPayload->pixelFloatConstantCount,
                          drawUniformPayload->psConst.float4,
                          drawUniformPayload->pixelFloatConstantCount);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegs(
                      stats.changed);
                  perf::countEncodeDrawArgbufPayloadDeltaChangedPsFloatRegsMax(
                      stats.changed);
                  recordArgbufPayloadChangedPsFloatRegBucket(stats.changed);
                }
              }
              if (lastComponents.psIntHash != drawComponents.psIntHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedPsInt(1u);
              }
              if (lastComponents.psBoolHash != drawComponents.psBoolHash) {
                perf::countEncodeDrawArgbufPayloadDeltaChangedPsBool(1u);
              }
            }
          }
        }
      }
      const bool baseStateCompatible =
          activeDrawStateKey.has_value() &&
          activeDrawStateUsesPrefetchedPsoLayout &&
          core::drawStateKeysCompatibleForDrawRunBatch(
              *activeDrawStateKey, drawStateView.hot->key);
      const bool overrideNeedsBaseStateBind =
          hasBindingOverride &&
          drawBindingOverrideRequiresBaseStateBind(
              bindingOverride, stateView.shaderLayout);
      const bool bindingOverridePrefetchedPsoCompatible =
          hasBindingOverride && !overrideNeedsBaseStateBind;
      const bool skipBaseStateBind =
          baseStateCompatible && !overrideNeedsBaseStateBind;
      maybeCollectDrawTextureDump(activeDrawTextureDumps,
                                  ctx.pool,
                                  drawStateView,
                                  activeRenderEncoderSeq,
                                  activeRenderEncoderIndex);
      const u64 encoderDrawIndexBeforeEncode =
          activeEncoderBreakdown.stats.drawCalls;
      const u64 drawTexture0 = drawStateView.hot->textures[0]
          ? drawStateView.hot->textures[0].value
          : 0ull;
      traceEncodeCommand("drawrun.before-encode-draw", commandIndex,
                         Kind::DrawRun, command);
      if (encodeDraw(ctx, commandBuffer, activeRenderEncoder, drawStateView, slot.seqId,
                     /*skipBaseStateBind=*/skipBaseStateBind,
                     anyUpData ? &preData : nullptr,
                     &param,
                     recordPayloadArena,
                     hasBindingSnapshot ? &bindingSnapshot : nullptr,
                     &uniformDirty,
                     /*tileFfpMode=*/activePassUsesTileFfp,
                     /*argbufHybridMode=*/activePassUsesArgbufHybrid,
                     /*argbufResourceArray=*/activePassUsesArgbufResourceArray,
                     /*argbufDirectCbufMode=*/activePassUsesArgbufDirectCbuf,
                     /*reopenArgbufHybrid=*/reopenArgbuf,
                     /*argbufVsPayloadSourceChanged=*/argbufVsPayloadSourceChanged,
                     /*argbufPsPayloadSourceChanged=*/argbufPsPayloadSourceChanged,
                     /*bindingOverridePrefetchedPsoCompatible=*/bindingOverridePrefetchedPsoCompatible,
                     renderPsoHandle,
                     tilePsoHandle,
                     depthStencilHandle,
                     &textureSamplerShadow,
                     commandIndex <= std::numeric_limits<std::uint32_t>::max()
                         ? static_cast<std::uint32_t>(commandIndex)
                         : std::numeric_limits<std::uint32_t>::max(),
                     static_cast<u64>(i),
                     static_cast<u64>(drawCount),
                     &activeEncoderBreakdown,
                     &argbufCbufCache,
                     &activeStreamIbStaging,
                     activeVisibilityScout ? &*activeVisibilityScout : nullptr)) {
        traceEncodeCommand("drawrun.after-encode-draw-ok", commandIndex,
                           Kind::DrawRun, command);
        activeDrawStateKey = drawStateView.hot->key;
        activeDrawStateUsesPrefetchedPsoLayout = !overrideNeedsBaseStateBind;
        lastArgbufPayloadHash = drawArgbufPayloadHash;
        lastArgbufPayloadDeltaKey = drawArgbufPayloadDeltaKey;
        if (argbufPayloadDeltaPerf &&
            drawArgbufPayloadDeltaComponentKey.has_value()) {
          lastArgbufPayloadDeltaComponentKey =
              *drawArgbufPayloadDeltaComponentKey;
          lastArgbufPayloadDeltaPayload = *drawUniformPayload;
        }
        if (colorAttachmentDumpAfterDrawWantsSplit(
                activeColorAttachmentDump,
                drawStateView,
                encoderDrawIndexBeforeEncode,
                commandIndex)) {
          activeColorAttachmentDump.afterDraw = true;
          activeColorAttachmentDump.draw = encoderDrawIndexBeforeEncode;
          activeColorAttachmentDump.commandIndex = commandIndex;
          activeColorAttachmentDump.commandDrawIndex = i;
          activeColorAttachmentDump.commandDrawCount = drawCount;
          activeColorAttachmentDump.texture0 = drawTexture0;
          flushRender(perf::EncoderSplitReason::Final);
          if (i + 1u < drawCount) {
            startRenderPass(drawStateView, std::nullopt, commandIndex,
                            renderPsoHandle);
          }
        }
      } else {
        traceEncodeCommand("drawrun.after-encode-draw-false", commandIndex,
                           Kind::DrawRun, command);
      }
    }
    traceEncodeCommand("drawrun.end", commandIndex, Kind::DrawRun, command);
    commandBufferHasWork = true;
  };

  auto applyPerRecordSplitPolicy = [&](bool presentRecord) {
    // R-BACK-2.29..2.32 — per-N-records policy. Counts every replayed
    // record (including helper-encoder commands), and fires a mid-chunk
    // commit when the threshold is hit AND there is no active encoder.
    // The flushBlit + flushRender(non-Final) sequence enforces the
    // splitMidChunk invariant: encoder must be ended before commit.
    // Final reason is used here because we are not opening a new render
    // pass after the commit; the next iteration will start one fresh.
    //
    // R-BACK-2.30: Present records attach drawable + presentDrawable to
    // the CURRENT command buffer; a split right after Present would
    // promote the present-bearing CB out of the chain tail position and
    // violate the "present metadata on the last sub-CB only" rule.
    // Suppress the per-N-records split immediately after a Present
    // record; splitBeforeBlockingPresent owns the present-tail boundary.
    ++recordsSinceLastSplit;
    if (commitPolicy == MidChunkCommitPolicy::PerNRecords &&
        recordsSinceLastSplit >= splitNRecords &&
        !presentRecord) {
      flushBlit();
      flushRender(perf::EncoderSplitReason::Final);
      assertNoActiveEncoder();
      splitMidChunkUnderCap();
      recordsSinceLastSplit = 0;
    }
  };

  if (slot.drawOnlyCommandStream()) {
    for (std::size_t commandIndex = replayRange.commandBegin;
         commandIndex < replayRange.commandEnd; ++commandIndex) {
      const auto command = slot.drawRunCommandAt(commandIndex);
      traceEncodeCommand("begin", commandIndex, Kind::DrawRun, command);
      // TLA+: EncoderLifecycle / opCount observes command replay progress.
      encodeDrawRunCommand(commandIndex, command);
      traceEncodeCommand("after-encode", commandIndex, Kind::DrawRun, command);
      traceEncodeCommand("before-split-policy", commandIndex, Kind::DrawRun, command);
      applyPerRecordSplitPolicy(/*presentRecord=*/false);
      traceEncodeCommand("end", commandIndex, Kind::DrawRun, command);
    }
  } else {
    for (std::size_t commandIndex = replayRange.commandBegin;
         commandIndex < replayRange.commandEnd; ++commandIndex) {
      const auto command = slot.commandAt(commandIndex);
      traceEncodeCommand("begin", commandIndex, command.kind, command);
      // TLA+: EncoderLifecycle / opCount observes command replay progress.
      switch (command.kind) {
      case Kind::Clear: {
        if (!command.clear) break;
        const auto& clear = *command.clear;
        flushRender(perf::EncoderSplitReason::ClearBarrier);
        flushBlit();
        flushPendingClear();
        if (clear.rects.empty()) {
          pendingClear = clear;
          pendingClearCommandIndex = commandIndex;
        } else {
          const auto sampleAttachment = makeRenderEncoderGpuAttachment(
              core::metalqueue::RenderEncoderGpuPassType::Clear,
              commandIndex,
              clear.colorAttachments[0].handle.value,
              clear.depthStencil.handle.value);
          dxmt9::encoders::encodeClearPass(commandBuffer, ctx.pool, clear,
                                           sampleAttachment.span());
          recordRenderEncoderGpuAttachment(sampleAttachment);
          commandBufferHasWork = true;
        }
        break;
      }
      case Kind::DrawRun: {
        encodeDrawRunCommand(commandIndex, command);
        break;
      }
      case Kind::SurfaceCopy: {
        if (!command.surfaceCopy) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::SurfaceCopy);
        assertHelperEncoderPrecondition();
        // R-BACK-15.5: destination handle's contents are overwritten;
        // the next render pass on it qualifies as first-use again.
        ctx.queue.invalidateColorHandle(command.surfaceCopy->destination);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::SurfaceCopy,
            commandIndex,
            command.surfaceCopy->destination.value,
            0);
        dxmt9::encoders::encodeSurfaceCopy(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                           ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                           *command.surfaceCopy,
                                           sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::StretchRect: {
        if (!command.stretchRect) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::StretchRect);
        assertHelperEncoderPrecondition();
        // R-BACK-15.5
        ctx.queue.invalidateColorHandle(command.stretchRect->destination);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::StretchRect,
            commandIndex,
            command.stretchRect->destination.value,
            0);
        dxmt9::encoders::encodeStretchRect(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                            ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                            *command.stretchRect,
                                            sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::Readback: {
        if (!command.readback) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::Readback);
        assertHelperEncoderPrecondition();
        // R-BACK-15.5: destination receives content; source is unaffected
        ctx.queue.invalidateColorHandle(command.readback->destination);
        dxmt9::encoders::encodeReadback(commandBuffer, ctx.pool, *command.readback);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::DepthResolve: {
        if (!command.depthResolve) break;
        flushPendingClear();
        // RESZ depth resolve is the DEPTH twin of the color StretchRect
        // resolve — reuse its split-reason bucket rather than expand the
        // perf-counter table for a rarely-hit op.
        flushRender(perf::EncoderSplitReason::StretchRect);
        assertHelperEncoderPrecondition();
        // R-FORMAT-11 — RESZ MSAA depth resolve. The DEPTH twin of the color
        // resolve already wired in encodeStretchRect/encodeColorFill: open a
        // depth-only render pass with store=MultisampleResolve and end it. The
        // INTZ destination's contents are overwritten, so it qualifies as
        // first-use again (R-BACK-15.5).
        ctx.queue.invalidateColorHandle(command.depthResolve->intzDest);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::DepthResolve,
            commandIndex,
            0,
            command.depthResolve->intzDest.value);
        dxmt9::encoders::encodeDepthResolve(commandBuffer, ctx.pool,
                                            command.depthResolve->msaaDepth,
                                            command.depthResolve->intzDest,
                                            sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::ColorFill: {
        if (!command.colorFill) break;
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::ColorFill);
        // TLA+: EncoderLifecycle / BeginRender(rt)
        // ColorFill owns a short-lived helper render encoder and ends it before returning.
        assertNoActiveEncoder();
        // R-BACK-15.5
        ctx.queue.invalidateColorHandle(command.colorFill->destination);
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::ColorFill,
            commandIndex,
            command.colorFill->destination.value,
            0);
        dxmt9::encoders::encodeColorFill(commandBuffer, ctx.pool, ctx.cache, ctx.device,
                                          ctx.limits, ctx.shaderArchive, ctx.shaderArchivePath,
                                          *command.colorFill,
                                          sampleAttachment.span());
        recordRenderEncoderGpuAttachment(sampleAttachment);
        assertNoActiveEncoder();
        commandBufferHasWork = true;
        break;
      }
      case Kind::Present: {
        if (!command.present) break;
        const auto& present = command.present->present;
        const auto presentSource = command.present->presentSource;
        if (!metalCaptureRequest.has_value()) {
          // Bump the controller's frame counter and, if this is the
          // target frame's Present chunk, recover the chunk-begin session
          // request so `record.metalCapture` triggers stopCapture at
          // commit time. For non-target frames the call is a no-op apart
          // from the counter bump.
          metalCaptureRequest = ctx.queue.notePresentChunkForCapture(slot.seqId);
          if (metalCaptureRequest.has_value()) {
            // Capture was started at an earlier chunk-begin; this
            // chunk's commit should only call stopCapture, never
            // re-start.
            captureAlreadyStartedAtChunkBegin = true;
          }
        }
        flushPendingClear();
        flushRender(perf::EncoderSplitReason::Present);
        flushBlit();
        splitBeforeBlockingPresent();
        // Resolve the queue-local Presenter binding once per Present
        // packet and reclaim any acquire-before-present token stashed by
        // submitPresent. A stale PresentId (swapchain destroyed since
        // submission) produces a nullptr Presenter — encodePresent then
        // short-circuits to a skipped present.
        dxmt9::Presenter* const presenter = ctx.queue.lookupPresenter(present.presentId);
        auto pendingDrawableToken = ctx.queue.takeDrawableToken(present.presentId);
        const bool noteAfterAcquire = presentBoundaryAfterAcquireEnabled();
        if (!noteAfterAcquire) {
          ctx.queue.notePresentDequeued(slot.seqId);
        }
        const auto sampleAttachment = makeRenderEncoderGpuAttachment(
            core::metalqueue::RenderEncoderGpuPassType::Present,
            commandIndex,
            presentSource.value,
            0);
        const bool presentEncoded = dxmt9::encodePresent(commandBuffer, ctx.pool,
                                                          presenter,
                                                          std::move(pendingDrawableToken),
                                                          present, presentSource, slot.seqId,
                                                          sampleAttachment.span());
        if (presentEncoded) {
          recordRenderEncoderGpuAttachment(sampleAttachment);
        }
        if (noteAfterAcquire) {
          ctx.queue.notePresentDequeued(slot.seqId);
        }
        if (presentEncoded) {
          commandBufferHasWork = true;
          ctx.queue.backBufferDiscardAfterPresent_ = true;
          if (presenter) {
            postCommitCallbacks.push_back([presenter, seqId = slot.seqId] {
              presenter->preAcquireNextDrawable(seqId);
            });
          }
        }
        // Per-frame snapshot mode (DXMT9_PERF_FRAME_SAMPLING=1). Fires
        // exactly once per Present packet on the encode thread, so it
        // does not make Present synchronous from the app side.
        // Default off → just one bool check, no atomic loads.
        if (perf::frameSamplingEnabled()) {
          static thread_local perf::CounterSnapshot prevSnapshot{};
          static thread_local std::uint64_t frameId = 0;
          perf::CounterSnapshot curr = perf::snapshot();
          perf::emitFrameDelta(frameId++, prevSnapshot, curr);
          prevSnapshot = curr;
        }
        renderPassFrameTracker.reset();
        // M3 — Instruments "frame" interval. End the frame that just
        // got a Present commit, then immediately begin the next one so
        // any encode work on this thread before the next Present is
        // attributed to that frame. Single encode thread → EXCLUSIVE
        // is the correct id. Always-on (no_op when no consumer).
        {
          os_log_t signpostLog = dxmt9::signposts::log();
          static thread_local bool frameSignpostActive = false;
          static thread_local std::uint64_t frameSignpostSeq = 0;
          if (frameSignpostActive) {
            os_signpost_interval_end(signpostLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                     "frame", "seq=%llu",
                                     static_cast<unsigned long long>(frameSignpostSeq));
          }
          ++frameSignpostSeq;
          os_signpost_interval_begin(signpostLog, OS_SIGNPOST_ID_EXCLUSIVE,
                                     "frame", "seq=%llu",
                                     static_cast<unsigned long long>(frameSignpostSeq));
          frameSignpostActive = true;
        }
        break;
      }
      }
      traceEncodeCommand("after-encode", commandIndex, command.kind, command);
      traceEncodeCommand("before-split-policy", commandIndex, command.kind, command);
      applyPerRecordSplitPolicy(command.kind == Kind::Present);
      traceEncodeCommand("end", commandIndex, command.kind, command);
    }
  }

  if (options.session && options.sessionSource.has_value()) {
    const bool appended =
        appendEncodeChunkSessionSource(*options.session,
                                       *options.sessionSource);
    DXMT_ASSERT(appended);
    if (!appended) {
      return std::nullopt;
    }
  }

  if (deferSessionFinalization) {
    perf::countEncodeSessionCarryDeferredChunk(
        static_cast<bool>(activeRenderEncoder),
        static_cast<bool>(activeBlitEncoder),
        pendingClear.has_value());
  } else {
    if (options.session) {
      perf::countEncodeSessionCarryFinalChunk();
    }
    finalizeEncodeChunkSessionForReturn();
  }

  traceEncodeStage("before-record-chunk-sub-cb-count");
  // R-BACK-2.29..2.32 — fold this encode call's sub-CB segment length into
  // chunkSubCBCountMax. A carried EncodeSession enforces the cap across
  // source boundaries with session.committedSubCommandBuffers, but the record
  // still publishes only the segment length added by this call. Queue-side
  // merge then subtracts the shared tail CB once when joining segments.
  if (commandBufferHasWork || perChunkSubCBCount > 0) {
    perf::recordChunkSubCBCount(perChunkSubCBCount + 1);
  }
  traceEncodeStage("after-record-chunk-sub-cb-count");

  const u64 seqId = slot.seqId;
  if (argbufPayloadDeltaSourcePerf) {
    traceEncodeStage("before-argbuf-payload-delta-source-emit");
    argbufPayloadDeltaSourceAttribution.emit(seqId);
    traceEncodeStage("after-argbuf-payload-delta-source-emit");
  }
  traceEncodeStage("before-build-submission-record");
  core::metalqueue::QueueSubmissionRecord record;
  record.commandBuffer = std::move(commandBuffer);
  record.commandBufferChainLength = perChunkSubCBCount + 1;
  if (!deferSessionFinalization && metalCaptureRequest.has_value()) {
    record.metalCaptureDevice = WMT::Device{ctx.device.handle};
    record.metalCapture = std::move(metalCaptureRequest);
    record.metalCaptureAlreadyStarted = captureAlreadyStartedAtChunkBegin;
  }
  record.slotIndex = slotIndex;
  record.seqId = seqId;
  record.context = "queue";
  if (!deferSessionFinalization) {
    record.renderEncoderGpuSampleBuffer =
        std::move(renderEncoderGpuSampleBuffer);
    record.renderEncoderGpuSamples = std::move(renderEncoderGpuSamples);
    record.postCommitCallbacks = std::move(postCommitCallbacks);
    record.completionCallbacks = std::move(completionCallbacks);
  }
  traceEncodeStage("after-build-submission-record");
  if (!deferSessionFinalization && options.session) {
    traceEncodeStage("before-publish-session-sources");
    const bool sourcesPublished =
        publishEncodeChunkSessionSources(*options.session, record);
    traceEncodeStage(sourcesPublished ? "after-publish-session-sources-ok"
                                      : "after-publish-session-sources-failed");
    DXMT_ASSERT(sourcesPublished);
    if (!sourcesPublished) {
      return std::nullopt;
    }
    traceEncodeStage("session-owner-retained-by-caller");
  }
  traceEncodeStage("return-record");
  return record;
  }  // @autoreleasepool
}

}  // namespace dxmt9::encoders
