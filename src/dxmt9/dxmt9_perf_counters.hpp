#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt9::perf {

bool enabled();

void countSubmitDraw();
void countSubmitClear();
void countSubmitStretch();
void countSubmitPresent();
void countSubmitFlush();
void countCommandBuffer();
void countMetalBuffer(std::size_t bytes);
void countPipelineBuild();
void countCompletionWait(std::uint64_t nanoseconds, bool hasDraw, bool hasPresent, bool hasBlit);
void countSyncWait(std::uint64_t nanoseconds);
void countPresentEncoded();
void countPresentSkipped();
void countPresentAcquireWait(std::uint64_t nanoseconds);
void countPresentSetPropsWait(std::uint64_t nanoseconds);

}  // namespace dxmt9::perf
