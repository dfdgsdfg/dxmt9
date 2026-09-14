#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "../../../src/winemetal/winemetal.h"
#include "../../../src/winemetal/unix/winemetal_sampling_lineage.hpp"

#include <array>
#include <cstdio>
#include <cstdlib>

#define CHECK(value) do { if (!(value)) { \
  std::fprintf(stderr, "depth lineage check failed at %d: %s\n", __LINE__, #value); \
  std::abort(); } } while (false)

template <typename T>
void issue(id<MTLRenderCommandEncoder> encoder, const T& command) {
  MTLRenderCommandEncoder_encodeCommands((obj_handle_t)encoder,
      reinterpret_cast<const wmtcmd_base*>(&command));
}

NSDictionary* snapshot(id<MTLRenderCommandEncoder> encoder) {
  const auto json = dxmt9::diagnostics::metalDepthStencilBindingsJson((obj_handle_t)encoder);
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
    CHECK(dxmt9::diagnostics::metalDepthStencilBindingsJson(0).empty());
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    CHECK(device);
    WMTDepthStencilInfo depthInfo{};
    depthInfo.depth_compare_function = WMTCompareFunctionLess;
    depthInfo.depth_write_enabled = true;
    id<MTLDepthStencilState> depth = (id<MTLDepthStencilState>)MTLDevice_newDepthStencilState(
        (obj_handle_t)device, &depthInfo);
    WMTDepthStencilInfo stencilInfo{};
    stencilInfo.depth_compare_function = WMTCompareFunctionAlways;
    for (auto* face : {&stencilInfo.front_stencil, &stencilInfo.back_stencil}) {
      face->enabled = true;
      face->stencil_compare_function = WMTCompareFunctionEqual;
      face->depth_stencil_pass_op = WMTStencilOperationKeep;
      face->depth_fail_op = WMTStencilOperationKeep;
      face->stencil_fail_op = WMTStencilOperationKeep;
    }
    stencilInfo.front_stencil.read_mask = 0x0f;
    stencilInfo.front_stencil.write_mask = 0xf0;
    stencilInfo.back_stencil.read_mask = 0xff;
    stencilInfo.back_stencil.write_mask = 0x3f;
    id<MTLDepthStencilState> stencil = (id<MTLDepthStencilState>)MTLDevice_newDepthStencilState(
        (obj_handle_t)device, &stencilInfo);
    auto* rawDesc = [[MTLDepthStencilDescriptor alloc] init];
    rawDesc.depthCompareFunction = MTLCompareFunctionNever;
    id<MTLDepthStencilState> unknown = [device newDepthStencilStateWithDescriptor:rawDesc];
    CHECK(depth && stencil && unknown);
    auto* colorDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
        width:4 height:4 mipmapped:NO];
    colorDesc.usage = MTLTextureUsageRenderTarget;
    colorDesc.storageMode = MTLStorageModeShared;
    id<MTLTexture> color = [device newTextureWithDescriptor:colorDesc];
    auto* depthDesc = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float_Stencil8
        width:4 height:4 mipmapped:NO];
    depthDesc.usage = MTLTextureUsageRenderTarget;
    depthDesc.storageMode = MTLStorageModePrivate;
    id<MTLTexture> attachment = [device newTextureWithDescriptor:depthDesc];
    CHECK(color && attachment);
    NSString* source = @"#include <metal_stdlib>\nusing namespace metal;\n"
        "struct V {float4 p [[position]];float4 c [[flat]];};\n"
        "vertex V vs(uint i [[vertex_id]], constant float4& v [[buffer(0)]]) {"
        "float2 p[3]={float2(-1,-1),float2(3,-1),float2(-1,3)};"
        "return {float4(p[i],v.x,1),float4(v.yzw,1)};}\n"
        "fragment float4 fs(V v [[stage_in]]) {return v.c;}\n";
    NSError* error = nil;
    id<MTLLibrary> library = [device newLibraryWithSource:source options:nil error:&error];
    CHECK(library && !error);
    auto* pipelineDesc = [[MTLRenderPipelineDescriptor alloc] init];
    id<MTLFunction> vs = [library newFunctionWithName:@"vs"];
    id<MTLFunction> fs = [library newFunctionWithName:@"fs"];
    pipelineDesc.vertexFunction = vs;
    pipelineDesc.fragmentFunction = fs;
    pipelineDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    pipelineDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    pipelineDesc.stencilAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
    id<MTLRenderPipelineState> pipeline = [device newRenderPipelineStateWithDescriptor:pipelineDesc error:&error];
    CHECK(pipeline && !error);
    id<MTLCommandQueue> queue = [device newCommandQueue];
    for (unsigned mode = 0; mode < 3; ++mode) {
      auto* pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = color;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
      pass.depthAttachment.texture = attachment;
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.clearDepth = 1;
      pass.stencilAttachment.texture = attachment;
      pass.stencilAttachment.loadAction = MTLLoadActionClear;
      pass.stencilAttachment.clearStencil = 5;
      id<MTLCommandBuffer> cb = [queue commandBuffer];
      id<MTLRenderCommandEncoder> encoder = [cb renderCommandEncoderWithDescriptor:pass];
      CHECK(encoder);
      if (observed) {
        auto* value = snapshot(encoder);
        CHECK([value[@"depth_stencil"][@"status"] isEqual:@"unobserved"]);
        CHECK(![value[@"stencil_reference_known"] boolValue]);
      }
      const auto bind = [&](id<MTLDepthStencilState> state, std::uint8_t reference) {
        wmtcmd_render_setdsso command{};
        command.type = WMTRenderCommandSetDSSO;
        command.dsso = (obj_handle_t)state;
        command.stencil_ref = reference;
        issue(encoder, command);
      };
      bind(unknown, 0);
      if (observed) CHECK([snapshot(encoder)[@"depth_stencil"][@"status"] isEqual:
          (mode == 0 ? @"unknown-creation" : @"ambiguous-creation")]);
      // Null is an observer defense, not a legal Metal API bind under validation.
      dxmt9::diagnostics::noteMetalDepthStencilBinding((obj_handle_t)encoder, 0);
      if (observed) CHECK([snapshot(encoder)[@"depth_stencil"][@"status"] isEqual:@"explicit-null"]);
      if (mode == 0) {
        dxmt9::diagnostics::noteMetalDepthStencilCreation((obj_handle_t)unknown, rawDesc);
        bind(unknown, 0);
        if (observed) CHECK([snapshot(encoder)[@"depth_stencil"][@"status"] isEqual:@"issued-descriptor"]);
        // Inject a conflicting creation witness, then retry the first witness:
        // neither may rehabilitate the ambiguous association.
        rawDesc.depthCompareFunction = MTLCompareFunctionAlways;
        dxmt9::diagnostics::noteMetalDepthStencilCreation((obj_handle_t)unknown, rawDesc);
        rawDesc.depthCompareFunction = MTLCompareFunctionNever;
        dxmt9::diagnostics::noteMetalDepthStencilCreation((obj_handle_t)unknown, rawDesc);
        bind(unknown, 0);
        if (observed) CHECK([snapshot(encoder)[@"depth_stencil"][@"status"] isEqual:@"ambiguous-creation"]);
      }
      bind(depth, 0);
      if (observed) {
        NSDictionary* value = snapshot(encoder)[@"depth_stencil"];
        CHECK([value[@"status"] isEqual:@"issued-descriptor"]);
        CHECK([value[@"depth_compare"] unsignedIntValue] == MTLCompareFunctionLess);
        CHECK([value[@"depth_write"] boolValue]);
      }
      if (mode) {
        bind(stencil, 6);
        if (observed) {
          NSDictionary* value = snapshot(encoder)[@"depth_stencil"];
          CHECK(![value[@"depth_write"] boolValue]);
          CHECK([value[@"depth_compare"] unsignedIntValue] == MTLCompareFunctionAlways);
          CHECK([value[@"front"][@"read_mask"] unsignedIntValue] == 0x0f);
          CHECK([value[@"front"][@"write_mask"] unsignedIntValue] == 0xf0);
          CHECK([value[@"back"][@"read_mask"] unsignedIntValue] == 0xff);
          CHECK([value[@"back"][@"write_mask"] unsignedIntValue] == 0x3f);
          for (NSString* face in @[@"front", @"back"]) {
            CHECK([value[face][@"compare"] unsignedIntValue] == MTLCompareFunctionEqual);
            for (NSString* op in @[@"fail", @"depth_fail", @"pass"])
              CHECK([value[face][op] unsignedIntValue] == MTLStencilOperationKeep);
          }
        }
      }
      if (mode == 2) {
        wmtcmd_render_setblendcolor command{};
        command.type = WMTRenderCommandSetBlendFactorAndStencilRef;
        command.stencil_ref = 5;
        issue(encoder, command);
      }
      if (observed) {
        CHECK([snapshot(encoder)[@"stencil_reference_known"] boolValue]);
        CHECK([snapshot(encoder)[@"stencil_reference"] unsignedIntValue] == (mode == 0 ? 0 : mode == 1 ? 6 : 5));
      } else { CHECK(snapshot(encoder) == nil); }
      [encoder setRenderPipelineState:pipeline];
      const auto draw = [&](std::array<float, 4> values) {
        [encoder setVertexBytes:values.data() length:sizeof(values) atIndex:0];
        [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
      };
      if (mode == 0) {
        draw({.25f, 1, 0, 0});
        draw({.75f, 0, 1, 0});  // Behind the earlier draw: must be rejected.
      } else { draw({.5f, 0, 0, 1}); }
      [encoder endEncoding];
      [cb commit];
      [cb waitUntilCompleted];
      CHECK(cb.status == MTLCommandBufferStatusCompleted && !cb.error);
      std::array<unsigned char, 64> pixels{};
      [color getBytes:pixels.data() bytesPerRow:16 fromRegion:MTLRegionMake2D(0, 0, 4, 4) mipmapLevel:0];
      for (unsigned i = 0; i < 16; ++i) {
        CHECK(pixels[4*i] == (mode == 0 ? 255 : 0));
        CHECK(pixels[4*i+1] == 0);
        CHECK(pixels[4*i+2] == (mode == 2 ? 255 : 0));
        CHECK(pixels[4*i+3] == 255);
      }
    }
    [queue release]; [pipeline release]; [pipelineDesc release];
    [vs release]; [fs release]; [library release];
    [attachment release]; [color release]; [unknown release];
    [rawDesc release]; [depth release]; [stencil release]; [device release];
    std::puts("actual depth/stencil descriptors, reference updates and three pixel cuts passed");
  }
}
