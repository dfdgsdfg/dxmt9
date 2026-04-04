#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static const char kStubError[] = "";
static const char kLogPath[] = "C:\\dxmt9-anno1701-bypass.log";
static const uint32_t kBinkMagic = 0x4b4e4942u;
static const uint32_t kFakeVideoWidth = 640u;
static const uint32_t kFakeVideoHeight = 480u;
static const uint32_t kFakeFrameRate = 30u;
static const uint32_t kFakeFrameRateDiv = 1u;
static const uint32_t kFakeBinkHandleBytes = 4096u;
static const int32_t kBinkSurfaceUnknown = -1;
static const int32_t kBinkSurface1 = 1;
static const int32_t kBinkSurface3 = 3;
static const int32_t kBinkSurface5 = 5;
static const int32_t kBinkSurface7 = 7;
static const int32_t kBinkSurface8 = 8;
static const int32_t kBinkSurface9 = 9;
static const int32_t kBinkSurface10 = 10;
static const int32_t kBinkSurfaceYUY2 = 13;
static const int32_t kBinkSurfaceUYVY = 14;

/* The real game ships Bink 1.8f. Older bypass stubs crashed because Anno reads
 * fields directly from the returned HBINK and our handle was only 12 bytes.
 * This stub mirrors the stable 1.x header fields and leaves generous padding
 * behind them so direct field access stays within valid heap memory. */
typedef struct FakeBinkHeader {
  uint32_t width;
  uint32_t height;
  uint32_t frames;
  uint32_t frame_num;
  uint32_t last_frame_num;
  uint32_t frame_rate;
  uint32_t frame_rate_div;
  uint32_t read_error;
  uint32_t open_flags;
  uint32_t bink_type;
  uint32_t size;
  uint32_t frame_size;
  uint32_t snd_size;
  uint8_t reserved[256];
} FakeBinkHeader;

typedef struct FakeBinkHandle {
  FakeBinkHeader header;
  uint8_t opaque[kFakeBinkHandleBytes - sizeof(FakeBinkHeader) - sizeof(uint32_t) * 6];
  uint32_t magic;
  int32_t paused;
  uint32_t video_on;
  uint32_t sound_on;
  uint32_t completed;
  uint32_t next_frame_tick;
} FakeBinkHandle;

static void stub_log(const char* fmt, ...) {
  HANDLE file = CreateFileA(kLogPath,
                            FILE_APPEND_DATA,
                            FILE_SHARE_READ | FILE_SHARE_WRITE,
                            NULL,
                            OPEN_ALWAYS,
                            FILE_ATTRIBUTE_NORMAL,
                            NULL);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  DWORD ignored = 0;
  char buffer[512];
  int prefix = snprintf(buffer, sizeof(buffer), "[bink] ");
  if (prefix < 0) {
    CloseHandle(file);
    return;
  }
  va_list args;
  va_start(args, fmt);
  int written = vsnprintf(buffer + prefix, sizeof(buffer) - (size_t)prefix, fmt, args);
  va_end(args);
  if (written < 0) {
    CloseHandle(file);
    return;
  }
  size_t total = strnlen(buffer, sizeof(buffer));
  if (total + 1 < sizeof(buffer)) {
    buffer[total++] = '\n';
  }
  WriteFile(file, buffer, (DWORD)total, &ignored, NULL);
  CloseHandle(file);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID reserved) {
  (void)instance;
  (void)reason;
  (void)reserved;
  return TRUE;
}

static const char* safe_bink_path(const char* path) {
  if (!path) {
    return "(null)";
  }
  if ((uintptr_t)path < 0x10000u) {
    return "(non-string)";
  }
  if (IsBadStringPtrA(path, 260)) {
    return "(invalid)";
  }
  return path;
}

