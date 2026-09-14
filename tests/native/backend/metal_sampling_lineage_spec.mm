#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../../src/winemetal/winemetal.h"
#include "../../../src/winemetal/unix/winemetal_sampling_lineage.hpp"

#include <array>
#include <bit>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#define CHECK(value) do { if (!(value)) { \
  std::fprintf(stderr, "sampling lineage check failed at %d: %s\n", __LINE__, #value); \
  std::abort(); } } while (false)

template <typename T>
void issue(id<MTLRenderCommandEncoder> encoder, const T& command) {
  MTLRenderCommandEncoder_encodeCommands((obj_handle_t)encoder,
      reinterpret_cast<const wmtcmd_base*>(&command));
}

NSDictionary* snapshot(id<MTLRenderCommandEncoder> encoder) {
  const auto json = dxmt9::diagnostics::metalSamplingBindingsJson((obj_handle_t)encoder);
  if (!dxmt9::diagnostics::metalSamplingLineageEnabled()) { CHECK(json.empty()); return nil; }
  CHECK(!json.empty());
  auto* bytes = [NSData dataWithBytes:json.data() length:json.size()];
  id value = [NSJSONSerialization JSONObjectWithData:bytes options:0 error:nil];
  CHECK([value isKindOfClass:[NSDictionary class]]);
  return value;
}

