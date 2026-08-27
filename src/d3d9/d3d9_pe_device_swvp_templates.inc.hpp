// Template definitions split from the D3D9DeviceImpl declaration shell. This
// file is included only after the complete class declaration.

template <typename Prepare>
__attribute__((noinline))
HRESULT D3D9DeviceImpl::prepareSoftwareDrawCandidate(
        Prepare&& prepare) noexcept {
    IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
    if (savedStream0) savedStream0->AddRef();
    const auto savedBindingStream0 = recorderState_.peBindingView.streams[0];
    const UINT savedOffset0 = streamOff_[0];
    const UINT savedStride0 = streamStr_[0];
    UINT savedOffsets[16]{};
    for (UINT stream = 0; stream < 16u; ++stream) {
        savedOffsets[stream] = streamOff_[stream];
    }

    HRESULT hr = S_FALSE;
    const auto allocation = dxmt9::d3d9::pe::runPublicAllocationPhase(
        [&] { hr = std::forward<Prepare>(prepare)(); });

    setRef(streamSrc_[0], savedStream0);
    streamOff_[0] = savedOffset0;
    streamStr_[0] = savedStride0;
    recorderState_.peBindingView.streams[0] = savedBindingStream0;
    restoreSoftwareInstanceStreamOffsets(savedOffsets);
    if (savedStream0) savedStream0->Release();
    if (allocation ==
        dxmt9::d3d9::pe::PublicAllocationResult::OutOfMemory) {
        return E_OUTOFMEMORY;
    }
    return hr;
}
