#!/usr/bin/env python3
"""Prepare and optionally run a standalone Metal mini replay from a manifest.

The 3DMark05 mini replay manifest contains raw stream0/index payloads and the
draw-hash MSL files for a small hot draw set. dxmt9's dumped MSL uses a
buffer(30) argument-buffer layout. This helper rewrites that signature into
plain constant-buffer slots so a standalone Metal process can bind zeroed/dummy
constant buffers without recreating dxmt9's full argument-buffer machinery.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import shlex
import shutil
import subprocess
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_MIN_CAPTURE_FREE_MB = 2048


def load_manifest(path: Path) -> dict[str, Any]:
    if not path.exists():
        raise SystemExit(f"missing manifest: {path}")
    with path.open(encoding="utf-8") as handle:
        data = json.load(handle)
    if data.get("schema") != "dxmt9.3dmark05.mini_replay_manifest.v1":
        raise SystemExit(f"unsupported manifest schema: {data.get('schema')}")
    if not data.get("draws"):
        raise SystemExit("manifest has no draws")
    return data


def resolve_path(path: str) -> Path:
    value = Path(path)
    return value if value.is_absolute() else REPO_ROOT / value


def validate_payloads(draws: list[dict[str, Any]]) -> None:
    for index, draw in enumerate(draws):
        geometry = draw.get("geometry", {})
        for key in ("index_file", "stream0_file"):
            path = resolve_path(str(geometry.get(key, "")))
            if not path.exists():
                raise SystemExit(f"draw {index}: missing {key}: {path}")
        if int(geometry.get("index_bytes", 0)) <= 0:
            raise SystemExit(f"draw {index}: index payload is empty")
        if int(geometry.get("stream0_bytes", 0)) <= 0:
            raise SystemExit(f"draw {index}: stream0 payload is empty")
        uniforms = draw.get("uniforms", {})
        for key in ("vsconsts", "psconsts", "ffpvs", "ffpps"):
            byte_count = int(uniforms.get(f"{key}_bytes", 0))
            path_text = str(uniforms.get(f"{key}_file", ""))
            if byte_count == 0 and not path_text:
                continue
            if byte_count > 0 and not path_text:
                raise SystemExit(f"draw {index}: missing {key} payload path")
            path = resolve_path(path_text)
            if byte_count > 0 and not path.exists():
                raise SystemExit(f"draw {index}: missing {key} payload: {path}")


def optional_payload_path(uniforms: dict[str, Any], key: str) -> str:
    if int(uniforms.get(f"{key}_bytes", 0)) <= 0:
        return ""
    return str(resolve_path(str(uniforms.get(f"{key}_file", ""))))


def stage_bindings(source: str) -> dict[str, list[int]]:
    bindings: dict[str, list[int]] = {"buffer": [], "texture": [], "sampler": []}
    for kind in bindings:
        values = sorted({int(match) for match in re.findall(rf"\[\[{kind}\((\d+)\)\]\]", source)})
        bindings[kind] = values
    return bindings


def transform_msl(source: str, stage: str) -> str:
    if stage == "vs":
        source = re.sub(
            r"constant\s+ArgbufLayout&\s+abuf\s+\[\[buffer\(30\)\]\],\s*",
            "constant VsConsts& vsConsts [[buffer(6)]],\n"
            "                     constant FfpVsConsts& ffpVs [[buffer(7)]],\n"
            "                     ",
            source,
            count=1,
        )
        source = re.sub(r"\s*constant VsConsts& vsConsts = \*abuf\.vsConsts;\n", "\n", source)
        source = re.sub(r"\s*constant FfpVsConsts& ffpVs = \*abuf\.ffpVs;\n", "", source)
    elif stage == "fs":
        source = re.sub(
            r"constant\s+ArgbufLayout&\s+abuf\s+\[\[buffer\(30\)\]\],\s*",
            "constant PsConsts& psConsts [[buffer(6)]],\n"
            "                     constant FfpPsConsts& ffpPs [[buffer(7)]], ",
            source,
            count=1,
        )
        source = re.sub(r"\s*constant PsConsts& psConsts = \*abuf\.psConsts;\n", "\n", source)
        source = re.sub(r"\s*constant FfpPsConsts& ffpPs = \*abuf\.ffpPs;\n", "", source)
    else:
        raise ValueError(stage)
    return source


def cxx_string(value: str) -> str:
    return json.dumps(value)


def render_source(draws: list[dict[str, Any]],
                  vs_path: Path,
                  fs_path: Path,
                  width: int,
                  height: int) -> str:
    first_state = draws[0]["state"]
    alpha_blend = int(first_state.get("alpha_blend", 0))
    src_blend = int(first_state.get("src_blend", 2))
    dst_blend = int(first_state.get("dst_blend", 1))
    blend_op = int(first_state.get("blend_op", 1))
    separate_alpha = int(first_state.get("separate_alpha", 0))
    src_blend_alpha = int(first_state.get("src_blend_alpha", src_blend))
    dst_blend_alpha = int(first_state.get("dst_blend_alpha", dst_blend))
    blend_op_alpha = int(first_state.get("blend_op_alpha", blend_op))
    color_write = int(str(first_state.get("color_write", "0xf")), 0)
    depth_enabled = int(first_state.get("depth_enabled", 0))
    depth_write = int(first_state.get("depth_write", 0))
    depth_func = int(first_state.get("depth_func", 8 if not depth_enabled else 4))
    cull = int(first_state.get("cull", 1))
    scissor = int(first_state.get("scissor", 0))
    scissor_l = int(first_state.get("scissor_l", 0))
    scissor_t = int(first_state.get("scissor_t", 0))
    scissor_r = int(first_state.get("scissor_r", width))
    scissor_b = int(first_state.get("scissor_b", height))
    draw_entries = []
    for draw in draws:
        geometry = draw["geometry"]
        uniforms = draw.get("uniforms", {})
        state = draw["state"]
        draw_entries.append(
            "  {"
            + ", ".join([
                cxx_string(str(resolve_path(geometry["index_file"]))),
                cxx_string(str(resolve_path(geometry["stream0_file"]))),
                cxx_string(optional_payload_path(uniforms, "vsconsts")),
                cxx_string(optional_payload_path(uniforms, "psconsts")),
                cxx_string(optional_payload_path(uniforms, "ffpvs")),
                cxx_string(optional_payload_path(uniforms, "ffpps")),
                str(int(state.get("index_count", 0))),
                str(int(state.get("base_vertex", 0))),
                str(int(state.get("stream0_stride", 0))),
                str(int(state.get("stream0_offset", 0))),
            ])
            + "}"
        )
    draw_array = ",\n".join(draw_entries)
    return f"""#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <iostream>

