#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../../src/winemetal/winemetal.h"
#include "../../../src/winemetal/unix/winemetal_shader_compile_profile.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#define CHECK(value) do { if (!(value)) { \
  std::fprintf(stderr, "shader compile profile failed at %d: %s\n", __LINE__, #value); \
  std::abort(); } } while (false)

static void checkVertexPositionProfile(id<MTLDevice> device) {
  // These exact operands distinguish separate rounding (+0) from fused
  // evaluation (-2^-46). Amplify only the position result to a one-unit shift.
  constexpr std::array<uint32_t, 3> bits{0x3f800001u, 0x3f7ffffeu, 0xbf800000u};
  id<MTLBuffer> input = [device newBufferWithBytes:bits.data() length:sizeof(bits)
      options:MTLResourceStorageModeShared];
  input.label = @"Vertex invariance cancellation operands";
  for (const bool explicitFma : {false, true}) {
    std::string source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V { float4 position [[position, invariant]]; };\n"
        "vertex V vs(uint id [[vertex_id]], device const float* input [[buffer(0)]]) {\n"
        "float2 p[6]={float2(-1,-1),float2(1,-1),float2(-1,1),"
        "float2(-1,1),float2(1,-1),float2(1,1)};\n"
        "V out; out.position=float4(p[id],0,1);\n"
        "out.position.x += ";
    source += explicitFma ? "fma(input[0],input[1],input[2])" :
        "(input[0]*input[1]+input[2])";
    source += " * 70368744177664.0f; return out; }\n"
        "fragment float4 fs() { return float4(1); }\n";
    obj_handle_t error = 0;
    id<MTLLibrary> library = CFBridgingRelease((CFTypeRef)MTLDevice_newLibraryFromSource(
        (obj_handle_t)(__bridge void*)device, source.c_str(), &error));
    CHECK(library && !error);
    id<MTLFunction> vertex = CFBridgingRelease((CFTypeRef)MTLLibrary_newFunction(
        (obj_handle_t)(__bridge void*)library, "vs"));
    id<MTLFunction> fragment = CFBridgingRelease((CFTypeRef)MTLLibrary_newFunction(
        (obj_handle_t)(__bridge void*)library, "fs"));
    WMTRenderPipelineInfo info{};
    info.vertex_function = (obj_handle_t)(__bridge void*)vertex;
    info.fragment_function = (obj_handle_t)(__bridge void*)fragment;
    info.colors[0].pixel_format = WMTPixelFormatRGBA8Unorm;
    info.colors[0].write_mask = MTLColorWriteMaskAll;
    info.rasterization_enabled = true;
    info.raster_sample_count = 1;
    info.max_tessellation_factor = 1;
    info.input_primitive_topology = WMTPrimitiveTopologyClassTriangle;
    id<MTLRenderPipelineState> pipeline = CFBridgingRelease((CFTypeRef)MTLDevice_newRenderPipelineState(
        (obj_handle_t)(__bridge void*)device, &info, &error));
    CHECK(pipeline && !error);
    auto* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:8 height:8 mipmapped:NO];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> target = [device newTextureWithDescriptor:td];
    target.label = @"Position arithmetic full-image oracle";
    auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = target;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,1);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLCommandBuffer> command = [queue commandBuffer];
    command.label = explicitFma ? @"Explicit FMA shifted negative control" : @"Invariant position reference";
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pipeline];
    [encoder setVertexBuffer:input offset:0 atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted && !command.error);
    std::array<uint8_t, 8*8*4> pixels{};
    [target getBytes:pixels.data() bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
    for (unsigned y = 0; y < 8; ++y) for (unsigned x = 0; x < 8; ++x) {
      const auto offset = (y*8+x)*4;
      const uint8_t expected = !explicitFma || x < 4 ? 255 : 0;
      CHECK(pixels[offset] == expected && pixels[offset+1] == expected &&
            pixels[offset+2] == expected && pixels[offset+3] == 255);
    }
  }
}

int main() {
  static_assert(dxmt9::shader::kCompileProfile.languageVersion == MTLLanguageVersion2_4);
  static_assert(dxmt9::shader::kCompileProfile.mathMode == MTLMathModeFast);
  static_assert(dxmt9::shader::kCompileProfile.mathFunctions == MTLMathFloatingPointFunctionsFast);
  @autoreleasepool {
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device);
    constexpr const char* source =
        "#include <metal_stdlib>\nusing namespace metal;\n"
        "kernel void profile(device const float* input [[buffer(0)]], device uint* output [[buffer(1)]]) {\n"
        "  output[0] = __METAL_VERSION__;\n"
        "  output[1] = as_type<uint>(input[0] * input[1] + input[2]);\n"
        "  output[2] = as_type<uint>(fma(input[0], input[1], input[2]));\n"
        "}\n";
    obj_handle_t error = 0;
    id<MTLLibrary> library = CFBridgingRelease((CFTypeRef)MTLDevice_newLibraryFromSource(
        (obj_handle_t)(__bridge void*)device, source, &error));
    CHECK(library && !error);
    id<MTLFunction> function = CFBridgingRelease((CFTypeRef)MTLLibrary_newFunction(
        (obj_handle_t)(__bridge void*)library, "profile"));
    CHECK(function);
    NSError* failure = nil;
    id<MTLComputePipelineState> pipeline = [device newComputePipelineStateWithFunction:function error:&failure];
    CHECK(pipeline && !failure);
    // Compute math remains fast; vertex-position invariance must not become
    // a global contract-off compiler policy. No constant folding from source.
    constexpr std::array<uint32_t, 3> bits{0x3f800001u, 0x3f7ffffeu, 0xbf800000u};
    id<MTLBuffer> input = [device newBufferWithBytes:bits.data() length:sizeof(bits) options:MTLResourceStorageModeShared];
    id<MTLBuffer> output = [device newBufferWithLength:12 options:MTLResourceStorageModeShared];
    CHECK(input && output);
    input.label = @"Exact cancellation operands";
    output.label = @"Production language and arithmetic witness";
    std::memset(output.contents, 0xff, 12);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    queue.label = @"Production compile profile oracle";
    id<MTLCommandBuffer> command = [queue commandBuffer];
    command.label = @"Compiler-language and separate/FMA comparison";
    id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
    encoder.label = @"One pure profile witness";
    [encoder setComputePipelineState:pipeline];
    [encoder setBuffer:input offset:0 atIndex:0];
    [encoder setBuffer:output offset:0 atIndex:1];
    [encoder dispatchThreadgroups:MTLSizeMake(1, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)];
    [encoder endEncoding]; [command commit]; [command waitUntilCompleted];
    CHECK(command.status == MTLCommandBufferStatusCompleted && !command.error);
    const auto* values = static_cast<const uint32_t*>(output.contents);
    std::printf("production MSL=%u, mad=0x%08x, fma=0x%08x\n", values[0], values[1], values[2]);
    CHECK(values[0] == 240u);
    CHECK(values[1] == 0xa8800000u);
    CHECK(values[2] == 0xa8800000u);
    checkVertexPositionProfile(device);
  }
}