static int reject_low_bink_open(void) {
  static int cached = -1;
  if (cached >= 0) {
    return cached;
  }
  {
    const char* env = getenv("DXMT9_BINK_REJECT_LOW_OPEN");
    cached = (env && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static uint32_t low_bink_max_fake_opens(void) {
  static uint32_t cached = UINT32_MAX;
  if (cached != UINT32_MAX) {
    return cached;
  }
  cached = 1u;
  {
    const char* env = getenv("DXMT9_BINK_LOW_OPEN_FAKE_COUNT");
    if (env && env[0] != '\0') {
      char* end = NULL;
      unsigned long value = strtoul(env, &end, 0);
      if (end && *end == '\0') {
        cached = (uint32_t)value;
      }
    }
  }
  return cached;
}

static uint32_t consume_low_bink_open_slot(uintptr_t key) {
  enum { kTrackedLowOpens = 16 };
  typedef struct LowOpenCounter {
    uintptr_t key;
    uint32_t count;
  } LowOpenCounter;
  static LowOpenCounter counters[kTrackedLowOpens];

  for (size_t i = 0; i < kTrackedLowOpens; ++i) {
    if (counters[i].key == key) {
      counters[i].count += 1u;
      return counters[i].count;
    }
  }
  for (size_t i = 0; i < kTrackedLowOpens; ++i) {
    if (counters[i].key == 0u) {
      counters[i].key = key;
      counters[i].count = 1u;
      return 1u;
    }
  }
  return UINT32_MAX;
}

static uint32_t fake_bink_frames(void) {
  static uint32_t cached = UINT32_MAX;
  if (cached != UINT32_MAX) {
    return cached;
  }
  cached = 1u;
  {
    const char* env = getenv("DXMT9_BINK_FAKE_FRAMES");
    if (env && env[0] != '\0') {
      char* end = NULL;
      unsigned long value = strtoul(env, &end, 0);
      if (end && *end == '\0' && value > 0ul) {
        cached = (uint32_t)value;
      }
    }
  }
  return cached;
}

static int32_t fake_bink_surface_type_override(void) {
  static int initialized = 0;
  static int32_t cached = INT32_MIN;
  if (initialized) {
    return cached;
  }
  initialized = 1;
  {
    const char* env = getenv("DXMT9_BINK_SURFACE_TYPE");
    if (env && env[0] != '\0') {
      char* end = NULL;
      long value = strtol(env, &end, 0);
      if (end && *end == '\0') {
        cached = (int32_t)value;
      }
    }
  }
  return cached;
}

static int32_t map_bink_surface_type(uint32_t format) {
  switch (format) {
    case 20u: return kBinkSurface1;
    case 21u: return kBinkSurface3;
    case 22u: return kBinkSurface5;
    case 23u: return kBinkSurface10;
    case 24u: return kBinkSurface1;
    case 25u: return kBinkSurface9;
    case 26u: return kBinkSurface8;
    case 30u: return kBinkSurface7;
    case 0x32595559u: return kBinkSurfaceYUY2;
    case 0x59565955u: return kBinkSurfaceUYVY;
    default: return kBinkSurfaceUnknown;
  }
}

static int32_t query_bink_surface_type(void* object) {
  uint32_t format = 0u;
  void*** com = (void***)object;
  typedef HRESULT(__stdcall *QueryFormatFn)(void* self, void* out_desc);
  QueryFormatFn query_format = NULL;

  if (!object || !com || !*com) {
    return kBinkSurfaceUnknown;
  }

  query_format = (QueryFormatFn)(*com)[12];
  if (!query_format) {
    return kBinkSurfaceUnknown;
  }

  {
    uint32_t desc[8];
    memset(desc, 0, sizeof(desc));
    if (FAILED(query_format(object, desc))) {
      return kBinkSurfaceUnknown;
    }
    format = desc[0];
  }

  return map_bink_surface_type(format);
}

static int realtime_bink_wait(void) {
  static int cached = -1;
  if (cached >= 0) {
    return cached;
  }
  {
    const char* env = getenv("DXMT9_BINK_REALTIME");
    cached = (env && env[0] != '\0' && strcmp(env, "0") != 0) ? 1 : 0;
  }
  return cached;
}

static uint32_t fake_bink_frame_delay_ms(const FakeBinkHandle* handle) {
  uint32_t rate = handle && handle->header.frame_rate ? handle->header.frame_rate : kFakeFrameRate;
  uint32_t div = handle && handle->header.frame_rate_div ? handle->header.frame_rate_div : kFakeFrameRateDiv;
  const uint32_t denom = rate / (div ? div : 1u);
  if (denom == 0u) {
    return 33u;
  }
  return (uint32_t)(1000u / denom ? 1000u / denom : 1u);
}

static FakeBinkHandle* fake_bink_from_handle(void* handle) {
  FakeBinkHandle* bink = (FakeBinkHandle*)handle;
  if (!bink || bink->magic != kBinkMagic) {
    return NULL;
  }
  return bink;
}

__declspec(dllexport) void* __stdcall BinkOpen(const char* path, uint32_t flags) {
  const int low_probe = ((uintptr_t)path < 0x10000u);
  const uint32_t low_probe_open_count = low_probe ? consume_low_bink_open_slot((uintptr_t)path) : 0u;
  const uint32_t low_probe_max_fakes = low_probe ? low_bink_max_fake_opens() : 0u;
  if (low_probe && reject_low_bink_open()) {
    stub_log("BinkOpen path=%p '%s' flags=0x%x -> rejected low-address open",
             path, safe_bink_path(path), flags);
    return NULL;
  }
  if (low_probe && low_probe_open_count > low_probe_max_fakes) {
    stub_log("BinkOpen path=%p '%s' flags=0x%x -> low-address open limit hit count=%u max=%u",
             path, safe_bink_path(path), flags, low_probe_open_count, low_probe_max_fakes);
    return NULL;
  }
  FakeBinkHandle* handle = (FakeBinkHandle*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
                                                      sizeof(FakeBinkHandle));
  if (!handle) {
    stub_log("BinkOpen path=%p '%s' flags=0x%x -> alloc failed",
             path, safe_bink_path(path), flags);
    return NULL;
  }
  handle->header.width = kFakeVideoWidth;
  handle->header.height = kFakeVideoHeight;
  handle->header.frames = low_probe ? 0u : fake_bink_frames();
  handle->header.frame_num = low_probe ? 0u : 1u;
  handle->header.last_frame_num = 0u;
  handle->header.frame_rate = kFakeFrameRate;
  handle->header.frame_rate_div = kFakeFrameRateDiv;
  handle->header.read_error = 0u;
  handle->header.open_flags = flags;
  handle->header.bink_type = 0u;
  handle->header.size = 1u;
  handle->header.frame_size = 0u;
  handle->header.snd_size = 0u;
  handle->magic = kBinkMagic;
  handle->video_on = low_probe ? 0u : 1u;
  handle->sound_on = low_probe ? 0u : 1u;
  handle->completed = low_probe ? 1u : 0u;
  handle->next_frame_tick = GetTickCount();
  stub_log("BinkOpen path=%p '%s' flags=0x%x -> fake handle=%p size=%u bytes low_probe=%d openCount=%u max=%u completed=%u",
           path, safe_bink_path(path), flags, handle, (unsigned)sizeof(FakeBinkHandle),
           low_probe, low_probe_open_count, low_probe_max_fakes, handle->completed);
  return handle;
}

__declspec(dllexport) const char* __stdcall BinkGetError(void) {
  stub_log("BinkGetError");
  return kStubError;
}

__declspec(dllexport) int32_t __stdcall BinkDX9SurfaceType(void* device) {
  const int32_t override = fake_bink_surface_type_override();
  const int32_t detected = query_bink_surface_type(device);
  const int32_t result = (override != INT32_MIN) ? override : detected;
  stub_log("BinkDX9SurfaceType device=%p detected=%d override=%d result=%d",
           device, detected, override, result);
  return result;
}

__declspec(dllexport) uint32_t __stdcall BinkShouldSkip(void* handle) {
  FakeBinkHandle* bink = fake_bink_from_handle(handle);
  uint32_t skip = (bink && bink->completed) ? 1u : 0u;
  stub_log("BinkShouldSkip handle=%p -> %u", handle, skip);
  return skip;
}

__declspec(dllexport) uint32_t __stdcall BinkWait(void* handle) {
  FakeBinkHandle* bink = fake_bink_from_handle(handle);
  if (bink && realtime_bink_wait() && !bink->completed && !bink->paused) {
    const uint32_t now = GetTickCount();
    if ((int32_t)(bink->next_frame_tick - now) > 0) {
      stub_log("BinkWait handle=%p completed=%u paused=%d -> wait", handle, bink->completed, bink->paused);
      return 1u;
    }
  }
  stub_log("BinkWait handle=%p completed=%u paused=%d",
           handle, bink ? bink->completed : 0u, bink ? bink->paused : 0);
  return 0;
}

__declspec(dllexport) void __stdcall BinkNextFrame(void* handle) {
  FakeBinkHandle* bink = fake_bink_from_handle(handle);
  if (bink && !bink->paused) {
    if (bink->header.frame_num <= bink->header.frames) {
      bink->header.last_frame_num = bink->header.frame_num;
      ++bink->header.frame_num;
    }
    if (bink->header.frame_num > bink->header.frames) {
      bink->completed = 1u;
    } else if (realtime_bink_wait()) {
      bink->next_frame_tick = GetTickCount() + fake_bink_frame_delay_ms(bink);
    }
  }
  stub_log("BinkNextFrame handle=%p frame=%u/%u last=%u completed=%u",
           handle,
           bink ? bink->header.frame_num : 0u,
           bink ? bink->header.frames : 0u,
           bink ? bink->header.last_frame_num : 0u,
           bink ? bink->completed : 0u);
}

__declspec(dllexport) int32_t __stdcall BinkDoFrame(void* handle) {
  FakeBinkHandle* bink = fake_bink_from_handle(handle);
  if (bink && !bink->paused && bink->header.frame_num <= bink->header.frames) {
    bink->header.last_frame_num = bink->header.frame_num;
    if (bink->header.frame_num == bink->header.frames) {
      bink->completed = 1u;
    }
  }
  stub_log("BinkDoFrame handle=%p frame=%u/%u last=%u completed=%u",
           handle,
           bink ? bink->header.frame_num : 0u,
           bink ? bink->header.frames : 0u,
           bink ? bink->header.last_frame_num : 0u,
           bink ? bink->completed : 0u);
  return 1;
}

__declspec(dllexport) void __stdcall BinkClose(void* handle) {
  stub_log("BinkClose handle=%p", handle);
  if (handle) {
    HeapFree(GetProcessHeap(), 0, handle);
  }
}

__declspec(dllexport) int32_t __stdcall BinkPause(void* handle, int32_t pause) {
  FakeBinkHandle* bink = fake_bink_from_handle(handle);
  if (bink) {
    bink->paused = pause;
  }
  stub_log("BinkPause handle=%p pause=%d", handle, pause);
  return 0;
}

__declspec(dllexport) int32_t __stdcall BinkService(void* handle) {
  stub_log("BinkService handle=%p", handle);
  (void)handle;
  return 0;
}

__declspec(dllexport) int32_t __stdcall BinkSetVideoOnOff(void* handle, int32_t enabled) {
  FakeBinkHandle* bink = fake_bink_from_handle(handle);
  if (bink) {
    bink->video_on = enabled ? 1u : 0u;
  }
  stub_log("BinkSetVideoOnOff handle=%p enabled=%d", handle, enabled);
  return 0;
}

__declspec(dllexport) int32_t __stdcall BinkSetSoundTrack(uint32_t total_tracks,
                                                          const uint32_t* tracks) {
  stub_log("BinkSetSoundTrack total=%u tracks=%p", total_tracks, tracks);
  (void)total_tracks;
  (void)tracks;
  return 0;
}

__declspec(dllexport) int32_t __stdcall BinkSetSoundSystem(void* open_fn, uint32_t param) {
  stub_log("BinkSetSoundSystem open_fn=%p param=%u", open_fn, param);
  (void)open_fn;
  (void)param;
  return 0;
}

__declspec(dllexport) int32_t __stdcall BinkOpenMiles(void* state) {
  stub_log("BinkOpenMiles state=%p", state);
  (void)state;
  return 1;
}

__declspec(dllexport) int32_t __stdcall BinkSetMixBinVolumes(void* handle,
                                                             uint32_t track_id,
                                                             const int32_t* mix_bins,
                                                             const int32_t* volumes,
                                                             uint32_t count) {
  stub_log("BinkSetMixBinVolumes handle=%p track=%u count=%u", handle, track_id, count);
  (void)handle;
  (void)track_id;
  (void)mix_bins;
  (void)volumes;
  (void)count;
  return 0;
}

__declspec(dllexport) int32_t __stdcall BinkSetVolume(void* handle,
                                                      uint32_t track_id,
                                                      int32_t volume) {
  FakeBinkHandle* bink = fake_bink_from_handle(handle);
  if (bink) {
    bink->sound_on = volume > 0 ? 1u : 0u;
  }
  stub_log("BinkSetVolume handle=%p track=%u volume=%d", handle, track_id, volume);
  (void)handle;
  (void)track_id;
  (void)volume;
  return 0;
}

__declspec(dllexport) int32_t __stdcall BinkCopyToBufferRect(void* handle,
                                                             void* dest,
                                                             int32_t dest_pitch,
                                                             uint32_t dest_height,
                                                             uint32_t dest_x,
                                                             uint32_t dest_y,
                                                             uint32_t src_x,
                                                             uint32_t src_y,
                                                             uint32_t src_w,
                                                             uint32_t src_h,
                                                             uint32_t flags) {
  stub_log("BinkCopyToBufferRect handle=%p dest=%p pitch=%d size=%ux%u flags=0x%x",
           handle, dest, dest_pitch, src_w, src_h, flags);
  if (dest && dest_pitch > 0 && src_h > 0) {
    size_t bytes = (size_t)dest_pitch * (size_t)src_h;
    memset(dest, 0, bytes);
  }
  (void)handle;
  (void)dest_height;
  (void)dest_x;
  (void)dest_y;
  (void)src_x;
  (void)src_y;
  (void)src_w;
  (void)src_h;
  (void)flags;
  return 0;
}
