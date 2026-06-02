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
import subprocess
from pathlib import Path
from typing import Any


REPO_ROOT = Path(__file__).resolve().parents[2]


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
    draw_entries = []
    for draw in draws:
        geometry = draw["geometry"]
        state = draw["state"]
        draw_entries.append(
            "  {"
            + ", ".join([
                cxx_string(str(resolve_path(geometry["index_file"]))),
                cxx_string(str(resolve_path(geometry["stream0_file"]))),
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
    psoDesc.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    NSError* error = nil;
    id<MTLRenderPipelineState> pso = [device newRenderPipelineStateWithDescriptor:psoDesc
                                                                            error:&error];
    if (!pso) {{
      std::cerr << "failed to create PSO: " << [[error localizedDescription] UTF8String] << "\\n";
      return 2;
    }}

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
    [encoder setVertexBuffer:vsConsts offset:0 atIndex:6];
    [encoder setVertexBuffer:ffpVs offset:0 atIndex:7];
    [encoder setFragmentBuffer:psConsts offset:0 atIndex:6];
    [encoder setFragmentBuffer:ffpPs offset:0 atIndex:7];
    [encoder setFragmentTexture:whiteTexture atIndex:0];
    [encoder setFragmentSamplerState:sampler atIndex:0];

    for (unsigned r = 0; r < repeat; ++r) {{
      for (const DrawEntry& draw : draws) {{
        NSData* streamData = readData(draw.streamPath);
        NSData* indexData = readData(draw.indexPath);
        if (!streamData || !indexData) return 2;
        id<MTLBuffer> stream =
            [device newBufferWithBytes:[streamData bytes]
                                length:[streamData length]
                               options:MTLResourceStorageModeShared];
        id<MTLBuffer> index =
            [device newBufferWithBytes:[indexData bytes]
                                length:[indexData length]
                               options:MTLResourceStorageModeShared];
        DrawVolatile dv = {{draw.baseVertex, draw.streamOffset, draw.streamStride, 0}};
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
    args = parser.parse_args()

    summary = prepare(args)
    print(json.dumps(summary, indent=2, sort_keys=True))
    if args.compile or args.run:
        compile_source(summary)
    if args.run:
        run_binary(summary, args.repeat, args.capture_path)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
