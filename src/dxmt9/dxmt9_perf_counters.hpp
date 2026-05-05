#pragma once

#include <cstddef>
#include <cstdint>

namespace dxmt9::perf {

bool enabled();

void countSubmitDraw();
void countSubmitClear();
void countSubmitStretch();
void countStretchBlitCopy();
void countStretchRenderPass();
void countStretchFullscreen();
void countSubmitPresent();
void countSubmitFlush();
void countCommandBuffer();
void countMetalBuffer(std::size_t bytes);
void countPipelineBuild();
void countSubmitDrawCpuTime(std::uint64_t nanoseconds);
void countEncodeChunkCpuTime(std::uint64_t nanoseconds);
void countEncodeDrawCpuTime(std::uint64_t nanoseconds);
void countTransientUploadCpuTime(std::uint64_t nanoseconds, std::size_t bytes);
void countCommandBufferCreateCpuTime(std::uint64_t nanoseconds);
void countCommandBufferCommitCpuTime(std::uint64_t nanoseconds);
void countCompletionWait(std::uint64_t nanoseconds,
                         bool hasDraw,
                         bool hasPresent,
                         bool hasBlit,
                         bool hasStretchRect);
void countSyncWait(std::uint64_t nanoseconds);
void countQueueWriterWait(std::uint64_t nanoseconds);
void countQueueCommitWait(std::uint64_t nanoseconds);
void countQueueSequenceWait(std::uint64_t nanoseconds);
void countPresentBoundaryApplied();
void countPresentBoundarySkipped();
void countPresentBoundaryWait(std::uint64_t nanoseconds);
void countPresentEncoded();
void countPresentSkipped();
void countPresentFullscreen();
void countPresentPass(std::uint32_t sourceWidth,
                      std::uint32_t sourceHeight,
                      std::uint64_t targetWidth,
                      std::uint64_t targetHeight);
void countPresentAcquireWait(std::uint64_t nanoseconds);
void countPresentAsyncAcquireRequest();
void countPresentAsyncAcquireIssued();
void countPresentAsyncAcquireFallback();
void countPresentAsyncAcquireWait(std::uint64_t nanoseconds);
void countPresentTokenWait(std::uint64_t nanoseconds);
void countPresentPreAcquireRequest();
void countPresentPreAcquireHit();
void countPresentPreAcquireMiss();
void countPresentPreAcquireWait(std::uint64_t nanoseconds);
void countPresentSetPropsWait(std::uint64_t nanoseconds);

}  // namespace dxmt9::perf
