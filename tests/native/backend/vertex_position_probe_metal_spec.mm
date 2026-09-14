#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <CommonCrypto/CommonDigest.h>

#include "../../../src/winemetal/winemetal.h"
#include "../../../src/winemetal/unix/winemetal_vertex_position_probe.hpp"
#include "../../../src/winemetal/unix/winemetal_vertex_skinning_probe_policy.hpp"
#include "../../../src/winemetal/unix/winemetal_vertex_position_probe_policy.hpp"
#include "vertex_skinning_probe_fixture.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(value) do { if (!(value)) { \
  std::fprintf(stderr, "vertex probe Metal failed at %d: %s\n", __LINE__, #value); \
  std::abort(); } } while (false)

static std::string sha256(const char* source) {
  unsigned char digest[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(source, static_cast<CC_LONG>(std::strlen(source)), digest);
  char text[65];
  for (unsigned i = 0; i < 32; ++i) std::snprintf(text + i * 2, 3, "%02x", digest[i]);
  return text;
}

int main(int argc, char** argv) {
  using namespace dxmt9::diagnostics;
  const std::string mode = argc > 1 ? argv[1] : "enabled";
  const bool noContract = mode == "no-contract";
  const bool skinning = mode == "skinning" || noContract;
  const bool enabled = mode == "enabled" || skinning;
  char temp[] = "/tmp/dxmt9-vertex-position-spec-XXXXXX";
  CHECK(mkdtemp(temp));
  std::fprintf(stderr, "vertex probe fixture artifacts: %s\n", temp);
  const std::string output = std::string(temp) + "/probe";
  const std::string shaders = std::string(temp) + "/shaders";
  std::string sourceStorage = "#include <metal_stdlib>\nusing namespace metal;\n"
      "struct VSOut { float4 position [[position]]; };\n"
      "vertex VSOut dxmt9_vs(uint vid [[vertex_id]]) {\n"
      "  float2 p[3] = {float2(-1,-1),float2(3,-1),float2(-1,3)};\n"
      "  VSOut out; out.position = float4(p[vid],0.5,1);\n"
      "  return out;\n}\n";
  if (skinning) sourceStorage = vertexSkinningProbeFixture();
  const char* source = sourceStorage.c_str();
  const std::string compiledSource = noContract ? *vertexNoContractControlSource(source) : sourceStorage;
  if (noContract) setenv("DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256",
      (std::string(64, '0') + "," + sha256(source)).c_str(), 1);
  else unsetenv("DXMT9_PROBE_NO_CONTRACT_SOURCE_SHA256");
  setenv("DXMT9_PROBE_VERTEX_POSITION_DETAIL", skinning ? "skinning-inputs" : "position", 1);
  setenv("DXMT9_EFFECT_DRAW_TRACE", "1", 1);
  setenv("DXMT9_EFFECT_DRAW_TRACE_BINDINGS", "1", 1);
  setenv("DXMT9_EFFECT_DRAW_TRACE_GEOMETRY", "1", 1);
  setenv("DXMT_DUMP_SHADER_DIR", shaders.c_str(), 1);
  setenv("DXMT9_PROBE_VERTEX_POSITION_SOURCE_SHA256", sha256(compiledSource.c_str()).c_str(), 1);
  setenv("DXMT9_PROBE_VERTEX_POSITION_MAX_DRAWS", "1", 1);
  setenv("DXMT9_PROBE_VERTEX_POSITION_MAX_VERTICES", "4", 1);
  setenv("DXMT9_PROBE_VERTEX_POSITION_INDEX_COUNT", mode == "invalid" ? "0" : "6", 1);
  if (mode != "disabled") setenv("DXMT9_PROBE_VERTEX_POSITION_DIR", output.c_str(), 1);
  else unsetenv("DXMT9_PROBE_VERTEX_POSITION_DIR");
  CHECK(vertexPositionProbeEnabled() == enabled);
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device);
    obj_handle_t error = 0;
    id<MTLLibrary> vsLib = CFBridgingRelease((CFTypeRef)MTLDevice_newLibraryFromSource(
        (obj_handle_t)(__bridge void*)device, source, &error));
    CHECK(vsLib && !error);
    id<MTLLibrary> fsLib = CFBridgingRelease((CFTypeRef)MTLDevice_newLibraryFromSource(
        (obj_handle_t)(__bridge void*)device,
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "fragment float4 fs(constant float4& color [[buffer(0)]]) { return color; }", &error));
    CHECK(fsLib && !error);
    id<MTLFunction> vs = CFBridgingRelease((CFTypeRef)MTLLibrary_newFunction((obj_handle_t)(__bridge void*)vsLib, "dxmt9_vs"));
    id<MTLFunction> fs = CFBridgingRelease((CFTypeRef)MTLLibrary_newFunction((obj_handle_t)(__bridge void*)fsLib, "fs"));
    WMTRenderPipelineInfo info{};
    info.vertex_function = (obj_handle_t)(__bridge void*)vs;
    info.fragment_function = (obj_handle_t)(__bridge void*)fs;
    info.colors[0].pixel_format = WMTPixelFormatRGBA8Unorm;
    info.colors[0].write_mask = MTLColorWriteMaskAll;
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    info.max_tessellation_factor = 1;
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    id<MTLRenderPipelineState> pso = CFBridgingRelease((CFTypeRef)MTLDevice_newRenderPipelineState(
        (obj_handle_t)(__bridge void*)device, &info, &error));
    CHECK(pso && !error);
    const auto prepared = prepareVertexPositionProbePipeline((obj_handle_t)(__bridge void*)pso);
    CHECK((prepared.handle != 0) == enabled);
    if (enabled) {
      CHECK(prepared.handle != (obj_handle_t)(__bridge void*)pso);
      CHECK(std::strcmp(prepared.original.vertexSha256, sha256(compiledSource.c_str()).c_str()) == 0);
      CHECK(std::strcmp(prepared.original.vertexSha256, prepared.instrumented.vertexSha256) != 0);
      CHECK(prepareVertexPositionProbePipeline((obj_handle_t)(__bridge void*)pso).handle == prepared.handle);
    }
    std::array<std::uint16_t, 6> indices{0, 1, 2, 0, 1, 2};
    id<MTLBuffer> ib = [device newBufferWithBytes:indices.data() length:sizeof(indices)
                                       options:MTLResourceStorageModeShared];
    auto* descriptor = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:8 height:4 mipmapped:NO];
    descriptor.storageMode = MTLStorageModeShared;
    descriptor.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> cb = [queue commandBuffer];
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = texture;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pso];
    const float positions[3][4] = {{-1,-1,0.5,1}, {3,-1,0.5,1}, {-1,3,0.5,1}};
    if (skinning) {
      std::array<float, 1024> constants{};
      for (unsigned row = 0; row < 3; ++row) {
        constants[row * 4 + row] = 1;
        constants[(row + 3) * 4 + row] = 1;
      }
      for (unsigned row = 0; row < 4; ++row) constants[(192 + row) * 4 + row] = 1;
      id<MTLBuffer> uniform = [device newBufferWithBytes:constants.data() length:sizeof(constants)
                                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> vertices = [device newBufferWithBytes:positions length:sizeof(positions)
                                               options:MTLResourceStorageModeShared];
      const float halfPixel[2] = {0, 0};
      [encoder setVertexBuffer:uniform offset:0 atIndex:0];
      [encoder setVertexBuffer:vertices offset:0 atIndex:1];
      [encoder setVertexBytes:halfPixel length:sizeof(halfPixel) atIndex:3];
    }
    std::array<float, 4> red{1, 0, 0, 1}, green{0, 1, 0, 1};
    [encoder setFragmentBytes:red.data() length:sizeof(red) atIndex:0];
    VertexPositionProbeDraw draw;
    draw.commandBuffer = (obj_handle_t)(__bridge void*)cb;
    draw.encoder = (obj_handle_t)(__bridge void*)encoder;
    draw.pipeline = (obj_handle_t)(__bridge void*)pso;
    draw.indexBuffer = (obj_handle_t)(__bridge void*)ib;
    draw.seq = 17; draw.ordinal = 23;
    draw.indexCount = indices.size(); draw.indexBytes = 2;
    draw.instances = 1; draw.sourceKnown = true; draw.triangleList = true;
    struct Issue {
      __unsafe_unretained id<MTLRenderCommandEncoder> encoder;
      __unsafe_unretained id<MTLBuffer> ib;
      unsigned count = 0;
      std::uint64_t pipeline = 0;
    } issued{encoder, ib};
    const auto callback = [](void* user, std::uint64_t actual) noexcept {
      auto& issue = *static_cast<Issue*>(user);
      ++issue.count; issue.pipeline = actual;
      [issue.encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6
          indexType:MTLIndexTypeUInt16 indexBuffer:issue.ib indexBufferOffset:0];
    };
    [encoder setScissorRect:MTLScissorRect{0, 0, 4, 4}];
    // Every preflight rejection must still issue exactly once, without consuming
    // the single allowed attempt or publishing a successful observer artifact.
    for (unsigned rejection = 0; rejection < 7; ++rejection) {
      auto rejected = draw;
      if (rejection == 0) rejected.sourceKnown = false;
      if (rejection == 1) rejected.triangleList = false;
      if (rejection == 2) rejected.instances = 2;
      if (rejection == 3) rejected.baseVertex = -1;
      if (rejection == 4) rejected.baseInstance = 1;
      if (rejection == 5) rejected.indexOffset = sizeof(indices);
      if (rejection == 6) rejected.commandBuffer = 0;
      const auto before = issued.count;
      withVertexPositionProbe(rejected, callback, &issued);
      CHECK(issued.count == before + 1 && issued.pipeline == draw.pipeline);
    }
    const auto before = issued.count;
    withVertexPositionProbe(draw, callback, &issued);
    CHECK(issued.count == before + 1);
    CHECK(issued.pipeline == (enabled ? prepared.handle : draw.pipeline));
    NSArray* preComplete = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:
        [NSString stringWithUTF8String:output.c_str()] error:nil];
    CHECK(preComplete.count == 0);
    // Budget rejection and a subsequent unwrapped draw both use the restored
    // original pipeline. A stale probe PSO would require its cleared buffer29.
    withVertexPositionProbe(draw, callback, &issued);
    CHECK(issued.count == before + 2 && issued.pipeline == draw.pipeline);
    [encoder setScissorRect:MTLScissorRect{4, 0, 4, 4}];
    [encoder setFragmentBytes:green.data() length:sizeof(green) atIndex:0];
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle indexCount:6 indexType:MTLIndexTypeUInt16
                      indexBuffer:ib indexBufferOffset:0];
    [encoder endEncoding];
    [cb commit];
    [cb waitUntilCompleted];
    CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error);
    std::array<unsigned char, 128> pixels{};
    [texture getBytes:pixels.data() bytesPerRow:32 fromRegion:MTLRegionMake2D(0, 0, 8, 4) mipmapLevel:0];
    for (unsigned y = 0; y < 4; ++y) for (unsigned x = 0; x < 8; ++x) {
      const auto* p = pixels.data() + (y * 8 + x) * 4;
      CHECK(p[0] == (x < 4 ? 255 : 0) && p[1] == (x < 4 ? 0 : 255) && p[2] == 0 && p[3] == 255);
    }
    NSString* directory = [NSString stringWithUTF8String:output.c_str()];
    NSArray<NSString*>* files = [[NSFileManager defaultManager] contentsOfDirectoryAtPath:directory error:nil];
    CHECK(files.count == (enabled ? 2u : 0u));
    if (enabled) {
      for (NSString* name in files) {
        NSData* data = [NSData dataWithContentsOfFile:[directory stringByAppendingPathComponent:name]];
        CHECK(data);
        if ([name hasSuffix:@".json"]) {
          NSDictionary* meta = [NSJSONSerialization JSONObjectWithData:data options:0 error:nil];
          CHECK([meta[@"gpuCompleted"] boolValue] && [meta[@"diagnosticOnly"] boolValue]);
          CHECK([meta[@"expectedIndexSetMatched"] boolValue]);
          CHECK([meta[@"observedVertices"] unsignedIntValue] == 3);
          CHECK([meta[@"conflictingVertices"] unsignedIntValue] == 0);
          CHECK([meta[@"vertexInvocations"] unsignedIntValue] >= 3);
          CHECK([meta[@"originalVertexSha256"] isEqualToString:[NSString stringWithUTF8String:sha256(compiledSource.c_str()).c_str()]]);
        } else {
          const auto valueWords = vertexProbeValueWords(skinning);
          const auto recordWords = vertexProbeRecordWords(skinning);
          CHECK(data.length == 4 * recordWords * sizeof(std::uint32_t));
          std::array<std::uint32_t, 4 * 185> words{};
          std::memcpy(words.data(), data.bytes, data.length);
          for (unsigned vertex = 0; vertex < 3; ++vertex) {
            CHECK(words[vertex * recordWords + valueWords * 2] != 0);
            for (unsigned c = 0; c < 4; ++c) {
              CHECK(words[vertex * recordWords + c] == std::bit_cast<std::uint32_t>(positions[vertex][c]));
            }
            for (unsigned c = 0; c < valueWords; ++c)
              CHECK(words[vertex * recordWords + c + valueWords] == words[vertex * recordWords + c]);
            if (skinning) {
              const auto checkFloat = [&](unsigned field, unsigned c, float value) {
                CHECK(words[vertex * recordWords + field * 4 + c] == std::bit_cast<std::uint32_t>(value));
              };
              for (unsigned c = 0; c < 4; ++c) {
                for (unsigned field : {1u, 18u, 22u}) checkFloat(field, c, positions[vertex][c]);
                checkFloat(2, c, c == 0 ? 0.25f : c == 1 ? 0.5f : c == 3 ? 1.0f : 0.0f);
                checkFloat(3, c, c == 1 ? 1.0f : 0.0f);
                checkFloat(4, c, c == 0 ? 0.25f : c == 1 ? 0.75f : c == 3 ? 1.0f : 0.0f);
                CHECK(words[vertex * recordWords + 5 * 4 + c] == (c == 1 ? 3u : 0u));
                checkFloat(16, c, 0.0f);
                for (unsigned row = 0; row < 3; ++row) {
                  checkFloat(6 + row * 2, c, c == row ? 1.0f : 0.0f);
                  checkFloat(7 + row * 2, c, c == row ? 1.0f : 0.0f);
                  checkFloat(19 + row, c, c == row ? 1.0f : 0.0f);
                }
                for (unsigned row = 0; row < 4; ++row) checkFloat(12 + row, c, c == row ? 1.0f : 0.0f);
              }
              const unsigned address[4] = {vertex * 32, 32, vertex, vertex};
              for (unsigned c = 0; c < 4; ++c) CHECK(words[vertex * recordWords + 17 * 4 + c] == address[c]);
            }
          }
          CHECK(words[4 * recordWords - 1] == 0);
        }
      }
    }
  }
  std::puts("actual WMT pipeline vertex probe, once-only issue, restore, completion and pixels passed");
}
