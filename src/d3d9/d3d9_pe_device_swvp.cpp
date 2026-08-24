/* src/d3d9/d3d9_pe_device_swvp.cpp — D3D9DeviceImpl software vertex processing.
 *
 * The software-vertex-processing and software-FFP draw fallback: the branch
 * D3D9DeviceImpl takes when the app asked for D3DCREATE_SOFTWARE_VERTEXPROCESSING
 * (or SetSoftwareVertexProcessing), plus the transformed-vertex clipper it
 * needs. Every entry point here is reached from Draw{,Indexed}Primitive{,UP}
 * behind that runtime branch, so none of it runs on the hardware path.
 *
 * Split out of the class header so the hardware path is not compiled through
 * it. The two helpers the hot UP-record path shares with this code --
 * primitiveVertexCount() and checkedByteCount() -- deliberately stay in the
 * header. */

#include "d3d9_pe_device_impl.hpp"

namespace {

template <typename Fn>
class SwvpScopeGuard final {
public:
    explicit SwvpScopeGuard(Fn&& fn) noexcept
        : fn_(std::forward<Fn>(fn)) {}
    ~SwvpScopeGuard() noexcept { fn_(); }

    SwvpScopeGuard(const SwvpScopeGuard&) = delete;
    SwvpScopeGuard& operator=(const SwvpScopeGuard&) = delete;

private:
    Fn fn_;
};

template <typename T>
class SwvpComReleaseGuard final {
public:
    explicit SwvpComReleaseGuard(T*& value) noexcept : value_(value) {}
    ~SwvpComReleaseGuard() noexcept {
        if (value_) {
            value_->Release();
            value_ = nullptr;
        }
    }

    SwvpComReleaseGuard(const SwvpComReleaseGuard&) = delete;
    SwvpComReleaseGuard& operator=(const SwvpComReleaseGuard&) = delete;

private:
    T*& value_;
};

class SwvpUnlockGuard final {
public:
    explicit SwvpUnlockGuard(IDirect3DVertexBuffer9* buffer) noexcept
        : buffer_(buffer) {}
    ~SwvpUnlockGuard() noexcept {
        if (buffer_) (void)buffer_->Unlock();
    }

    void dismiss() noexcept { buffer_ = nullptr; }

    SwvpUnlockGuard(const SwvpUnlockGuard&) = delete;
    SwvpUnlockGuard& operator=(const SwvpUnlockGuard&) = delete;

private:
    IDirect3DVertexBuffer9* buffer_;
};

class SwvpIndexUnlockGuard final {
public:
    explicit SwvpIndexUnlockGuard(IDirect3DIndexBuffer9* buffer) noexcept
        : buffer_(buffer) {}
    ~SwvpIndexUnlockGuard() noexcept {
        if (buffer_) (void)buffer_->Unlock();
    }

    void dismiss() noexcept { buffer_ = nullptr; }

    SwvpIndexUnlockGuard(const SwvpIndexUnlockGuard&) = delete;
    SwvpIndexUnlockGuard& operator=(const SwvpIndexUnlockGuard&) = delete;

private:
    IDirect3DIndexBuffer9* buffer_;
};

}  // namespace

