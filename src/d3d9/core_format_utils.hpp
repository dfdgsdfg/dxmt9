#pragma once

#include "dxmt9/core.hpp"

#include <span>
#include <string>
#include <vector>

namespace dxmt9::core {

// Pure pixel-format helpers extracted from core_resources.cpp. These
// functions perform byte/pixel arithmetic only — they do not touch D3D9
// resource state or Metal-side state and remain unit-testable in isolation.

u32 pitchForFormat(Format format, u32 width);

void fillBuffer(std::vector<u8> &bytes, u32 pitch, u32 width, u32 height,
                Format format, const Rect *rect, ColorRGBA color);

bool copyPixels(std::vector<u8> &dst, u32 dstPitch, u32 dstWidth, u32 dstHeight,
                Format dstFormat, const std::vector<u8> &src, u32 srcPitch,
                u32 srcWidth, u32 srcHeight, Format srcFormat);

bool stretchPixels(std::vector<u8> &dst, u32 dstPitch, u32 dstWidth,
                   u32 dstHeight, Format dstFormat,
                   const std::vector<u8> &src, u32 srcPitch, u32 srcWidth,
                   u32 srcHeight, Format srcFormat);

void fillDepthStencil(std::vector<u8> &bytes, u32 pitch, u32 width, u32 height,
                      Format format, const Rect *rect, bool clearDepth,
                      f32 depth, bool clearStencil, u32 stencil);

bool writeBmpScreenshot(const std::string &path, Format format, u32 width,
                        u32 height, u32 pitch, std::span<const u8> bytes);

} // namespace dxmt9::core
