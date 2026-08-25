#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include "../winemetal.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <mutex>

namespace {

void execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
}

// R-BACK-3.9 — MTLBinaryArchive is not documented thread-safe. dxmt9's PSO
// compile-queue workers (dxmt9_pipeline_cache.cpp, 1-8 concurrent threads)
// and, since R-BACK-3.9, the async prewarm load thread and the R-BACK-3.10
// milestone-save thread can all reach MTLDevice_newRenderPipelineState /
// MTLDevice_newComputePipelineState (which call
// add{Render,Compute}PipelineFunctionsWithDescriptor:) and
// MTLBinaryArchive_serialize (serializeToURL:) concurrently on the same
// archive object. Serialize the three mutating entry points with one
// process-wide mutex; a single archive add/serialize call is not on any
// per-draw hot path, so the extra contention is negligible.
std::mutex &binary_archive_mutex() {
  static std::mutex m;
  return m;
}

typedef struct macdrv_opaque_metal_device *macdrv_metal_device;
typedef struct macdrv_opaque_metal_view *macdrv_metal_view;
typedef struct macdrv_opaque_metal_layer *macdrv_metal_layer;
typedef struct macdrv_opaque_view *macdrv_view;
typedef struct opaque_HWND *HWND;

struct macdrv_win_data {
  HWND hwnd;
  void *cocoa_window;
  macdrv_view cocoa_view;
  macdrv_view client_cocoa_view;
};

struct macdrv_functions_t {
  void (*macdrv_init_display_devices)(BOOL);
  struct macdrv_win_data *(*get_win_data)(HWND hwnd);
  void (*release_win_data)(struct macdrv_win_data *data);
  void *(*macdrv_get_cocoa_window)(HWND hwnd, BOOL require_on_screen);
  macdrv_metal_device (*macdrv_create_metal_device)(void);
  void (*macdrv_release_metal_device)(macdrv_metal_device d);
  macdrv_metal_view (*macdrv_view_create_metal_view)(macdrv_view v, macdrv_metal_device d);
  macdrv_metal_layer (*macdrv_view_get_metal_layer)(macdrv_metal_view v);
  void (*macdrv_view_release_metal_view)(macdrv_metal_view v);
  void (*on_main_thread)(dispatch_block_t b);
};

template <typename T>
T resolveMacdrvSymbol(const char *name) {
  return reinterpret_cast<T>(dlsym(RTLD_DEFAULT, name));
}

}  // namespace

// Bridge defense-in-depth (winemetal hardening): a real Objective-C object
// pointer is always at least 8-byte aligned on macOS. A handle that is
// non-null but misaligned (e.g., an integer like 0x1234 that survived a
// stale-record path) would otherwise reach `[(id)obj retain]` and trigger
// a SIGBUS / SIGSEGV cascade on the unix side. The cheap alignment check
// rejects bogus values before objc_msgSend dispatches. We log to stderr in
// verbose mode (DXMT9_BRIDGE_VERBOSE=1) so a corrupted bridge call is
// visible, but we never abort: the caller's intent was a no-op (the
// resource is presumed already gone), so silent reject is the safe path.
//
// This is NOT a generation-counter / handle-table validation — full
// stale-handle detection requires a registry refactor that is out of
// scope for this hardening commit. See gap.md / future winemetal-bridge
// expansion track for the full handle-tagging proposal.
namespace {

constexpr uintptr_t kObjcHandleAlignmentMask = 0x7;

bool gBridgeVerboseInit = false;
bool gBridgeVerbose = false;

inline bool bridgeVerbose() {
  if (!gBridgeVerboseInit) {
    const char *v = std::getenv("DXMT9_BRIDGE_VERBOSE");
    gBridgeVerbose = v && v[0] != '\0' && std::strcmp(v, "0") != 0;
    gBridgeVerboseInit = true;
  }
  return gBridgeVerbose;
}

inline bool tracePipelineBuild() {
  const char *queue = std::getenv("DXMT_TRACE_QUEUE");
  const char *pipeline = std::getenv("DXMT9_TRACE_PIPELINE_BUILD");
  return (queue && queue[0] != '\0' && std::strcmp(queue, "0") != 0) ||
         (pipeline && pipeline[0] != '\0' && std::strcmp(pipeline, "0") != 0);
}

inline bool renderPsoOnMainThread() {
  const char *env = std::getenv("DXMT9_CAPTURE_LAYER_PSO_ON_MAIN");
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

inline bool renderPsoSetFragmentFirst() {
  const char *env = std::getenv("DXMT9_CAPTURE_LAYER_FRAGMENT_FIRST");
  return env && env[0] != '\0' && std::strcmp(env, "0") != 0;
}

inline bool isPlausibleObjectHandle(obj_handle_t obj) {
  if ((obj & kObjcHandleAlignmentMask) != 0) {
    if (bridgeVerbose()) {
      fprintf(stderr,
              "winemetal: rejecting misaligned handle 0x%llx (likely stale)\n",
              static_cast<unsigned long long>(obj));
    }
    return false;
  }
  return true;
}

}  // namespace

extern "C" void NSObject_retain(obj_handle_t obj) {
  if (!obj) {
    return;
  }
  if (!isPlausibleObjectHandle(obj)) {
    return;
  }
  [(id)obj retain];
}

extern "C" void NSObject_release(obj_handle_t obj) {
  if (!obj) {
    return;
  }
  if (!isPlausibleObjectHandle(obj)) {
    return;
  }
  [(id)obj release];
}

extern "C" obj_handle_t NSArray_object(obj_handle_t array, uint64_t index) {
  if (!array) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(NSArray *)array objectAtIndex:index];
}

extern "C" uint64_t NSArray_count(obj_handle_t array) {
  if (!array) {
    return 0;
  }
  return [(NSArray *)array count];
}

extern "C" obj_handle_t WMTCopyAllDevices() {
  return (obj_handle_t)MTLCopyAllDevices();
}

extern "C" obj_handle_t MTLCaptureManager_sharedCaptureManager() {
  return (obj_handle_t)[MTLCaptureManager sharedCaptureManager];
}

extern "C" bool MTLCaptureManager_startCapture(obj_handle_t mgr, struct WMTCaptureInfo *info) {
  if (!mgr || !info) {
    std::fprintf(stderr,
                 "[dxmt9-capture] startCapture invalid mgr=0x%llx info=0x%llx\n",
                 static_cast<unsigned long long>(mgr),
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(info)));
    return false;
  }
  @autoreleasepool {
    MTLCaptureDescriptor *desc = [[MTLCaptureDescriptor alloc] init];
    desc.destination = (MTLCaptureDestination)info->destination;
    desc.captureObject = (id)info->capture_object;

    const char *path = static_cast<const char *>(info->output_url.ptr);
    NSString *pathString = path && path[0] != '\0'
        ? [[NSString alloc] initWithCString:path encoding:NSUTF8StringEncoding]
        : nil;
    if (pathString) {
      desc.outputURL = [NSURL fileURLWithPath:pathString];
    }

    NSError *error = nil;
    const bool started = [(MTLCaptureManager *)mgr startCaptureWithDescriptor:desc error:&error];
    if (!started) {
      NSString *domain = error ? [error domain] : nil;
      NSString *message = error ? [error localizedDescription] : nil;
      NSString *reason = error ? [error localizedFailureReason] : nil;
      const bool canCheckDestination =
          [(MTLCaptureManager *)mgr respondsToSelector:@selector(supportsDestination:)];
      const bool destinationSupported =
          canCheckDestination
              ? [(MTLCaptureManager *)mgr supportsDestination:desc.destination]
              : false;
      std::fprintf(
          stderr,
          "[dxmt9-capture] startCapture failed destination=%ld "
          "destination_supported=%d capture_object=0x%llx path=%s "
          "error_domain=%s error_code=%ld error=%s reason=%s\n",
          static_cast<long>(desc.destination),
          canCheckDestination ? (destinationSupported ? 1 : 0) : -1,
          static_cast<unsigned long long>(info->capture_object),
          path && path[0] != '\0' ? path : "",
          domain ? [domain UTF8String] : "",
          error ? static_cast<long>([error code]) : 0L,
          message ? [message UTF8String] : "",
          reason ? [reason UTF8String] : "");
    }
    [pathString release];
    [desc release];
    return started;
  }
}

extern "C" void MTLCaptureManager_stopCapture(obj_handle_t mgr) {
  if (!mgr) {
    return;
  }
  [(MTLCaptureManager *)mgr stopCapture];
}

extern "C" obj_handle_t MacdrvMetalDevice_create() {
  auto *macdrv_functions = resolveMacdrvSymbol<macdrv_functions_t *>("macdrv_functions");
  auto pfn_create_metal_device =
      macdrv_functions ? macdrv_functions->macdrv_create_metal_device : nullptr;
  if (!pfn_create_metal_device) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)pfn_create_metal_device();
}

extern "C" void MacdrvMetalDevice_release(obj_handle_t device) {
  auto *macdrv_functions = resolveMacdrvSymbol<macdrv_functions_t *>("macdrv_functions");
  auto pfn_release_metal_device =
      macdrv_functions ? macdrv_functions->macdrv_release_metal_device : nullptr;
  if (pfn_release_metal_device && device) {
    pfn_release_metal_device((macdrv_metal_device)device);
  }
}

extern "C" uint64_t MTLDevice_recommendedMaxWorkingSetSize(obj_handle_t device) {
  if (!device) {
    return 0;
  }
  return [(id<MTLDevice>)device recommendedMaxWorkingSetSize];
}

extern "C" uint64_t MTLDevice_currentAllocatedSize(obj_handle_t device) {
  if (!device) {
    return 0;
  }
  return [(id<MTLDevice>)device currentAllocatedSize];
}

extern "C" obj_handle_t MTLDevice_name(obj_handle_t device) {
  if (!device) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(id<MTLDevice>)device name];
}

extern "C" uint64_t MTLDevice_registryID(obj_handle_t device) {
  if (!device) {
    return 0;
  }
  return [(id<MTLDevice>)device registryID];
}

extern "C" obj_handle_t MTLDevice_newCommandQueue(obj_handle_t device, uint64_t maxCommandBufferCount) {
  if (!device) {
    return NULL_OBJECT_HANDLE;
  }
  id<MTLDevice> metal_device = (id<MTLDevice>)device;
  if (maxCommandBufferCount != 0 &&
      [metal_device respondsToSelector:@selector(newCommandQueueWithMaxCommandBufferCount:)]) {
    return (obj_handle_t)[metal_device newCommandQueueWithMaxCommandBufferCount:maxCommandBufferCount];
  }
  return (obj_handle_t)[metal_device newCommandQueue];
}

extern "C" obj_handle_t MTLCommandQueue_commandBuffer(obj_handle_t queue) {
  if (!queue) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(id<MTLCommandQueue>)queue commandBuffer];
}

extern "C" obj_handle_t MTLDevice_newSharedEvent(obj_handle_t device) {
  if (!device) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(id<MTLDevice>)device newSharedEvent];
}

extern "C" uint32_t
NSString_getCString(obj_handle_t str, char *buffer, uint64_t maxLength, enum WMTStringEncoding encoding) {
  if (!str || !buffer || maxLength == 0) {
    return 0;
  }
  return (uint32_t)[(NSString *)str getCString:buffer maxLength:maxLength encoding:(NSStringEncoding)encoding];
}

extern "C" uint64_t NSString_lengthOfBytesUsingEncoding(obj_handle_t str, enum WMTStringEncoding encoding) {
  if (!str) {
    return 0;
  }
  return [(NSString *)str lengthOfBytesUsingEncoding:(NSStringEncoding)encoding];
}

extern "C" obj_handle_t NSObject_description(obj_handle_t obj) {
  if (!obj) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(id)obj description];
}

extern "C" int64_t NSError_code(obj_handle_t err) {
  if (!err) {
    return 0;
  }
  return (int64_t)[(NSError *)err code];
}

extern "C" obj_handle_t NSError_domain(obj_handle_t err) {
  if (!err) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(NSError *)err domain];
}

extern "C" obj_handle_t NSString_string(const char *data, enum WMTStringEncoding encoding) {
  if (!data) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[NSString stringWithCString:data encoding:(NSStringEncoding)encoding];
}

extern "C" obj_handle_t NSString_alloc_init(const char *data, enum WMTStringEncoding encoding) {
  if (!data) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[[NSString alloc] initWithCString:data encoding:(NSStringEncoding)encoding];
}

extern "C" obj_handle_t NSAutoreleasePool_alloc_init() {
  return (obj_handle_t)[[NSAutoreleasePool alloc] init];
}

extern "C" obj_handle_t DeveloperHUDProperties_instance() {
  Class hud_class = objc_lookUpClass("_CADeveloperHUDProperties");
  if (!hud_class) {
    return NULL_OBJECT_HANDLE;
  }
  const auto instance_fn = reinterpret_cast<id (*)(id, SEL)>(objc_msgSend);
  return (obj_handle_t)instance_fn(reinterpret_cast<id>(hud_class), @selector(instance));
}

extern "C" bool DeveloperHUDProperties_addLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t after) {
  if (!obj || !label || !after) {
    return false;
  }
  const auto add_fn = reinterpret_cast<BOOL (*)(id, SEL, id, id)>(objc_msgSend);
  return add_fn((id)obj, @selector(addLabel:after:), (id)label, (id)after);
}

extern "C" void DeveloperHUDProperties_updateLabel(obj_handle_t obj, obj_handle_t label, obj_handle_t value) {
  if (!obj || !label || !value) {
    return;
  }
  const auto update_fn = reinterpret_cast<void (*)(id, SEL, id, id)>(objc_msgSend);
  update_fn((id)obj, @selector(updateLabel:value:), (id)label, (id)value);
}

extern "C" void DeveloperHUDProperties_remove(obj_handle_t obj, obj_handle_t label) {
  if (!obj || !label) {
    return;
  }
  const auto remove_fn = reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend);
  remove_fn((id)obj, @selector(remove:), (id)label);
}

extern "C" obj_handle_t MetalDrawable_texture(obj_handle_t drawable) {
  if (!drawable) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(id<CAMetalDrawable>)drawable texture];
}

extern "C" void MTLCommandBuffer_commit(obj_handle_t cmdbuf) {
  if (!cmdbuf) {
    return;
  }
  [(id<MTLCommandBuffer>)cmdbuf commit];
}