std::uint32_t D3D9DeviceImpl::transformedSwvpVertexClipFlags(
        const std::vector<std::uint8_t>& vertices,
        UINT stride,
        UINT index) const {
    if (stride < 16u) return 0u;
    const std::uint64_t offset = static_cast<std::uint64_t>(index) * stride;
    if (offset > vertices.size() ||
        16u > vertices.size() - static_cast<size_t>(offset)) {
        return kSwvpClipOutsideAll;
    }
    float x = 0.0f, y = 0.0f, z = 0.0f, rhw = 0.0f;
    std::memcpy(&x, vertices.data() + static_cast<size_t>(offset),
                sizeof(x));
    std::memcpy(&y, vertices.data() + static_cast<size_t>(offset) + 4u,
                sizeof(y));
    std::memcpy(&z, vertices.data() + static_cast<size_t>(offset) + 8u,
                sizeof(z));
    std::memcpy(&rhw,
                vertices.data() + static_cast<size_t>(offset) + 12u,
                sizeof(rhw));
    if (!std::isfinite(x) || !std::isfinite(y) ||
        !std::isfinite(z) || !std::isfinite(rhw)) {
        return kSwvpClipOutsideAll;
    }

    std::uint32_t flags = 0u;
    if (rhw <= 0.0f) flags |= kSwvpClipOutsideEye;

    const auto& vp = recorderState_.peState.viewportShadow();
    const float left = static_cast<float>(vp.x);
    const float right = left + static_cast<float>(vp.width);
    const float top = static_cast<float>(vp.y);
    const float bottom = top + static_cast<float>(vp.height);
    if (vp.width != 0u) {
        if (x < left) flags |= kSwvpClipOutsideLeft;
        if (x > right) flags |= kSwvpClipOutsideRight;
    }
    if (vp.height != 0u) {
        if (y < top) flags |= kSwvpClipOutsideTop;
        if (y > bottom) flags |= kSwvpClipOutsideBottom;
    }
    const float nearZ = std::min(vp.minZ, vp.maxZ);
    const float farZ = std::max(vp.minZ, vp.maxZ);
    if (z < nearZ) flags |= kSwvpClipOutsideNear;
    if (z > farZ) flags |= kSwvpClipOutsideFar;
    const DWORD userClipMask =
        renderStateValue(D3DRS_CLIPPLANEENABLE) & 0x3fu;
    const float zScale = vp.maxZ - vp.minZ;
    if (userClipMask != 0u && rhw != 0.0f && vp.width != 0u &&
        vp.height != 0u && zScale != 0.0f) {
        const float scaleX = static_cast<float>(vp.width) * 0.5f;
        const float scaleY = static_cast<float>(vp.height) * 0.5f;
        const float offsetX = static_cast<float>(vp.x) + scaleX;
        const float offsetY = static_cast<float>(vp.y) + scaleY;
        const float w = 1.0f / rhw;
        const float clip[4] = {
            ((x - offsetX) / scaleX) * w,
            (-(y - offsetY) / scaleY) * w,
            ((z - vp.minZ) / zScale) * w,
            w,
        };
        for (UINT i = 0; i < 6u; ++i) {
            if ((userClipMask & (1u << i)) == 0u) continue;
            const float* plane = &recorderState_.peState.clipPlaneShadow()[i * 4u];
            const float distance = plane[0] * clip[0] +
                                   plane[1] * clip[1] +
                                   plane[2] * clip[2] +
                                   plane[3] * clip[3];
            if (!std::isfinite(distance) || distance < 0.0f) {
                flags |= kSwvpClipOutsideUserPlane0 << i;
            }
        }
    }
    return flags;
}

	    HRESULT D3D9DeviceImpl::appendTransformedSwvpVertex(
	            const std::vector<std::uint8_t>& source,
	            UINT stride,
	            UINT index,
	            std::vector<std::uint8_t>& out) {
    if (stride == 0u) return D3DERR_INVALIDCALL;
    const std::uint64_t offset =
        static_cast<std::uint64_t>(index) * stride;
    if (offset > source.size() ||
        stride > source.size() - static_cast<size_t>(offset)) {
        return D3DERR_INVALIDCALL;
    }
    out.insert(out.end(),
               source.begin() + static_cast<size_t>(offset),
               source.begin() + static_cast<size_t>(offset) + stride);
	        return S_OK;
	    }

	    HRESULT D3D9DeviceImpl::copyTransformedSwvpVertex(
	            const std::vector<std::uint8_t>& source,
	            UINT stride,
	            UINT index,
	            SwvpClippedVertex& out) {
	        if (stride == 0u) return D3DERR_INVALIDCALL;
	        const std::uint64_t offset =
	            static_cast<std::uint64_t>(index) * stride;
	        if (offset > source.size() ||
	            stride > source.size() - static_cast<size_t>(offset)) {
	            return D3DERR_INVALIDCALL;
	        }
	        out.bytes.assign(source.begin() + static_cast<size_t>(offset),
	                         source.begin() + static_cast<size_t>(offset) + stride);
	        return S_OK;
	    }

	    float D3D9DeviceImpl::swvpReadFloat(const std::vector<std::uint8_t>& bytes,
	                               UINT offset) {
	        float value = 0.0f;
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(&value, bytes.data() + offset, sizeof(value));
	        }
	        return value;
	    }

	    void D3D9DeviceImpl::swvpWriteFloat(std::vector<std::uint8_t>& bytes,
	                               UINT offset,
	                               float value) {
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(bytes.data() + offset, &value, sizeof(value));
	        }
	    }

	    DWORD D3D9DeviceImpl::swvpReadDword(const std::vector<std::uint8_t>& bytes,
	                               UINT offset) {
	        DWORD value = 0;
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(&value, bytes.data() + offset, sizeof(value));
	        }
	        return value;
	    }

	    void D3D9DeviceImpl::swvpWriteDword(std::vector<std::uint8_t>& bytes,
	                               UINT offset,
	                               DWORD value) {
	        if (offset + sizeof(value) <= bytes.size()) {
	            std::memcpy(bytes.data() + offset, &value, sizeof(value));
	        }
	    }

	    DWORD D3D9DeviceImpl::interpolateD3dColor(DWORD a, DWORD b, float t) {
	        auto channel = [&](UINT shift) -> DWORD {
	            const float av = static_cast<float>((a >> shift) & 0xffu);
	            const float bv = static_cast<float>((b >> shift) & 0xffu);
	            const float v = av + (bv - av) * t;
	            return static_cast<DWORD>(
	                std::clamp<int>(static_cast<int>(std::lround(v)), 0, 255));
	        };
	        return (channel(24u) << 24u) |
	               (channel(16u) << 16u) |
	               (channel(8u) << 8u) |
	               channel(0u);
	    }

	    D3D9DeviceImpl::SwvpClippedVertex D3D9DeviceImpl::interpolateTransformedSwvpVertex(
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            DWORD fvf,
	            UINT stride,
	            float t) {
	        SwvpClippedVertex out{a.bytes};
	        if (out.bytes.size() != stride) out.bytes.resize(stride);
	        t = std::clamp(t, 0.0f, 1.0f);

	        FvfProcessLayout layout{};
	        if (!describeProcessFvf(fvf, layout) || layout.stride > stride ||
	            layout.positionBytes < 16u) {
	            for (UINT offset = 0; offset + sizeof(float) <= stride; offset += 4u) {
	                const float av = swvpReadFloat(a.bytes, offset);
	                const float bv = swvpReadFloat(b.bytes, offset);
	                swvpWriteFloat(out.bytes, offset, av + (bv - av) * t);
	            }
	            return out;
	        }

	        auto lerpFloat = [&](UINT offset) {
	            const float av = swvpReadFloat(a.bytes, offset);
	            const float bv = swvpReadFloat(b.bytes, offset);
	            swvpWriteFloat(out.bytes, offset, av + (bv - av) * t);
	        };
	        for (UINT offset = layout.positionOffset;
	             offset < layout.positionOffset + layout.positionBytes;
	             offset += 4u) {
	            lerpFloat(offset);
	        }
	        if (layout.psize) lerpFloat(layout.psizeOffset);
	        if (layout.diffuse) {
	            swvpWriteDword(out.bytes, layout.diffuseOffset,
	                interpolateD3dColor(swvpReadDword(a.bytes, layout.diffuseOffset),
	                                    swvpReadDword(b.bytes, layout.diffuseOffset),
	                                    t));
	        }
	        if (layout.specular) {
	            swvpWriteDword(out.bytes, layout.specularOffset,
	                interpolateD3dColor(swvpReadDword(a.bytes, layout.specularOffset),
	                                    swvpReadDword(b.bytes, layout.specularOffset),
	                                    t));
	        }
	        for (UINT tex = 0; tex < layout.texCount; ++tex) {
	            for (UINT offset = layout.texOffset[tex];
	                 offset < layout.texOffset[tex] + layout.texBytes[tex];
	                 offset += 4u) {
	                lerpFloat(offset);
	            }
	        }
	        return out;
	    }

	    std::vector<std::uint32_t> D3D9DeviceImpl::transformedSwvpActiveClipPlanes() const {
	        std::vector<std::uint32_t> planes;
	        planes.reserve(13u);
	        planes.push_back(kSwvpClipOutsideEye);
	        const auto& vp = recorderState_.peState.viewportShadow();
	        if (vp.width != 0u) {
	            planes.push_back(kSwvpClipOutsideLeft);
	            planes.push_back(kSwvpClipOutsideRight);
	        }
	        if (vp.height != 0u) {
	            planes.push_back(kSwvpClipOutsideTop);
	            planes.push_back(kSwvpClipOutsideBottom);
	        }
	        planes.push_back(kSwvpClipOutsideNear);
	        planes.push_back(kSwvpClipOutsideFar);
	        const DWORD userClipMask =
	            renderStateValue(D3DRS_CLIPPLANEENABLE) & 0x3fu;
	        for (UINT i = 0; i < 6u; ++i) {
	            if (userClipMask & (1u << i)) {
	                planes.push_back(kSwvpClipOutsideUserPlane0 << i);
	            }
	        }
	        return planes;
	    }

	    float D3D9DeviceImpl::transformedSwvpVertexPlaneDistance(
	            const SwvpClippedVertex& vertex,
	            std::uint32_t planeFlag) const {
	        const float x = swvpReadFloat(vertex.bytes, 0u);
	        const float y = swvpReadFloat(vertex.bytes, 4u);
	        const float z = swvpReadFloat(vertex.bytes, 8u);
	        const float rhw = swvpReadFloat(vertex.bytes, 12u);
	        if (!std::isfinite(x) || !std::isfinite(y) ||
	            !std::isfinite(z) || !std::isfinite(rhw)) {
	            return -1.0f;
	        }
	        const auto& vp = recorderState_.peState.viewportShadow();
	        const float left = static_cast<float>(vp.x);
	        const float right = left + static_cast<float>(vp.width);
	        const float top = static_cast<float>(vp.y);
	        const float bottom = top + static_cast<float>(vp.height);
	        switch (planeFlag) {
	            case kSwvpClipOutsideEye:
	                return rhw - 1.0e-6f;
	            case kSwvpClipOutsideLeft:
	                return x - left;
	            case kSwvpClipOutsideRight:
	                return right - x;
	            case kSwvpClipOutsideTop:
	                return y - top;
	            case kSwvpClipOutsideBottom:
	                return bottom - y;
	            case kSwvpClipOutsideNear:
	                return z - std::min(vp.minZ, vp.maxZ);
	            case kSwvpClipOutsideFar:
	                return std::max(vp.minZ, vp.maxZ) - z;
	            default:
	                break;
	        }
	        if (planeFlag >= kSwvpClipOutsideUserPlane0 &&
	            planeFlag < (kSwvpClipOutsideUserPlane0 << 6u) && rhw != 0.0f &&
	            vp.width != 0u && vp.height != 0u &&
	            vp.maxZ != vp.minZ) {
	            UINT userPlane = 0u;
	            for (; userPlane < 6u; ++userPlane) {
	                if (planeFlag == (kSwvpClipOutsideUserPlane0 << userPlane)) break;
	            }
	            if (userPlane < 6u) {
	                const float scaleX = static_cast<float>(vp.width) * 0.5f;
	                const float scaleY = static_cast<float>(vp.height) * 0.5f;
	                const float offsetX = static_cast<float>(vp.x) + scaleX;
	                const float offsetY = static_cast<float>(vp.y) + scaleY;
	                const float w = 1.0f / rhw;
	                const float clip[4] = {
	                    ((x - offsetX) / scaleX) * w,
	                    (-(y - offsetY) / scaleY) * w,
	                    ((z - vp.minZ) / (vp.maxZ - vp.minZ)) * w,
	                    w,
	                };
	                const float* plane = &recorderState_.peState.clipPlaneShadow()[userPlane * 4u];
	                const float distance = plane[0] * clip[0] +
	                                       plane[1] * clip[1] +
	                                       plane[2] * clip[2] +
	                                       plane[3] * clip[3];
	                return std::isfinite(distance) ? distance : -1.0f;
	            }
	        }
	        return 1.0f;
	    }

	    HRESULT D3D9DeviceImpl::clipTransformedSwvpTriangle(
	            DWORD fvf,
	            UINT stride,
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            const SwvpClippedVertex& c,
	            std::vector<std::uint8_t>& out,
	            UINT& primitiveCount) const {
	        if (stride < 16u || a.bytes.size() != stride ||
	            b.bytes.size() != stride || c.bytes.size() != stride) {
	            return D3DERR_INVALIDCALL;
	        }
	        std::vector<SwvpClippedVertex> polygon{a, b, c};
	        std::vector<SwvpClippedVertex> clipped;
	        const auto planes = transformedSwvpActiveClipPlanes();
	        for (std::uint32_t plane : planes) {
	            if (polygon.empty()) break;
	            clipped.clear();
	            clipped.reserve(polygon.size() + 1u);
	            SwvpClippedVertex previous = polygon.back();
	            float previousDistance =
	                transformedSwvpVertexPlaneDistance(previous, plane);
	            bool previousInside = previousDistance >= -1.0e-5f;
	            for (const auto& current : polygon) {
	                const float currentDistance =
	                    transformedSwvpVertexPlaneDistance(current, plane);
	                const bool currentInside = currentDistance >= -1.0e-5f;
	                if (currentInside != previousInside) {
	                    const float denom = previousDistance - currentDistance;
	                    const float t = std::fabs(denom) > 1.0e-12f
	                        ? previousDistance / denom : 0.0f;
	                    clipped.push_back(interpolateTransformedSwvpVertex(
	                        previous, current, fvf, stride, t));
	                }
	                if (currentInside) clipped.push_back(current);
	                previous = current;
	                previousDistance = currentDistance;
	                previousInside = currentInside;
	            }
	            polygon = clipped;
	        }
	        if (polygon.size() < 3u) return S_OK;
	        for (size_t i = 1; i + 1u < polygon.size(); ++i) {
	            out.insert(out.end(), polygon[0].bytes.begin(), polygon[0].bytes.end());
	            out.insert(out.end(), polygon[i].bytes.begin(), polygon[i].bytes.end());
	            out.insert(out.end(), polygon[i + 1u].bytes.begin(),
	                       polygon[i + 1u].bytes.end());
	            ++primitiveCount;
	        }
	        return S_OK;
	    }

	    HRESULT D3D9DeviceImpl::clipTransformedSwvpLine(
	            DWORD fvf,
	            UINT stride,
	            const SwvpClippedVertex& a,
	            const SwvpClippedVertex& b,
	            std::vector<std::uint8_t>& out,
	            UINT& primitiveCount) const {
	        if (stride < 16u || a.bytes.size() != stride ||
	            b.bytes.size() != stride) {
	            return D3DERR_INVALIDCALL;
	        }
	        SwvpClippedVertex start = a;
	        SwvpClippedVertex end = b;
	        const auto planes = transformedSwvpActiveClipPlanes();
	        for (std::uint32_t plane : planes) {
	            const float startDistance =
	                transformedSwvpVertexPlaneDistance(start, plane);
	            const float endDistance =
	                transformedSwvpVertexPlaneDistance(end, plane);
	            const bool startInside = startDistance >= -1.0e-5f;
	            const bool endInside = endDistance >= -1.0e-5f;
	            if (!startInside && !endInside) return S_OK;
	            if (startInside && endInside) continue;

	            const float denom = startDistance - endDistance;
	            const float t = std::fabs(denom) > 1.0e-12f
	                ? startDistance / denom : 0.0f;
	            SwvpClippedVertex intersection =
	                interpolateTransformedSwvpVertex(start, end, fvf, stride, t);
	            if (!startInside) {
	                start = std::move(intersection);
	            } else {
	                end = std::move(intersection);
	            }
	        }
	        out.insert(out.end(), start.bytes.begin(), start.bytes.end());
	        out.insert(out.end(), end.bytes.begin(), end.bytes.end());
	        ++primitiveCount;
	        return S_OK;
	    }

