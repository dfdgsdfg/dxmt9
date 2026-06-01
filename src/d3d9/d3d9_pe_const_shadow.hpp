#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <vector>

struct ConstShadow {
    std::vector<std::uint8_t> values;
    std::vector<std::uint8_t> dirtyElems;
    std::uint32_t dirtyStart = 0;
    std::uint32_t dirtyEnd = 0;

    bool dirty() const {
        return dirtyEnd > dirtyStart;
    }
    void clear() {
        dirtyStart = dirtyEnd = 0;
        std::fill(dirtyElems.begin(), dirtyElems.end(), std::uint8_t{0});
    }
    void reset() {
        values.clear();
        dirtyElems.clear();
        clear();
    }
};

struct PeConstShadowBlock {
    ConstShadow vsConstF{};
    ConstShadow vsConstI{};
    ConstShadow vsConstB{};
    ConstShadow psConstF{};
    ConstShadow psConstI{};
    ConstShadow psConstB{};

    void reset() {
        vsConstF.reset();
        vsConstI.reset();
        vsConstB.reset();
        psConstF.reset();
        psConstI.reset();
        psConstB.reset();
    }
};

inline void touchConstShadow(ConstShadow& shadow,
                             std::uint32_t start,
                             std::uint32_t count,
                             const void* data,
                             std::size_t elemSize) {
    const std::uint64_t needed64 =
        (static_cast<std::uint64_t>(start) + count) * elemSize;
    if (needed64 > 0xffffffffull) {
        return;
    }
    const auto needed = static_cast<std::size_t>(needed64);
    if (shadow.values.size() < needed) {
        shadow.values.resize(needed);
    }
    const std::size_t neededElems = static_cast<std::size_t>(start) + count;
    if (shadow.dirtyElems.size() < neededElems) {
        shadow.dirtyElems.resize(neededElems);
    }
    const auto offset = static_cast<std::size_t>(start) * elemSize;
    const auto bytes = static_cast<std::size_t>(count) * elemSize;
    bool changed = false;
    std::uint32_t firstChanged = count;
    std::uint32_t lastChanged = 0;
    if (bytes != 0 && data) {
        auto* dst = shadow.values.data() + offset;
        const auto* src = static_cast<const std::uint8_t*>(data);
        for (std::uint32_t i = 0; i < count; ++i) {
            const auto elemOffset = static_cast<std::size_t>(i) * elemSize;
            if (std::memcmp(dst + elemOffset, src + elemOffset, elemSize) == 0) {
                continue;
            }
            std::memcpy(dst + elemOffset, src + elemOffset, elemSize);
            shadow.dirtyElems[static_cast<std::size_t>(start) + i] = 1;
            changed = true;
            firstChanged = std::min<std::uint32_t>(firstChanged, i);
            lastChanged = i + 1u;
        }
    }
    if (!changed) {
        return;
    }
    const std::uint32_t changedStart = start + firstChanged;
    const std::uint32_t end = start + lastChanged;
    if (!shadow.dirty()) {
        shadow.dirtyStart = changedStart;
        shadow.dirtyEnd = end;
    } else {
        shadow.dirtyStart = std::min<std::uint32_t>(shadow.dirtyStart, changedStart);
        shadow.dirtyEnd = std::max<std::uint32_t>(shadow.dirtyEnd, end);
    }
}