extern "C" void MTLCommandBuffer_presentDrawable(obj_handle_t cmdbuf, obj_handle_t drawable) {
  if (!cmdbuf || !drawable) {
    return;
  }
  [(id<MTLCommandBuffer>)cmdbuf presentDrawable:(id<CAMetalDrawable>)drawable];
}

extern "C" void MTLCommandBuffer_presentDrawableAfterMinimumDuration(obj_handle_t cmdbuf,
                                                                     obj_handle_t drawable,
                                                                     double after) {
  if (!cmdbuf || !drawable) {
    return;
  }
  [(id<MTLCommandBuffer>)cmdbuf presentDrawable:(id<CAMetalDrawable>)drawable afterMinimumDuration:after];
}

extern "C" void MTLCommandBuffer_waitUntilCompleted(obj_handle_t cmdbuf) {
  if (!cmdbuf) {
    return;
  }
  [(id<MTLCommandBuffer>)cmdbuf waitUntilCompleted];
}

extern "C" enum WMTCommandBufferStatus MTLCommandBuffer_status(obj_handle_t cmdbuf) {
  if (!cmdbuf) {
    return WMTCommandBufferStatusError;
  }
  return (enum WMTCommandBufferStatus)[(id<MTLCommandBuffer>)cmdbuf status];
}

extern "C" obj_handle_t MTLCommandBuffer_error(obj_handle_t cmdbuf) {
  if (!cmdbuf) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(id<MTLCommandBuffer>)cmdbuf error];
}

extern "C" obj_handle_t MTLCommandBuffer_logs(obj_handle_t cmdbuf) {
  if (!cmdbuf) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(id<MTLCommandBuffer>)cmdbuf logs];
}

extern "C" uint64_t MTLLogContainer_enumerate(obj_handle_t logs,
                                              uint64_t start,
                                              uint64_t buffer_size,
                                              obj_handle_t *buffer) {
  if (!logs || !buffer_size || !buffer) {
    return 0;
  }
  uint64_t count = 0;
  uint64_t read = 0;
  for (id entry in (id<MTLLogContainer>)logs) {
    if (count >= start) {
      if (count < start + buffer_size) {
        buffer[count - start] = (obj_handle_t)entry;
        ++read;
      } else {
        break;
      }
    }
    ++count;
  }
  return read;
}

extern "C" obj_handle_t MetalLayer_nextDrawable(obj_handle_t layer) {
  if (!layer) {
    return NULL_OBJECT_HANDLE;
  }
  return (obj_handle_t)[(CAMetalLayer *)layer nextDrawable];
}

extern "C" obj_handle_t MetalLayer_nextDrawableRetained(obj_handle_t layer) {
  if (!layer) {
    return NULL_OBJECT_HANDLE;
  }
  id<CAMetalDrawable> drawable = [(CAMetalLayer *)layer nextDrawable];
  return (obj_handle_t)[drawable retain];
}

extern "C" void MetalLayer_setProps(obj_handle_t layer, const struct WMTLayerProps *props) {
  if (!layer || !props) {
    return;
  }
  execute_on_main(^{
    CAMetalLayer *metal_layer = (CAMetalLayer *)layer;
    metal_layer.device = (id<MTLDevice>)props->device;
    metal_layer.opaque = props->opaque;
    metal_layer.framebufferOnly = props->framebuffer_only;
    metal_layer.contentsScale = props->contents_scale;
    metal_layer.displaySyncEnabled = props->display_sync_enabled;
    metal_layer.drawableSize = CGSizeMake(props->drawable_width, props->drawable_height);
    metal_layer.pixelFormat = (MTLPixelFormat)props->pixel_format;
  });
}

extern "C" void MetalLayer_getProps(obj_handle_t layer, struct WMTLayerProps *props) {
  if (!layer || !props) {
    return;
  }
  CAMetalLayer *metal_layer = (CAMetalLayer *)layer;
  props->device = (obj_handle_t)metal_layer.device;
  props->contents_scale = metal_layer.contentsScale;
  props->drawable_width = metal_layer.drawableSize.width;
  props->drawable_height = metal_layer.drawableSize.height;
  props->opaque = metal_layer.opaque;
  props->display_sync_enabled = metal_layer.displaySyncEnabled;
  props->framebuffer_only = metal_layer.framebufferOnly;
  props->pixel_format = (WMTPixelFormat)metal_layer.pixelFormat;
}

extern "C" void MetalLayer_setMaximumDrawableCount(obj_handle_t layer, uint32_t count) {
  if (!layer) {
    return;
  }
  execute_on_main(^{
    CAMetalLayer *metal_layer = (CAMetalLayer *)layer;
    if ([metal_layer respondsToSelector:@selector(setMaximumDrawableCount:)]) {
      metal_layer.maximumDrawableCount = count;
    }
  });
}

extern "C" obj_handle_t CreateMetalViewFromHWND(intptr_t hwnd, obj_handle_t device, obj_handle_t *layer) {
  auto *macdrv_functions = resolveMacdrvSymbol<macdrv_functions_t *>("macdrv_functions");
  auto pfn_get_win_data = macdrv_functions ? macdrv_functions->get_win_data : nullptr;
  auto pfn_release_win_data = macdrv_functions ? macdrv_functions->release_win_data : nullptr;
  auto pfn_create_metal_view =
      macdrv_functions ? macdrv_functions->macdrv_view_create_metal_view : nullptr;
  auto pfn_get_metal_layer =
      macdrv_functions ? macdrv_functions->macdrv_view_get_metal_layer : nullptr;

  if (!pfn_get_win_data || !pfn_release_win_data || !pfn_create_metal_view || !pfn_get_metal_layer) {
    if (layer) {
      *layer = NULL_OBJECT_HANDLE;
    }
    return NULL_OBJECT_HANDLE;
  }

  macdrv_win_data *win_data = pfn_get_win_data((HWND)hwnd);
  if (!win_data) {
    if (layer) {
      *layer = NULL_OBJECT_HANDLE;
    }
    return NULL_OBJECT_HANDLE;
  }

  if (!win_data->client_cocoa_view) {
    pfn_release_win_data(win_data);
    if (layer) {
      *layer = NULL_OBJECT_HANDLE;
    }
    return NULL_OBJECT_HANDLE;
  }

  macdrv_metal_view metal_view =
      pfn_create_metal_view(win_data->client_cocoa_view, (macdrv_metal_device)device);
  obj_handle_t view_handle = (obj_handle_t)metal_view;
  obj_handle_t layer_handle = metal_view ? (obj_handle_t)pfn_get_metal_layer(metal_view) : NULL_OBJECT_HANDLE;
  pfn_release_win_data(win_data);

  if (layer) {
    *layer = layer_handle;
  }
  return view_handle;
}

extern "C" obj_handle_t CreateMetalViewFromCocoaView(obj_handle_t cocoa_view, obj_handle_t device,
                                                      obj_handle_t *layer) {
  auto *macdrv_functions = resolveMacdrvSymbol<macdrv_functions_t *>("macdrv_functions");
  auto pfn_create_metal_view =
      macdrv_functions ? macdrv_functions->macdrv_view_create_metal_view : nullptr;
  auto pfn_get_metal_layer =
      macdrv_functions ? macdrv_functions->macdrv_view_get_metal_layer : nullptr;

  if (!pfn_create_metal_view || !pfn_get_metal_layer || !cocoa_view) {
    if (layer) {
      *layer = NULL_OBJECT_HANDLE;
    }
    return NULL_OBJECT_HANDLE;
  }

  macdrv_metal_view metal_view =
      pfn_create_metal_view((macdrv_view)cocoa_view, (macdrv_metal_device)device);
  obj_handle_t view_handle = (obj_handle_t)metal_view;
  obj_handle_t layer_handle = metal_view ? (obj_handle_t)pfn_get_metal_layer(metal_view) : NULL_OBJECT_HANDLE;

  if (layer) {
    *layer = layer_handle;
  }
  return view_handle;
}

extern "C" void ReleaseMetalView(obj_handle_t view) {
  auto *macdrv_functions = resolveMacdrvSymbol<macdrv_functions_t *>("macdrv_functions");
  auto pfn_release_metal_view =
      macdrv_functions ? macdrv_functions->macdrv_view_release_metal_view : nullptr;
  if (pfn_release_metal_view && view) {
    pfn_release_metal_view((macdrv_metal_view)view);
  }
}

// ---------------------------------------------------------------------------
// WMT API implementations — full Metal wrapper surface
// ---------------------------------------------------------------------------

static MTLPixelFormat to_metal_pixel_format(enum WMTPixelFormat format) {
  return (MTLPixelFormat)(format & ~WMTPixelFormatCustomSwizzle);
}

static void fill_texture_descriptor(MTLTextureDescriptor *desc, const struct WMTTextureInfo *info) {
  desc.textureType     = (MTLTextureType)info->type;
  desc.pixelFormat     = to_metal_pixel_format(info->pixel_format);
  desc.width           = info->width;
  desc.height          = info->height;
  desc.depth           = info->depth;
  desc.arrayLength     = info->array_length;
  desc.mipmapLevelCount= info->mipmap_level_count;
  desc.sampleCount     = info->sample_count;
  desc.usage           = (MTLTextureUsage)info->usage;
  desc.resourceOptions = (MTLResourceOptions)info->options;
}

// -- Device capability queries --

extern "C" bool MTLDevice_supportsDepth24Stencil8(obj_handle_t device) {
  if (!device) return false;
  id<MTLDevice> d = (id<MTLDevice>)device;
  if ([d respondsToSelector:@selector(isDepth24Stencil8PixelFormatSupported)])
    return [d isDepth24Stencil8PixelFormatSupported];
  return false;
}

extern "C" bool MTLDevice_supportsFamily(obj_handle_t device, enum WMTGPUFamily gpu_family) {
  if (!device) return false;
  return [(id<MTLDevice>)device supportsFamily:(MTLGPUFamily)gpu_family];
}

extern "C" bool MTLDevice_supportsBCTextureCompression(obj_handle_t device) {
  if (!device) return false;
  return [(id<MTLDevice>)device supportsBCTextureCompression];
}

extern "C" bool MTLDevice_supportsTextureSampleCount(obj_handle_t device, uint8_t sample_count) {
  if (!device) return false;
  return [(id<MTLDevice>)device supportsTextureSampleCount:sample_count];
}

extern "C" bool MTLDevice_hasUnifiedMemory(obj_handle_t device) {
  if (!device) return false;
  return [(id<MTLDevice>)device hasUnifiedMemory];
}

extern "C" void MTLDevice_setShouldMaximizeConcurrentCompilation(obj_handle_t device, bool value) {
  if (!device) return;
  [(id<MTLDevice>)device setShouldMaximizeConcurrentCompilation:value];
}

extern "C" uint64_t MTLDevice_minimumLinearTextureAlignmentForPixelFormat(obj_handle_t device,
                                                                           enum WMTPixelFormat format) {
  if (!device) return 0;
  return [(id<MTLDevice>)device minimumLinearTextureAlignmentForPixelFormat:to_metal_pixel_format(format)];
}

// -- Buffer creation --

extern "C" obj_handle_t MTLDevice_newBuffer(obj_handle_t device, struct WMTBufferInfo *info) {
  if (!device || !info) return NULL_OBJECT_HANDLE;
  id<MTLDevice> d = (id<MTLDevice>)device;
  MTLResourceOptions opts = (MTLResourceOptions)info->options;
  id<MTLBuffer> buf;
  if (info->memory.ptr) {
    buf = [d newBufferWithBytes:(const void *)info->memory.ptr
                        length:(NSUInteger)info->length
                       options:opts];
  } else {
    buf = [d newBufferWithLength:(NSUInteger)info->length options:opts];
    info->memory.ptr = ([buf storageMode] == MTLStorageModePrivate) ? nullptr : [buf contents];
  }
  if (!buf) return NULL_OBJECT_HANDLE;
  info->gpu_address = (uint64_t)[buf gpuAddress];
  return (obj_handle_t)buf;
}

extern "C" void MTLBuffer_didModifyRange(obj_handle_t buffer, uint64_t start, uint64_t length) {
  if (!buffer) return;
  [(id<MTLBuffer>)buffer didModifyRange:NSMakeRange((NSUInteger)start, (NSUInteger)length)];
}

extern "C" void MTLBuffer_updateContents(obj_handle_t buffer, uint64_t offset,
                                          struct WMTConstMemoryPointer data, uint64_t length) {  // NOLINT
  if (!buffer || !data.ptr) return;
  memcpy((char *)[(id<MTLBuffer>)buffer contents] + offset, (const void *)data.ptr, (size_t)length);
  if ([(id<MTLBuffer>)buffer storageMode] == MTLStorageModeManaged)
    [(id<MTLBuffer>)buffer didModifyRange:NSMakeRange((NSUInteger)offset, (NSUInteger)length)];
}

// -- Texture creation --

extern "C" obj_handle_t MTLDevice_newTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  if (!device || !info) return NULL_OBJECT_HANDLE;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);
  id<MTLTexture> tex = [(id<MTLDevice>)device newTextureWithDescriptor:desc];
  [desc release];
  if (!tex) return NULL_OBJECT_HANDLE;
  info->gpu_resource_id = [tex gpuResourceID]._impl;
  info->mach_port = 0;
  return (obj_handle_t)tex;
}

extern "C" obj_handle_t MTLDevice_newSharedTexture(obj_handle_t device, struct WMTTextureInfo *info) {
  if (!device || !info) return NULL_OBJECT_HANDLE;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);
  id<MTLTexture> tex = [(id<MTLDevice>)device newSharedTextureWithDescriptor:desc];
  [desc release];
  if (!tex) return NULL_OBJECT_HANDLE;
  info->gpu_resource_id = [tex gpuResourceID]._impl;
  info->mach_port = 0;
  return (obj_handle_t)tex;
}

extern "C" obj_handle_t MTLBuffer_newTexture(obj_handle_t buffer, struct WMTTextureInfo *info,
                                              uint64_t offset, uint64_t bytes_per_row) {
  if (!buffer || !info) return NULL_OBJECT_HANDLE;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);
  id<MTLTexture> tex = [(id<MTLBuffer>)buffer newTextureWithDescriptor:desc
                                                               offset:(NSUInteger)offset
                                                          bytesPerRow:(NSUInteger)bytes_per_row];
  [desc release];
  if (!tex) return NULL_OBJECT_HANDLE;
  info->gpu_resource_id = [tex gpuResourceID]._impl;
  info->mach_port = 0;
  return (obj_handle_t)tex;
}