DWORD D3D9DeviceImpl::processFfpDeclarationOutputFvf(
        const FvfProcessLayout& srcLayout,
        bool lighting,
        bool specularLighting,
        bool allowBlendAttributes) {
    if (srcLayout.positionBytes == 0u) {
        return 0u;
    }
    if (!allowBlendAttributes &&
        (srcLayout.blendWeight || srcLayout.blendIndices)) {
        return 0u;
    }
    DWORD outputFvf = D3DFVF_XYZRHW;
    if (srcLayout.psize) outputFvf |= D3DFVF_PSIZE;
    if (lighting || srcLayout.diffuse) outputFvf |= D3DFVF_DIFFUSE;
    if (specularLighting || srcLayout.specular) outputFvf |= D3DFVF_SPECULAR;
    if (srcLayout.texCount > 8u) return 0u;
    for (UINT i = 0; i < srcLayout.texCount; ++i) {
        switch (srcLayout.texBytes[i]) {
            case 4u:
                outputFvf |= D3DFVF_TEXCOORDSIZE1(i);
                break;
            case 8u:
                outputFvf |= D3DFVF_TEXCOORDSIZE2(i);
                break;
            case 12u:
                outputFvf |= D3DFVF_TEXCOORDSIZE3(i);
                break;
            case 16u:
                outputFvf |= D3DFVF_TEXCOORDSIZE4(i);
                break;
            default:
                return 0u;
        }
    }
    outputFvf |= srcLayout.texCount << D3DFVF_TEXCOUNT_SHIFT;
    return outputFvf;
}

HRESULT D3D9DeviceImpl::describeSoftwareFfpDrawTarget(DWORD& outputFvf,
                                      FvfProcessLayout& srcLayout,
                                      FvfProcessLayout& dstLayout) {
    outputFvf = 0;
    srcLayout = {};
    dstLayout = {};
    if (!softwareVertexProcessing_ || vs_ != nullptr) {
        return S_FALSE;
    }
    const bool lighting = renderStateValue(D3DRS_LIGHTING) != FALSE;
    const bool specularLighting =
        lighting && renderStateValue(D3DRS_SPECULARENABLE) != FALSE;
    if (fvf_ != 0) {
        const DWORD positionMask = fvf_ & D3DFVF_POSITION_MASK;
        if ((positionMask != D3DFVF_XYZ &&
             !processFvfXyzbPosition(positionMask)) ||
            !describeProcessFvf(fvf_, srcLayout)) {
            return S_FALSE;
        }
        outputFvf = processFfpDeclarationOutputFvf(
            srcLayout, lighting, specularLighting, true);
        if (outputFvf == 0u) return S_FALSE;
    } else if (vdecl_) {
        D3D9PeValidatedDeclaration validatedDeclaration{};
        if (FAILED(D3D9PeValidateVertexDecl(
                vdecl_, static_cast<IDirect3DDevice9*>(this),
                &validatedDeclaration)) ||
            !describeProcessDeclaration(vdecl_, srcLayout, false,
                                         validatedDeclaration.raw())) {
            return S_FALSE;
        }
        outputFvf = processFfpDeclarationOutputFvf(
            srcLayout, lighting, specularLighting, true);
        if (outputFvf == 0u) return S_FALSE;
    } else {
        return S_FALSE;
    }
    if (lighting && !srcLayout.normal) {
        return S_FALSE;
    }
    if (!describeProcessFvf(outputFvf, dstLayout) ||
        dstLayout.positionBytes != 16u) {
        return S_FALSE;
    }
    return S_OK;
}

DWORD D3D9DeviceImpl::processProgrammableOutputFvf(const ProcessShaderIo& shaderIo) {
    DWORD outputFvf = D3DFVF_XYZRHW;
    if (shaderIo.hasOutputPSize) outputFvf |= D3DFVF_PSIZE;
    if (shaderIo.hasOutputDiffuse) outputFvf |= D3DFVF_DIFFUSE;
    if (shaderIo.hasOutputSpecular) outputFvf |= D3DFVF_SPECULAR;
    int highestTex = -1;
    for (UINT i = 0; i < 8u; ++i) {
        if (shaderIo.hasOutputTex[i]) highestTex = static_cast<int>(i);
    }
    if (highestTex >= 0) {
        for (int i = 0; i <= highestTex; ++i) {
            if (!shaderIo.hasOutputTex[i]) return 0u;
            outputFvf |= D3DFVF_TEXCOORDSIZE4(i);
        }
        outputFvf |= static_cast<DWORD>(highestTex + 1)
                     << D3DFVF_TEXCOUNT_SHIFT;
    }
    return outputFvf;
}

bool D3D9DeviceImpl::processLayoutUsesOnlyStream0(const FvfProcessLayout& layout) {
    for (UINT stream = 1; stream < D9C_DRAW_PACKET_MAX_STREAMS; ++stream) {
        if (layout.streamStride[stream] != 0u) return false;
    }
    return true;
}

bool D3D9DeviceImpl::softwareDrawCanConcatenateInstances(D3DPRIMITIVETYPE type) {
    return type == D3DPT_POINTLIST ||
           type == D3DPT_LINELIST ||
           type == D3DPT_TRIANGLELIST;
}

bool D3D9DeviceImpl::softwareDrawCanExpandInstances(D3DPRIMITIVETYPE type) {
    return softwareDrawCanConcatenateInstances(type) ||
           type == D3DPT_LINESTRIP ||
           type == D3DPT_TRIANGLESTRIP ||
           type == D3DPT_TRIANGLEFAN;
}

D3DPRIMITIVETYPE D3D9DeviceImpl::softwareDrawExpandedPrimitiveType(
        D3DPRIMITIVETYPE type) {
    if (type == D3DPT_LINESTRIP) return D3DPT_LINELIST;
    if (type == D3DPT_TRIANGLESTRIP || type == D3DPT_TRIANGLEFAN) {
        return D3DPT_TRIANGLELIST;
    }
    return type;
}

HRESULT D3D9DeviceImpl::appendSoftwarePrimitiveVertices(
        const std::vector<std::uint8_t>& source,
        UINT stride,
        D3DPRIMITIVETYPE type,
        UINT primitiveCount,
        std::vector<std::uint8_t>& out) {
    if (stride == 0u) return D3DERR_INVALIDCALL;
    auto appendVertex = [&](UINT index) -> HRESULT {
        const std::uint64_t offset =
            static_cast<std::uint64_t>(index) * stride;
        if (offset > source.size() ||
            stride > source.size() - static_cast<size_t>(offset)) {
            return D3DERR_INVALIDCALL;
        }
        out.insert(out.end(),
                   source.begin() + static_cast<size_t>(offset),
                   source.begin() + static_cast<size_t>(offset) + stride);
        return S_OK;
    };
    switch (type) {
        case D3DPT_POINTLIST:
        case D3DPT_LINELIST:
        case D3DPT_TRIANGLELIST:
            out.insert(out.end(), source.begin(), source.end());
            return S_OK;
        case D3DPT_LINESTRIP:
            for (UINT i = 0; i < primitiveCount; ++i) {
                HRESULT hr = appendVertex(i);
                if (FAILED(hr)) return hr;
                hr = appendVertex(i + 1u);
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        case D3DPT_TRIANGLESTRIP:
            for (UINT i = 0; i < primitiveCount; ++i) {
                const UINT a = (i & 1u) ? i + 1u : i;
                const UINT b = (i & 1u) ? i : i + 1u;
                HRESULT hr = appendVertex(a);
                if (FAILED(hr)) return hr;
                hr = appendVertex(b);
                if (FAILED(hr)) return hr;
                hr = appendVertex(i + 2u);
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        case D3DPT_TRIANGLEFAN:
            for (UINT i = 0; i < primitiveCount; ++i) {
                HRESULT hr = appendVertex(0u);
                if (FAILED(hr)) return hr;
                hr = appendVertex(i + 1u);
                if (FAILED(hr)) return hr;
                hr = appendVertex(i + 2u);
                if (FAILED(hr)) return hr;
            }
            return S_OK;
	    default:
            return D3DERR_INVALIDCALL;
    }
}

HRESULT D3D9DeviceImpl::filterSoftwareDrawOutsideClipPrimitives(SoftwareFfpDrawData& draw) {
    if (renderStateValue(D3DRS_CLIPPING) == FALSE ||
        draw.vertices.empty() || draw.stride < 16u ||
        draw.primitiveCount == 0u) {
        return S_OK;
    }
    if (draw.vertices.size() % draw.stride != 0u) {
        return D3DERR_INVALIDCALL;
    }
    const UINT vertexCount =
        static_cast<UINT>(draw.vertices.size() / draw.stride);
    const UINT expectedVertexCount =
        primitiveVertexCount(draw.primitiveType, draw.primitiveCount);
    if (expectedVertexCount > vertexCount) {
        return D3DERR_INVALIDCALL;
    }

	        std::vector<std::uint8_t> filtered;
	        filtered.reserve(draw.vertices.size());
	        UINT keptPrimitiveCount = 0u;
	        bool droppedAny = false;
	        bool clippedTriangles = false;
	        bool clippedLines = false;

    auto clipFlags = [&](UINT index) {
        return transformedSwvpVertexClipFlags(
            draw.vertices, draw.stride, index);
    };
    auto vertexInside = [&](UINT index) {
        return clipFlags(index) == 0u;
    };
	        auto appendVertex = [&](UINT index) {
	            return appendTransformedSwvpVertex(
	                draw.vertices, draw.stride, index, filtered);
    };

    switch (draw.primitiveType) {
        case D3DPT_POINTLIST:
            for (UINT i = 0; i < draw.primitiveCount; ++i) {
                if (!vertexInside(i)) {
                    droppedAny = true;
                    continue;
                }
                HRESULT hr = appendVertex(i);
                if (FAILED(hr)) return hr;
                ++keptPrimitiveCount;
            }
            break;
	            case D3DPT_LINELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = i * 2u;
	                    const UINT b = a + 1u;
	                    SwvpClippedVertex av{}, bv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b)) != 0u) {
	                        clippedLines = true;
	                    }
	                }
	                break;
	            case D3DPT_LINESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    SwvpClippedVertex av{}, bv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, i, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, i + 1u, bv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(i) | clipFlags(i + 1u)) != 0u) {
	                        clippedLines = true;
	                    }
	                }
	                draw.primitiveType = D3DPT_LINELIST;
	                clippedLines = true;
	                break;
	            case D3DPT_TRIANGLELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = i * 3u;
	                    const UINT b = a + 1u;
	                    const UINT c = a + 2u;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                clippedTriangles = clippedTriangles || droppedAny;
	                break;
	            case D3DPT_TRIANGLESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = (i & 1u) ? i + 1u : i;
	                    const UINT b = (i & 1u) ? i : i + 1u;
	                    const UINT c = i + 2u;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
	            case D3DPT_TRIANGLEFAN:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    const UINT a = 0u;
	                    const UINT b = i + 1u;
	                    const UINT c = i + 2u;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    HRESULT hr = copyTransformedSwvpVertex(
	                        draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, filtered,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
        default:
            return D3DERR_INVALIDCALL;
    }

	        if (!droppedAny && !clippedTriangles && !clippedLines) return S_OK;
	        if (clippedTriangles) draw.primitiveType = D3DPT_TRIANGLELIST;
	        if (clippedLines) draw.primitiveType = D3DPT_LINELIST;
	        draw.vertices = std::move(filtered);
	        draw.primitiveCount = keptPrimitiveCount;
	        return S_OK;
}

