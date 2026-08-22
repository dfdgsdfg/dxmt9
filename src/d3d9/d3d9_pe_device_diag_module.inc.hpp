inline const char* dxmt9PeCallerModuleLeaf(const Dxmt9PeCallerModuleInfo& info) {
    const char* leaf = info.path.data();
    for (const char* p = info.path.data(); *p; ++p) {
        if (*p == '\\' || *p == '/') {
            leaf = p + 1;
        }
    }
    return *leaf ? leaf : "unknown";
}

inline Dxmt9PeCallerModuleInfo dxmt9PeResolveCallerModule(
    const void* callerPc) {
    Dxmt9PeCallerModuleInfo info{};
    if (!callerPc) {
        return info;
    }
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(callerPc, &mbi, sizeof(mbi)) || !mbi.AllocationBase) {
        return info;
    }
    info.base = mbi.AllocationBase;
    info.rva = reinterpret_cast<std::uintptr_t>(callerPc) -
               reinterpret_cast<std::uintptr_t>(mbi.AllocationBase);
    const DWORD written = GetModuleFileNameA(
        reinterpret_cast<HMODULE>(mbi.AllocationBase),
        info.path.data(), static_cast<DWORD>(info.path.size()));
    if (written == 0) {
        std::strncpy(info.path.data(), "unknown", info.path.size() - 1);
    } else {
        info.path.back() = '\0';
    }
    return info;
}