extern "C" obj_handle_t MTLTexture_newTextureView(obj_handle_t texture, enum WMTPixelFormat format,
                                                   enum WMTTextureType texture_type,
                                                   uint16_t level_start, uint16_t level_count,
                                                   uint16_t slice_start, uint16_t slice_count,
                                                   struct WMTTextureSwizzleChannels swizzle,
                                                   uint64_t *out_gpu_resource_id) {
  if (!texture) return NULL_OBJECT_HANDLE;
  MTLTextureSwizzleChannels mtl_swizzle = MTLTextureSwizzleChannelsMake(
      (MTLTextureSwizzle)swizzle.r, (MTLTextureSwizzle)swizzle.g,
      (MTLTextureSwizzle)swizzle.b, (MTLTextureSwizzle)swizzle.a);
  if (format & WMTPixelFormatRGB1Swizzle)
    mtl_swizzle.alpha = MTLTextureSwizzleOne;
  else if (format & WMTPixelFormatR001Swizzle)
    mtl_swizzle = MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleZero, MTLTextureSwizzleZero, MTLTextureSwizzleOne);
  else if (format & WMTPixelFormat0R01Swizzle)
    mtl_swizzle = MTLTextureSwizzleChannelsMake(
        MTLTextureSwizzleOne, (MTLTextureSwizzle)swizzle.r, MTLTextureSwizzleOne, MTLTextureSwizzleOne);
  else if (format & WMTPixelFormatGBARSwizzle)
    mtl_swizzle = MTLTextureSwizzleChannelsMake(
        (MTLTextureSwizzle)swizzle.g, (MTLTextureSwizzle)swizzle.b,
        (MTLTextureSwizzle)swizzle.a, (MTLTextureSwizzle)swizzle.r);
  id<MTLTexture> view = [(id<MTLTexture>)texture
      newTextureViewWithPixelFormat:to_metal_pixel_format(format)
                        textureType:(MTLTextureType)texture_type
                             levels:NSMakeRange(level_start, level_count)
                             slices:NSMakeRange(slice_start, slice_count)
                            swizzle:mtl_swizzle];
  if (out_gpu_resource_id)
    *out_gpu_resource_id = view ? [view gpuResourceID]._impl : 0;
  return (obj_handle_t)view;
}

extern "C" enum WMTPixelFormat MTLTexture_pixelFormat(obj_handle_t texture) {
  if (!texture) return WMTPixelFormatInvalid;
  return (enum WMTPixelFormat)[(id<MTLTexture>)texture pixelFormat];
}

extern "C" enum WMTTextureType MTLTexture_textureType(obj_handle_t texture) {
  if (!texture) return WMTTextureType2D;
  return (enum WMTTextureType)[(id<MTLTexture>)texture textureType];
}

extern "C" uint64_t MTLTexture_width(obj_handle_t texture) {
  return texture ? (uint64_t)[(id<MTLTexture>)texture width] : 0;
}

extern "C" uint64_t MTLTexture_height(obj_handle_t texture) {
  return texture ? (uint64_t)[(id<MTLTexture>)texture height] : 0;
}

extern "C" uint64_t MTLTexture_depth(obj_handle_t texture) {
  return texture ? (uint64_t)[(id<MTLTexture>)texture depth] : 0;
}

extern "C" uint64_t MTLTexture_arrayLength(obj_handle_t texture) {
  return texture ? (uint64_t)[(id<MTLTexture>)texture arrayLength] : 0;
}

extern "C" uint64_t MTLTexture_mipmapLevelCount(obj_handle_t texture) {
  return texture ? (uint64_t)[(id<MTLTexture>)texture mipmapLevelCount] : 0;
}

extern "C" void MTLTexture_replaceRegion(obj_handle_t texture, struct WMTOrigin origin,
                                          struct WMTSize size, uint64_t level, uint64_t slice,
                                          struct WMTMemoryPointer data, uint64_t bytes_per_row,
                                          uint64_t bytes_per_image) {
  if (!texture || !data.ptr) return;
  [(id<MTLTexture>)texture replaceRegion:MTLRegionMake3D(origin.x, origin.y, origin.z,
                                                          size.width, size.height, size.depth)
                             mipmapLevel:(NSUInteger)level
                                   slice:(NSUInteger)slice
                               withBytes:(const void *)data.ptr
                             bytesPerRow:(NSUInteger)bytes_per_row
                           bytesPerImage:(NSUInteger)bytes_per_image];
}

// -- Sampler state --

extern "C" obj_handle_t MTLDevice_newSamplerState(obj_handle_t device, struct WMTSamplerInfo *info) {
  if (!device || !info) return NULL_OBJECT_HANDLE;
  MTLSamplerDescriptor *desc = [[MTLSamplerDescriptor alloc] init];
  desc.minFilter           = (MTLSamplerMinMagFilter)info->min_filter;
  desc.magFilter           = (MTLSamplerMinMagFilter)info->mag_filter;
  desc.mipFilter           = (MTLSamplerMipFilter)info->mip_filter;
  desc.rAddressMode        = (MTLSamplerAddressMode)info->r_address_mode;
  desc.sAddressMode        = (MTLSamplerAddressMode)info->s_address_mode;
  desc.tAddressMode        = (MTLSamplerAddressMode)info->t_address_mode;
  desc.borderColor         = (MTLSamplerBorderColor)info->border_color;
  desc.compareFunction     = (MTLCompareFunction)info->compare_function;
  desc.lodMinClamp         = info->lod_min_clamp;
  desc.lodMaxClamp         = info->lod_max_clamp;
  desc.maxAnisotropy       = (NSUInteger)info->max_anisotroy;
  desc.normalizedCoordinates = info->normalized_coords;
  desc.lodAverage          = info->lod_average;
  desc.supportArgumentBuffers = info->support_argument_buffers;
  id<MTLSamplerState> s = [(id<MTLDevice>)device newSamplerStateWithDescriptor:desc];
  [desc release];
  if (!s) return NULL_OBJECT_HANDLE;
  info->gpu_resource_id = info->support_argument_buffers ? [s gpuResourceID]._impl : 0;
  return (obj_handle_t)s;
}

// -- Depth-stencil state --

extern "C" obj_handle_t MTLDevice_newDepthStencilState(obj_handle_t device,
                                                        const struct WMTDepthStencilInfo *info) {
  if (!device || !info) return NULL_OBJECT_HANDLE;
  MTLDepthStencilDescriptor *desc = [[MTLDepthStencilDescriptor alloc] init];
  desc.depthCompareFunction = (MTLCompareFunction)info->depth_compare_function;
  desc.depthWriteEnabled    = info->depth_write_enabled;
  if (info->front_stencil.enabled) {
    desc.frontFaceStencil.depthStencilPassOperation = (MTLStencilOperation)info->front_stencil.depth_stencil_pass_op;
    desc.frontFaceStencil.depthFailureOperation     = (MTLStencilOperation)info->front_stencil.depth_fail_op;
    desc.frontFaceStencil.stencilFailureOperation   = (MTLStencilOperation)info->front_stencil.stencil_fail_op;
    desc.frontFaceStencil.stencilCompareFunction    = (MTLCompareFunction)info->front_stencil.stencil_compare_function;
    desc.frontFaceStencil.writeMask                 = info->front_stencil.write_mask;
    desc.frontFaceStencil.readMask                  = info->front_stencil.read_mask;
  }
  if (info->back_stencil.enabled) {
    desc.backFaceStencil.depthStencilPassOperation  = (MTLStencilOperation)info->back_stencil.depth_stencil_pass_op;
    desc.backFaceStencil.depthFailureOperation      = (MTLStencilOperation)info->back_stencil.depth_fail_op;
    desc.backFaceStencil.stencilFailureOperation    = (MTLStencilOperation)info->back_stencil.stencil_fail_op;
    desc.backFaceStencil.stencilCompareFunction     = (MTLCompareFunction)info->back_stencil.stencil_compare_function;
    desc.backFaceStencil.writeMask                  = info->back_stencil.write_mask;
    desc.backFaceStencil.readMask                   = info->back_stencil.read_mask;
  }
  id<MTLDepthStencilState> ds = [(id<MTLDevice>)device newDepthStencilStateWithDescriptor:desc];
  [desc release];
  return (obj_handle_t)ds;
}

// -- Shader libraries --

extern "C" obj_handle_t DispatchData_alloc_init(uint64_t native_ptr, uint64_t length) {
  return (obj_handle_t)dispatch_data_create((const void *)native_ptr, (size_t)length, NULL, NULL);
}

extern "C" obj_handle_t MTLDevice_newLibrary(obj_handle_t device, obj_handle_t data, obj_handle_t *err_out) {
  if (!device || !data) return NULL_OBJECT_HANDLE;
  NSError *err = nil;
  id<MTLLibrary> lib = [(id<MTLDevice>)device newLibraryWithData:(dispatch_data_t)data error:&err];
  if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
  return (obj_handle_t)lib;
}

extern "C" obj_handle_t MTLDevice_newLibraryFromSource(obj_handle_t device, const char *source,
                                                        obj_handle_t *err_out) {
  if (!device || !source) return NULL_OBJECT_HANDLE;
  NSError *err = nil;
  NSString *src = [NSString stringWithUTF8String:source];
  id<MTLLibrary> lib = [(id<MTLDevice>)device newLibraryWithSource:src options:nil error:&err];
  if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
  return (obj_handle_t)lib;
}

extern "C" obj_handle_t MTLLibrary_newFunction(obj_handle_t library, const char *name) {
  if (!library || !name) return NULL_OBJECT_HANDLE;
  NSString *ns_name = [NSString stringWithUTF8String:name];
  return (obj_handle_t)[(id<MTLLibrary>)library newFunctionWithName:ns_name];
}

extern "C" obj_handle_t MTLLibrary_newFunctionWithConstants(obj_handle_t library, const char *name,
                                                             const struct WMTFunctionConstant *constants,
                                                             uint32_t num_constants,
                                                             obj_handle_t *err_out) {
  if (!library || !name) return NULL_OBJECT_HANDLE;
  NSError *err = nil;
  NSString *ns_name = [NSString stringWithUTF8String:name];
  MTLFunctionConstantValues *values = [[MTLFunctionConstantValues alloc] init];
  for (uint32_t i = 0; i < num_constants; i++) {
    [values setConstantValue:(const void *)constants[i].data.ptr
                       type:(MTLDataType)constants[i].type
                    atIndex:constants[i].index];
  }
  id<MTLFunction> fn = [(id<MTLLibrary>)library newFunctionWithName:ns_name
                                                     constantValues:values error:&err];
  [values release];
  if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
  return (obj_handle_t)fn;
}

// -- Pipeline states --

