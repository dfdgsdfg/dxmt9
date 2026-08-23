#pragma once

#include <utility>

namespace dxmt9::d3d9::pe {

enum class ImplicitFvfDeclFailure {
    None,
    Backend,
    Allocation,
};

template<typename BackendHandle, typename WrapperHandle>
struct ImplicitFvfDeclResult {
    BackendHandle backend{};
    WrapperHandle wrapper{};
    ImplicitFvfDeclFailure failure = ImplicitFvfDeclFailure::None;

    explicit operator bool() const noexcept {
        return failure == ImplicitFvfDeclFailure::None && wrapper != WrapperHandle{};
    }
};

template<typename Handle, typename Release>
class PeScopedHandle final {
public:
    PeScopedHandle(Handle handle, Release release) noexcept
        : handle_(handle), release_(std::move(release)) {}

    ~PeScopedHandle() {
        if (handle_ != Handle{}) release_(handle_);
    }

    PeScopedHandle(const PeScopedHandle&) = delete;
    PeScopedHandle& operator=(const PeScopedHandle&) = delete;

    Handle release() noexcept {
        const Handle handle = handle_;
        handle_ = Handle{};
        return handle;
    }

private:
    Handle handle_{};
    Release release_;
};

// Cold cache-miss transaction for SetFVF's implicit declaration. The backend
// handle is owned until the PE wrapper accepts it, and the wrapper is owned
// until the cache publishes it. Any allocation exception or explicit null
// leaves both ownership domains settled and is translated into a value result
// suitable for a noexcept COM entry point.
template<typename BackendHandle, typename WrapperHandle,
         typename CreateBackend, typename ReleaseBackend,
         typename CreateWrapper, typename ReleaseWrapper,
         typename PublishWrapper>
ImplicitFvfDeclResult<BackendHandle, WrapperHandle>
createImplicitFvfDeclTransaction(
    CreateBackend&& createBackend, ReleaseBackend releaseBackend,
    CreateWrapper&& createWrapper, ReleaseWrapper releaseWrapper,
    PublishWrapper&& publishWrapper) noexcept {
    try {
        const BackendHandle backend =
            std::forward<CreateBackend>(createBackend)();
        if (backend == BackendHandle{}) {
            return {.failure = ImplicitFvfDeclFailure::Backend};
        }
        PeScopedHandle backendOwner(backend, std::move(releaseBackend));

        const WrapperHandle wrapper =
            std::forward<CreateWrapper>(createWrapper)(backend);
        if (wrapper == WrapperHandle{}) {
            return {.failure = ImplicitFvfDeclFailure::Allocation};
        }
        // The wrapper owns the backend handle from this point onward.
        backendOwner.release();
        PeScopedHandle wrapperOwner(wrapper, std::move(releaseWrapper));

        if (!std::forward<PublishWrapper>(publishWrapper)(wrapper)) {
            return {.failure = ImplicitFvfDeclFailure::Allocation};
        }
        wrapperOwner.release();
        return {
            .backend = backend,
            .wrapper = wrapper,
            .failure = ImplicitFvfDeclFailure::None,
        };
    } catch (...) {
        return {.failure = ImplicitFvfDeclFailure::Allocation};
    }
}

}  // namespace dxmt9::d3d9::pe