struct DrawEntry {{
  const char* indexPath;
  const char* streamPath;
  const char* vsConstsPath;
  const char* psConstsPath;
  const char* ffpVsPath;
  const char* ffpPsPath;
  unsigned indexCount;
  int baseVertex;
  unsigned streamStride;
  unsigned streamOffset;
}};

struct DrawVolatile {{
  int vertexBaseIndex;
  unsigned vertexStreamOffset;
  unsigned vertexStreamStride;
  unsigned pad;
}};

static NSData* readData(const char* path) {{
  return [NSData dataWithContentsOfFile:[NSString stringWithUTF8String:path]];
}}

static NSString* readString(const char* path) {{
  NSError* error = nil;
  NSString* value = [NSString stringWithContentsOfFile:[NSString stringWithUTF8String:path]
                                              encoding:NSUTF8StringEncoding
                                                 error:&error];
  if (!value) {{
    std::cerr << "failed to read " << path << "\\n";
  }}
  return value;
}}

static id<MTLLibrary> makeLibrary(id<MTLDevice> device, const char* path) {{
  NSError* error = nil;
  id<MTLLibrary> library = [device newLibraryWithSource:readString(path)
                                                options:nil
                                                  error:&error];
  if (!library) {{
    std::cerr << "failed to compile MSL " << path << ": "
              << [[error localizedDescription] UTF8String] << "\\n";
  }}
  return library;
}}

static id<MTLBuffer> zeroBuffer(id<MTLDevice> device, NSUInteger size) {{
  id<MTLBuffer> buffer = [device newBufferWithLength:size options:MTLResourceStorageModeShared];
  std::memset([buffer contents], 0, size);
  return buffer;
}}

static id<MTLBuffer> bufferFromFileOrDefault(id<MTLDevice> device,
                                             const char* path,
                                             id<MTLBuffer> fallback) {{
  if (!path || path[0] == '\\0') {{
    return fallback;
  }}
  NSData* data = readData(path);
  if (!data) {{
    std::cerr << "failed to read cbuf " << path << "\\n";
    return nil;
  }}
  return [device newBufferWithBytes:[data bytes]
                              length:[data length]
                             options:MTLResourceStorageModeShared];
}}

