#pragma once

#include "dxmt9/core.hpp"

#include <vector>

namespace dxmt9::core {

struct MetalCommandRecord {
  enum class Kind {
    Draw,
    Clear,
    SurfaceCopy,
    StretchRect,
    Readback,
    ColorFill,
    Present,
  };

  Kind kind = Kind::Draw;
  DrawDesc draw{};
  ClearDesc clear{};
  SurfaceCopyDesc surfaceCopy{};
  StretchRectDesc stretchRect{};
  ReadbackDesc readback{};
  ColorFillDesc colorFill{};
  SwapDesc present{};
  Handle presentSource{};
};

struct ChunkSlot {
  enum class State { Free, Writing, Pending, Encoding, GPU };

  State state = State::Free;
  u64 seqId = 0;
  std::vector<MetalCommandRecord> commands;
};

}  // namespace dxmt9::core