extern "C" obj_handle_t MTLDevice_newRenderPipelineState(obj_handle_t device,
                                                          const struct WMTRenderPipelineInfo *info,
                                                          obj_handle_t *err_out) {
  if (!device || !info) return NULL_OBJECT_HANDLE;
  if (renderPsoOnMainThread() && ![NSThread isMainThread]) {
    if (tracePipelineBuild()) {
      fprintf(stderr, "[dxmt9-wmt-pso] dispatch-main begin\n");
      fflush(stderr);
    }
    __block obj_handle_t result = NULL_OBJECT_HANDLE;
    __block obj_handle_t blockErr = NULL_OBJECT_HANDLE;
    execute_on_main(^{
      result = MTLDevice_newRenderPipelineState(device, info, &blockErr);
    });
    if (err_out) *err_out = blockErr;
    if (tracePipelineBuild()) {
      fprintf(stderr, "[dxmt9-wmt-pso] dispatch-main end result=%p err=%p\n",
              (void*)result,
              (void*)blockErr);
      fflush(stderr);
    }
    return result;
  }
  @autoreleasepool {
  const bool tracePso = tracePipelineBuild();
  if (tracePso) {
    fprintf(stderr,
            "[dxmt9-wmt-pso] stage=entry main_thread=%u device=%p vs=%p fs=%p "
            "sample=%u color0=%u depth=%u stencil=%u topology=%u\n",
            [NSThread isMainThread] ? 1u : 0u,
            (void*)device,
            (void*)info->vertex_function,
            (void*)info->fragment_function,
            (unsigned)info->raster_sample_count,
            (unsigned)info->colors[0].pixel_format,
            (unsigned)info->depth_pixel_format,
            (unsigned)info->stencil_pixel_format,
            (unsigned)info->input_primitive_topology);
    fflush(stderr);
  }
  MTLRenderPipelineDescriptor *desc = [[MTLRenderPipelineDescriptor alloc] init];
  if (tracePso) {
    fprintf(stderr, "[dxmt9-wmt-pso] stage=after-desc-alloc desc=%p\n", (void*)desc);
    fflush(stderr);
  }
  for (unsigned i = 0; i < 8; i++) {
    desc.colorAttachments[i].pixelFormat             = to_metal_pixel_format(info->colors[i].pixel_format);
    desc.colorAttachments[i].blendingEnabled         = info->colors[i].blending_enabled;
    desc.colorAttachments[i].writeMask               = (MTLColorWriteMask)info->colors[i].write_mask;
    desc.colorAttachments[i].alphaBlendOperation     = (MTLBlendOperation)info->colors[i].alpha_blend_operation;
    desc.colorAttachments[i].rgbBlendOperation       = (MTLBlendOperation)info->colors[i].rgb_blend_operation;
    desc.colorAttachments[i].sourceRGBBlendFactor    = (MTLBlendFactor)info->colors[i].src_rgb_blend_factor;
    desc.colorAttachments[i].sourceAlphaBlendFactor  = (MTLBlendFactor)info->colors[i].src_alpha_blend_factor;
    desc.colorAttachments[i].destinationRGBBlendFactor   = (MTLBlendFactor)info->colors[i].dst_rgb_blend_factor;
    desc.colorAttachments[i].destinationAlphaBlendFactor = (MTLBlendFactor)info->colors[i].dst_alpha_blend_factor;
  }
  if (tracePso) {
    fprintf(stderr, "[dxmt9-wmt-pso] stage=after-colors\n");
    fflush(stderr);
  }
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_vertex_buffers   & (1u << i)) desc.vertexBuffers[i].mutability   = MTLMutabilityImmutable;
    if (info->immutable_fragment_buffers & (1u << i)) desc.fragmentBuffers[i].mutability = MTLMutabilityImmutable;
  }
  if (tracePso) {
    fprintf(stderr, "[dxmt9-wmt-pso] stage=after-mutability\n");
    fflush(stderr);
  }
  if (tracePso) {
    fprintf(stderr, "[dxmt9-wmt-pso] stage=before-depth-format\n");
    fflush(stderr);
  }
  desc.depthAttachmentPixelFormat   = to_metal_pixel_format(info->depth_pixel_format);
  if (tracePso) {
    fprintf(stderr, "[dxmt9-wmt-pso] stage=after-depth-format\n");
    fflush(stderr);
  }
  desc.stencilAttachmentPixelFormat = to_metal_pixel_format(info->stencil_pixel_format);
  if (tracePso) {
    fprintf(stderr, "[dxmt9-wmt-pso] stage=after-stencil-format\n");
    fflush(stderr);
  }
  desc.alphaToCoverageEnabled       = info->alpha_to_coverage_enabled;
  desc.rasterizationEnabled         = info->rasterization_enabled;
  desc.rasterSampleCount            = info->raster_sample_count;
  desc.inputPrimitiveTopology       = (MTLPrimitiveTopologyClass)info->input_primitive_topology;
  desc.tessellationPartitionMode    = (MTLTessellationPartitionMode)info->tessellation_partition_mode;
  desc.tessellationFactorStepFunction = (MTLTessellationFactorStepFunction)info->tessellation_factor_step;
  desc.tessellationOutputWindingOrder = (MTLWinding)info->tessellation_output_winding_order;
  desc.maxTessellationFactor        = info->max_tessellation_factor;
  if (tracePso) {
    fprintf(stderr, "[dxmt9-wmt-pso] stage=after-fixed-state\n");
    fflush(stderr);
  }
  if (renderPsoSetFragmentFirst()) {
    desc.fragmentFunction             = (id<MTLFunction>)info->fragment_function;
    if (tracePso) {
      fprintf(stderr, "[dxmt9-wmt-pso] stage=after-fragment-function-first\n");
      fflush(stderr);
    }
    desc.vertexFunction               = (id<MTLFunction>)info->vertex_function;
    if (tracePso) {
      fprintf(stderr, "[dxmt9-wmt-pso] stage=after-vertex-function-second\n");
      fflush(stderr);
    }
  } else {
    desc.vertexFunction               = (id<MTLFunction>)info->vertex_function;
    if (tracePso) {
      fprintf(stderr, "[dxmt9-wmt-pso] stage=after-vertex-function\n");
      fflush(stderr);
    }
    desc.fragmentFunction             = (id<MTLFunction>)info->fragment_function;
    if (tracePso) {
      fprintf(stderr, "[dxmt9-wmt-pso] stage=after-fragment-function\n");
      fflush(stderr);
    }
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    desc.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                             count:info->num_binary_archives_for_lookup];
  NSError *err = nil;
  MTLPipelineOption opts = info->fail_on_binary_archive_miss
                             ? MTLPipelineOptionFailOnBinaryArchiveMiss
                             : MTLPipelineOptionNone;
  if (tracePso) {
    fprintf(stderr,
            "[dxmt9-wmt-pso] before-render-pso main_thread=%u device=%p vs=%p fs=%p "
            "sample=%u color0=%u depth=%u stencil=%u write0=%u blend0=%u "
            "srcRGB0=%u dstRGB0=%u srcA0=%u dstA0=%u rgbOp0=%u alphaOp0=%u "
            "immutableV=0x%x immutableF=0x%x topology=%u maxTess=%u "
            "archive=%p lookupCount=%u failArchiveMiss=%u opts=0x%lx\n",
            [NSThread isMainThread] ? 1u : 0u,
            (void*)device,
            (void*)info->vertex_function,
            (void*)info->fragment_function,
            (unsigned)info->raster_sample_count,
            (unsigned)info->colors[0].pixel_format,
            (unsigned)info->depth_pixel_format,
            (unsigned)info->stencil_pixel_format,
            (unsigned)info->colors[0].write_mask,
            info->colors[0].blending_enabled ? 1u : 0u,
            (unsigned)info->colors[0].src_rgb_blend_factor,
            (unsigned)info->colors[0].dst_rgb_blend_factor,
            (unsigned)info->colors[0].src_alpha_blend_factor,
            (unsigned)info->colors[0].dst_alpha_blend_factor,
            (unsigned)info->colors[0].rgb_blend_operation,
            (unsigned)info->colors[0].alpha_blend_operation,
            info->immutable_vertex_buffers,
            info->immutable_fragment_buffers,
            (unsigned)info->input_primitive_topology,
            (unsigned)info->max_tessellation_factor,
            (void*)info->binary_archive_for_serialization,
            (unsigned)info->num_binary_archives_for_lookup,
            info->fail_on_binary_archive_miss ? 1u : 0u,
            (unsigned long)opts);
    fflush(stderr);
  }
  id<MTLRenderPipelineState> pso = [(id<MTLDevice>)device
      newRenderPipelineStateWithDescriptor:desc options:opts reflection:nil error:&err];
  if (tracePso) {
    fprintf(stderr,
            "[dxmt9-wmt-pso] after-render-pso pso=%p err=%s\n",
            (void*)pso,
            err ? err.localizedDescription.UTF8String : "none");
    fflush(stderr);
  }
  if (!err && info->binary_archive_for_serialization) {
    NSError *archErr = nil;
    std::lock_guard<std::mutex> archive_lock(binary_archive_mutex());
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
        addRenderPipelineFunctionsWithDescriptor:desc error:&archErr];
  }
  [desc release];
  if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
  return (obj_handle_t)pso;
  }
}

extern "C" obj_handle_t MTLDevice_newComputePipelineState(obj_handle_t device,
                                                           const struct WMTComputePipelineInfo *info,
                                                           obj_handle_t *err_out) {
  if (!device || !info) return NULL_OBJECT_HANDLE;
  MTLComputePipelineDescriptor *desc = [[MTLComputePipelineDescriptor alloc] init];
  desc.computeFunction = (id<MTLFunction>)info->compute_function;
  desc.threadGroupSizeIsMultipleOfThreadExecutionWidth = info->tgsize_is_multiple_of_sgwidth;
  for (unsigned i = 0; i < 31; i++) {
    if (info->immutable_buffers & (1u << i)) desc.buffers[i].mutability = MTLMutabilityImmutable;
  }
  if (info->num_binary_archives_for_lookup && info->binary_archives_for_lookup.ptr)
    desc.binaryArchives = [NSArray arrayWithObjects:(id<MTLBinaryArchive> *)info->binary_archives_for_lookup.ptr
                                             count:info->num_binary_archives_for_lookup];
  NSError *err = nil;
  MTLPipelineOption opts = info->fail_on_binary_archive_miss
                             ? MTLPipelineOptionFailOnBinaryArchiveMiss
                             : MTLPipelineOptionNone;
  id<MTLComputePipelineState> pso = [(id<MTLDevice>)device
      newComputePipelineStateWithDescriptor:desc options:opts reflection:nil error:&err];
  if (!err && info->binary_archive_for_serialization) {
    NSError *archErr = nil;
    std::lock_guard<std::mutex> archive_lock(binary_archive_mutex());
    [(id<MTLBinaryArchive>)info->binary_archive_for_serialization
        addComputePipelineFunctionsWithDescriptor:desc error:&archErr];
  }
  [desc release];
  if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
  return (obj_handle_t)pso;
}

// Tile-stage pipeline factory (R-BACK-13.* prerequisite). Tile-FFP only
// needs a minimal subset of MTLTileRenderPipelineDescriptor — see the
// matching descriptor in winemetal.h for the rationale.
extern "C" obj_handle_t MTLDevice_newRenderPipelineStateWithTileDescriptor(
    obj_handle_t device, const struct WMTTileRenderPipelineDescriptor *desc_in, obj_handle_t *err_out) {
  if (!device || !desc_in) return NULL_OBJECT_HANDLE;
  MTLTileRenderPipelineDescriptor *desc = [[MTLTileRenderPipelineDescriptor alloc] init];
  desc.tileFunction                    = (id<MTLFunction>)desc_in->tile_function;
  desc.rasterSampleCount               = desc_in->raster_sample_count;
  desc.threadgroupSizeMatchesTileSize  = desc_in->threadgroup_size_matches_tile_size != 0;
  if (desc_in->max_total_threads_per_threadgroup) {
    desc.maxTotalThreadsPerThreadgroup = desc_in->max_total_threads_per_threadgroup;
  }
  uint32_t count = desc_in->color_attachment_count;
  if (count > 8) count = 8;
  for (uint32_t i = 0; i < count; i++) {
    desc.colorAttachments[i].pixelFormat = to_metal_pixel_format(desc_in->color_attachment_pixel_formats[i]);
  }
  NSError *err = nil;
  id<MTLRenderPipelineState> pso = [(id<MTLDevice>)device
      newRenderPipelineStateWithTileDescriptor:desc options:MTLPipelineOptionNone reflection:nil error:&err];
  [desc release];
  if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
  return (obj_handle_t)pso;
}

// -- Tile-stage encoder ops (passthroughs) --

extern "C" void MTLRenderCommandEncoder_setTileRenderPipelineState(obj_handle_t encoder, obj_handle_t pipeline) {
  if (!encoder) return;
  [(id<MTLRenderCommandEncoder>)encoder setTileRenderPipelineState:(id<MTLRenderPipelineState>)pipeline];
}

extern "C" void MTLRenderCommandEncoder_dispatchThreadsPerTile(obj_handle_t encoder, struct WMTSize threads_per_tile) {
  if (!encoder) return;
  [(id<MTLRenderCommandEncoder>)encoder
      dispatchThreadsPerTile:MTLSizeMake(threads_per_tile.width, threads_per_tile.height, threads_per_tile.depth)];
}

extern "C" void MTLRenderCommandEncoder_setTileBuffer(obj_handle_t encoder, obj_handle_t buffer, uint64_t offset, uint32_t index) {
  if (!encoder) return;
  [(id<MTLRenderCommandEncoder>)encoder setTileBuffer:(id<MTLBuffer>)buffer offset:offset atIndex:index];
}

extern "C" void MTLRenderCommandEncoder_setTileTexture(obj_handle_t encoder, obj_handle_t texture, uint32_t index) {
  if (!encoder) return;
  [(id<MTLRenderCommandEncoder>)encoder setTileTexture:(id<MTLTexture>)texture atIndex:index];
}

extern "C" void MTLRenderCommandEncoder_setTileBytes(obj_handle_t encoder, const void *bytes, uint64_t length, uint32_t index) {
  if (!encoder || !bytes || !length) return;
  [(id<MTLRenderCommandEncoder>)encoder setTileBytes:bytes length:length atIndex:index];
}

extern "C" void MTLRenderCommandEncoder_setTileSamplerState(obj_handle_t encoder, obj_handle_t sampler, uint32_t index) {
  if (!encoder) return;
  [(id<MTLRenderCommandEncoder>)encoder setTileSamplerState:(id<MTLSamplerState>)sampler atIndex:index];
}

extern "C" uint64_t MTLRenderCommandEncoder_tileWidth(obj_handle_t encoder) {
  if (!encoder) return 0;
  return (uint64_t)[(id<MTLRenderCommandEncoder>)encoder tileWidth];
}

extern "C" uint64_t MTLRenderCommandEncoder_tileHeight(obj_handle_t encoder) {
  if (!encoder) return 0;
  return (uint64_t)[(id<MTLRenderCommandEncoder>)encoder tileHeight];
}

// -- Fence / Event --

extern "C" obj_handle_t MTLDevice_newFence(obj_handle_t device) {
  return device ? (obj_handle_t)[(id<MTLDevice>)device newFence] : NULL_OBJECT_HANDLE;
}

extern "C" obj_handle_t MTLDevice_newEvent(obj_handle_t device) {
  return device ? (obj_handle_t)[(id<MTLDevice>)device newEvent] : NULL_OBJECT_HANDLE;
}

extern "C" uint64_t MTLSharedEvent_signaledValue(obj_handle_t event) {
  return event ? [(id<MTLSharedEvent>)event signaledValue] : 0;
}

extern "C" void MTLSharedEvent_signalValue(obj_handle_t event, uint64_t value) {
  if (event) [(id<MTLSharedEvent>)event setSignaledValue:value];
}

extern "C" mach_port_t MTLSharedEvent_createMachPort(obj_handle_t event) {
  if (!event) return MACH_PORT_NULL;
  MTLSharedEventListener *listener = [[MTLSharedEventListener alloc] init];
  // Return the underlying dispatch semaphore's mach port via the listener's queue
  // For simplicity, use the existing waitUntilSignaledValue approach instead
  [listener release];
  return MACH_PORT_NULL;
}

extern "C" bool MTLSharedEvent_waitUntilSignaledValue(obj_handle_t event, uint64_t value,
                                                       uint64_t timeout_ns) {
  if (!event) return false;
  // Use a semaphore-based wait
  dispatch_semaphore_t sema = dispatch_semaphore_create(0);
  MTLSharedEventListener *listener = [[MTLSharedEventListener alloc] initWithDispatchQueue:
      dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_HIGH, 0)];
  __block bool signaled = false;
  [(id<MTLSharedEvent>)event notifyListener:listener atValue:value block:^(id<MTLSharedEvent> e, uint64_t v) {
    signaled = true;
    dispatch_semaphore_signal(sema);
  }];
  // Check if already signaled
  if ([(id<MTLSharedEvent>)event signaledValue] >= value) {
    [listener release];
    dispatch_release(sema);
    return true;
  }
  if (timeout_ns == UINT64_MAX) {
    dispatch_semaphore_wait(sema, DISPATCH_TIME_FOREVER);
  } else {
    dispatch_semaphore_wait(sema, dispatch_time(DISPATCH_TIME_NOW, (int64_t)timeout_ns));
  }
  [listener release];
  dispatch_release(sema);
  return signaled || [(id<MTLSharedEvent>)event signaledValue] >= value;
}

// -- Command buffer encoders --

extern "C" void MTLCommandBuffer_encodeSignalEvent(obj_handle_t cmdbuf, obj_handle_t event,
                                                    uint64_t value) {
  if (!cmdbuf || !event) return;
  [(id<MTLCommandBuffer>)cmdbuf encodeSignalEvent:(id<MTLSharedEvent>)event value:value];
}