HRESULT D3D9DeviceImpl::readSoftwareIndexValue(const std::vector<std::uint8_t>& indices,
                                      D3DFORMAT indexFormat,
                                      UINT ordinal,
                                      DWORD& out) {
    if (indexFormat != D3DFMT_INDEX16 && indexFormat != D3DFMT_INDEX32) {
        return D3DERR_INVALIDCALL;
    }
    const UINT indexSize = indexFormat == D3DFMT_INDEX32 ? 4u : 2u;
    const std::uint64_t offset =
        static_cast<std::uint64_t>(ordinal) * indexSize;
    if (offset > indices.size() ||
        indexSize > indices.size() - static_cast<size_t>(offset)) {
        return D3DERR_INVALIDCALL;
    }
    if (indexFormat == D3DFMT_INDEX32) {
        std::memcpy(&out, indices.data() + static_cast<size_t>(offset),
                    sizeof(out));
    } else {
        WORD index16 = 0;
        std::memcpy(&index16, indices.data() + static_cast<size_t>(offset),
                    sizeof(index16));
        out = index16;
    }
    return S_OK;
}

HRESULT D3D9DeviceImpl::appendSoftwareIndex32(std::vector<std::uint8_t>& indices,
                                     DWORD index) {
    const auto oldSize = indices.size();
    indices.resize(oldSize + sizeof(index));
    std::memcpy(indices.data() + oldSize, &index, sizeof(index));
    return S_OK;
}

HRESULT D3D9DeviceImpl::filterSoftwareIndexedDrawOutsideClipPrimitives(
        SoftwareFfpDrawData& draw,
        std::vector<std::uint8_t>& indices,
        D3DFORMAT& indexFormat) {
    if (renderStateValue(D3DRS_CLIPPING) == FALSE ||
        draw.vertices.empty() || draw.stride < 16u ||
        draw.primitiveCount == 0u) {
        return S_OK;
    }
    if (draw.vertices.size() % draw.stride != 0u) {
        return D3DERR_INVALIDCALL;
    }
    const UINT vertexCount =
        static_cast<UINT>(draw.vertices.size() / draw.stride);
    const UINT sourceIndexCount =
        primitiveVertexCount(draw.primitiveType, draw.primitiveCount);
    const UINT sourceIndexSize = indexFormat == D3DFMT_INDEX32 ? 4u : 2u;
    if ((indexFormat != D3DFMT_INDEX16 && indexFormat != D3DFMT_INDEX32) ||
        static_cast<std::uint64_t>(sourceIndexCount) * sourceIndexSize >
            indices.size()) {
        return D3DERR_INVALIDCALL;
    }

	        std::vector<std::uint8_t> filtered;
	        filtered.reserve(static_cast<size_t>(sourceIndexCount) * sizeof(DWORD));
	        std::vector<std::uint8_t> clippedVertices;
	        UINT keptPrimitiveCount = 0u;
	        bool clippedTriangles = false;
	        bool clippedLines = false;

    auto readIndex = [&](UINT ordinal, DWORD& outIndex) -> HRESULT {
        HRESULT hr = readSoftwareIndexValue(indices, indexFormat, ordinal,
                                            outIndex);
        if (FAILED(hr)) return hr;
        if (outIndex >= vertexCount) return D3DERR_INVALIDCALL;
        return S_OK;
    };
    auto clipFlags = [&](DWORD index) {
        return transformedSwvpVertexClipFlags(
            draw.vertices, draw.stride, index);
    };
    auto vertexInside = [&](DWORD index) {
        return clipFlags(index) == 0u;
    };
    auto appendIndex = [&](DWORD index) -> HRESULT {
        return appendSoftwareIndex32(filtered, index);
    };
	        switch (draw.primitiveType) {
        case D3DPT_POINTLIST:
            for (UINT i = 0; i < draw.primitiveCount; ++i) {
                DWORD a = 0;
                HRESULT hr = readIndex(i, a);
                if (FAILED(hr)) return hr;
                if (!vertexInside(a)) continue;
                hr = appendIndex(a);
                if (FAILED(hr)) return hr;
                ++keptPrimitiveCount;
            }
            break;
	            case D3DPT_LINELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    DWORD a = 0, b = 0;
	                    HRESULT hr = readIndex(i * 2u, a);
	                    if (FAILED(hr)) return hr;
	                    hr = readIndex(i * 2u + 1u, b);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b)) != 0u) {
	                        clippedLines = true;
	                    }
	                }
	                clippedLines = true;
	                break;
	            case D3DPT_LINESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    DWORD a = 0, b = 0;
	                    HRESULT hr = readIndex(i, a);
	                    if (FAILED(hr)) return hr;
	                    hr = readIndex(i + 1u, b);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = clipTransformedSwvpLine(
	                        draw.fvf, draw.stride, av, bv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                }
	                draw.primitiveType = D3DPT_LINELIST;
	                clippedLines = true;
	                break;
	            case D3DPT_TRIANGLELIST:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
	                    DWORD a = 0, b = 0, c = 0;
	                    HRESULT hr = readIndex(i * 3u, a);
                if (FAILED(hr)) return hr;
                hr = readIndex(i * 3u + 1u, b);
                if (FAILED(hr)) return hr;
	                    hr = readIndex(i * 3u + 2u, c);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    const UINT beforeCount = keptPrimitiveCount;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                    if (keptPrimitiveCount != beforeCount + 1u ||
	                        (clipFlags(a) | clipFlags(b) | clipFlags(c)) != 0u) {
	                        clippedTriangles = true;
	                    }
	                }
	                clippedTriangles = true;
	                break;
	            case D3DPT_TRIANGLESTRIP:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
                DWORD a = 0, b = 0, c = 0;
                HRESULT hr = readIndex((i & 1u) ? i + 1u : i, a);
                if (FAILED(hr)) return hr;
                hr = readIndex((i & 1u) ? i : i + 1u, b);
                if (FAILED(hr)) return hr;
	                    hr = readIndex(i + 2u, c);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
	            case D3DPT_TRIANGLEFAN:
	                for (UINT i = 0; i < draw.primitiveCount; ++i) {
                DWORD a = 0, b = 0, c = 0;
                HRESULT hr = readIndex(0u, a);
                if (FAILED(hr)) return hr;
                hr = readIndex(i + 1u, b);
                if (FAILED(hr)) return hr;
	                    hr = readIndex(i + 2u, c);
	                    if (FAILED(hr)) return hr;
	                    SwvpClippedVertex av{}, bv{}, cv{};
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, a, av);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, b, bv);
	                    if (FAILED(hr)) return hr;
	                    hr = copyTransformedSwvpVertex(draw.vertices, draw.stride, c, cv);
	                    if (FAILED(hr)) return hr;
	                    hr = clipTransformedSwvpTriangle(
	                        draw.fvf, draw.stride, av, bv, cv, clippedVertices,
	                        keptPrimitiveCount);
	                    if (FAILED(hr)) return hr;
	                }
	                draw.primitiveType = D3DPT_TRIANGLELIST;
	                clippedTriangles = true;
	                break;
        default:
            return D3DERR_INVALIDCALL;
    }

	        if (clippedLines || clippedTriangles) {
	            const UINT vertexCount =
	                draw.stride != 0u
	                    ? static_cast<UINT>(clippedVertices.size() / draw.stride)
	                    : 0u;
	            filtered.clear();
	            filtered.reserve(static_cast<size_t>(vertexCount) * sizeof(DWORD));
	            for (UINT i = 0; i < vertexCount; ++i) {
	                HRESULT hr = appendSoftwareIndex32(filtered, i);
	                if (FAILED(hr)) return hr;
	            }
	            draw.vertices = std::move(clippedVertices);
	            draw.primitiveType = clippedTriangles
	                ? D3DPT_TRIANGLELIST : D3DPT_LINELIST;
	        }
	        indices = std::move(filtered);
	        indexFormat = D3DFMT_INDEX32;
	        draw.primitiveCount = keptPrimitiveCount;
	        return S_OK;
	    }

UINT D3D9DeviceImpl::softwareDrawInstanceCount() const {
    if ((streamFreq_[0] & D3DSTREAMSOURCE_INDEXEDDATA) == 0u) {
        return 1u;
    }
    const UINT count = streamFreq_[0] & 0x3fffffffu;
    return count ? count : 1u;
}

