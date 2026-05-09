#pragma once

// Lightweight wrapper around os_signpost so callers don't drag <os/log.h>
// and <os/signpost.h> into headers that get included widely. The log
// object lives behind an accessor so its initialization cost (one
// os_log_create per process) is paid lazily and we keep the include
// surface minimal here.
//
// Subsystem/category convention: "com.dxmt9.translator" / "metal".
// Intervals defined by this batch (M3):
//   - "frame"  — first present-relevant work after the previous Present
//                through the Present commit on the encode thread.
//   - "commit" — surrounds the WMT::CommandBuffer::commit() call on the
//                queue submission thread. Argument: seq id.
//   - "draw"   — outer encodeDraw() call, paired per-draw via
//                os_signpost_id_generate so Instruments can match the
//                begin/end across overlapping work.
//
// All intervals are no-ops when no Instruments consumer is recording, so
// this is wired unconditionally rather than gated on DXMT_PERF_COUNTERS.

#include <os/log.h>
#include <os/signpost.h>

namespace dxmt9::signposts {

// Returns the process-global os_log_t used for all dxmt9 signposts.
// Safe to call before main(); the underlying os_log_create is invoked
// lazily on the first call via a function-local static.
os_log_t log();

}  // namespace dxmt9::signposts