static void initVsConsts(id<MTLBuffer> buffer) {{
  float* f = static_cast<float*>([buffer contents]);
  f[0] = 1.0f;
  f[5] = 1.0f;
  f[10] = 1.0f;
  f[15] = 1.0f;
  f[16 + 3] = 1.0f;
  f[32 + 3] = 1.0f;
  f[48 + 3] = 1.0f;
}}

static MTLCompareFunction compareFunction(unsigned value) {{
  switch (value) {{
    case 1: return MTLCompareFunctionNever;
    case 2: return MTLCompareFunctionLess;
    case 3: return MTLCompareFunctionEqual;
    case 4: return MTLCompareFunctionLessEqual;
    case 5: return MTLCompareFunctionGreater;
    case 6: return MTLCompareFunctionNotEqual;
    case 7: return MTLCompareFunctionGreaterEqual;
    case 8: return MTLCompareFunctionAlways;
    default: return MTLCompareFunctionAlways;
  }}
}}

static MTLBlendFactor blendFactor(unsigned value, bool alphaLane) {{
  switch (value) {{
    case 1: return MTLBlendFactorZero;
    case 2: return MTLBlendFactorOne;
    case 3: return MTLBlendFactorSourceColor;
    case 4: return MTLBlendFactorOneMinusSourceColor;
    case 5: return MTLBlendFactorSourceAlpha;
    case 6: return MTLBlendFactorOneMinusSourceAlpha;
    case 7: return MTLBlendFactorDestinationAlpha;
    case 8: return MTLBlendFactorOneMinusDestinationAlpha;
    case 9: return MTLBlendFactorDestinationColor;
    case 10: return MTLBlendFactorOneMinusDestinationColor;
    case 11: return alphaLane ? MTLBlendFactorOne : MTLBlendFactorSourceAlphaSaturated;
    case 12: return MTLBlendFactorSourceAlpha;
    case 13: return MTLBlendFactorOneMinusSourceAlpha;
    case 14: return alphaLane ? MTLBlendFactorBlendAlpha : MTLBlendFactorBlendColor;
    case 15: return alphaLane ? MTLBlendFactorOneMinusBlendAlpha : MTLBlendFactorOneMinusBlendColor;
    default: return MTLBlendFactorOne;
  }}
}}

static MTLBlendOperation blendOperation(unsigned value) {{
  switch (value) {{
    case 1: return MTLBlendOperationAdd;
    case 2: return MTLBlendOperationSubtract;
    case 3: return MTLBlendOperationReverseSubtract;
    case 4: return MTLBlendOperationMin;
    case 5: return MTLBlendOperationMax;
    default: return MTLBlendOperationAdd;
  }}
}}

static MTLCullMode cullMode(unsigned value) {{
  switch (value) {{
    case 1: return MTLCullModeNone;
    case 2: return MTLCullModeFront;
    case 3: return MTLCullModeBack;
    default: return MTLCullModeNone;
  }}
}}

static MTLColorWriteMask colorWriteMask(unsigned value) {{
  MTLColorWriteMask mask = 0;
  if (value & 0x1) mask |= MTLColorWriteMaskRed;
  if (value & 0x2) mask |= MTLColorWriteMaskGreen;
  if (value & 0x4) mask |= MTLColorWriteMaskBlue;
  if (value & 0x8) mask |= MTLColorWriteMaskAlpha;
  return mask;
}}