HRESULT D3D9DeviceImpl::applySoftwareInstanceStreamOffsets(UINT instance,
                                           UINT savedOffsets[16]) {
    for (UINT stream = 0; stream < 16u; ++stream) {
        savedOffsets[stream] = streamOff_[stream];
    }
    for (UINT stream = 1; stream < 16u; ++stream) {
        if ((streamFreq_[stream] & D3DSTREAMSOURCE_INSTANCEDATA) == 0u) {
            continue;
        }
        const UINT divider = std::max<UINT>(streamFreq_[stream] & 0x3fffffffu, 1u);
        const UINT element = instance / divider;
        const std::uint64_t offset =
            static_cast<std::uint64_t>(streamOff_[stream]) +
            static_cast<std::uint64_t>(element) * streamStr_[stream];
        if (offset > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        streamOff_[stream] = static_cast<UINT>(offset);
    }
    return S_OK;
}

void D3D9DeviceImpl::restoreSoftwareInstanceStreamOffsets(const UINT savedOffsets[16]) {
    for (UINT stream = 0; stream < 16u; ++stream) {
        streamOff_[stream] = savedOffsets[stream];
    }
}

HRESULT D3D9DeviceImpl::describeSoftwareProgrammableDrawTarget(
    DWORD& outputFvf,
    FvfProcessLayout& srcLayout,
    FvfProcessLayout& dstLayout) {
    outputFvf = 0;
    srcLayout = {};
    dstLayout = {};
    if (!softwareVertexProcessing_ || !vs_) {
        return S_FALSE;
    }
    if (fvf_ != 0) {
        const DWORD positionMask = fvf_ & D3DFVF_POSITION_MASK;
        if ((positionMask != D3DFVF_XYZ &&
             positionMask != D3DFVF_XYZW &&
             !processFvfXyzbPosition(positionMask)) ||
            !describeProcessFvf(fvf_, srcLayout)) {
            return S_FALSE;
        }
    } else if (vdecl_) {
        D3D9PeValidatedDeclaration validatedDeclaration{};
        if (FAILED(D3D9PeValidateVertexDecl(
                vdecl_, static_cast<IDirect3DDevice9*>(this),
                &validatedDeclaration)) ||
            !describeProcessDeclaration(vdecl_, srcLayout, false,
                                         validatedDeclaration.raw())) {
            return S_FALSE;
        }
    } else {
        return S_FALSE;
    }
    UINT shaderBytes = 0;
    HRESULT hr = vs_->GetFunction(nullptr, &shaderBytes);
    if (FAILED(hr) || shaderBytes == 0u ||
        (shaderBytes % sizeof(DWORD)) != 0u) {
        return S_FALSE;
    }
    std::vector<DWORD> shaderWords(shaderBytes / sizeof(DWORD));
    hr = vs_->GetFunction(shaderWords.data(), &shaderBytes);
    ProcessShaderIo shaderIo{};
    if (FAILED(hr) ||
        !analyzeSimpleProcessVertexShader(shaderWords, shaderIo)) {
        return S_FALSE;
    }
    outputFvf = processProgrammableOutputFvf(shaderIo);
    if (outputFvf == 0u ||
        !describeProcessFvf(outputFvf, dstLayout) ||
        dstLayout.positionBytes != 16u) {
        outputFvf = 0;
        return S_FALSE;
    }
    return S_OK;
}

HRESULT D3D9DeviceImpl::readTransformedVertexBuffer(IDirect3DVertexBuffer9* dstBuffer,
                                    UINT bytes,
                                    SoftwareFfpDrawData& out,
                                    DWORD outputFvf,
                                    UINT outputStride) {
    out = {};
    if (bytes == 0u) return S_FALSE;
    void* mapped = nullptr;
    HRESULT hr = dstBuffer->Lock(0, bytes, &mapped, D3DLOCK_READONLY);
    if (FAILED(hr)) return hr;
    SwvpUnlockGuard unlockGuard(dstBuffer);
    if (!mapped) {
        return D3DERR_INVALIDCALL;
    }
    out.vertices.resize(bytes);
    std::memcpy(out.vertices.data(), mapped, bytes);
    hr = dstBuffer->Unlock();
    unlockGuard.dismiss();
    if (FAILED(hr)) return hr;
    out.fvf = outputFvf;
    out.stride = outputStride;
    return S_OK;
}

HRESULT D3D9DeviceImpl::trySoftwareProgrammableTransformBoundVertices(
    UINT startVertex,
    UINT vertexCount,
    SoftwareFfpDrawData& out) {
    out = {};
    if (vertexCount == 0u) return S_FALSE;
    DWORD outputFvf = 0;
    FvfProcessLayout srcLayout{};
    FvfProcessLayout dstLayout{};
    HRESULT hr = describeSoftwareProgrammableDrawTarget(
        outputFvf, srcLayout, dstLayout);
    if (hr != S_OK) return hr;
    std::uint32_t outputBytes = 0;
    if (!checkedByteCount(vertexCount, dstLayout.stride, outputBytes)) {
        return D3DERR_INVALIDCALL;
    }
    IDirect3DVertexBuffer9* dstBuffer = nullptr;
    SwvpComReleaseGuard dstBufferGuard(dstBuffer);
    hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                            &dstBuffer, nullptr);
    if (FAILED(hr)) return hr;
    hr = ProcessVertices(startVertex, 0, vertexCount, dstBuffer, nullptr, 0);
    if (SUCCEEDED(hr)) {
        hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                         outputFvf, dstLayout.stride);
        if (SUCCEEDED(hr)) out.bypassVertexShader = true;
    }
    return hr;
}

HRESULT D3D9DeviceImpl::trySoftwareFfpTransformBoundVertices(UINT startVertex,
                                             UINT vertexCount,
                                             SoftwareFfpDrawData& out) {
    out = {};
    if (vertexCount == 0u) return S_FALSE;
    DWORD outputFvf = 0;
    FvfProcessLayout srcLayout{};
    FvfProcessLayout dstLayout{};
    HRESULT hr = describeSoftwareFfpDrawTarget(outputFvf, srcLayout, dstLayout);
    if (hr != S_OK) return hr;
    if (!streamSrc_[0] || streamStr_[0] < srcLayout.stride) {
        return S_FALSE;
    }
    std::uint32_t outputBytes = 0;
    if (!checkedByteCount(vertexCount, dstLayout.stride, outputBytes)) {
        return D3DERR_INVALIDCALL;
    }
    IDirect3DVertexBuffer9* dstBuffer = nullptr;
    SwvpComReleaseGuard dstBufferGuard(dstBuffer);
    hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                            &dstBuffer, nullptr);
    if (FAILED(hr)) return hr;
    hr = ProcessVertices(startVertex, 0, vertexCount, dstBuffer, nullptr, 0);
    if (SUCCEEDED(hr)) {
        hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                         outputFvf, dstLayout.stride);
    }
    return hr;
}

HRESULT D3D9DeviceImpl::trySoftwareFfpDrawPrimitive(D3DPRIMITIVETYPE type,
                                    UINT startVertex,
                                    UINT primitiveCount,
                                    SoftwareFfpDrawData& out) {
    out = {};
    // Hoisted shared conjunct only -- see trySoftwareFfpDrawIndexedPrimitive.
    if (!softwareVertexProcessing_) return S_FALSE;
    const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
    const UINT instanceCount = softwareDrawInstanceCount();
    if (instanceCount <= 1u) {
        HRESULT hr = trySoftwareFfpTransformBoundVertices(startVertex, vertexCount, out);
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
        }
        return hr;
    }
    if (!softwareDrawCanExpandInstances(type) ||
        primitiveCount > 0xffffffffu / instanceCount) {
        return S_FALSE;
    }
    out.primitiveType = softwareDrawExpandedPrimitiveType(type);
    for (UINT instance = 0; instance < instanceCount; ++instance) {
        UINT savedOffsets[16]{};
        HRESULT hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
        if (SUCCEEDED(hr)) {
            SoftwareFfpDrawData instanceDraw{};
            hr = trySoftwareFfpTransformBoundVertices(startVertex, vertexCount,
                                                      instanceDraw);
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                if (out.vertices.empty()) {
                    out.fvf = instanceDraw.fvf;
                    out.stride = instanceDraw.stride;
                    out.bypassVertexShader = instanceDraw.bypassVertexShader;
                } else if (out.fvf != instanceDraw.fvf ||
                           out.stride != instanceDraw.stride ||
                           out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                    hr = D3DERR_INVALIDCALL;
                }
                if (SUCCEEDED(hr)) {
                    hr = appendSoftwarePrimitiveVertices(
                        instanceDraw.vertices, instanceDraw.stride, type,
                        primitiveCount, out.vertices);
                }
            }
        }
        restoreSoftwareInstanceStreamOffsets(savedOffsets);
        if (FAILED(hr) || hr == S_FALSE) {
            out = {};
            return hr;
        }
    }
    out.primitiveCount = primitiveCount * instanceCount;
    return S_OK;
}

HRESULT D3D9DeviceImpl::trySoftwareProgrammableDrawPrimitive(D3DPRIMITIVETYPE type,
                                             UINT startVertex,
                                             UINT primitiveCount,
                                             SoftwareFfpDrawData& out) {
    out = {};
    // Hoisted shared conjunct only -- see trySoftwareFfpDrawIndexedPrimitive.
    if (!softwareVertexProcessing_) return S_FALSE;
    const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
    const UINT instanceCount = softwareDrawInstanceCount();
    if (instanceCount <= 1u) {
        HRESULT hr = trySoftwareProgrammableTransformBoundVertices(
            startVertex, vertexCount, out);
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
        }
        return hr;
    }
    if (!softwareDrawCanExpandInstances(type) ||
        primitiveCount > 0xffffffffu / instanceCount) {
        return S_FALSE;
    }
    out.primitiveType = softwareDrawExpandedPrimitiveType(type);
    for (UINT instance = 0; instance < instanceCount; ++instance) {
        UINT savedOffsets[16]{};
        HRESULT hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
        if (SUCCEEDED(hr)) {
            SoftwareFfpDrawData instanceDraw{};
            hr = trySoftwareProgrammableTransformBoundVertices(
                startVertex, vertexCount, instanceDraw);
            if (SUCCEEDED(hr) && hr != S_FALSE) {
                if (out.vertices.empty()) {
                    out.fvf = instanceDraw.fvf;
                    out.stride = instanceDraw.stride;
                    out.bypassVertexShader = instanceDraw.bypassVertexShader;
                } else if (out.fvf != instanceDraw.fvf ||
                           out.stride != instanceDraw.stride ||
                           out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                    hr = D3DERR_INVALIDCALL;
                }
                if (SUCCEEDED(hr)) {
                    hr = appendSoftwarePrimitiveVertices(
                        instanceDraw.vertices, instanceDraw.stride, type,
                        primitiveCount, out.vertices);
                }
            }
        }
        restoreSoftwareInstanceStreamOffsets(savedOffsets);
        if (FAILED(hr) || hr == S_FALSE) {
            out = {};
            return hr;
        }
    }
    out.primitiveCount = primitiveCount * instanceCount;
    return S_OK;
}