extern "C" void MTLCommandBuffer_encodeWaitForEvent(obj_handle_t cmdbuf, obj_handle_t event,
                                                     uint64_t value) {
  if (!cmdbuf || !event) return;
  [(id<MTLCommandBuffer>)cmdbuf encodeWaitForEvent:(id<MTLSharedEvent>)event value:value];
}

extern "C" obj_handle_t MTLCommandBuffer_blitCommandEncoder(obj_handle_t cmdbuf) {
  return cmdbuf ? (obj_handle_t)[(id<MTLCommandBuffer>)cmdbuf blitCommandEncoder] : NULL_OBJECT_HANDLE;
}

extern "C" obj_handle_t MTLCommandBuffer_computeCommandEncoder(obj_handle_t cmdbuf, bool concurrent) {
  if (!cmdbuf) return NULL_OBJECT_HANDLE;
  MTLDispatchType dt = concurrent ? MTLDispatchTypeConcurrent : MTLDispatchTypeSerial;
  return (obj_handle_t)[(id<MTLCommandBuffer>)cmdbuf computeCommandEncoderWithDispatchType:dt];
}

static MTLRenderPassDescriptor *makeRenderPassDescriptor(
    struct WMTRenderPassInfo *info) {
  if (!info) return nil;
  MTLRenderPassDescriptor *desc = [[MTLRenderPassDescriptor alloc] init];
  for (unsigned i = 0; i < 8; i++) {
    desc.colorAttachments[i].clearColor   = MTLClearColorMake(
        info->colors[i].clear_color.r, info->colors[i].clear_color.g,
        info->colors[i].clear_color.b, info->colors[i].clear_color.a);
    desc.colorAttachments[i].level        = info->colors[i].level;
    desc.colorAttachments[i].slice        = info->colors[i].slice;
    desc.colorAttachments[i].depthPlane   = info->colors[i].depth_plane;
    desc.colorAttachments[i].texture      = (id<MTLTexture>)info->colors[i].texture;
    desc.colorAttachments[i].loadAction   = (MTLLoadAction)info->colors[i].load_action;
    desc.colorAttachments[i].storeAction  = (MTLStoreAction)info->colors[i].store_action;
    desc.colorAttachments[i].resolveTexture    = (id<MTLTexture>)info->colors[i].resolve_texture;
    desc.colorAttachments[i].resolveLevel      = info->colors[i].resolve_level;
    desc.colorAttachments[i].resolveSlice      = info->colors[i].resolve_slice;
    desc.colorAttachments[i].resolveDepthPlane = info->colors[i].resolve_depth_plane;
  }
  if (info->depth.texture) {
    desc.depthAttachment.clearDepth   = info->depth.clear_depth;
    desc.depthAttachment.depthPlane   = info->depth.depth_plane;
    desc.depthAttachment.level        = info->depth.level;
    desc.depthAttachment.slice        = info->depth.slice;
    desc.depthAttachment.texture      = (id<MTLTexture>)info->depth.texture;
    desc.depthAttachment.loadAction   = (MTLLoadAction)info->depth.load_action;
    desc.depthAttachment.storeAction  = (MTLStoreAction)info->depth.store_action;
    // R-FORMAT-11 — multisample depth resolve (RESZ). Mirrors the color
    // attachment resolve wiring above. depthResolveFilter has no implicit
    // default that matches D3D9, so set it whenever a resolve texture is
    // bound (the store action selects whether the resolve actually runs).
    if (info->depth.resolve_texture) {
      desc.depthAttachment.resolveTexture = (id<MTLTexture>)info->depth.resolve_texture;
      desc.depthAttachment.depthResolveFilter =
          (MTLMultisampleDepthResolveFilter)info->depth.resolve_filter;
    }
  }
  if (info->stencil.texture) {
    desc.stencilAttachment.clearStencil = info->stencil.clear_stencil;
    desc.stencilAttachment.depthPlane   = info->stencil.depth_plane;
    desc.stencilAttachment.level        = info->stencil.level;
    desc.stencilAttachment.slice        = info->stencil.slice;
    desc.stencilAttachment.texture      = (id<MTLTexture>)info->stencil.texture;
    desc.stencilAttachment.loadAction   = (MTLLoadAction)info->stencil.load_action;
    desc.stencilAttachment.storeAction  = (MTLStoreAction)info->stencil.store_action;
  }
  desc.defaultRasterSampleCount  = info->default_raster_sample_count;
  desc.renderTargetArrayLength   = info->render_target_array_length;
  desc.renderTargetHeight        = info->render_target_height;
  desc.renderTargetWidth         = info->render_target_width;
  desc.visibilityResultBuffer    = (id<MTLBuffer>)info->visibility_buffer;
  const uint8_t sampleBufferAttachmentCount =
      MIN(info->num_sample_buffer_attachments, (uint8_t)4);
  for (uint8_t i = 0; i < sampleBufferAttachmentCount; ++i) {
    const auto& attachment = info->sample_buffer_attachments[i];
    if (!attachment.sample_buffer) continue;
    desc.sampleBufferAttachments[i].sampleBuffer =
        (id<MTLCounterSampleBuffer>)attachment.sample_buffer;
    desc.sampleBufferAttachments[i].startOfVertexSampleIndex =
        (NSUInteger)attachment.start_of_encoder_sample_index;
    desc.sampleBufferAttachments[i].endOfVertexSampleIndex = MTLCounterDontSample;
    desc.sampleBufferAttachments[i].startOfFragmentSampleIndex = MTLCounterDontSample;
    desc.sampleBufferAttachments[i].endOfFragmentSampleIndex =
        (NSUInteger)attachment.end_of_encoder_sample_index;
  }
  if (info->tile_width && info->tile_height) {
    desc.tileWidth  = info->tile_width;
    desc.tileHeight = info->tile_height;
  }
  return desc;
}

extern "C" obj_handle_t MTLCommandBuffer_renderCommandEncoder(obj_handle_t cmdbuf,
                                                               struct WMTRenderPassInfo *info) {
  if (!cmdbuf || !info) return NULL_OBJECT_HANDLE;
  MTLRenderPassDescriptor *desc = makeRenderPassDescriptor(info);
  if (!desc) return NULL_OBJECT_HANDLE;
  id<MTLRenderCommandEncoder> enc = [(id<MTLCommandBuffer>)cmdbuf
      renderCommandEncoderWithDescriptor:desc];
  [desc release];
  return (obj_handle_t)enc;
}

extern "C" obj_handle_t MTLCommandBuffer_parallelRenderCommandEncoder(
    obj_handle_t cmdbuf, struct WMTRenderPassInfo *info) {
  if (!cmdbuf || !info) return NULL_OBJECT_HANDLE;
  MTLRenderPassDescriptor *desc = makeRenderPassDescriptor(info);
  if (!desc) return NULL_OBJECT_HANDLE;
  id<MTLParallelRenderCommandEncoder> enc = [(id<MTLCommandBuffer>)cmdbuf
      parallelRenderCommandEncoderWithDescriptor:desc];
  [desc release];
  return (obj_handle_t)enc;
}

extern "C" obj_handle_t
MTLParallelRenderCommandEncoder_renderCommandEncoder(obj_handle_t encoder) {
  return encoder
      ? (obj_handle_t)[(id<MTLParallelRenderCommandEncoder>)encoder
            renderCommandEncoder]
      : NULL_OBJECT_HANDLE;
}

extern "C" void MTLRenderCommandEncoder_setColorStoreAction(
    obj_handle_t encoder, enum WMTStoreAction action,
    uint32_t color_attachment_index) {
  if (encoder && color_attachment_index < 8) {
    [(id<MTLRenderCommandEncoder>)encoder
        setColorStoreAction:(MTLStoreAction)action
                    atIndex:(NSUInteger)color_attachment_index];
  }
}

extern "C" void MTLRenderCommandEncoder_setDepthStoreAction(
    obj_handle_t encoder, enum WMTStoreAction action) {
  if (encoder) {
    [(id<MTLRenderCommandEncoder>)encoder
        setDepthStoreAction:(MTLStoreAction)action];
  }
}

extern "C" void MTLRenderCommandEncoder_setStencilStoreAction(
    obj_handle_t encoder, enum WMTStoreAction action) {
  if (encoder) {
    [(id<MTLRenderCommandEncoder>)encoder
        setStencilStoreAction:(MTLStoreAction)action];
  }
}

extern "C" void MTLCommandEncoder_endEncoding(obj_handle_t encoder) {
  if (encoder) [(id<MTLCommandEncoder>)encoder endEncoding];
}

extern "C" void MTLCommandEncoder_setLabel(obj_handle_t encoder, obj_handle_t label) {
  if (encoder && label) [(id<MTLCommandEncoder>)encoder setLabel:(NSString *)label];
}

// M1 — resource-side setLabel entry points. Each is a no-op on null args.
// MTLBuffer, MTLTexture conform to MTLResource which exposes a writable
// .label. MTLCommandQueue and MTLCommandBuffer protocols expose .label
// directly. MTLRenderPipelineState and MTLComputePipelineState expose
// .label as readonly on the public protocol — but Apple's concrete
// implementation does respond to setLabel: (verified via respondsToSelector
// to stay correct on older runtimes that never added the setter).
extern "C" void MTLBuffer_setLabel(obj_handle_t buffer, obj_handle_t label) {
  if (buffer && label) [(id<MTLBuffer>)buffer setLabel:(NSString *)label];
}

extern "C" void MTLTexture_setLabel(obj_handle_t texture, obj_handle_t label) {
  if (texture && label) [(id<MTLTexture>)texture setLabel:(NSString *)label];
}

extern "C" void MTLCommandQueue_setLabel(obj_handle_t queue, obj_handle_t label) {
  if (queue && label) [(id<MTLCommandQueue>)queue setLabel:(NSString *)label];
}

extern "C" void MTLCommandBuffer_setLabel(obj_handle_t cmdbuf, obj_handle_t label) {
  if (cmdbuf && label) [(id<MTLCommandBuffer>)cmdbuf setLabel:(NSString *)label];
}

extern "C" void MTLRenderPipelineState_setLabel(obj_handle_t pso, obj_handle_t label) {
  if (!pso || !label) return;
  id obj = (id)pso;
  if ([obj respondsToSelector:@selector(setLabel:)]) {
    [obj setLabel:(NSString *)label];
  }
}

extern "C" void MTLComputePipelineState_setLabel(obj_handle_t pso, obj_handle_t label) {
  if (!pso || !label) return;
  id obj = (id)pso;
  if ([obj respondsToSelector:@selector(setLabel:)]) {
    [obj setLabel:(NSString *)label];
  }
}

// M2 — pushDebugGroup / popDebugGroup. Both selectors are MTLCommandEncoder
// protocol surface; null-guard the encoder handle and the name.
extern "C" void MTLCommandEncoder_pushDebugGroup(obj_handle_t encoder, obj_handle_t name) {
  if (encoder && name) [(id<MTLCommandEncoder>)encoder pushDebugGroup:(NSString *)name];
}

extern "C" void MTLCommandEncoder_popDebugGroup(obj_handle_t encoder) {
  if (encoder) [(id<MTLCommandEncoder>)encoder popDebugGroup];
}

// M6 — counter-sampling capability probe. The protocol method is
// supportsCounterSampling: which takes an MTLCounterSamplingPoint enum.
// respondsToSelector check tolerates the (rare) older Metal runtime where
// the API isn't present at all, in which case we conservatively return
// false.
extern "C" bool MTLDevice_supportsCounterSampling(obj_handle_t device, enum WMTCounterSamplingPoint point) {
  if (!device) return false;
  id<MTLDevice> d = (id<MTLDevice>)device;
  if (![d respondsToSelector:@selector(supportsCounterSampling:)]) return false;
  return [d supportsCounterSampling:(MTLCounterSamplingPoint)point];
}

extern "C" obj_handle_t
MTLCounterSampleBuffer_newTimestampBuffer(obj_handle_t device, uint32_t sample_count, bool shared) {
  if (!device || sample_count == 0) return NULL_OBJECT_HANDLE;
  id<MTLDevice> d = (id<MTLDevice>)device;
  if (![d respondsToSelector:@selector(counterSets)] ||
      ![d respondsToSelector:@selector(newCounterSampleBufferWithDescriptor:error:)]) {
    return NULL_OBJECT_HANDLE;
  }

  id<MTLCounterSet> timestampSet = nil;
  for (id<MTLCounterSet> counterSet in [d counterSets]) {
    if ([[counterSet name] isEqualToString:MTLCommonCounterSetTimestamp]) {
      timestampSet = counterSet;
      break;
    }
  }
  if (!timestampSet) return NULL_OBJECT_HANDLE;

  MTLCounterSampleBufferDescriptor *desc =
      [[MTLCounterSampleBufferDescriptor alloc] init];
  desc.counterSet = timestampSet;
  desc.sampleCount = sample_count;
  desc.storageMode = shared ? MTLStorageModeShared : MTLStorageModePrivate;
  NSError *err = nil;
  id<MTLCounterSampleBuffer> buffer =
      [d newCounterSampleBufferWithDescriptor:desc error:&err];
  [desc release];
  return (obj_handle_t)buffer;
}

extern "C" void MTLCounterSampleBuffer_resolveCounterRange(
    obj_handle_t sample_buffer, uint32_t start, uint32_t len, void *data_out,
    uint64_t data_length) {
  if (!sample_buffer || !data_out || len == 0) return;
  const uint64_t required = (uint64_t)len * sizeof(MTLCounterResultTimestamp);
  if (data_length < required) return;
  NSRange range = NSMakeRange((NSUInteger)start, (NSUInteger)len);
  NSData *data = [(id<MTLCounterSampleBuffer>)sample_buffer resolveCounterRange:range];
  if (!data || [data length] < required) return;
  const auto* timestamps =
      reinterpret_cast<const MTLCounterResultTimestamp*>([data bytes]);
  auto* out = reinterpret_cast<uint64_t*>(data_out);
  for (uint32_t i = 0; i < len; ++i) {
    out[i] = timestamps[i].timestamp;
  }
}

