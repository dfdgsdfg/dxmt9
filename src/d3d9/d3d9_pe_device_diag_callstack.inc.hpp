inline void dxmt9PeCaptureCallStack(D3D9PePresentCallToken& sample) {
#if defined(_WIN32)
    void* frames[D3D9PePresentCallStackDepth]{};
    const USHORT count = RtlCaptureStackBackTrace(
        0, static_cast<DWORD>(D3D9PePresentCallStackDepth), frames, nullptr);
    sample.callerStackCount = static_cast<std::uint8_t>(
        std::min<std::size_t>(count, sample.callerStack.size()));
    for (std::size_t i = 0; i < sample.callerStackCount; ++i) {
        sample.callerStack[i] = frames[i];
    }
#else
    (void)sample;
#endif
}

inline std::array<char, 2048> dxmt9PeFormatCallerStack(
    const D3D9PePresentCallToken& sample) {
    std::array<char, 2048> out{};
    std::size_t used = 0;
    if (sample.callerStackCount == 0) {
        std::snprintf(out.data(), out.size(), "empty");
        return out;
    }
    for (std::size_t i = 0; i < sample.callerStackCount && used < out.size(); ++i) {
        const auto frameInfo = dxmt9PeResolveCallerModule(sample.callerStack[i]);
        const int written = std::snprintf(
            out.data() + used, out.size() - used, "%s%u:%s+0x%llx@%p",
            i == 0 ? "" : ";", static_cast<unsigned>(i),
            dxmt9PeCallerModuleLeaf(frameInfo),
            static_cast<unsigned long long>(frameInfo.rva),
            sample.callerStack[i]);
        if (written <= 0) {
            break;
        }
        used += std::min<std::size_t>(static_cast<std::size_t>(written),
                                      out.size() - used);
    }
    out.back() = '\0';
    return out;
}