HRESULT D3D9DeviceImpl::readSoftwareFfpAdjustedIndices(UINT startIndex,
                                       UINT indexCount,
                                       UINT minVertex,
                                       UINT numVertices,
                                       std::vector<std::uint8_t>& out,
                                       D3DFORMAT& indexFormat) {
    out.clear();
    indexFormat = D3DFMT_UNKNOWN;
    if (!indexBuf_ || indexCount == 0u || numVertices == 0u) return S_FALSE;
    D3DINDEXBUFFER_DESC desc{};
    HRESULT hr = indexBuf_->GetDesc(&desc);
    if (FAILED(hr)) return S_FALSE;
    if (desc.Format != D3DFMT_INDEX16 && desc.Format != D3DFMT_INDEX32) {
        return S_FALSE;
    }
    const UINT indexSize = desc.Format == D3DFMT_INDEX32 ? 4u : 2u;
    std::uint32_t indexBytes = 0;
    if (!checkedByteCount(indexCount, indexSize, indexBytes)) {
        return D3DERR_INVALIDCALL;
    }
    const std::uint64_t byteOffset =
        static_cast<std::uint64_t>(startIndex) * indexSize;
    if (byteOffset > 0xffffffffull ||
        indexBytes > desc.Size ||
        byteOffset > desc.Size - indexBytes) {
        return D3DERR_INVALIDCALL;
    }
    void* mapped = nullptr;
    hr = indexBuf_->Lock(static_cast<UINT>(byteOffset), indexBytes,
                         &mapped, D3DLOCK_READONLY);
    if (FAILED(hr)) return S_FALSE;
    SwvpIndexUnlockGuard unlockGuard(indexBuf_);
    if (!mapped) {
        return D3DERR_INVALIDCALL;
    }
    out.resize(indexBytes);
    bool supportedRange = true;
    if (desc.Format == D3DFMT_INDEX16) {
        const auto* src = static_cast<const std::uint8_t*>(mapped);
        for (UINT i = 0; i < indexCount; ++i) {
            WORD index = 0;
            std::memcpy(&index, src + i * indexSize, sizeof(index));
            if (index < minVertex || index - minVertex >= numVertices) {
                supportedRange = false;
                break;
            }
            const WORD adjusted = static_cast<WORD>(index - minVertex);
            std::memcpy(out.data() + i * indexSize, &adjusted, sizeof(adjusted));
        }
    } else {
        const auto* src = static_cast<const std::uint8_t*>(mapped);
        for (UINT i = 0; i < indexCount; ++i) {
            DWORD index = 0;
            std::memcpy(&index, src + i * indexSize, sizeof(index));
            if (index < minVertex || index - minVertex >= numVertices) {
                supportedRange = false;
                break;
            }
            const DWORD adjusted = index - minVertex;
            std::memcpy(out.data() + i * indexSize, &adjusted, sizeof(adjusted));
        }
    }
    hr = indexBuf_->Unlock();
    unlockGuard.dismiss();
    if (FAILED(hr)) return hr;
    if (!supportedRange) {
        out.clear();
        return S_FALSE;
    }
    indexFormat = desc.Format;
    return S_OK;
}

HRESULT D3D9DeviceImpl::appendSoftwareIndicesWithBase32(
    const std::vector<std::uint8_t>& source,
    D3DFORMAT sourceFormat,
    UINT indexCount,
    UINT baseVertex,
    std::vector<std::uint8_t>& out) {
    if (sourceFormat != D3DFMT_INDEX16 && sourceFormat != D3DFMT_INDEX32) {
        return D3DERR_INVALIDCALL;
    }
    const UINT sourceIndexSize = sourceFormat == D3DFMT_INDEX32 ? 4u : 2u;
    const std::uint64_t required =
        static_cast<std::uint64_t>(indexCount) * sourceIndexSize;
    if (required > source.size()) {
        return D3DERR_INVALIDCALL;
    }
    const auto* bytes = source.data();
    for (UINT i = 0; i < indexCount; ++i) {
        DWORD index = 0;
        if (sourceFormat == D3DFMT_INDEX32) {
            std::memcpy(&index, bytes + i * sourceIndexSize, sizeof(index));
        } else {
            WORD index16 = 0;
            std::memcpy(&index16, bytes + i * sourceIndexSize, sizeof(index16));
            index = index16;
        }
        const std::uint64_t adjusted =
            static_cast<std::uint64_t>(index) + baseVertex;
        if (adjusted > 0xffffffffull) {
            return D3DERR_INVALIDCALL;
        }
        const DWORD adjusted32 = static_cast<DWORD>(adjusted);
        const auto oldSize = out.size();
        out.resize(oldSize + sizeof(adjusted32));
        std::memcpy(out.data() + oldSize, &adjusted32, sizeof(adjusted32));
    }
    return S_OK;
}

HRESULT D3D9DeviceImpl::appendSoftwarePrimitiveIndicesWithBase32(
    const std::vector<std::uint8_t>& source,
    D3DFORMAT sourceFormat,
    D3DPRIMITIVETYPE type,
    UINT primitiveCount,
    UINT baseVertex,
    std::vector<std::uint8_t>& out) {
    if (sourceFormat != D3DFMT_INDEX16 && sourceFormat != D3DFMT_INDEX32) {
        return D3DERR_INVALIDCALL;
    }
    const UINT sourceIndexSize = sourceFormat == D3DFMT_INDEX32 ? 4u : 2u;
    const UINT sourceIndexCount = primitiveVertexCount(type, primitiveCount);
    const std::uint64_t required =
        static_cast<std::uint64_t>(sourceIndexCount) * sourceIndexSize;
    if (required > source.size()) {
        return D3DERR_INVALIDCALL;
    }
    auto readIndex = [&](UINT index, DWORD& outIndex) -> HRESULT {
        if (index >= sourceIndexCount) return D3DERR_INVALIDCALL;
        const auto* bytes = source.data() +
            static_cast<size_t>(index) * sourceIndexSize;
        if (sourceFormat == D3DFMT_INDEX32) {
            std::memcpy(&outIndex, bytes, sizeof(outIndex));
        } else {
            WORD index16 = 0;
            std::memcpy(&index16, bytes, sizeof(index16));
            outIndex = index16;
        }
        return S_OK;
    };
    auto appendIndex = [&](UINT index) -> HRESULT {
        DWORD value = 0;
        HRESULT hr = readIndex(index, value);
        if (FAILED(hr)) return hr;
        const std::uint64_t adjusted =
            static_cast<std::uint64_t>(value) + baseVertex;
        if (adjusted > 0xffffffffull) return D3DERR_INVALIDCALL;
        const DWORD adjusted32 = static_cast<DWORD>(adjusted);
        const auto oldSize = out.size();
        out.resize(oldSize + sizeof(adjusted32));
        std::memcpy(out.data() + oldSize, &adjusted32, sizeof(adjusted32));
        return S_OK;
    };
    switch (type) {
        case D3DPT_POINTLIST:
        case D3DPT_LINELIST:
        case D3DPT_TRIANGLELIST:
            return appendSoftwareIndicesWithBase32(
                source, sourceFormat, sourceIndexCount, baseVertex, out);
        case D3DPT_LINESTRIP:
            for (UINT i = 0; i < primitiveCount; ++i) {
                HRESULT hr = appendIndex(i);
                if (FAILED(hr)) return hr;
                hr = appendIndex(i + 1u);
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        case D3DPT_TRIANGLESTRIP:
            for (UINT i = 0; i < primitiveCount; ++i) {
                const UINT a = (i & 1u) ? i + 1u : i;
                const UINT b = (i & 1u) ? i : i + 1u;
                HRESULT hr = appendIndex(a);
                if (FAILED(hr)) return hr;
                hr = appendIndex(b);
                if (FAILED(hr)) return hr;
                hr = appendIndex(i + 2u);
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        case D3DPT_TRIANGLEFAN:
            for (UINT i = 0; i < primitiveCount; ++i) {
                HRESULT hr = appendIndex(0u);
                if (FAILED(hr)) return hr;
                hr = appendIndex(i + 1u);
                if (FAILED(hr)) return hr;
                hr = appendIndex(i + 2u);
                if (FAILED(hr)) return hr;
            }
            return S_OK;
        default:
            return D3DERR_INVALIDCALL;
    }
}

HRESULT D3D9DeviceImpl::trySoftwareFfpDrawIndexedPrimitive(D3DPRIMITIVETYPE type,
                                           INT baseVertex,
                                           UINT minVertex,
                                           UINT numVertices,
                                           UINT startIndex,
                                           UINT primitiveCount,
                                           SoftwareFfpDrawData& out,
                                           std::vector<std::uint8_t>& indices,
                                           D3DFORMAT& indexFormat) {
    out = {};
    indices.clear();
    indexFormat = D3DFMT_UNKNOWN;
    // Hoisted applicability gate. Every path out of this probe ends in
    // describeSoftware{Ffp,Programmable}DrawTarget, whose first test is
    // `!softwareVertexProcessing_ || <vs_ term>`. Only the SHARED conjunct
    // is hoisted: the vs_ terms are complementary between the FFP and
    // programmable families (`vs_ != nullptr` vs `!vs_`), so hoisting
    // either one here would silently disable the other family on a genuine
    // SWVP device. See
    // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.09.md.
    if (!softwareVertexProcessing_) return S_FALSE;
    const UINT indexCount = primitiveVertexCount(type, primitiveCount);
    if (indexCount == 0u || numVertices == 0u) return S_FALSE;
    const std::int64_t srcStart =
        static_cast<std::int64_t>(baseVertex) +
        static_cast<std::int64_t>(minVertex);
    if (srcStart < 0 || srcStart > 0xffffffffll) return S_FALSE;
    HRESULT hr = readSoftwareFfpAdjustedIndices(
        startIndex, indexCount, minVertex, numVertices, indices, indexFormat);
    if (hr != S_OK) return hr;
    const UINT instanceCount = softwareDrawInstanceCount();
    if (instanceCount <= 1u) {
        hr = trySoftwareFfpTransformBoundVertices(
            static_cast<UINT>(srcStart), numVertices, out);
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
        }
    } else {
        if (!softwareDrawCanExpandInstances(type) ||
            primitiveCount > 0xffffffffu / instanceCount ||
            indexCount > 0xffffffffu / instanceCount ||
            numVertices > 0xffffffffu / instanceCount) {
            indices.clear();
            return S_FALSE;
        }
        const std::vector<std::uint8_t> sourceIndices = indices;
        const D3DFORMAT sourceIndexFormat = indexFormat;
        indices.clear();
        indexFormat = D3DFMT_INDEX32;
        out.primitiveType = softwareDrawExpandedPrimitiveType(type);
        for (UINT instance = 0; instance < instanceCount; ++instance) {
            UINT savedOffsets[16]{};
            hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
            if (SUCCEEDED(hr)) {
                SoftwareFfpDrawData instanceDraw{};
                hr = trySoftwareFfpTransformBoundVertices(
                    static_cast<UINT>(srcStart), numVertices, instanceDraw);
                if (SUCCEEDED(hr) && hr != S_FALSE) {
                    if (out.vertices.empty()) {
                        out.fvf = instanceDraw.fvf;
                        out.stride = instanceDraw.stride;
                        out.bypassVertexShader = instanceDraw.bypassVertexShader;
                    } else if (out.fvf != instanceDraw.fvf ||
                               out.stride != instanceDraw.stride ||
                               out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                        hr = D3DERR_INVALIDCALL;
                    }
                    if (SUCCEEDED(hr)) {
                        out.vertices.insert(out.vertices.end(),
                                            instanceDraw.vertices.begin(),
                                            instanceDraw.vertices.end());
                        hr = appendSoftwarePrimitiveIndicesWithBase32(
                            sourceIndices, sourceIndexFormat, type, primitiveCount,
                            instance * numVertices, indices);
                    }
                }
            }
            restoreSoftwareInstanceStreamOffsets(savedOffsets);
            if (FAILED(hr) || hr == S_FALSE) {
                break;
            }
        }
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            out.primitiveCount = primitiveCount * instanceCount;
        }
    }
    if (FAILED(hr) || hr == S_FALSE) {
        out = {};
        indices.clear();
    }
    return hr;
}

