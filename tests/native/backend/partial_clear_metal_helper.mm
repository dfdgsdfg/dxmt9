// Objective-C++ readback helper for packed depth/stencil textures.  Metal's
// blit options expose the two aspects independently, which is the only
// portable way to validate a Depth32Float_Stencil8 backing selected for a
// D3D9 D24S8 surface on Apple Silicon.

#import <Metal/Metal.h>

#include "../../../src/winemetal/winemetal.h"

#include <cstdint>
#include <cstring>

extern "C" bool dxmt9_read_depth_stencil(obj_handle_t textureHandle,
                                         std::uint32_t width,
                                         std::uint32_t height,
                                         float* depthOut,
                                         std::uint8_t* stencilOut) {
  if (textureHandle == 0 || width == 0 || height == 0 || !depthOut || !stencilOut) {
    return false;
  }
  @autoreleasepool {
  id<MTLTexture> texture = (__bridge id<MTLTexture>)(void*)textureHandle;
  // The fixture deliberately selects this backing for D24S8 in its limits.
  // Never reinterpret a packed depth24 blit as IEEE float32.
  if (texture.pixelFormat != MTLPixelFormatDepth32Float_Stencil8) return false;
  id<MTLDevice> device = texture.device;
  id<MTLCommandQueue> queue = [device newCommandQueue];
  if (!queue) return false;
  const NSUInteger depthBytes = static_cast<NSUInteger>(width) * height * sizeof(float);
  const NSUInteger stencilBytes = static_cast<NSUInteger>(width) * height;
  id<MTLBuffer> depthBuffer = [device newBufferWithLength:depthBytes
                                                  options:MTLResourceStorageModeShared];
  id<MTLBuffer> stencilBuffer = [device newBufferWithLength:stencilBytes
                                                    options:MTLResourceStorageModeShared];
  if (!depthBuffer || !stencilBuffer) return false;
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (!blit) return false;
  MTLSize size = MTLSizeMake(width, height, 1);
  [blit copyFromTexture:texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOrigin{0, 0, 0}
             sourceSize:size
               toBuffer:depthBuffer
      destinationOffset:0
 destinationBytesPerRow:static_cast<NSUInteger>(width) * sizeof(float)
destinationBytesPerImage:depthBytes
                options:MTLBlitOptionDepthFromDepthStencil];
  [blit copyFromTexture:texture
            sourceSlice:0
            sourceLevel:0
           sourceOrigin:MTLOrigin{0, 0, 0}
             sourceSize:size
               toBuffer:stencilBuffer
      destinationOffset:0
 destinationBytesPerRow:static_cast<NSUInteger>(width)
destinationBytesPerImage:stencilBytes
                options:MTLBlitOptionStencilFromDepthStencil];
  [blit endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  if (commandBuffer.status != MTLCommandBufferStatusCompleted) return false;
  std::memcpy(depthOut, depthBuffer.contents, depthBytes);
  std::memcpy(stencilOut, stencilBuffer.contents, stencilBytes);
  return true;
  }
}
