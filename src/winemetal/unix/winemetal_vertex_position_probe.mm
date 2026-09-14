#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>

#include "winemetal_vertex_position_probe.hpp"
#include "winemetal_vertex_position_probe_policy.hpp"
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>
#include <unistd.h>

namespace dxmt9::diagnostics {
namespace {
std::string_view setting(const char* name) noexcept {
  const char* value = std::getenv(name);
  return value ? std::string_view(value) : std::string_view{};
}

NSString* digest(NSData* data) {
  unsigned char bytes[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(data.bytes, static_cast<CC_LONG>(data.length), bytes);
  char text[65];
  constexpr char hex[] = "0123456789abcdef";
  for (unsigned i = 0; i < 32; ++i) {
    text[2 * i] = hex[bytes[i] >> 4];
    text[2 * i + 1] = hex[bytes[i] & 15];
  }
  text[64] = 0;
  return [NSString stringWithUTF8String:text];
}
}  // namespace

const VertexPositionProbeConfig& vertexPositionProbeConfig() noexcept {
  static const VertexPositionProbeConfig config = [] {
    VertexPositionProbeConfig result;
    if (setting("DXMT9_PROBE_VERTEX_POSITION_DIR").empty()) return result;
    try {
      const auto draws = vertexProbeBound(setting("DXMT9_PROBE_VERTEX_POSITION_MAX_DRAWS"), 32);
      const auto vertices = vertexProbeBound(setting("DXMT9_PROBE_VERTEX_POSITION_MAX_VERTICES"), 8192);
      const auto count = vertexProbeBound(setting("DXMT9_PROBE_VERTEX_POSITION_INDEX_COUNT"), 3 * 8192);
      const auto source = setting("DXMT9_PROBE_VERTEX_POSITION_SOURCE_SHA256");
      const auto detail = setting("DXMT9_PROBE_VERTEX_POSITION_DETAIL");
      const bool skinning = detail == "skinning-inputs";
      const bool trace = setting("DXMT9_EFFECT_DRAW_TRACE") == "1" &&
          setting("DXMT9_EFFECT_DRAW_TRACE_BINDINGS") == "1" &&
          setting("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY") == "1" &&
          !setting("DXMT_DUMP_SHADER_DIR").empty();
      if (!draws || !vertices || !count || *count % 3 != 0 ||
          !vertexProbeDigestValid(source) || !trace ||
          (!detail.empty() && detail != "position" && !skinning) ||
          (skinning && (*draws > 16 || *vertices > 2048))) {
        std::fprintf(stderr, "[dxmt9-vertex-position] invalid bounded configuration; disabled\n");
        return result;
      }
      result.directory = setting("DXMT9_PROBE_VERTEX_POSITION_DIR");
      result.sourceDigest = source;
      result.maxDraws = *draws;
      result.maxVertices = *vertices;
      result.indexCount = *count;
      result.skinningInputs = skinning;
      result.enabled = true;
    } catch (...) {
      std::fprintf(stderr, "[dxmt9-vertex-position] configuration allocation failed; disabled\n");
    }
    return result;
  }();
  return config;
}

bool vertexPositionProbeEnabled() noexcept {
  static const bool enabled = vertexPositionProbeConfig().enabled;
  return enabled;
}

void withVertexPositionProbe(const VertexPositionProbeDraw& draw,
                            VertexPositionProbeIssue issue, void* userdata) noexcept {
  if (!issue) std::abort();
  bool effects = false;
  try {
    @autoreleasepool {
      @try {
        const auto& config = vertexPositionProbeConfig();
        MetalPipelineLineage original;
        const bool candidate = config.enabled && draw.commandBuffer && draw.encoder && draw.pipeline &&
            draw.indexBuffer && (draw.indexBytes == 2 || draw.indexBytes == 4) &&
            vertexProbeDrawEligible(draw.indexCount, config.indexCount, draw.instances,
                draw.baseVertex, draw.baseInstance, 0, config.maxVertices, draw.sourceKnown,
                draw.triangleList) &&
            copyMetalPipelineLineage(draw.pipeline, original) &&
            config.sourceDigest == original.vertexSha256;
        if (!candidate) { issue(userdata, draw.pipeline); return; }
        id<MTLBuffer> indexBuffer = (id<MTLBuffer>)draw.indexBuffer;
        const std::uint64_t indexSize = draw.indexCount * draw.indexBytes;
        if (indexBuffer.storageMode != MTLStorageModeShared || !indexBuffer.contents ||
            draw.indexOffset > indexBuffer.length || indexSize > indexBuffer.length - draw.indexOffset) {
          issue(userdata, draw.pipeline); return;
        }
        std::vector<std::uint32_t> expected(config.maxVertices, 0);
        const auto* data = static_cast<const unsigned char*>(indexBuffer.contents) + draw.indexOffset;
        for (std::uint64_t i = 0; i < draw.indexCount; ++i) {
          std::uint32_t index = 0;
          if (draw.indexBytes == 2) { std::uint16_t value; std::memcpy(&value, data + i * 2, 2); index = value; }
          else std::memcpy(&index, data + i * 4, 4);
          if (index >= config.maxVertices) { issue(userdata, draw.pipeline); return; }
          ++expected[index];
        }
        static std::atomic<std::uint32_t> attempts{0};
        auto attempt = attempts.load(std::memory_order_relaxed);
        do {
          if (attempt >= config.maxDraws) { issue(userdata, draw.pipeline); return; }
        } while (!attempts.compare_exchange_weak(attempt, attempt + 1, std::memory_order_relaxed));
        const auto pipeline = prepareVertexPositionProbePipeline(draw.pipeline);
        if (!pipeline.handle) {
          std::fprintf(stderr, "[dxmt9-vertex-position] seq=%llu ordinal=%llu pipeline-preflight-rejected\n",
                       (unsigned long long)draw.seq, (unsigned long long)draw.ordinal);
          issue(userdata, draw.pipeline); return;
        }
        id<MTLCommandBuffer> commandBuffer = (id<MTLCommandBuffer>)draw.commandBuffer;
        id<MTLRenderCommandEncoder> encoder = (id<MTLRenderCommandEncoder>)draw.encoder;
        const auto valueWords = vertexProbeValueWords(config.skinningInputs);
        const auto recordWords = vertexProbeRecordWords(config.skinningInputs);
        std::vector<std::uint32_t> initial(config.maxVertices * recordWords, 0);
        for (std::uint32_t i = 0; i < config.maxVertices; ++i)
          std::fill_n(initial.begin() + i * recordWords, valueWords, UINT32_MAX);
        id<MTLBuffer> output = [[commandBuffer.device newBufferWithBytes:initial.data()
            length:initial.size() * sizeof(std::uint32_t) options:MTLResourceStorageModeShared] autorelease];
        if (!output) { issue(userdata, draw.pipeline); return; }
        output.label = @"Bounded actual-draw clip-position atomic observation";
        NSString* directory = [NSString stringWithUTF8String:config.directory.c_str()];
        NSString* stem = [NSString stringWithFormat:@"position-p%d-s%llu-d%llu-a%u", getpid(),
            (unsigned long long)draw.seq, (unsigned long long)draw.ordinal, attempt];
        NSString* rawPath = [directory stringByAppendingPathComponent:[stem stringByAppendingString:@".bin"]];
        NSString* metaPath = [directory stringByAppendingPathComponent:[stem stringByAppendingString:@".json"]];
        NSError* error = nil;
        if (![[NSFileManager defaultManager] createDirectoryAtPath:directory withIntermediateDirectories:YES
                attributes:nil error:&error] ||
            [[NSFileManager defaultManager] fileExistsAtPath:rawPath] ||
            [[NSFileManager defaultManager] fileExistsAtPath:metaPath]) {
          issue(userdata, draw.pipeline); return;
        }
        NSMutableArray* fields = [NSMutableArray array];
        if (config.skinningInputs) {
          for (auto field : vertexSkinningProbeFields)
            [fields addObject:[NSString stringWithUTF8String:std::string(field).c_str()]];
        } else [fields addObject:@"position"];
        NSDictionary* identity = @{
          @"schema":config.skinningInputs ? @"dxmt9.gpu_vertex_input_probe.v1" : @"dxmt9.gpu_vertex_position_probe.v1",
          @"diagnosticOnly":@YES, @"valueWords":@(valueWords), @"recordWords":@(recordWords),
          @"fields":fields, @"detail":config.skinningInputs ? @"skinning-inputs" : @"position",
          @"shaderPerturbed":@YES, @"seq":@(draw.seq), @"ordinal":@(draw.ordinal),
          @"captureSourceOwner":@(draw.captureOwner), @"captureSourceOrdinal":@(draw.sourceOrdinal),
          @"captureRawOrdinal":@(draw.rawOrdinal), @"captureSourceGeneration":@(draw.sourceGeneration),
          @"captureStorageGeneration":@(draw.storageGeneration), @"capacity":@(config.maxVertices),
          @"identityScope":draw.captureOwner ? @"frozen-capture-source" : @"replay-local-seq",
          @"indexCount":@(draw.indexCount), @"indexBytes":@(draw.indexBytes),
          @"originalPsoSha256":[NSString stringWithUTF8String:pipeline.original.receiptSha256],
          @"instrumentedPsoSha256":[NSString stringWithUTF8String:pipeline.instrumented.receiptSha256],
          @"originalVertexSha256":[NSString stringWithUTF8String:pipeline.original.vertexSha256],
          @"instrumentedVertexSha256":[NSString stringWithUTF8String:pipeline.instrumented.vertexSha256],
          @"layout":@"capacity records of uint32-le minBits[valueWords],maxBits[valueWords],invocations; fields contain four words each",
          @"scope":@"Actual selected draw with instrumented PSO; not passive original GPU-read proof"};
        // The block owns the buffer/identity until this exact CB completes.
        // No subsequent CPU write, queue-side retirement, or global cache owns it.
        effects = true;
        [commandBuffer addCompletedHandler:^(id<MTLCommandBuffer> completed) {
          try {
            @autoreleasepool {
              @try {
                NSMutableDictionary* metadata = [[identity mutableCopy] autorelease];
                const bool success = completed.status == MTLCommandBufferStatusCompleted && !completed.error;
                metadata[@"gpuCompleted"] = @(success);
                metadata[@"byteCount"] = @(output.length);
                if (success) {
                  NSData* bytes = [NSData dataWithBytes:output.contents length:output.length];
                  const auto* words = static_cast<const std::uint32_t*>(output.contents);
                  std::uint64_t invocations = 0, observed = 0, conflicts = 0;
                  bool coverage = true;
                  for (std::size_t i = 0; i < expected.size(); ++i) {
                    const auto count = words[i * recordWords + valueWords * 2];
                    invocations += count;
                    observed += count != 0;
                    coverage &= (count != 0) == (expected[i] != 0);
                    if (count != 0 && !std::equal(words + i * recordWords,
                        words + i * recordWords + valueWords, words + i * recordWords + valueWords)) ++conflicts;
                  }
                  metadata[@"observedVertices"] = @(observed);
                  metadata[@"vertexInvocations"] = @(invocations);
                  metadata[@"conflictingVertices"] = @(conflicts);
                  metadata[@"expectedIndexSetMatched"] = @(coverage && observed != 0);
                  metadata[@"sha256"] = digest(bytes);
                  NSError* writeError = nil;
                  if (![bytes writeToFile:rawPath options:NSDataWritingAtomic error:&writeError]) {
                    std::fprintf(stderr, "[dxmt9-vertex-position] raw-write-failed\n");
                    return;
                  }
                }
                NSData* json = [NSJSONSerialization dataWithJSONObject:metadata options:0 error:nil];
                if (!json || ![json writeToFile:metaPath options:NSDataWritingAtomic error:nil])
                  std::fprintf(stderr, "[dxmt9-vertex-position] metadata-write-failed\n");
              } @catch (NSException*) {
                std::fprintf(stderr, "[dxmt9-vertex-position] completion-observation-failed\n");
              }
            }
          } catch (...) { std::fprintf(stderr, "[dxmt9-vertex-position] completion-allocation-failed\n"); }
        }];
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)pipeline.handle];
        [encoder setVertexBuffer:output offset:0 atIndex:29];
        issue(userdata, pipeline.handle);
        [encoder setRenderPipelineState:(id<MTLRenderPipelineState>)draw.pipeline];
        [encoder setVertexBuffer:nil offset:0 atIndex:29];
        return;
      } @catch (NSException*) {
        std::fprintf(stderr, "[dxmt9-vertex-position] Metal exception; %s\n", effects ? "fail-stop" : "preflight fallback");
      }
    }
  } catch (...) {
    std::fprintf(stderr, "[dxmt9-vertex-position] allocation exception; %s\n", effects ? "fail-stop" : "preflight fallback");
  }
  if (effects) std::abort();
  issue(userdata, draw.pipeline);
}

}  // namespace dxmt9::diagnostics