HRESULT D3D9DeviceImpl::trySoftwareProgrammableDrawIndexedPrimitive(
    D3DPRIMITIVETYPE type,
    INT baseVertex,
    UINT minVertex,
    UINT numVertices,
    UINT startIndex,
    UINT primitiveCount,
    SoftwareFfpDrawData& out,
    std::vector<std::uint8_t>& indices,
    D3DFORMAT& indexFormat) {
    out = {};
    indices.clear();
    indexFormat = D3DFMT_UNKNOWN;
    // Hoisted applicability gate. Every path out of this probe ends in
    // describeSoftware{Ffp,Programmable}DrawTarget, whose first test is
    // `!softwareVertexProcessing_ || <vs_ term>`. Only the SHARED conjunct
    // is hoisted: the vs_ terms are complementary between the FFP and
    // programmable families (`vs_ != nullptr` vs `!vs_`), so hoisting
    // either one here would silently disable the other family on a genuine
    // SWVP device. See
    // docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.09.md.
    if (!softwareVertexProcessing_) return S_FALSE;
    const UINT indexCount = primitiveVertexCount(type, primitiveCount);
    if (indexCount == 0u || numVertices == 0u) return S_FALSE;
    const std::int64_t srcStart =
        static_cast<std::int64_t>(baseVertex) +
        static_cast<std::int64_t>(minVertex);
    if (srcStart < 0 || srcStart > 0xffffffffll) return S_FALSE;
    HRESULT hr = readSoftwareFfpAdjustedIndices(
        startIndex, indexCount, minVertex, numVertices, indices, indexFormat);
    if (hr != S_OK) return hr;
    const UINT instanceCount = softwareDrawInstanceCount();
    if (instanceCount <= 1u) {
        hr = trySoftwareProgrammableTransformBoundVertices(
            static_cast<UINT>(srcStart), numVertices, out);
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
        }
    } else {
        if (!softwareDrawCanExpandInstances(type) ||
            primitiveCount > 0xffffffffu / instanceCount ||
            indexCount > 0xffffffffu / instanceCount ||
            numVertices > 0xffffffffu / instanceCount) {
            indices.clear();
            return S_FALSE;
        }
        const std::vector<std::uint8_t> sourceIndices = indices;
        const D3DFORMAT sourceIndexFormat = indexFormat;
        indices.clear();
        indexFormat = D3DFMT_INDEX32;
        out.primitiveType = softwareDrawExpandedPrimitiveType(type);
        for (UINT instance = 0; instance < instanceCount; ++instance) {
            UINT savedOffsets[16]{};
            hr = applySoftwareInstanceStreamOffsets(instance, savedOffsets);
            if (SUCCEEDED(hr)) {
                SoftwareFfpDrawData instanceDraw{};
                hr = trySoftwareProgrammableTransformBoundVertices(
                    static_cast<UINT>(srcStart), numVertices, instanceDraw);
                if (SUCCEEDED(hr) && hr != S_FALSE) {
                    if (out.vertices.empty()) {
                        out.fvf = instanceDraw.fvf;
                        out.stride = instanceDraw.stride;
                        out.bypassVertexShader = instanceDraw.bypassVertexShader;
                    } else if (out.fvf != instanceDraw.fvf ||
                               out.stride != instanceDraw.stride ||
                               out.bypassVertexShader != instanceDraw.bypassVertexShader) {
                        hr = D3DERR_INVALIDCALL;
                    }
                    if (SUCCEEDED(hr)) {
                        out.vertices.insert(out.vertices.end(),
                                            instanceDraw.vertices.begin(),
                                            instanceDraw.vertices.end());
                        hr = appendSoftwarePrimitiveIndicesWithBase32(
                            sourceIndices, sourceIndexFormat, type, primitiveCount,
                            instance * numVertices, indices);
                    }
                }
            }
            restoreSoftwareInstanceStreamOffsets(savedOffsets);
            if (FAILED(hr) || hr == S_FALSE) {
                break;
            }
        }
        if (SUCCEEDED(hr) && hr != S_FALSE) {
            out.primitiveCount = primitiveCount * instanceCount;
        }
    }
    if (FAILED(hr) || hr == S_FALSE) {
        out = {};
        indices.clear();
    }
    return hr;
}

HRESULT D3D9DeviceImpl::trySoftwareFfpDrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                      UINT primitiveCount,
                                      const void* data,
                                      UINT stride,
                                      SoftwareFfpDrawData& out) {
    out = {};
    // Hoisted shared conjunct only -- see trySoftwareFfpDrawIndexedPrimitive.
    if (!softwareVertexProcessing_) return S_FALSE;
    const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
    if (vertexCount == 0u) return S_FALSE;
    DWORD outputFvf = 0;
    FvfProcessLayout srcLayout{};
    FvfProcessLayout dstLayout{};
    HRESULT hr = describeSoftwareFfpDrawTarget(outputFvf, srcLayout, dstLayout);
    if (hr != S_OK) return hr;
    if (stride < srcLayout.stride) return S_FALSE;
    std::uint32_t inputBytes = 0;
    std::uint32_t outputBytes = 0;
    if (!checkedByteCount(vertexCount, stride, inputBytes) ||
        !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
        (inputBytes != 0u && !data)) {
        return D3DERR_INVALIDCALL;
    }
    IDirect3DVertexBuffer9* srcBuffer = nullptr;
    IDirect3DVertexBuffer9* dstBuffer = nullptr;
    SwvpComReleaseGuard srcBufferGuard(srcBuffer);
    SwvpComReleaseGuard dstBufferGuard(dstBuffer);
    hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                            &srcBuffer, nullptr);
    if (FAILED(hr)) return hr;
    void* mapped = nullptr;
    hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
    if (FAILED(hr)) return hr;
    SwvpUnlockGuard srcUnlockGuard(srcBuffer);
    if (mapped) {
        std::memcpy(mapped, data, inputBytes);
        hr = srcBuffer->Unlock();
        srcUnlockGuard.dismiss();
    } else {
        hr = D3DERR_INVALIDCALL;
    }
    if (FAILED(hr)) {
        return hr;
    }
    hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                            &dstBuffer, nullptr);
    if (FAILED(hr)) return hr;
    D3D9PeValidatedVertexBuffer validatedSource{};
    hr = D3D9PeValidateVertexBuffer(
        srcBuffer, static_cast<IDirect3DDevice9*>(this), &validatedSource);
    if (FAILED(hr)) return hr;

    IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
    if (savedStream0) savedStream0->AddRef();
    const auto savedBindingStream0 = recorderState_.peBindingView.streams[0];
    const UINT savedOffset0 = streamOff_[0];
    const UINT savedStride0 = streamStr_[0];
    setRef(streamSrc_[0], srcBuffer);
    streamOff_[0] = 0;
    streamStr_[0] = stride;
    recorderState_.peBindingView.streams[0] = {
        validatedSource.wire(), 0u, stride};
    auto restoreStream = SwvpScopeGuard([&]() noexcept {
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        recorderState_.peBindingView.streams[0] = savedBindingStream0;
        if (savedStream0) savedStream0->Release();
        savedStream0 = nullptr;
    });
    hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);

    if (SUCCEEDED(hr)) {
        hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                         outputFvf, dstLayout.stride);
        if (SUCCEEDED(hr)) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
        }
    }
    return hr;
}

