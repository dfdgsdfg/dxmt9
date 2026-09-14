#include "winemetal_sampling_lineage.hpp"

#import <CommonCrypto/CommonDigest.h>
#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <objc/runtime.h>

#include <atomic>
#include <bit>
#include <mutex>

namespace dxmt9::diagnostics {
namespace {
char samplerKey, fragmentKey, argumentKey, tableKey, textureIdentityKey;
char depthCreationKey, depthBindingKey;
char directBufferKey, bufferIdentityKey;
std::mutex samplingMutex;
std::atomic<bool> samplingFailed{false};
constexpr NSUInteger kMaximumTableBytes = 4096u;
constexpr NSUInteger kMaximumTablesPerBuffer = 64u;

// These dictionaries contain only value snapshots/numeric handles. Attaching
// them never retains a Metal resource and their owner controls their lifetime.
NSMutableDictionary* dictionary(id owner, const void* key) {
  id value = objc_getAssociatedObject(owner, key);
  if (!value) {
    value = [NSMutableDictionary dictionary];
    objc_setAssociatedObject(owner, key, value, OBJC_ASSOCIATION_RETAIN);
  }
  return value;
}

template <typename Fn>
void observe(Fn&& fn) noexcept {
  if (!metalSamplingLineageEnabled() || samplingFailed.load()) return;
  try {
    @autoreleasepool {
      @try {
        std::lock_guard lock(samplingMutex);
        fn();
      } @catch (NSException*) { samplingFailed.store(true); }
    }
  } catch (...) { samplingFailed.store(true); }
}

std::uint64_t textureObservationId(id texture) {
  if (!texture) return 0u;
  NSNumber* previous = objc_getAssociatedObject(texture, &textureIdentityKey);
  if (previous) return previous.unsignedLongLongValue;
  // Called under samplingMutex. The association dies with its real object;
  // a reused address cannot inherit its earlier observation identity.
  static std::uint64_t next = 1u;
  if (next == UINT64_MAX) return 0u;
  const auto value = next++;
  objc_setAssociatedObject(texture, &textureIdentityKey, @(value), OBJC_ASSOCIATION_RETAIN);
  return value;
}

NSDictionary* textureValue(std::uint64_t handle) {
  if (!handle) return @{ @"status": @"explicit-null" };
  id<MTLTexture> texture = (id<MTLTexture>)handle;
  const auto swizzle = texture.swizzle;
  return @{
    @"status": @"issued-descriptor",
    @"object_token": @(textureObservationId(texture)),
    @"type": @(texture.textureType), @"format": @(texture.pixelFormat),
    @"width": @(texture.width), @"height": @(texture.height),
    @"depth": @(texture.depth), @"levels": @(texture.mipmapLevelCount),
    @"array_length": @(texture.arrayLength), @"samples": @(texture.sampleCount),
    @"usage": @(texture.usage), @"storage": @(texture.storageMode),
    @"cache_mode": @(texture.cpuCacheMode), @"hazard_mode": @(texture.hazardTrackingMode),
    @"framebuffer_only": @(texture.framebufferOnly),
    @"parent_present": @(texture.parentTexture != nil),
    @"parent_level": @(texture.parentRelativeLevel),
    @"parent_slice": @(texture.parentRelativeSlice),
    @"swizzle": @[@(swizzle.red), @(swizzle.green), @(swizzle.blue), @(swizzle.alpha)]
  };
}

id samplerValue(std::uint64_t handle) {
  if (!handle) return @{ @"status": @"explicit-null" };
  id value = objc_getAssociatedObject((id)handle, &samplerKey);
  return value ?: @{ @"status": @"unknown-creation" };
}

NSString* tableDigest(id<MTLBuffer> buffer, NSUInteger offset, NSUInteger length) {
  if (!buffer || buffer.storageMode != MTLStorageModeShared || !length ||
      length > kMaximumTableBytes || offset > buffer.length ||
      length > buffer.length - offset || !buffer.contents) return nil;
  unsigned char hash[CC_SHA256_DIGEST_LENGTH];
  CC_SHA256(static_cast<const char*>(buffer.contents) + offset,
            static_cast<CC_LONG>(length), hash);
  char hex[65];
  constexpr char digits[] = "0123456789abcdef";
  for (unsigned i = 0; i < CC_SHA256_DIGEST_LENGTH; ++i) {
    hex[2 * i] = digits[hash[i] >> 4];
    hex[2 * i + 1] = digits[hash[i] & 15];
  }
  hex[64] = 0;
  return [NSString stringWithUTF8String:hex];
}
}  // namespace

void noteMetalDirectBuffer(std::uint64_t encoder, bool vertex, std::uint32_t index,
                           std::uint64_t buffer, std::uint64_t offset, bool offsetOnly) noexcept {
  observe([&] {
    if (!encoder || (index != 0u && index != 3u)) return;
    auto* bindings = dictionary((id)encoder, &directBufferKey);
    NSString* key = [NSString stringWithFormat:@"%c_%u", vertex ? 'v' : 'f', index];
    if (offsetOnly) {
      NSDictionary* old = bindings[key];
      bindings[key] = [old[@"kind"] isEqual:@"buffer"]
          ? @{@"kind": @"buffer", @"token": old[@"token"], @"offset": @(offset)}
          : @{@"kind": @"unknown-offset-origin"};
      return;
    }
    if (!buffer) { bindings[key] = @{@"kind": @"explicit-null"}; return; }
    NSNumber* token = objc_getAssociatedObject((id)buffer, &bufferIdentityKey);
    if (!token) {
      static std::uint64_t next = 1u;  // serialized by samplingMutex
      if (next == UINT64_MAX) { samplingFailed.store(true); return; }
      token = @(next++);
      objc_setAssociatedObject((id)buffer, &bufferIdentityKey, token, OBJC_ASSOCIATION_RETAIN);
    }
    bindings[key] = @{@"kind": @"buffer", @"token": token, @"offset": @(offset)};
  });
}

void noteMetalDirectBytes(std::uint64_t encoder, bool vertex, std::uint32_t index) noexcept {
  observe([&] {
    if (!encoder || (index != 0u && index != 3u)) return;
    NSString* key = [NSString stringWithFormat:@"%c_%u", vertex ? 'v' : 'f', index];
    dictionary((id)encoder, &directBufferKey)[key] = @{@"kind": @"inline-bytes"};
  });
}

const char* metalDirectBufferMatch(std::uint64_t encoder, bool vertex, std::uint32_t index,
                                   std::uint64_t expectedBuffer, std::uint64_t expectedOffset) noexcept {
  const char* result = "unknown";
  observe([&] {
    if (!encoder || (index != 0u && index != 3u)) return;
    NSDictionary* bindings = objc_getAssociatedObject((id)encoder, &directBufferKey);
    NSDictionary* current = bindings[[NSString stringWithFormat:@"%c_%u", vertex ? 'v' : 'f', index]];
    if (!current) { result = "unobserved"; return; }
    if ([current[@"kind"] isEqual:@"inline-bytes"]) { result = "inline-bytes"; return; }
    if ([current[@"kind"] isEqual:@"explicit-null"]) { result = "explicit-null"; return; }
    if (![current[@"kind"] isEqual:@"buffer"]) return;
    NSNumber* expectedToken = expectedBuffer
        ? objc_getAssociatedObject((id)expectedBuffer, &bufferIdentityKey) : nil;
    result = expectedToken && [expectedToken isEqual:current[@"token"]] &&
        [current[@"offset"] unsignedLongLongValue] == expectedOffset ? "match" : "mismatch";
  });
  return samplingFailed.load() ? "unknown" : result;
}

std::uint64_t metalTextureObservationId(std::uint64_t texture) noexcept {
  std::uint64_t result = 0u;
  observe([&] { result = textureObservationId((id)texture); });
  return samplingFailed.load() ? 0u : result;
}

void noteMetalSamplerCreation(std::uint64_t sampler, const void* descriptor) noexcept {
  observe([&] {
    if (!sampler || !descriptor) return;
    auto* d = (MTLSamplerDescriptor*)descriptor;
    NSDictionary* value = @{
      @"status": @"issued-descriptor", @"min": @(d.minFilter), @"mag": @(d.magFilter),
      @"mip": @(d.mipFilter), @"s": @(d.sAddressMode), @"t": @(d.tAddressMode),
      @"r": @(d.rAddressMode), @"border": @(d.borderColor), @"compare": @(d.compareFunction),
      @"lod_min_bits": @(std::bit_cast<std::uint32_t>(d.lodMinClamp)),
      @"lod_max_bits": @(std::bit_cast<std::uint32_t>(d.lodMaxClamp)),
      @"anisotropy": @(d.maxAnisotropy), @"normalized": @(d.normalizedCoordinates),
      @"lod_average": @(d.lodAverage), @"argument_buffers": @(d.supportArgumentBuffers)
    };
    id previous = objc_getAssociatedObject((id)sampler, &samplerKey);
    if (previous && ![previous isEqual:value])
      value = @{ @"status": @"ambiguous-creation" };
    objc_setAssociatedObject((id)sampler, &samplerKey, value, OBJC_ASSOCIATION_RETAIN);
  });
}

void noteMetalFragmentTexture(std::uint64_t encoder, std::uint64_t texture,
                              std::uint32_t index) noexcept {
  observe([&] {
    if (encoder && index < 16u)
      dictionary((id)encoder, &fragmentKey)[[NSString stringWithFormat:@"texture_%u", index]] =
          textureValue(texture);
  });
}

void noteMetalFragmentSampler(std::uint64_t encoder, std::uint64_t sampler,
                              std::uint32_t index) noexcept {
  observe([&] {
    if (encoder && index < 16u)
      dictionary((id)encoder, &fragmentKey)[[NSString stringWithFormat:@"sampler_%u", index]] =
          samplerValue(sampler);
  });
}

void noteMetalFragmentTable(std::uint64_t encoder, std::uint64_t buffer,
                            std::uint64_t offset, bool offsetOnly) noexcept {
  observe([&] {
    if (!encoder) return;
    auto* state = dictionary((id)encoder, &fragmentKey);
    if (offsetOnly) buffer = [state[@"table_buffer"] unsignedLongLongValue];
    state[@"table_buffer"] = @(buffer);
    state[@"table_offset"] = @(offset);
  });
}

void noteMetalArgumentTable(std::uint64_t encoder, std::uint64_t buffer,
                            std::uint64_t offset) noexcept {
  observe([&] {
    if (!encoder) return;
    objc_setAssociatedObject((id)encoder, &argumentKey,
        @{ @"buffer": @(buffer), @"offset": @(offset) }, OBJC_ASSOCIATION_RETAIN);
    // Reopening a slice invalidates its old provenance, even if address/bytes
    // repeat. Only subsequent actual writes establish the new table's entries.
    if (buffer) [dictionary((id)buffer, &tableKey) removeObjectForKey:@(offset)];
  });
}

void prepareMetalArgumentWrite(std::uint64_t encoder) noexcept {
  observe([&] {
    if (!encoder) return;
    NSDictionary* target = objc_getAssociatedObject((id)encoder, &argumentKey);
    id<MTLBuffer> buffer = (id<MTLBuffer>)[target[@"buffer"] unsignedLongLongValue];
    if (!buffer) return;
    NSNumber* offset = target[@"offset"];
    auto* tables = dictionary(buffer, &tableKey);
    NSDictionary* table = tables[offset];
    if (!table) return;
    NSString* before = tableDigest(buffer, offset.unsignedLongLongValue,
                                    [table[@"length"] unsignedLongLongValue]);
    // A later observed write must not bless stale entries after an unobserved
    // raw table mutation. Drop the old witness before executing that write.
    if (!before || ![before isEqual:table[@"sha256"]])
      [tables removeObjectForKey:offset];
  });
}

void noteMetalArgumentWrite(std::uint64_t encoder, std::uint64_t resource,
                            std::uint32_t index, unsigned kind) noexcept {
  observe([&] {
    if (!encoder || index >= 32u) return;
    NSDictionary* target = objc_getAssociatedObject((id)encoder, &argumentKey);
    id<MTLBuffer> buffer = (id<MTLBuffer>)[target[@"buffer"] unsignedLongLongValue];
    if (!buffer) return;
    NSUInteger offset = [target[@"offset"] unsignedLongLongValue];
    NSUInteger length = [(id<MTLArgumentEncoder>)encoder encodedLength];
    NSString* digest = tableDigest(buffer, offset, length);
    auto* tables = dictionary(buffer, &tableKey);
    if (!digest) { [tables removeObjectForKey:@(offset)]; return; }
    auto* table = (NSMutableDictionary*)tables[@(offset)];
    if (!table) {
      // Bounded cache eviction yields unknown, never reuses an older witness.
      if (tables.count >= kMaximumTablesPerBuffer) [tables removeAllObjects];
      table = [NSMutableDictionary dictionary];
      tables[@(offset)] = table;
    }
    NSString* key = [NSString stringWithFormat:@"entry_%u", index];
    if (kind == 1u) table[key] = @{ @"kind": @"texture", @"value": textureValue(resource) };
    else if (kind == 2u) table[key] = @{ @"kind": @"sampler", @"value": samplerValue(resource) };
    else [table removeObjectForKey:key];
    table[@"length"] = @(length);
    table[@"sha256"] = digest;
  });
}

std::string metalSamplingBindingsJson(std::uint64_t encoder) noexcept {
  std::string result;
  observe([&] {
    if (!encoder) return;
    NSDictionary* state = objc_getAssociatedObject((id)encoder, &fragmentKey);
    auto* direct = [NSMutableDictionary dictionary];
    for (NSString* key in state)
      if ([key hasPrefix:@"texture_"] || [key hasPrefix:@"sampler_"]) direct[key] = state[key];
    id<MTLBuffer> buffer = (id<MTLBuffer>)[state[@"table_buffer"] unsignedLongLongValue];
    NSNumber* offset = state[@"table_offset"];
    NSDictionary* table = buffer ? objc_getAssociatedObject(buffer, &tableKey) : nil;
    table = offset ? table[offset] : nil;
    NSString* digest = table ? tableDigest(buffer, offset.unsignedLongLongValue,
                                           [table[@"length"] unsignedLongLongValue]) : nil;
    bool known = digest && [digest isEqual:table[@"sha256"]];
    NSDictionary* snapshot = @{
      @"schema": @"dxmt9.metal_sampling_binding.v1", @"direct": direct,
      @"argument_table_known": @(known),
      @"argument_table": known ? table : (id)[NSNull null],
      @"scope": @"issued-descriptors-not-contents-or-shader-consumption"
    };
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:snapshot
        options:NSJSONWritingSortedKeys error:nil];
    if (bytes && bytes.length <= 65536u)
      result.assign(static_cast<const char*>(bytes.bytes), bytes.length);
  });
  if (samplingFailed.load()) result.clear();
  return result;
}