int main() {
  @autoreleasepool {
    const bool observed = dxmt9::diagnostics::metalSamplingLineageEnabled();
    CHECK(dxmt9::diagnostics::metalTextureObservationId(0u) == 0u);
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device);
    auto* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:8 height:8 mipmapped:YES];
    td.storageMode = MTLStorageModeShared;
    td.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    id<MTLTexture> texture = [device newTextureWithDescriptor:td];
    std::array<unsigned char, 8 * 8 * 4> red{}, green{};
    for (unsigned i = 0; i < 64u; ++i) {
      red[i * 4] = red[i * 4 + 3] = 255;
      green[i * 4 + 1] = green[i * 4 + 3] = 255;
    }
    [texture replaceRegion:MTLRegionMake2D(0, 0, 8, 8) mipmapLevel:0
        withBytes:red.data() bytesPerRow:32];
    [texture replaceRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:1
        withBytes:green.data() bytesPerRow:16];
    id<MTLTexture> view = [texture newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm
        textureType:MTLTextureType2D levels:NSMakeRange(1, 1) slices:NSMakeRange(0, 1)];
    CHECK(view);
    if (!observed) {
      CHECK(dxmt9::diagnostics::metalTextureObservationId((obj_handle_t)texture) == 0u);
      CHECK(dxmt9::diagnostics::metalTextureObservationId((obj_handle_t)view) == 0u);
    }
    if (observed) {
      const auto rootToken = dxmt9::diagnostics::metalTextureObservationId((obj_handle_t)texture);
      const auto viewToken = dxmt9::diagnostics::metalTextureObservationId((obj_handle_t)view);
      CHECK(rootToken && viewToken && rootToken != viewToken);
      CHECK(viewToken == dxmt9::diagnostics::metalTextureObservationId((obj_handle_t)view));
      id<MTLTexture> distinct = [device newTextureWithDescriptor:td];
      CHECK(distinct && rootToken != dxmt9::diagnostics::metalTextureObservationId((obj_handle_t)distinct));
      [distinct release];
    }
    WMTSamplerInfo info{};
    info.normalized_coords = true;
    info.support_argument_buffers = true;
    info.max_anisotroy = 1u;
    info.lod_max_clamp = 8.0f;
    id<MTLSamplerState> sampler = (id<MTLSamplerState>)MTLDevice_newSamplerState(
        (obj_handle_t)device, &info);
    CHECK(sampler);
    auto* rawDesc = [[MTLSamplerDescriptor alloc] init];
    rawDesc.minFilter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> unknownSampler = [device newSamplerStateWithDescriptor:rawDesc];
    [rawDesc release];
    WMTArgumentDescriptor descriptors[]{
      {WMTArgumentTypeTexture, 4u, 1u, 0u, (uint32_t)MTLTextureType2D, 0u},
      {WMTArgumentTypeSampler, 12u, 1u, 0u, 0u, 0u},
    };
    obj_handle_t argument = MTLDevice_newArgumentEncoder((obj_handle_t)device, descriptors, 2u);
    const auto length = MTLArgumentEncoder_encodedLength(argument);
    CHECK(argument && length > 0 && length <= 4096u);
    id<MTLBuffer> table = [device newBufferWithLength:65536 options:MTLResourceStorageModeShared];
    std::memset(table.contents, 0, table.length);
    const auto bindTable = [&](id<MTLRenderCommandEncoder> encoder, unsigned offset) {
      wmtcmd_render_setbuffer command{};
      command.type = WMTRenderCommandSetFragmentBuffer;
      command.buffer = (obj_handle_t)table;
      command.offset = offset;
      command.index = 30u;
      issue(encoder, command);
    };
    const auto setTexture = [&](id<MTLRenderCommandEncoder> encoder, id<MTLTexture> value) {
      wmtcmd_render_settexture command{};
      command.type = WMTRenderCommandSetFragmentTexture;
      command.texture = (obj_handle_t)value;
      issue(encoder, command);
    };
    const auto setSampler = [&](id<MTLRenderCommandEncoder> encoder, id<MTLSamplerState> value) {
      wmtcmd_render_setsamplerstate command{};
      command.type = WMTRenderCommandSetFragmentSamplerState;
      command.sampler_state = (obj_handle_t)value;
      issue(encoder, command);
    };
    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V {float4 p [[position]];};\n"
        "vertex V vs(uint i [[vertex_id]]) {float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "return {float4(p[i],0,1)};}\n"
        "fragment float4 direct_fs(V v [[stage_in]], texture2d<float> t [[texture(0)]],"
        "sampler s [[sampler(0)]]) {return t.sample(s,float2(.5));}\n"
        "struct A {texture2d<float> t [[id(4)]];sampler s [[id(12)]];};\n"
        "fragment float4 table_fs(V v [[stage_in]], constant A& a [[buffer(30)]])"
        "{return a.t.sample(a.s,float2(.5));}\n";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    CHECK(library && !error);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (unsigned route = 0; route < 2; ++route) {
      auto* outputDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
          width:4 height:4 mipmapped:NO];
      outputDesc.storageMode = MTLStorageModeShared;
      outputDesc.usage = MTLTextureUsageRenderTarget;
      id<MTLTexture> output = [device newTextureWithDescriptor:outputDesc];
      auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = output;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
      if (observed) CHECK([snapshot(encoder)[@"direct"] count] == 0u);
      setTexture(encoder, texture);
      if (observed) CHECK([snapshot(encoder)[@"direct"][@"texture_0"][@"width"] unsignedIntValue] == 8u);
      setTexture(encoder, nil);
      if (observed) CHECK([snapshot(encoder)[@"direct"][@"texture_0"][@"status"] isEqual:@"explicit-null"]);
      if (observed) CHECK(snapshot(encoder)[@"direct"][@"texture_0"][@"object_token"] == nil);
      setTexture(encoder, view);
      setSampler(encoder, unknownSampler);
      if (observed) CHECK([snapshot(encoder)[@"direct"][@"sampler_0"][@"status"] isEqual:@"unknown-creation"]);
      setSampler(encoder, sampler);
      if (observed) {
        NSDictionary* direct = snapshot(encoder)[@"direct"];
        CHECK([direct[@"texture_0"][@"width"] unsignedIntValue] == 4u);
        CHECK([direct[@"texture_0"][@"parent_level"] unsignedIntValue] == 1u);
        CHECK([direct[@"texture_0"][@"object_token"] unsignedLongLongValue] ==
              dxmt9::diagnostics::metalTextureObservationId((obj_handle_t)view));
        CHECK([direct[@"sampler_0"][@"lod_max_bits"] unsignedIntValue] == std::bit_cast<uint32_t>(8.0f));
      }
      bindTable(encoder, 256u);
      MTLArgumentEncoder_setArgumentBuffer(argument, (obj_handle_t)table, 256u);
      MTLArgumentEncoder_setTexture(argument, (obj_handle_t)view, 4u);
      MTLArgumentEncoder_setSamplerState(argument, (obj_handle_t)sampler, 12u);
      if (observed) {
        auto* value = snapshot(encoder);
        CHECK([value[@"argument_table_known"] boolValue]);
        CHECK([value[@"argument_table"][@"entry_4"][@"value"] isEqual:value[@"direct"][@"texture_0"]]);
        CHECK([value[@"argument_table"][@"entry_12"][@"value"] isEqual:value[@"direct"][@"sampler_0"]]);
      }
      // An unobserved table mutation must not be blessed by a later sampler
      // write. The actual texture entry may have changed without our witness.
      static_cast<unsigned char*>(table.contents)[256] ^= 1u;
      if (observed) CHECK(![snapshot(encoder)[@"argument_table_known"] boolValue]);
      MTLArgumentEncoder_setSamplerState(argument, (obj_handle_t)sampler, 12u);
      if (observed) CHECK(snapshot(encoder)[@"argument_table"][@"entry_4"] == nil);
      MTLArgumentEncoder_setTexture(argument, (obj_handle_t)view, 4u);
      // Nonzero slice identity, offset-only change, setBytes invalidation.
      wmtcmd_render_setbufferoffset moved{};
      moved.type = WMTRenderCommandSetFragmentBufferOffset;
      moved.offset = 512u;
      moved.index = 30u;
      issue(encoder, moved);
      if (observed) CHECK(![snapshot(encoder)[@"argument_table_known"] boolValue]);
      bindTable(encoder, 256u);
      uint64_t zero = 0;
      wmtcmd_render_setbytes bytes{};
      bytes.type = WMTRenderCommandSetFragmentBytes;
      bytes.bytes.ptr = &zero;
      bytes.length = sizeof(zero);
      bytes.index = 30u;
      issue(encoder, bytes);
      if (observed) CHECK(![snapshot(encoder)[@"argument_table_known"] boolValue]);
      bindTable(encoder, 256u);
      MTLArgumentEncoder_setArgumentBuffer(argument, (obj_handle_t)table, 256u);
      if (observed) CHECK(![snapshot(encoder)[@"argument_table_known"] boolValue]);
      MTLArgumentEncoder_setTexture(argument, (obj_handle_t)view, 4u);
      MTLArgumentEncoder_setSamplerState(argument, (obj_handle_t)sampler, 12u);
      // More than the fixed 64-slice cache must evict to unknown, not retain
      // stale source-address witnesses. Rebuild the actual draw's slice after.
      for (unsigned i = 2u; i <= 66u; ++i) {
        MTLArgumentEncoder_setArgumentBuffer(argument, (obj_handle_t)table, i * 256u);
        MTLArgumentEncoder_setTexture(argument, (obj_handle_t)view, 4u);
      }
      if (observed) CHECK(![snapshot(encoder)[@"argument_table_known"] boolValue]);
      MTLArgumentEncoder_setArgumentBuffer(argument, (obj_handle_t)table, 256u);
      MTLArgumentEncoder_setTexture(argument, (obj_handle_t)view, 4u);
      MTLArgumentEncoder_setSamplerState(argument, (obj_handle_t)sampler, 12u);
      auto* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
      pipelineDesc.vertexFunction = [library newFunctionWithName:@"vs"];
      pipelineDesc.fragmentFunction = [library newFunctionWithName:route ? @"table_fs" : @"direct_fs"];
      pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
      id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
      CHECK(pso && !error);
      [encoder setRenderPipelineState:pso];
      [encoder useResource:view usage:MTLResourceUsageRead stages:MTLRenderStageFragment];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      [encoder endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error);
      std::array<unsigned char, 64> pixels{};
      [output getBytes:pixels.data() bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
      CHECK(std::memcmp(pixels.data(), green.data(), pixels.size()) == 0);
      [pipelineDesc.vertexFunction release];
      [pipelineDesc.fragmentFunction release];
      [pipelineDesc release];
      [pso release];
      [output release];
    }
    [queue release];
    [library release];
    [(id)argument release];
    [table release];
    [unknownSampler release];
    [sampler release];
    [view release];
    [texture release];
    [device release];
    std::puts("actual sampling lineage and both pixel routes passed");
  }
}