HRESULT D3D9DeviceImpl::trySoftwareProgrammableDrawPrimitiveUP(D3DPRIMITIVETYPE type,
                                               UINT primitiveCount,
                                               const void* data,
                                               UINT stride,
                                               SoftwareFfpDrawData& out) {
    out = {};
    // Hoisted shared conjunct only -- see trySoftwareFfpDrawIndexedPrimitive.
    if (!softwareVertexProcessing_) return S_FALSE;
    const UINT vertexCount = primitiveVertexCount(type, primitiveCount);
    if (vertexCount == 0u) return S_FALSE;
    DWORD outputFvf = 0;
    FvfProcessLayout srcLayout{};
    FvfProcessLayout dstLayout{};
    HRESULT hr = describeSoftwareProgrammableDrawTarget(
        outputFvf, srcLayout, dstLayout);
    if (hr != S_OK) return hr;
    if (!processLayoutUsesOnlyStream0(srcLayout) || stride < srcLayout.stride) {
        return S_FALSE;
    }
    std::uint32_t inputBytes = 0;
    std::uint32_t outputBytes = 0;
    if (!checkedByteCount(vertexCount, stride, inputBytes) ||
        !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
        (inputBytes != 0u && !data)) {
        return D3DERR_INVALIDCALL;
    }
    IDirect3DVertexBuffer9* srcBuffer = nullptr;
    IDirect3DVertexBuffer9* dstBuffer = nullptr;
    SwvpComReleaseGuard srcBufferGuard(srcBuffer);
    SwvpComReleaseGuard dstBufferGuard(dstBuffer);
    hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                            &srcBuffer, nullptr);
    if (FAILED(hr)) return hr;
    void* mapped = nullptr;
    hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
    if (FAILED(hr)) return hr;
    SwvpUnlockGuard srcUnlockGuard(srcBuffer);
    if (mapped) {
        std::memcpy(mapped, data, inputBytes);
        hr = srcBuffer->Unlock();
        srcUnlockGuard.dismiss();
    } else {
        hr = D3DERR_INVALIDCALL;
    }
    if (FAILED(hr)) {
        return hr;
    }
    hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                            &dstBuffer, nullptr);
    if (FAILED(hr)) return hr;
    D3D9PeValidatedVertexBuffer validatedSource{};
    hr = D3D9PeValidateVertexBuffer(
        srcBuffer, static_cast<IDirect3DDevice9*>(this), &validatedSource);
    if (FAILED(hr)) return hr;

    IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
    if (savedStream0) savedStream0->AddRef();
    const auto savedBindingStream0 = recorderState_.peBindingView.streams[0];
    const UINT savedOffset0 = streamOff_[0];
    const UINT savedStride0 = streamStr_[0];
    setRef(streamSrc_[0], srcBuffer);
    streamOff_[0] = 0;
    streamStr_[0] = stride;
    recorderState_.peBindingView.streams[0] = {
        validatedSource.wire(), 0u, stride};
    auto restoreStream = SwvpScopeGuard([&]() noexcept {
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        recorderState_.peBindingView.streams[0] = savedBindingStream0;
        if (savedStream0) savedStream0->Release();
        savedStream0 = nullptr;
    });
    hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);

    if (SUCCEEDED(hr)) {
        hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                         outputFvf, dstLayout.stride);
        if (SUCCEEDED(hr)) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
            out.bypassVertexShader = true;
        }
    }
    return hr;
}

HRESULT D3D9DeviceImpl::trySoftwareFfpDrawIndexedPrimitiveUP(D3DPRIMITIVETYPE type,
                                             UINT minVertex,
                                             UINT numVertices,
                                             UINT primitiveCount,
                                             const void* vertexData,
                                             UINT stride,
                                             SoftwareFfpDrawData& out) {
    out = {};
    // Hoisted shared conjunct only -- see trySoftwareFfpDrawIndexedPrimitive.
    if (!softwareVertexProcessing_) return S_FALSE;
    if (primitiveVertexCount(type, primitiveCount) == 0u || numVertices == 0u) {
        return S_FALSE;
    }
    if (minVertex > 0xffffffffu - numVertices) {
        return D3DERR_INVALIDCALL;
    }
    DWORD outputFvf = 0;
    FvfProcessLayout srcLayout{};
    FvfProcessLayout dstLayout{};
    HRESULT hr = describeSoftwareFfpDrawTarget(outputFvf, srcLayout, dstLayout);
    if (hr != S_OK) return hr;
    if (stride < srcLayout.stride) return S_FALSE;
    const UINT vertexCount = minVertex + numVertices;
    std::uint32_t inputBytes = 0;
    std::uint32_t outputBytes = 0;
    if (!checkedByteCount(vertexCount, stride, inputBytes) ||
        !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
        (inputBytes != 0u && !vertexData)) {
        return D3DERR_INVALIDCALL;
    }
    IDirect3DVertexBuffer9* srcBuffer = nullptr;
    IDirect3DVertexBuffer9* dstBuffer = nullptr;
    SwvpComReleaseGuard srcBufferGuard(srcBuffer);
    SwvpComReleaseGuard dstBufferGuard(dstBuffer);
    hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                            &srcBuffer, nullptr);
    if (FAILED(hr)) return hr;
    void* mapped = nullptr;
    hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
    if (FAILED(hr)) return hr;
    SwvpUnlockGuard srcUnlockGuard(srcBuffer);
    if (mapped) {
        std::memcpy(mapped, vertexData, inputBytes);
        hr = srcBuffer->Unlock();
        srcUnlockGuard.dismiss();
    } else {
        hr = D3DERR_INVALIDCALL;
    }
    if (FAILED(hr)) {
        return hr;
    }
    hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                            &dstBuffer, nullptr);
    if (FAILED(hr)) return hr;
    D3D9PeValidatedVertexBuffer validatedSource{};
    hr = D3D9PeValidateVertexBuffer(
        srcBuffer, static_cast<IDirect3DDevice9*>(this), &validatedSource);
    if (FAILED(hr)) return hr;

    IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
    if (savedStream0) savedStream0->AddRef();
    const auto savedBindingStream0 = recorderState_.peBindingView.streams[0];
    const UINT savedOffset0 = streamOff_[0];
    const UINT savedStride0 = streamStr_[0];
    setRef(streamSrc_[0], srcBuffer);
    streamOff_[0] = 0;
    streamStr_[0] = stride;
    recorderState_.peBindingView.streams[0] = {
        validatedSource.wire(), 0u, stride};
    auto restoreStream = SwvpScopeGuard([&]() noexcept {
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        recorderState_.peBindingView.streams[0] = savedBindingStream0;
        if (savedStream0) savedStream0->Release();
        savedStream0 = nullptr;
    });
    hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);

    if (SUCCEEDED(hr)) {
        hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                         outputFvf, dstLayout.stride);
        if (SUCCEEDED(hr)) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
        }
    }
    return hr;
}

HRESULT D3D9DeviceImpl::trySoftwareProgrammableDrawIndexedPrimitiveUP(
    D3DPRIMITIVETYPE type,
    UINT minVertex,
    UINT numVertices,
    UINT primitiveCount,
    const void* vertexData,
    UINT stride,
    SoftwareFfpDrawData& out) {
    out = {};
    // Hoisted shared conjunct only -- see trySoftwareFfpDrawIndexedPrimitive.
    if (!softwareVertexProcessing_) return S_FALSE;
    if (primitiveVertexCount(type, primitiveCount) == 0u || numVertices == 0u) {
        return S_FALSE;
    }
    if (minVertex > 0xffffffffu - numVertices) {
        return D3DERR_INVALIDCALL;
    }
    DWORD outputFvf = 0;
    FvfProcessLayout srcLayout{};
    FvfProcessLayout dstLayout{};
    HRESULT hr = describeSoftwareProgrammableDrawTarget(
        outputFvf, srcLayout, dstLayout);
    if (hr != S_OK) return hr;
    if (!processLayoutUsesOnlyStream0(srcLayout) || stride < srcLayout.stride) {
        return S_FALSE;
    }
    const UINT vertexCount = minVertex + numVertices;
    std::uint32_t inputBytes = 0;
    std::uint32_t outputBytes = 0;
    if (!checkedByteCount(vertexCount, stride, inputBytes) ||
        !checkedByteCount(vertexCount, dstLayout.stride, outputBytes) ||
        (inputBytes != 0u && !vertexData)) {
        return D3DERR_INVALIDCALL;
    }
    IDirect3DVertexBuffer9* srcBuffer = nullptr;
    IDirect3DVertexBuffer9* dstBuffer = nullptr;
    SwvpComReleaseGuard srcBufferGuard(srcBuffer);
    SwvpComReleaseGuard dstBufferGuard(dstBuffer);
    hr = CreateVertexBuffer(inputBytes, 0, fvf_, D3DPOOL_SYSTEMMEM,
                            &srcBuffer, nullptr);
    if (FAILED(hr)) return hr;
    void* mapped = nullptr;
    hr = srcBuffer->Lock(0, inputBytes, &mapped, 0);
    if (FAILED(hr)) return hr;
    SwvpUnlockGuard srcUnlockGuard(srcBuffer);
    if (mapped) {
        std::memcpy(mapped, vertexData, inputBytes);
        hr = srcBuffer->Unlock();
        srcUnlockGuard.dismiss();
    } else {
        hr = D3DERR_INVALIDCALL;
    }
    if (FAILED(hr)) {
        return hr;
    }
    hr = CreateVertexBuffer(outputBytes, 0, outputFvf, D3DPOOL_SYSTEMMEM,
                            &dstBuffer, nullptr);
    if (FAILED(hr)) return hr;
    D3D9PeValidatedVertexBuffer validatedSource{};
    hr = D3D9PeValidateVertexBuffer(
        srcBuffer, static_cast<IDirect3DDevice9*>(this), &validatedSource);
    if (FAILED(hr)) return hr;

    IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
    if (savedStream0) savedStream0->AddRef();
    const auto savedBindingStream0 = recorderState_.peBindingView.streams[0];
    const UINT savedOffset0 = streamOff_[0];
    const UINT savedStride0 = streamStr_[0];
    setRef(streamSrc_[0], srcBuffer);
    streamOff_[0] = 0;
    streamStr_[0] = stride;
    recorderState_.peBindingView.streams[0] = {
        validatedSource.wire(), 0u, stride};
    auto restoreStream = SwvpScopeGuard([&]() noexcept {
        setRef(streamSrc_[0], savedStream0);
        streamOff_[0] = savedOffset0;
        streamStr_[0] = savedStride0;
        recorderState_.peBindingView.streams[0] = savedBindingStream0;
        if (savedStream0) savedStream0->Release();
        savedStream0 = nullptr;
    });
    hr = ProcessVertices(0, 0, vertexCount, dstBuffer, nullptr, 0);

    if (SUCCEEDED(hr)) {
        hr = readTransformedVertexBuffer(dstBuffer, outputBytes, out,
                                         outputFvf, dstLayout.stride);
        if (SUCCEEDED(hr)) {
            out.primitiveType = type;
            out.primitiveCount = primitiveCount;
            out.bypassVertexShader = true;
        }
    }
    return hr;
}