extern "C" uint64_t MTLCommandBuffer_property(obj_handle_t cmdbuf, enum WMTCommandBufferProperty prop) {
  if (!cmdbuf) return 0;
  id<MTLCommandBuffer> cb = (id<MTLCommandBuffer>)cmdbuf;
  switch (prop) {
    case WMTCommandBufferPropertyKernelStartTime: return (uint64_t)(cb.kernelStartTime * 1e9);
    case WMTCommandBufferPropertyKernelEndTime:   return (uint64_t)(cb.kernelEndTime   * 1e9);
    case WMTCommandBufferPropertyGPUStartTime:    return (uint64_t)(cb.GPUStartTime    * 1e9);
    case WMTCommandBufferPropertyGPUEndTime:      return (uint64_t)(cb.GPUEndTime      * 1e9);
    default: return 0;
  }
}

// -- Render command encoder dispatch --

extern "C" void MTLRenderCommandEncoder_encodeCommands(obj_handle_t encoder,
                                                        const struct wmtcmd_base *cmd_head) {
  if (!encoder || !cmd_head) return;
  id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)encoder;
  const struct wmtcmd_base *next = cmd_head;
  while (next) {
    switch ((enum WMTRenderCommandType)next->type) {
    default: break;
    case WMTRenderCommandNop: break;
    case WMTRenderCommandUseResource: {
      const struct wmtcmd_render_useresource *b = (const struct wmtcmd_render_useresource *)next;
      [enc useResource:(id<MTLResource>)b->resource usage:(MTLResourceUsage)b->usage
               stages:(MTLRenderStages)b->stages];
      break;
    }
    case WMTRenderCommandSetVertexBuffer: {
      const struct wmtcmd_render_setbuffer *b = (const struct wmtcmd_render_setbuffer *)next;
      [enc setVertexBuffer:(id<MTLBuffer>)b->buffer offset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetVertexBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *b = (const struct wmtcmd_render_setbufferoffset *)next;
      [enc setVertexBufferOffset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetFragmentBuffer: {
      const struct wmtcmd_render_setbuffer *b = (const struct wmtcmd_render_setbuffer *)next;
      [enc setFragmentBuffer:(id<MTLBuffer>)b->buffer offset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetFragmentBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *b = (const struct wmtcmd_render_setbufferoffset *)next;
      [enc setFragmentBufferOffset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetMeshBuffer: {
      const struct wmtcmd_render_setbuffer *b = (const struct wmtcmd_render_setbuffer *)next;
      [enc setMeshBuffer:(id<MTLBuffer>)b->buffer offset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetMeshBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *b = (const struct wmtcmd_render_setbufferoffset *)next;
      [enc setMeshBufferOffset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetObjectBuffer: {
      const struct wmtcmd_render_setbuffer *b = (const struct wmtcmd_render_setbuffer *)next;
      [enc setObjectBuffer:(id<MTLBuffer>)b->buffer offset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetObjectBufferOffset: {
      const struct wmtcmd_render_setbufferoffset *b = (const struct wmtcmd_render_setbufferoffset *)next;
      [enc setObjectBufferOffset:b->offset atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetFragmentBytes: {
      const struct wmtcmd_render_setbytes *b = (const struct wmtcmd_render_setbytes *)next;
      [enc setFragmentBytes:(const void *)b->bytes.ptr length:(NSUInteger)b->length atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetVertexBytes: {
      const struct wmtcmd_render_setbytes *b = (const struct wmtcmd_render_setbytes *)next;
      [enc setVertexBytes:(const void *)b->bytes.ptr length:(NSUInteger)b->length atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetFragmentTexture: {
      const struct wmtcmd_render_settexture *b = (const struct wmtcmd_render_settexture *)next;
      [enc setFragmentTexture:(id<MTLTexture>)b->texture atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetVertexTexture: {
      const struct wmtcmd_render_settexture *b = (const struct wmtcmd_render_settexture *)next;
      [enc setVertexTexture:(id<MTLTexture>)b->texture atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetRasterizerState: {
      const struct wmtcmd_render_setrasterizerstate *b = (const struct wmtcmd_render_setrasterizerstate *)next;
      [enc setTriangleFillMode:(MTLTriangleFillMode)b->fill_mode];
      [enc setCullMode:(MTLCullMode)b->cull_mode];
      [enc setDepthClipMode:(MTLDepthClipMode)b->depth_clip_mode];
      [enc setDepthBias:b->depth_bias slopeScale:b->scole_scale clamp:b->depth_bias_clamp];
      [enc setFrontFacingWinding:(MTLWinding)b->winding];
      break;
    }
    case WMTRenderCommandSetViewports: {
      const struct wmtcmd_render_setviewports *b = (const struct wmtcmd_render_setviewports *)next;
      [enc setViewports:(const MTLViewport *)b->viewports.ptr count:b->viewport_count];
      break;
    }
    case WMTRenderCommandSetViewport: {
      const struct wmtcmd_render_setviewport *b = (const struct wmtcmd_render_setviewport *)next;
      MTLViewport vp;
      memcpy(&vp, &b->viewport, sizeof(vp));
      [enc setViewport:vp];
      break;
    }
    case WMTRenderCommandSetScissorRects: {
      const struct wmtcmd_render_setscissorrects *b = (const struct wmtcmd_render_setscissorrects *)next;
      [enc setScissorRects:(const MTLScissorRect *)b->scissor_rects.ptr count:b->rect_count];
      break;
    }
    case WMTRenderCommandSetScissorRect: {
      const struct wmtcmd_render_setscissorrect *b = (const struct wmtcmd_render_setscissorrect *)next;
      MTLScissorRect sr;
      memcpy(&sr, &b->scissor_rect, sizeof(sr));
      [enc setScissorRect:sr];
      break;
    }
    case WMTRenderCommandSetPSO: {
      const struct wmtcmd_render_setpso *b = (const struct wmtcmd_render_setpso *)next;
      [enc setRenderPipelineState:(id<MTLRenderPipelineState>)b->pso];
      break;
    }
    case WMTRenderCommandSetDSSO: {
      const struct wmtcmd_render_setdsso *b = (const struct wmtcmd_render_setdsso *)next;
      [enc setDepthStencilState:(id<MTLDepthStencilState>)b->dsso];
      [enc setStencilReferenceValue:b->stencil_ref];
      break;
    }
    case WMTRenderCommandSetBlendFactorAndStencilRef: {
      const struct wmtcmd_render_setblendcolor *b = (const struct wmtcmd_render_setblendcolor *)next;
      [enc setBlendColorRed:b->red green:b->green blue:b->blue alpha:b->alpha];
      [enc setStencilReferenceValue:b->stencil_ref];
      break;
    }
    case WMTRenderCommandSetVisibilityMode: {
      const struct wmtcmd_render_setvisibilitymode *b = (const struct wmtcmd_render_setvisibilitymode *)next;
      [enc setVisibilityResultMode:(MTLVisibilityResultMode)b->mode offset:(NSUInteger)b->offset];
      break;
    }
    case WMTRenderCommandDraw: {
      const struct wmtcmd_render_draw *b = (const struct wmtcmd_render_draw *)next;
      [enc drawPrimitives:(MTLPrimitiveType)b->primitive_type
              vertexStart:(NSUInteger)b->vertex_start
              vertexCount:(NSUInteger)b->vertex_count
            instanceCount:(NSUInteger)b->instance_count
             baseInstance:(NSUInteger)b->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndexed: {
      const struct wmtcmd_render_draw_indexed *b = (const struct wmtcmd_render_draw_indexed *)next;
      [enc drawIndexedPrimitives:(MTLPrimitiveType)b->primitive_type
                      indexCount:(NSUInteger)b->index_count
                       indexType:(MTLIndexType)b->index_type
                     indexBuffer:(id<MTLBuffer>)b->index_buffer
               indexBufferOffset:(NSUInteger)b->index_buffer_offset
                   instanceCount:(NSUInteger)b->instance_count
                      baseVertex:(NSInteger)b->base_vertex
                    baseInstance:(NSUInteger)b->base_instance];
      break;
    }
    case WMTRenderCommandDrawIndirect: {
      const struct wmtcmd_render_draw_indirect *b = (const struct wmtcmd_render_draw_indirect *)next;
      [enc drawPrimitives:(MTLPrimitiveType)b->primitive_type
            indirectBuffer:(id<MTLBuffer>)b->indirect_args_buffer
      indirectBufferOffset:(NSUInteger)b->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawIndexedIndirect: {
      const struct wmtcmd_render_draw_indexed_indirect *b = (const struct wmtcmd_render_draw_indexed_indirect *)next;
      [enc drawIndexedPrimitives:(MTLPrimitiveType)b->primitive_type
                       indexType:(MTLIndexType)b->index_type
                     indexBuffer:(id<MTLBuffer>)b->index_buffer
               indexBufferOffset:(NSUInteger)b->index_buffer_offset
                  indirectBuffer:(id<MTLBuffer>)b->indirect_args_buffer
            indirectBufferOffset:(NSUInteger)b->indirect_args_offset];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroups: {
      const struct wmtcmd_render_draw_meshthreadgroups *b = (const struct wmtcmd_render_draw_meshthreadgroups *)next;
      [enc drawMeshThreadgroups:MTLSizeMake(b->threadgroup_per_grid.width, b->threadgroup_per_grid.height,
                                            b->threadgroup_per_grid.depth)
       threadsPerObjectThreadgroup:MTLSizeMake(b->object_threadgroup_size.width, b->object_threadgroup_size.height,
                                              b->object_threadgroup_size.depth)
         threadsPerMeshThreadgroup:MTLSizeMake(b->mesh_threadgroup_size.width, b->mesh_threadgroup_size.height,
                                              b->mesh_threadgroup_size.depth)];
      break;
    }
    case WMTRenderCommandDrawMeshThreadgroupsIndirect: {
      const struct wmtcmd_render_draw_meshthreadgroups_indirect *b =
          (const struct wmtcmd_render_draw_meshthreadgroups_indirect *)next;
      [enc drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b->indirect_args_buffer
                             indirectBufferOffset:(NSUInteger)b->indirect_args_offset
                      threadsPerObjectThreadgroup:MTLSizeMake(b->object_threadgroup_size.width,
                                                              b->object_threadgroup_size.height,
                                                              b->object_threadgroup_size.depth)
                        threadsPerMeshThreadgroup:MTLSizeMake(b->mesh_threadgroup_size.width,
                                                              b->mesh_threadgroup_size.height,
                                                              b->mesh_threadgroup_size.depth)];
      break;
    }
    case WMTRenderCommandMemoryBarrier: {
      const struct wmtcmd_render_memory_barrier *b = (const struct wmtcmd_render_memory_barrier *)next;
      [enc memoryBarrierWithScope:(MTLBarrierScope)b->scope
                      afterStages:(MTLRenderStages)b->stages_after
                     beforeStages:(MTLRenderStages)b->stages_before];
      break;
    }
    case WMTRenderCommandWaitForFence: {
      const struct wmtcmd_render_fence_op *b = (const struct wmtcmd_render_fence_op *)next;
      [enc waitForFence:(id<MTLFence>)b->fence beforeStages:(MTLRenderStages)b->stages];
      break;
    }
    case WMTRenderCommandUpdateFence: {
      const struct wmtcmd_render_fence_op *b = (const struct wmtcmd_render_fence_op *)next;
      [enc updateFence:(id<MTLFence>)b->fence afterStages:(MTLRenderStages)b->stages];
      break;
    }
    case WMTRenderCommandDXMTGeometryDraw: {
      const struct wmtcmd_render_dxmt_geometry_draw *b = (const struct wmtcmd_render_dxmt_geometry_draw *)next;
      [enc setObjectBufferOffset:b->draw_arguments_offset atIndex:21];
      [enc drawMeshThreadgroups:MTLSizeMake(b->warp_count, b->instance_count, 1)
       threadsPerObjectThreadgroup:MTLSizeMake(b->vertex_per_warp, 1, 1)
         threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexed: {
      const struct wmtcmd_render_dxmt_geometry_draw_indexed *b =
          (const struct wmtcmd_render_dxmt_geometry_draw_indexed *)next;
      [enc setObjectBuffer:(id<MTLBuffer>)b->index_buffer offset:b->index_buffer_offset atIndex:20];
      [enc setObjectBufferOffset:b->draw_arguments_offset atIndex:21];
      [enc drawMeshThreadgroups:MTLSizeMake(b->warp_count, b->instance_count, 1)
       threadsPerObjectThreadgroup:MTLSizeMake(b->vertex_per_warp, 1, 1)
         threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndirect: {
      const struct wmtcmd_render_dxmt_geometry_draw_indirect *b =
          (const struct wmtcmd_render_dxmt_geometry_draw_indirect *)next;
      [enc setObjectBuffer:(id<MTLBuffer>)b->indirect_args_buffer offset:b->indirect_args_offset atIndex:21];
      [enc drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b->dispatch_args_buffer
                             indirectBufferOffset:b->dispatch_args_offset
                      threadsPerObjectThreadgroup:MTLSizeMake(b->vertex_per_warp, 1, 1)
                        threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [enc setObjectBuffer:(id<MTLBuffer>)b->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTGeometryDrawIndexedIndirect: {
      const struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *b =
          (const struct wmtcmd_render_dxmt_geometry_draw_indexed_indirect *)next;
      [enc setObjectBuffer:(id<MTLBuffer>)b->index_buffer offset:b->index_buffer_offset atIndex:20];
      [enc setObjectBuffer:(id<MTLBuffer>)b->indirect_args_buffer offset:b->indirect_args_offset atIndex:21];
      [enc drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b->dispatch_args_buffer
                             indirectBufferOffset:b->dispatch_args_offset
                      threadsPerObjectThreadgroup:MTLSizeMake(b->vertex_per_warp, 1, 1)
                        threadsPerMeshThreadgroup:MTLSizeMake(1, 1, 1)];
      [enc setObjectBuffer:(id<MTLBuffer>)b->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDraw: {
      const struct wmtcmd_render_dxmt_tessellation_mesh_draw *b =
          (const struct wmtcmd_render_dxmt_tessellation_mesh_draw *)next;
      [enc setObjectBufferOffset:b->draw_arguments_offset atIndex:21];
      [enc drawMeshThreadgroups:MTLSizeMake(b->patch_per_mesh_instance, b->instance_count, 1)
       threadsPerObjectThreadgroup:MTLSizeMake(b->threads_per_patch, b->patch_per_group, 1)
         threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexed: {
      const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *b =
          (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed *)next;
      [enc setObjectBuffer:(id<MTLBuffer>)b->index_buffer offset:b->index_buffer_offset atIndex:20];
      [enc setObjectBufferOffset:b->draw_arguments_offset atIndex:21];
      [enc drawMeshThreadgroups:MTLSizeMake(b->patch_per_mesh_instance, b->instance_count, 1)
       threadsPerObjectThreadgroup:MTLSizeMake(b->threads_per_patch, b->patch_per_group, 1)
         threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndirect: {
      const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *b =
          (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indirect *)next;
      [enc setObjectBuffer:(id<MTLBuffer>)b->indirect_args_buffer offset:b->indirect_args_offset atIndex:21];
      [enc drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b->dispatch_args_buffer
                             indirectBufferOffset:b->dispatch_args_offset
                      threadsPerObjectThreadgroup:MTLSizeMake(b->threads_per_patch, b->patch_per_group, 1)
                        threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [enc setObjectBuffer:(id<MTLBuffer>)b->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDXMTTessellationMeshDrawIndexedIndirect: {
      const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *b =
          (const struct wmtcmd_render_dxmt_tessellation_mesh_draw_indexed_indirect *)next;
      [enc setObjectBuffer:(id<MTLBuffer>)b->index_buffer offset:b->index_buffer_offset atIndex:20];
      [enc setObjectBuffer:(id<MTLBuffer>)b->indirect_args_buffer offset:b->indirect_args_offset atIndex:21];
      [enc drawMeshThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b->dispatch_args_buffer
                             indirectBufferOffset:b->dispatch_args_offset
                      threadsPerObjectThreadgroup:MTLSizeMake(b->threads_per_patch, b->patch_per_group, 1)
                        threadsPerMeshThreadgroup:MTLSizeMake(32, 1, 1)];
      [enc setObjectBuffer:(id<MTLBuffer>)b->imm_draw_arguments offset:0 atIndex:21];
      break;
    }
    case WMTRenderCommandDispatchThreadsPerTile: {
      const struct wmtcmd_render_dispatch_threads_per_tile *b =
          (const struct wmtcmd_render_dispatch_threads_per_tile *)next;
      [enc dispatchThreadsPerTile:MTLSizeMake(b->width, b->height, 1)];
      break;
    }
    case WMTRenderCommandSetFragmentSamplerState: {
      const struct wmtcmd_render_setsamplerstate *b = (const struct wmtcmd_render_setsamplerstate *)next;
      [enc setFragmentSamplerState:(id<MTLSamplerState>)b->sampler_state atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetVertexSamplerState: {
      const struct wmtcmd_render_setsamplerstate *b = (const struct wmtcmd_render_setsamplerstate *)next;
      [enc setVertexSamplerState:(id<MTLSamplerState>)b->sampler_state atIndex:b->index];
      break;
    }
    case WMTRenderCommandSetStencilReferenceValue: {
      const struct wmtcmd_render_setstencilref *b = (const struct wmtcmd_render_setstencilref *)next;
      [enc setStencilReferenceValue:b->stencil_ref];
      break;
    }
    }
    next = (const struct wmtcmd_base *)next->next.ptr;
  }
}

// -- Blit command encoder dispatch --

extern "C" void MTLBlitCommandEncoder_encodeCommands(obj_handle_t encoder,
                                                      const struct wmtcmd_base *cmd_head) {
  if (!encoder || !cmd_head) return;
  id<MTLBlitCommandEncoder> enc = (id<MTLBlitCommandEncoder>)encoder;
  const struct wmtcmd_base *next = cmd_head;
  while (next) {
    switch ((enum WMTBlitCommandType)next->type) {
    default: break;
    case WMTBlitCommandNop: break;
    case WMTBlitCommandCopyFromBufferToBuffer: {
      const struct wmtcmd_blit_copy_from_buffer_to_buffer *b =
          (const struct wmtcmd_blit_copy_from_buffer_to_buffer *)next;
      [enc copyFromBuffer:(id<MTLBuffer>)b->src sourceOffset:b->src_offset
               toBuffer:(id<MTLBuffer>)b->dst destinationOffset:b->dst_offset
                   size:b->copy_length];
      break;
    }
    case WMTBlitCommandCopyFromBufferToTexture: {
      const struct wmtcmd_blit_copy_from_buffer_to_texture *b =
          (const struct wmtcmd_blit_copy_from_buffer_to_texture *)next;
      [enc copyFromBuffer:(id<MTLBuffer>)b->src sourceOffset:b->src_offset
         sourceBytesPerRow:b->bytes_per_row sourceBytesPerImage:b->bytes_per_image
               sourceSize:MTLSizeMake(b->size.width, b->size.height, b->size.depth)
                toTexture:(id<MTLTexture>)b->dst destinationSlice:b->slice
         destinationLevel:b->level
        destinationOrigin:MTLOriginMake(b->origin.x, b->origin.y, b->origin.z)];
      break;
    }
    case WMTBlitCommandCopyFromTextureToBuffer: {
      const struct wmtcmd_blit_copy_from_texture_to_buffer *b =
          (const struct wmtcmd_blit_copy_from_texture_to_buffer *)next;
      [enc copyFromTexture:(id<MTLTexture>)b->src sourceSlice:b->slice sourceLevel:b->level
             sourceOrigin:MTLOriginMake(b->origin.x, b->origin.y, b->origin.z)
               sourceSize:MTLSizeMake(b->size.width, b->size.height, b->size.depth)
                toBuffer:(id<MTLBuffer>)b->dst destinationOffset:b->offset
    destinationBytesPerRow:b->bytes_per_row destinationBytesPerImage:b->bytes_per_image];
      break;
    }
    case WMTBlitCommandCopyFromTextureToTexture: {
      const struct wmtcmd_blit_copy_from_texture_to_texture *b =
          (const struct wmtcmd_blit_copy_from_texture_to_texture *)next;
      [enc copyFromTexture:(id<MTLTexture>)b->src sourceSlice:b->src_slice sourceLevel:b->src_level
             sourceOrigin:MTLOriginMake(b->src_origin.x, b->src_origin.y, b->src_origin.z)
               sourceSize:MTLSizeMake(b->src_size.width, b->src_size.height, b->src_size.depth)
               toTexture:(id<MTLTexture>)b->dst destinationSlice:b->dst_slice
       destinationLevel:b->dst_level
      destinationOrigin:MTLOriginMake(b->dst_origin.x, b->dst_origin.y, b->dst_origin.z)];
      break;
    }
    case WMTBlitCommandGenerateMipmaps: {
      const struct wmtcmd_blit_generate_mipmaps *b = (const struct wmtcmd_blit_generate_mipmaps *)next;
      [enc generateMipmapsForTexture:(id<MTLTexture>)b->texture];
      break;
    }
    case WMTBlitCommandWaitForFence: {
      const struct wmtcmd_blit_fence_op *b = (const struct wmtcmd_blit_fence_op *)next;
      [enc waitForFence:(id<MTLFence>)b->fence];
      break;
    }
    case WMTBlitCommandUpdateFence: {
      const struct wmtcmd_blit_fence_op *b = (const struct wmtcmd_blit_fence_op *)next;
      [enc updateFence:(id<MTLFence>)b->fence];
      break;
    }
    case WMTBlitCommandFillBuffer: {
      const struct wmtcmd_blit_fillbuffer *b = (const struct wmtcmd_blit_fillbuffer *)next;
      [enc fillBuffer:(id<MTLBuffer>)b->buffer
                range:NSMakeRange((NSUInteger)b->offset, (NSUInteger)b->length)
                value:b->value];
      break;
    }
    case WMTBlitCommandResolveCounters: {
      const struct wmtcmd_blit_resolvecounters *b = (const struct wmtcmd_blit_resolvecounters *)next;
      [enc resolveCounters:(id<MTLCounterSampleBuffer>)b->sample_buffer
                   inRange:NSMakeRange(b->start, b->len)
         destinationBuffer:(id<MTLBuffer>)b->dst_buffer
         destinationOffset:b->dst_offset];
      break;
    }
    }
    next = (const struct wmtcmd_base *)next->next.ptr;
  }
}

// -- Compute command encoder dispatch --

extern "C" void MTLComputeCommandEncoder_encodeCommands(obj_handle_t encoder,
                                                         const struct wmtcmd_base *cmd_head) {
  if (!encoder || !cmd_head) return;
  id<MTLComputeCommandEncoder> enc = (id<MTLComputeCommandEncoder>)encoder;
  MTLSize threadgroup_size = {1, 1, 1};
  const struct wmtcmd_base *next = cmd_head;
  while (next) {
    switch ((enum WMTComputeCommandType)next->type) {
    default: break;
    case WMTComputeCommandNop: break;
    case WMTComputeCommandSetPSO: {
      const struct wmtcmd_compute_setpso *b = (const struct wmtcmd_compute_setpso *)next;
      [enc setComputePipelineState:(id<MTLComputePipelineState>)b->pso];
      threadgroup_size = MTLSizeMake(b->threadgroup_size.width, b->threadgroup_size.height,
                                     b->threadgroup_size.depth);
      break;
    }
    case WMTComputeCommandSetBuffer: {
      const struct wmtcmd_compute_setbuffer *b = (const struct wmtcmd_compute_setbuffer *)next;
      [enc setBuffer:(id<MTLBuffer>)b->buffer offset:b->offset atIndex:b->index];
      break;
    }
    case WMTComputeCommandSetBufferOffset: {
      const struct wmtcmd_compute_setbufferoffset *b = (const struct wmtcmd_compute_setbufferoffset *)next;
      [enc setBufferOffset:b->offset atIndex:b->index];
      break;
    }
    case WMTComputeCommandSetTexture: {
      const struct wmtcmd_compute_settexture *b = (const struct wmtcmd_compute_settexture *)next;
      [enc setTexture:(id<MTLTexture>)b->texture atIndex:b->index];
      break;
    }
    case WMTComputeCommandSetBytes: {
      const struct wmtcmd_compute_setbytes *b = (const struct wmtcmd_compute_setbytes *)next;
      [enc setBytes:(const void *)b->bytes.ptr length:(NSUInteger)b->length atIndex:b->index];
      break;
    }
    case WMTComputeCommandUseResource: {
      const struct wmtcmd_compute_useresource *b = (const struct wmtcmd_compute_useresource *)next;
      [enc useResource:(id<MTLResource>)b->resource usage:(MTLResourceUsage)b->usage];
      break;
    }
    case WMTComputeCommandDispatch: {
      const struct wmtcmd_compute_dispatch *b = (const struct wmtcmd_compute_dispatch *)next;
      [enc dispatchThreadgroups:MTLSizeMake(b->size.width, b->size.height, b->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchThreads: {
      const struct wmtcmd_compute_dispatch *b = (const struct wmtcmd_compute_dispatch *)next;
      [enc dispatchThreads:MTLSizeMake(b->size.width, b->size.height, b->size.depth)
          threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandDispatchIndirect: {
      const struct wmtcmd_compute_dispatch_indirect *b = (const struct wmtcmd_compute_dispatch_indirect *)next;
      [enc dispatchThreadgroupsWithIndirectBuffer:(id<MTLBuffer>)b->indirect_args_buffer
                             indirectBufferOffset:(NSUInteger)b->indirect_args_offset
                            threadsPerThreadgroup:threadgroup_size];
      break;
    }
    case WMTComputeCommandWaitForFence: {
      const struct wmtcmd_compute_fence_op *b = (const struct wmtcmd_compute_fence_op *)next;
      [enc waitForFence:(id<MTLFence>)b->fence];
      break;
    }
    case WMTComputeCommandUpdateFence: {
      const struct wmtcmd_compute_fence_op *b = (const struct wmtcmd_compute_fence_op *)next;
      [enc updateFence:(id<MTLFence>)b->fence];
      break;
    }
    case WMTComputeCommandMemoryBarrier: {
      const struct wmtcmd_compute_memory_barrier *b = (const struct wmtcmd_compute_memory_barrier *)next;
      [enc memoryBarrierWithScope:(MTLBarrierScope)b->scope];
      break;
    }
    }
    next = (const struct wmtcmd_base *)next->next.ptr;
  }
}

// -- Binary archive --

extern "C" obj_handle_t MTLDevice_newBinaryArchive(obj_handle_t device, const char *url,
                                                    obj_handle_t *err_out) {
  if (!device) return NULL_OBJECT_HANDLE;
  @autoreleasepool {
    MTLBinaryArchiveDescriptor *desc = [[MTLBinaryArchiveDescriptor alloc] init];
    if (url && url[0]) {
      NSString *path = [NSString stringWithUTF8String:url];
      desc.url = [NSURL fileURLWithPath:path];
    }
    NSError *err = nil;
    id<MTLBinaryArchive> archive = [(id<MTLDevice>)device newBinaryArchiveWithDescriptor:desc error:&err];
    if (!archive && url && url[0]) {
      // Fallback: create empty archive
      desc.url = nil;
      archive = [(id<MTLDevice>)device newBinaryArchiveWithDescriptor:desc error:&err];
    }
    [desc release];
    if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
    return archive ? (obj_handle_t)archive : NULL_OBJECT_HANDLE;
  }
}

extern "C" void MTLBinaryArchive_serialize(obj_handle_t archive, const char *url, obj_handle_t *err_out) {
  if (!archive || !url) return;
  @autoreleasepool {
    NSString *path = [NSString stringWithUTF8String:url];
    NSURL *nsurl = [NSURL fileURLWithPath:path];
    NSError *err = nil;
    {
      std::lock_guard<std::mutex> archive_lock(binary_archive_mutex());
      [(id<MTLBinaryArchive>)archive serializeToURL:nsurl error:&err];
    }
    if (err_out) *err_out = err ? (obj_handle_t)CFBridgingRetain(err) : NULL_OBJECT_HANDLE;
  }
}

// -- Shader cache path --

extern "C" void WMTGetShaderCachePath(char *out, uint64_t capacity) {
  if (!out || !capacity) return;
  @autoreleasepool {
    NSArray<NSString *> *caches =
        NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES);
    NSString *base = caches.count > 0 ? caches[0] : NSTemporaryDirectory();
    NSString *dir  = [base stringByAppendingPathComponent:@"dxmt9"];
    [[NSFileManager defaultManager] createDirectoryAtPath:dir
                              withIntermediateDirectories:YES attributes:nil error:nil];
    NSString *full = [dir stringByAppendingPathComponent:@"shader-archive.metallib"];
    [full getCString:out maxLength:(NSUInteger)capacity encoding:NSUTF8StringEncoding];
  }
}

// ---------------------------------------------------------------------------
// MTLHeap surface (R-BACK-5.x / R-BACK-14.* small-resource heap pooling).
//
// The descriptor exposes both the packed `resourceOptions` bitfield (matching
// the existing WMTBufferInfo / WMTTextureInfo convention) and the individual
// MTLHeapDescriptor properties (storageMode / cpuCacheMode / hazardTracking).
// We always assign the individual descriptor properties because Apple's
// MTLHeapDescriptor uses individual setters; if the caller only set
// `resourceOptions`, we decode the bitfield. If the caller set individual
// fields, we use those directly. This matches how MTLHeapDescriptor is
// configured natively on macOS.
// ---------------------------------------------------------------------------

namespace {

// Decompose WMTResourceOptions bits into their individual axes. The constants
// match WMTResourceOptions in winemetal.h (storage in bits 4-5, cache in bit 0,
// hazard tracking in bits 8-9).
void decompose_resource_options(enum WMTResourceOptions options,
                                MTLStorageMode *storage,
                                MTLCPUCacheMode *cache,
                                MTLHazardTrackingMode *hazard) {
  *cache = (options & 0x1) ? MTLCPUCacheModeWriteCombined : MTLCPUCacheModeDefaultCache;
  switch (options & 0xF0) {
    case 0:   *storage = MTLStorageModeShared;     break;
    case 16:  *storage = MTLStorageModeManaged;    break;
    case 32:  *storage = MTLStorageModePrivate;    break;
    case 48:  *storage = MTLStorageModeMemoryless; break;
    default:  *storage = MTLStorageModeShared;     break;
  }
  switch (options & 0x300) {
    case 256: *hazard = MTLHazardTrackingModeUntracked; break;
    case 512: *hazard = MTLHazardTrackingModeTracked;   break;
    default:  *hazard = MTLHazardTrackingModeDefault;   break;
  }
}

MTLStorageMode to_metal_storage_mode(enum WMTResourceStorageMode mode) {
  switch (mode) {
    case WMTStorageModeShared:     return MTLStorageModeShared;
    case WMTStorageModeManaged:    return MTLStorageModeManaged;
    case WMTStorageModePrivate:    return MTLStorageModePrivate;
    case WMTStorageModeMemoryless: return MTLStorageModeMemoryless;
  }
  return MTLStorageModeShared;
}

MTLCPUCacheMode to_metal_cpu_cache_mode(enum WMTResourceCpuCacheMode mode) {
  switch (mode) {
    case WMTCpuCacheModeDefault:       return MTLCPUCacheModeDefaultCache;
    case WMTCpuCacheModeWriteCombined: return MTLCPUCacheModeWriteCombined;
  }
  return MTLCPUCacheModeDefaultCache;
}

MTLHazardTrackingMode to_metal_hazard_mode(enum WMTResourceHazardTrackingMode mode) {
  switch (mode) {
    case WMTHazardTrackingModeDefault:   return MTLHazardTrackingModeDefault;
    case WMTHazardTrackingModeUntracked: return MTLHazardTrackingModeUntracked;
    case WMTHazardTrackingModeTracked:   return MTLHazardTrackingModeTracked;
  }
  return MTLHazardTrackingModeDefault;
}

}  // namespace

extern "C" obj_handle_t MTLDevice_newHeapWithDescriptor(obj_handle_t device, struct WMTHeapDescriptor *desc) {
  if (!device || !desc) return NULL_OBJECT_HANDLE;
  @autoreleasepool {
    MTLHeapDescriptor *mtl_desc = [[MTLHeapDescriptor alloc] init];
    mtl_desc.size = (NSUInteger)desc->size;
    mtl_desc.type = (MTLHeapType)desc->type;

    // If any individual axis is set to a non-zero (non-default) value we
    // honor it; otherwise we decode the packed `resourceOptions` field.
    bool any_individual = (desc->storageMode != WMTStorageModeShared) ||
                          (desc->cpuCacheMode != WMTCpuCacheModeDefault) ||
                          (desc->hazardTrackingMode != WMTHazardTrackingModeDefault);
    if (any_individual) {
      mtl_desc.storageMode        = to_metal_storage_mode(desc->storageMode);
      mtl_desc.cpuCacheMode       = to_metal_cpu_cache_mode(desc->cpuCacheMode);
      mtl_desc.hazardTrackingMode = to_metal_hazard_mode(desc->hazardTrackingMode);
      mtl_desc.resourceOptions    = (MTLResourceOptions)desc->resourceOptions;
    } else {
      MTLStorageMode        storage;
      MTLCPUCacheMode       cache;
      MTLHazardTrackingMode hazard;
      decompose_resource_options(desc->resourceOptions, &storage, &cache, &hazard);
      mtl_desc.storageMode        = storage;
      mtl_desc.cpuCacheMode       = cache;
      mtl_desc.hazardTrackingMode = hazard;
      mtl_desc.resourceOptions    = (MTLResourceOptions)desc->resourceOptions;
    }

    id<MTLHeap> heap = [(id<MTLDevice>)device newHeapWithDescriptor:mtl_desc];
    [mtl_desc release];
    return heap ? (obj_handle_t)heap : NULL_OBJECT_HANDLE;
  }
}

extern "C" obj_handle_t MTLHeap_makeBuffer(obj_handle_t heap, uint64_t length, uint64_t options) {
  if (!heap) return NULL_OBJECT_HANDLE;
  id<MTLBuffer> buf = [(id<MTLHeap>)heap newBufferWithLength:(NSUInteger)length
                                                     options:(MTLResourceOptions)options];
  return buf ? (obj_handle_t)buf : NULL_OBJECT_HANDLE;
}

extern "C" obj_handle_t MTLHeap_makeTexture(obj_handle_t heap, struct WMTTextureInfo *info) {
  if (!heap || !info) return NULL_OBJECT_HANDLE;
  MTLTextureDescriptor *desc = [[MTLTextureDescriptor alloc] init];
  fill_texture_descriptor(desc, info);
  id<MTLTexture> tex = [(id<MTLHeap>)heap newTextureWithDescriptor:desc];
  [desc release];
  if (!tex) return NULL_OBJECT_HANDLE;
  info->gpu_resource_id = [tex gpuResourceID]._impl;
  info->mach_port = 0;
  return (obj_handle_t)tex;
}

extern "C" uint64_t MTLHeap_size(obj_handle_t heap) {
  if (!heap) return 0;
  return (uint64_t)[(id<MTLHeap>)heap size];
}

extern "C" uint64_t MTLHeap_usedSize(obj_handle_t heap) {
  if (!heap) return 0;
  return (uint64_t)[(id<MTLHeap>)heap usedSize];
}

extern "C" uint64_t MTLHeap_currentAllocatedSize(obj_handle_t heap) {
  if (!heap) return 0;
  return (uint64_t)[(id<MTLHeap>)heap currentAllocatedSize];
}

extern "C" void MTLHeap_setLabel(obj_handle_t heap, const char *label) {
  if (!heap) return;
  if (label && label[0]) {
    [(id<MTLHeap>)heap setLabel:[NSString stringWithUTF8String:label]];
  } else {
    [(id<MTLHeap>)heap setLabel:nil];
  }
}

extern "C" void MTLRenderCommandEncoder_useHeap(obj_handle_t encoder, obj_handle_t heap) {
  if (!encoder || !heap) return;
  // Apple deprecated bare useHeap: in macOS 13 in favour of useHeap:stages:.
  // Prefer the stages-aware variant when present; default to "all stages"
  // since the dxmt9 caller has not yet split heap residency by stage.
  id<MTLRenderCommandEncoder> enc = (id<MTLRenderCommandEncoder>)encoder;
  if ([enc respondsToSelector:@selector(useHeap:stages:)]) {
    [enc useHeap:(id<MTLHeap>)heap stages:(MTLRenderStageVertex | MTLRenderStageFragment)];
  } else {
    // Pre-macOS-13 fallback: dispatch the deprecated bare useHeap: via
    // objc_msgSend so we don't trip -Wdeprecated-declarations on builds
    // targeting modern SDKs.
    const auto fn = reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend);
    fn(enc, @selector(useHeap:), (id<MTLHeap>)heap);
  }
}

extern "C" void MTLBlitCommandEncoder_useHeap(obj_handle_t encoder, obj_handle_t heap) {
  if (!encoder || !heap) return;
  // MTLBlitCommandEncoder does not expose useHeap on all OS versions; gate
  // on responsiveness so older runtimes degrade gracefully (the resource
  // simply needs to be made resident some other way, e.g., via useResource
  // on a sibling encoder).
  id enc = (id)encoder;
  if ([enc respondsToSelector:@selector(useHeap:)]) {
    const auto fn = reinterpret_cast<void (*)(id, SEL, id)>(objc_msgSend);
    fn(enc, @selector(useHeap:), (id<MTLHeap>)heap);
  }
}

extern "C" void MTLComputeCommandEncoder_useHeap(obj_handle_t encoder, obj_handle_t heap) {
  if (!encoder || !heap) return;
  [(id<MTLComputeCommandEncoder>)encoder useHeap:(id<MTLHeap>)heap];
}

// -- Argument buffer / argument encoder --

extern "C" enum WMTArgumentBuffersTier MTLDevice_argumentBuffersSupport(obj_handle_t device) {
  if (!device) return WMTArgumentBuffersTier1;
  return (enum WMTArgumentBuffersTier)[(id<MTLDevice>)device argumentBuffersSupport];
}

extern "C" obj_handle_t MTLDevice_newArgumentEncoder(obj_handle_t device,
                                                      const struct WMTArgumentDescriptor *descriptors,
                                                      uint32_t descriptor_count) {
  if (!device || !descriptors || !descriptor_count) return NULL_OBJECT_HANDLE;
  @autoreleasepool {
    NSMutableArray<MTLArgumentDescriptor *> *arr = [NSMutableArray arrayWithCapacity:descriptor_count];
    for (uint32_t i = 0; i < descriptor_count; ++i) {
      const struct WMTArgumentDescriptor &src = descriptors[i];
      MTLArgumentDescriptor *desc = [MTLArgumentDescriptor argumentDescriptor];
      switch (src.argumentType) {
        case WMTArgumentTypeBuffer:  desc.dataType = MTLDataTypePointer; break;
        case WMTArgumentTypeTexture: desc.dataType = MTLDataTypeTexture; break;
        case WMTArgumentTypeSampler: desc.dataType = MTLDataTypeSampler; break;
        default:                     desc.dataType = MTLDataTypePointer; break;
      }
      desc.index       = (NSUInteger)src.index;
      desc.arrayLength = (NSUInteger)src.arrayLength;
      switch (src.access) {
        case 1:  desc.access = MTLBindingAccessReadWrite; break;
        case 2:  desc.access = MTLBindingAccessWriteOnly; break;
        case 0:
        default: desc.access = MTLBindingAccessReadOnly;  break;
      }
      if (src.argumentType == WMTArgumentTypeTexture) {
        desc.textureType = (MTLTextureType)src.textureType;
      }
      if (src.argumentType == WMTArgumentTypeBuffer) {
        desc.constantBlockAlignment = (NSUInteger)src.constantBlockAlignment;
      }
      [arr addObject:desc];
    }
    id<MTLArgumentEncoder> encoder = [(id<MTLDevice>)device newArgumentEncoderWithArguments:arr];
    return encoder ? (obj_handle_t)encoder : NULL_OBJECT_HANDLE;
  }
}

extern "C" uint64_t MTLArgumentEncoder_encodedLength(obj_handle_t encoder) {
  if (!encoder) return 0;
  return (uint64_t)[(id<MTLArgumentEncoder>)encoder encodedLength];
}

extern "C" uint64_t MTLArgumentEncoder_alignment(obj_handle_t encoder) {
  if (!encoder) return 0;
  return (uint64_t)[(id<MTLArgumentEncoder>)encoder alignment];
}

extern "C" void MTLArgumentEncoder_setArgumentBuffer(obj_handle_t encoder, obj_handle_t buffer, uint64_t offset) {
  if (!encoder) return;
  [(id<MTLArgumentEncoder>)encoder setArgumentBuffer:(id<MTLBuffer>)buffer offset:(NSUInteger)offset];
}

extern "C" void MTLArgumentEncoder_setBuffer(obj_handle_t encoder, obj_handle_t buffer, uint64_t offset,
                                              uint32_t index) {
  if (!encoder) return;
  [(id<MTLArgumentEncoder>)encoder setBuffer:(id<MTLBuffer>)buffer
                                     offset:(NSUInteger)offset
                                    atIndex:(NSUInteger)index];
}

extern "C" void MTLArgumentEncoder_setTexture(obj_handle_t encoder, obj_handle_t texture, uint32_t index) {
  if (!encoder) return;
  [(id<MTLArgumentEncoder>)encoder setTexture:(id<MTLTexture>)texture atIndex:(NSUInteger)index];
}

extern "C" void MTLArgumentEncoder_setSamplerState(obj_handle_t encoder, obj_handle_t sampler, uint32_t index) {
  if (!encoder) return;
  [(id<MTLArgumentEncoder>)encoder setSamplerState:(id<MTLSamplerState>)sampler atIndex:(NSUInteger)index];
}

extern "C" uint64_t MTLBuffer_gpuResourceID(obj_handle_t buffer) {
  if (!buffer) return 0;
  // Tier 2 argbuf encodes buffers as their 8-byte gpuAddress (the value a shader
  // dereferences directly). Textures/samplers encode their MTLResourceID. We
  // unify the bridge name as gpuResourceID but return the correct argbuf-slot
  // payload per resource kind.
  return (uint64_t)[(id<MTLBuffer>)buffer gpuAddress];
}

extern "C" uint64_t MTLTexture_gpuResourceID(obj_handle_t texture) {
  if (!texture) return 0;
  return (uint64_t)[(id<MTLTexture>)texture gpuResourceID]._impl;
}

extern "C" uint64_t MTLSamplerState_gpuResourceID(obj_handle_t sampler) {
  if (!sampler) return 0;
  return (uint64_t)[(id<MTLSamplerState>)sampler gpuResourceID]._impl;
}
