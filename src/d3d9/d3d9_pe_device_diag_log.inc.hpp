inline void dxmt9WriteStderrLineAtomic(const char* line, std::size_t len) noexcept {
    if (!line || len == 0u) return;
#if defined(_WIN32)
    (void)_write(_fileno(stderr), line, static_cast<unsigned int>(len));
#else
    (void)::write(STDERR_FILENO, line, len);
#endif
}

inline void dxmt9PerfLogStderrAtomic(const char* fmt, ...) noexcept {
    char line[512]{};
    va_list args;
    va_start(args, fmt);
    const int written = std::vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);
    if (written <= 0) return;
    std::size_t len = static_cast<std::size_t>(written);
    if (len >= sizeof(line)) {
        len = sizeof(line) - 1u;
        line[len - 1u] = '\n';
    }
    dxmt9WriteStderrLineAtomic(line, len);
}