void noteMetalDepthStencilCreation(std::uint64_t state, const void* descriptor) noexcept {
  observe([&] {
    if (!state || !descriptor) return;
    auto* desc = (MTLDepthStencilDescriptor*)descriptor;
    const auto stencil = [](MTLStencilDescriptor* d) -> id {
      if (!d) return [NSNull null];
      return @{@"compare": @(d.stencilCompareFunction), @"read_mask": @(d.readMask),
        @"write_mask": @(d.writeMask), @"fail": @(d.stencilFailureOperation),
        @"depth_fail": @(d.depthFailureOperation), @"pass": @(d.depthStencilPassOperation)};
    };
    NSDictionary* value = @{@"status": @"issued-descriptor",
      @"depth_compare": @(desc.depthCompareFunction), @"depth_write": @(desc.depthWriteEnabled),
      @"front": stencil(desc.frontFaceStencil), @"back": stencil(desc.backFaceStencil)};
    id previous = objc_getAssociatedObject((id)state, &depthCreationKey);
    if (previous && ![previous isEqual:value]) value = @{@"status": @"ambiguous-creation"};
    objc_setAssociatedObject((id)state, &depthCreationKey, value, OBJC_ASSOCIATION_RETAIN);
  });
}

void noteMetalDepthStencilBinding(std::uint64_t encoder, std::uint64_t state) noexcept {
  observe([&] {
    if (!encoder) return;
    id value = state ? objc_getAssociatedObject((id)state, &depthCreationKey) : nil;
    dictionary((id)encoder, &depthBindingKey)[@"state"] = state
        ? (value ?: @{@"status": @"unknown-creation"}) : @{@"status": @"explicit-null"};
  });
}