int main() {{
  @autoreleasepool {{
    id<MTLDevice> device = MTLCreateSystemDefaultDevice();
    if (!device) {{
      std::cerr << "no Metal device\\n";
      return 2;
    }}

    id<MTLCommandQueue> queue = [device newCommandQueue];
    id<MTLLibrary> vsLib = makeLibrary(device, {cxx_string(str(vs_path))});
    id<MTLLibrary> fsLib = makeLibrary(device, {cxx_string(str(fs_path))});
    if (!vsLib || !fsLib) return 2;
    id<MTLFunction> vs = [vsLib newFunctionWithName:@"dxmt9_vs"];
    id<MTLFunction> fs = [fsLib newFunctionWithName:@"dxmt9_fs"];

    MTLRenderPipelineDescriptor* psoDesc = [MTLRenderPipelineDescriptor new];
    psoDesc.vertexFunction = vs;
    psoDesc.fragmentFunction = fs;
    psoDesc.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    psoDesc.colorAttachments[0].writeMask = colorWriteMask({color_write});
    psoDesc.colorAttachments[0].blendingEnabled = {alpha_blend};
    psoDesc.colorAttachments[0].sourceRGBBlendFactor = blendFactor({src_blend}, false);
    psoDesc.colorAttachments[0].destinationRGBBlendFactor = blendFactor({dst_blend}, false);
    psoDesc.colorAttachments[0].rgbBlendOperation = blendOperation({blend_op});
    psoDesc.colorAttachments[0].sourceAlphaBlendFactor =
        blendFactor({src_blend_alpha if separate_alpha else src_blend}, true);
    psoDesc.colorAttachments[0].destinationAlphaBlendFactor =
        blendFactor({dst_blend_alpha if separate_alpha else dst_blend}, true);
    psoDesc.colorAttachments[0].alphaBlendOperation =
        blendOperation({blend_op_alpha if separate_alpha else blend_op});
    psoDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:psoDesc
                                                                            error:&error];
    if (!pso) {{
      std::cerr << "failed to create PSO: " << [[error localizedDescription] UTF8String] << "\\n";
      return 2;
    }}
    MTLDepthStencilDescriptor* depthStateDesc = [MTLDepthStencilDescriptor new];
    depthStateDesc.depthCompareFunction =
        {depth_enabled} ? compareFunction({depth_func}) : MTLCompareFunctionAlways;
    depthStateDesc.depthWriteEnabled = {1 if depth_enabled and depth_write else 0};
    id<MTLDepthStencilState> depthState = [device newDepthStencilStateWithDescriptor:depthStateDesc];

    MTLTextureDescriptor* colorDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:{width}
                                                          height:{height}
                                                       mipmapped:NO];
    colorDesc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> color = [device newTextureWithDescriptor:colorDesc];
    MTLTextureDescriptor* depthDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                                           width:{width}
                                                          height:{height}
                                                       mipmapped:NO];
    depthDesc.usage = MTLTextureUsageRenderTarget;
    id<MTLTexture> depth = [device newTextureWithDescriptor:depthDesc];

    id<MTLBuffer> vsConsts = zeroBuffer(device, 16384);
    initVsConsts(vsConsts);
    id<MTLBuffer> ffpVs = zeroBuffer(device, 16384);
    id<MTLBuffer> psConsts = zeroBuffer(device, 16384);
    id<MTLBuffer> ffpPs = zeroBuffer(device, 16384);
    MTLTextureDescriptor* whiteDesc =
        [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                           width:1
                                                          height:1
                                                       mipmapped:NO];
    whiteDesc.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> whiteTexture = [device newTextureWithDescriptor:whiteDesc];
    const unsigned char whitePixel[4] = {{255, 255, 255, 255}};
    [whiteTexture replaceRegion:MTLRegionMake2D(0, 0, 1, 1)
                     mipmapLevel:0
                       withBytes:whitePixel
                     bytesPerRow:4];
    MTLSamplerDescriptor* samplerDesc = [MTLSamplerDescriptor new];
    samplerDesc.minFilter = MTLSamplerMinMagFilterLinear;
    samplerDesc.magFilter = MTLSamplerMinMagFilterLinear;
    id<MTLSamplerState> sampler = [device newSamplerStateWithDescriptor:samplerDesc];

    DrawEntry draws[] = {{
{draw_array}
    }};

    unsigned repeat = 1;
    if (const char* env = std::getenv("DXMT9_MINI_REPLAY_REPEAT")) {{
      repeat = std::max(1, std::atoi(env));
    }}

    if (const char* capturePath = std::getenv("DXMT9_MINI_REPLAY_CAPTURE_PATH")) {{
      MTLCaptureDescriptor* capture = [MTLCaptureDescriptor new];
      capture.captureObject = device;
      capture.destination = MTLCaptureDestinationGPUTraceDocument;
      capture.outputURL = [NSURL fileURLWithPath:[NSString stringWithUTF8String:capturePath]];
      [[MTLCaptureManager sharedCaptureManager] startCaptureWithDescriptor:capture error:&error];
      if (error) {{
        std::cerr << "capture start failed: " << [[error localizedDescription] UTF8String] << "\\n";
      }}
    }}

    id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
    MTLRenderPassDescriptor* pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = color;
    pass.colorAttachments[0].loadAction = MTLLoadActionClear;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
    pass.depthAttachment.texture = depth;
    pass.depthAttachment.loadAction = MTLLoadActionClear;
    pass.depthAttachment.storeAction = MTLStoreActionDontCare;
    pass.depthAttachment.clearDepth = 1.0;
    id<MTLRenderCommandEncoder> encoder = [commandBuffer renderCommandEncoderWithDescriptor:pass];
    [encoder setRenderPipelineState:pso];
    [encoder setDepthStencilState:depthState];
    [encoder setCullMode:cullMode({cull})];
    if ({scissor}) {{
      MTLScissorRect rect = {{
        static_cast<NSUInteger>({scissor_l}),
        static_cast<NSUInteger>({scissor_t}),
        static_cast<NSUInteger>(std::max(0, {scissor_r} - {scissor_l})),
        static_cast<NSUInteger>(std::max(0, {scissor_b} - {scissor_t}))
      }};
      [encoder setScissorRect:rect];
    }}
    [encoder setFragmentTexture:whiteTexture atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];

    for (unsigned r = 0; r < repeat; ++r) {{
      for (const DrawEntry& draw : draws) {{
        NSData* streamData = readData(draw.streamPath);
        NSData* indexData = readData(draw.indexPath);
        if (!streamData || !indexData) return 2;
        id<MTLBuffer> drawVsConsts = bufferFromFileOrDefault(device, draw.vsConstsPath, vsConsts);
        id<MTLBuffer> drawPsConsts = bufferFromFileOrDefault(device, draw.psConstsPath, psConsts);
        id<MTLBuffer> drawFfpVs = bufferFromFileOrDefault(device, draw.ffpVsPath, ffpVs);
        id<MTLBuffer> drawFfpPs = bufferFromFileOrDefault(device, draw.ffpPsPath, ffpPs);
        if (!drawVsConsts || !drawPsConsts || !drawFfpVs || !drawFfpPs) return 2;
        id<MTLBuffer> stream =
            [device newBufferWithBytes:[streamData bytes]
                                length:[streamData length]
                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> index =
            [device newBufferWithBytes:[indexData bytes]
                                length:[indexData length]
                               options:MTLResourceStorageModeShared];
        DrawVolatile dv = {{draw.baseVertex, draw.streamOffset, draw.streamStride, 0}};
        [encoder setVertexBuffer:drawVsConsts offset:0 atIndex:6];
        [encoder setVertexBuffer:drawFfpVs offset:0 atIndex:7];
        [encoder setFragmentBuffer:drawPsConsts offset:0 atIndex:6];
        [encoder setFragmentBuffer:drawFfpPs offset:0 atIndex:7];
        [encoder setVertexBuffer:stream offset:0 atIndex:1];
        [encoder setVertexBytes:&dv length:sizeof(dv) atIndex:5];
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:draw.indexCount
                             indexType:MTLIndexTypeUInt16
                           indexBuffer:index
                     indexBufferOffset:0];
      }}
    }}
    [encoder endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if ([[MTLCaptureManager sharedCaptureManager] isCapturing]) {{
      [[MTLCaptureManager sharedCaptureManager] stopCapture];
    }}
    std::cout << "mini replay draws=" << (sizeof(draws) / sizeof(draws[0]))
              << " repeat=" << repeat << "\\n";
  }}
  return 0;
}}
"""


def prepare(args: argparse.Namespace) -> dict[str, Any]:
    manifest = load_manifest(args.manifest)
    draws = manifest["draws"]
    validate_payloads(draws)
    first = draws[0]
    vs_file = resolve_path(first["shaders"]["vs_file"])
    fs_file = resolve_path(first["shaders"]["ps_file"])
    if not vs_file.exists() or not fs_file.exists():
        raise SystemExit("manifest shader files are missing")

    args.output_dir.mkdir(parents=True, exist_ok=True)
    vs_replay = args.output_dir / "dxmt9_vs.replay.metal"
    fs_replay = args.output_dir / "dxmt9_fs.replay.metal"
    vs_source = transform_msl(vs_file.read_text(encoding="utf-8"), "vs")
    fs_source = transform_msl(fs_file.read_text(encoding="utf-8"), "fs")
    vs_replay.write_text(vs_source, encoding="utf-8")
    fs_replay.write_text(fs_source, encoding="utf-8")

    source_path = args.output_dir / "dxmt9_3dmark05_mini_replay.mm"
    source_path.write_text(
        render_source(draws, vs_replay, fs_replay, args.width, args.height),
        encoding="utf-8",
    )
    binary_path = args.output_dir / "dxmt9-3dmark05-mini-replay"
    summary = {
        "manifest": str(args.manifest),
        "output_dir": str(args.output_dir),
        "source": str(source_path),
        "binary": str(binary_path),
        "vs_replay": str(vs_replay),
        "fs_replay": str(fs_replay),
        "draw_count": len(draws),
        "index_bytes": sum(int(draw["geometry"]["index_bytes"]) for draw in draws),
        "stream0_bytes": sum(int(draw["geometry"]["stream0_bytes"]) for draw in draws),
        "uniform_draw_count": sum(
            1 for draw in draws
            if any(int(draw.get("uniforms", {}).get(f"{key}_bytes", 0)) > 0
                   for key in ("vsconsts", "psconsts", "ffpvs", "ffpps"))
        ),
        "uniform_bytes": sum(
            int(draw.get("uniforms", {}).get(f"{key}_bytes", 0))
            for draw in draws
            for key in ("vsconsts", "psconsts", "ffpvs", "ffpps")
        ),
        "vs_bindings": stage_bindings(vs_source),
        "fs_bindings": stage_bindings(fs_source),
    }
    (args.output_dir / "mini-replay-summary.json").write_text(
        json.dumps(summary, indent=2, sort_keys=True), encoding="utf-8")
    return summary


def compile_source(summary: dict[str, Any]) -> None:
    cmd = [
        "xcrun", "-sdk", "macosx", "clang++",
        "-std=c++20",
        "-fobjc-arc",
        "-O2",
        "-Wall",
        "-Wextra",
        "-framework", "Foundation",
        "-framework", "Metal",
        "-o", summary["binary"],
        summary["source"],
    ]
    print("compile_cmd:", " ".join(shlex.quote(part) for part in cmd))
    subprocess.run(cmd, check=True)


def parse_min_capture_free_mb(value: str | None) -> int:
    if value is None or value == "":
        return DEFAULT_MIN_CAPTURE_FREE_MB
    try:
        parsed = int(value)
    except ValueError as exc:
        raise SystemExit(f"invalid min capture free MB: {value}") from exc
    if parsed < 0:
        raise SystemExit(f"invalid min capture free MB: {value}")
    return parsed


def check_capture_free_space(capture_path: Path, min_free_mb: int) -> None:
    if min_free_mb <= 0:
        return
    target_dir = capture_path.parent
    target_dir.mkdir(parents=True, exist_ok=True)
    free_mb = shutil.disk_usage(target_dir).free // (1024 * 1024)
    if free_mb < min_free_mb:
        raise SystemExit(
            f"insufficient free space for mini replay gputrace: "
            f"{free_mb}MiB available at {target_dir}, require {min_free_mb}MiB "
            f"(override with --min-capture-free-mb or DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB)"
        )


def run_binary(summary: dict[str, Any], repeat: int, capture_path: Path | None) -> None:
    env = os.environ.copy()
    env["DXMT9_MINI_REPLAY_REPEAT"] = str(repeat)
    if capture_path is not None:
        env["MTL_CAPTURE_ENABLED"] = "1"
        env["DXMT9_MINI_REPLAY_CAPTURE_PATH"] = str(capture_path)
    subprocess.run([summary["binary"]], check=True, env=env)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("manifest", type=Path)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--height", type=int, default=768)
    parser.add_argument("--compile", action="store_true")
    parser.add_argument("--run", action="store_true")
    parser.add_argument("--repeat", type=int, default=1)
    parser.add_argument("--capture-path", type=Path)
    parser.add_argument(
        "--min-capture-free-mb",
        type=parse_min_capture_free_mb,
        default=parse_min_capture_free_mb(os.environ.get("DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB")),
        help=(
            "minimum free space required before --capture-path is used "
            f"(default: {DEFAULT_MIN_CAPTURE_FREE_MB}MiB, env DXMT9_MINI_REPLAY_MIN_CAPTURE_FREE_MB)"
        ),
    )
    args = parser.parse_args()

    summary = prepare(args)
    print(json.dumps(summary, indent=2, sort_keys=True))
    if args.run and args.capture_path is not None:
        check_capture_free_space(args.capture_path, args.min_capture_free_mb)
    if args.compile or args.run:
        compile_source(summary)
    if args.run:
        run_binary(summary, args.repeat, args.capture_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
