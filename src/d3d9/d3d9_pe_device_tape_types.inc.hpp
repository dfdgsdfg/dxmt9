struct RenderTapeLiveObject {
    enum class Role : uint8_t {
        Ordinary,
        PresentOutput,
    };

    D9CWireObjectIdentity identity{};
    std::vector<std::byte> descriptor{};
    std::vector<std::byte> immutablePayload{};
    std::uint32_t contentCount = 0u;
    std::vector<std::vector<std::byte>> content{};
    // A D3D9 texture may hand out multiple COM wrappers for one underlying
    // surface level. The tape has one identity, definition, and destruction
    // event for that storage; this count keeps wrapper lifetime separate from
    // tape object lifetime.
    dxmt9::d3d9::RenderTapeSurfaceAliasLifetime lifetime{};
    D9CWireObjectIdentity aliasParentTexture{};
    Role role = Role::Ordinary;
};

struct RenderTapeArmObjectSnapshot {
    std::size_t objectIndex = 0u;
    std::uint64_t armOrdinal = 0u;
    D9CWireObjectIdentity identity{};
    std::vector<std::byte> descriptor{};
    std::vector<std::vector<std::byte>> content{};
};

enum class RenderTapeObjectRegistration : std::uint8_t {
    Rejected,
    Existing,
    New,
};

struct RenderTapeLiveRegistry {
    std::vector<RenderTapeLiveObject> objects{};
    std::vector<D9CWireObjectIdentity> knownDead{};
    bool invalid = false;
    const char *invalidReason = nullptr;
    std::uint32_t invalidKind = 0u;
    std::uint32_t invalidGeneration = 0u;
    std::uint64_t invalidObjectId = 0u;
    std::uint32_t invalidSubresource = std::numeric_limits<std::uint32_t>::max();
    dxmt9::d3d9::RenderTapeCaptureLayoutDiagnostic invalidLayout{};
    // Current holder of the PresentOutput role plus the exact initial-content
    // state the admission displaced, so a demotion restores the object rather
    // than approximating it from the descriptor.
    dxmt9::d3d9::RenderTapePresentOutputRole presentOutputRole{};
    std::vector<std::byte> presentOutputPriorDescriptor{};
    std::uint32_t presentOutputPriorContentCount = 0u;
    std::vector<std::vector<std::byte>> presentOutputPriorContent{};
};