void noteMetalStencilReference(std::uint64_t encoder, std::uint32_t reference) noexcept {
  observe([&] {
    if (encoder) dictionary((id)encoder, &depthBindingKey)[@"reference"] = @(reference);
  });
}

std::string metalDepthStencilBindingsJson(std::uint64_t encoder) noexcept {
  std::string result;
  observe([&] {
    if (!encoder) return;
    NSDictionary* state = objc_getAssociatedObject((id)encoder, &depthBindingKey);
    NSDictionary* snapshot = @{
      @"schema": @"dxmt9.metal_depth_stencil_binding.v1",
      @"depth_stencil": state[@"state"] ?: @{@"status": @"unobserved"},
      @"stencil_reference_known": @(state[@"reference"] != nil),
      @"stencil_reference": state[@"reference"] ?: [NSNull null],
      @"scope": @"issued-descriptors-not-attachment-contents-or-gpu-tests"};
    NSData* bytes = [NSJSONSerialization dataWithJSONObject:snapshot
        options:NSJSONWritingSortedKeys error:nil];
    if (bytes && bytes.length <= 4096u)
      result.assign(static_cast<const char*>(bytes.bytes), bytes.length);
  });
  if (samplingFailed.load()) result.clear();
  return result;
}
}  // namespace dxmt9::diagnostics
