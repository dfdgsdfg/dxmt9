#import <AppKit/AppKit.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include "../winemetal.h"

#include <dlfcn.h>

namespace {

void execute_on_main(dispatch_block_t block) {
  if ([NSThread isMainThread]) {
    block();
  } else {
    dispatch_sync(dispatch_get_main_queue(), block);
  }
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

extern "C" void NSObject_retain(obj_handle_t obj) {
  if (!obj) {
    return;
  }
  [(id)obj retain];
}

extern "C" void NSObject_release(obj_handle_t obj) {
  if (!obj) {
    return;
  }
  [(id)obj release];
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
  auto pfn_get_win_data = macdrv_functions ? macdrv_functions->get_win_data
                                           : resolveMacdrvSymbol<macdrv_win_data *(*)(HWND)>("get_win_data");
  auto pfn_release_win_data = macdrv_functions ? macdrv_functions->release_win_data
                                               : resolveMacdrvSymbol<void (*)(macdrv_win_data *)>("release_win_data");
  auto pfn_create_metal_view = macdrv_functions
                                   ? macdrv_functions->macdrv_view_create_metal_view
                                   : resolveMacdrvSymbol<macdrv_metal_view (*)(macdrv_view, macdrv_metal_device)>(
                                         "macdrv_view_create_metal_view");
  auto pfn_get_metal_layer = macdrv_functions
                                 ? macdrv_functions->macdrv_view_get_metal_layer
                                 : resolveMacdrvSymbol<macdrv_metal_layer (*)(macdrv_metal_view)>(
                                       "macdrv_view_get_metal_layer");

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

extern "C" void ReleaseMetalView(obj_handle_t view) {
  auto *macdrv_functions = resolveMacdrvSymbol<macdrv_functions_t *>("macdrv_functions");
  auto pfn_release_metal_view = macdrv_functions
                                    ? macdrv_functions->macdrv_view_release_metal_view
                                    : resolveMacdrvSymbol<void (*)(macdrv_metal_view)>(
                                          "macdrv_view_release_metal_view");
  if (pfn_release_metal_view && view) {
    pfn_release_metal_view((macdrv_metal_view)view);
  }
}
