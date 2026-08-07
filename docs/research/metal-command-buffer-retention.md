# Metal command-buffer reference retention

Apple documents that the ordinary `MTLCommandQueue.commandBuffer` factory sets
`MTLCommandBuffer.retainedReferences` to `true`; the explicitly named
`commandBufferWithUnretainedReferences` factory sets it to `false`. See
[`MTLCommandBuffer.retainedReferences`](https://developer.apple.com/documentation/metal/mtlcommandbuffer/retainedreferences?language=objc)
and
[`MTLCommandQueue`](https://developer.apple.com/documentation/metal/mtlcommandqueue?language=objc).

dxmt9's production `CommandQueue::newCommandBuffer()` calls
`WMT::CommandQueue::commandBuffer()`. That local WMT seam calls
`MTLCommandQueue_commandBuffer(handle)`, whose Objective-C implementation calls
`[(id<MTLCommandQueue>)queue commandBuffer]`. The
`dxmt9-post-encode-payload-retirement-spec` source-contract audit pins both
wrappers plus the production caller and rejects use of
`commandBufferWithUnretainedReferences` in those bodies.

This contract retains Metal resources referenced by encoded commands. It does
not retain queue-owned CPU-ready Tape pages, source descriptors, diagnostic
locators, completion-source metadata, or C++ callback captures. A future
post-encode retirement path must therefore prove that every CPU payload read is
finished synchronously and separately retain any non-Metal owner needed by
completion callbacks or diagnostics. If dxmt9 adopts the unretained factory or
a descriptor with `retainedReferences=false`, this source audit must fail and
the retirement proof must be revised before promotion.

The P2 completion shadow does not broaden this Metal contract. A queue-internal
authority derives a locator-free `EncodedCompletionSpan` projection from the
existing fixed completion-source list and carries it redundantly through
submission and pending completion. It covers only dense sequence endpoints,
explicit source count, and tail-Present; source/storage generations, slot and
command ranges, Tape completion, and two-phase reclaim still use the old list.
Consequently P2 adds consistency detection and observability but neither early
payload retirement nor a new completion authority.
