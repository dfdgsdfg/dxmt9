// Template definitions split from the D3D9DeviceImpl declaration shell. This
// file is included only after the complete class declaration.

template <typename Prepare>
__attribute__((noinline))
HRESULT D3D9DeviceImpl::prepareSoftwareDrawCandidate(
        Prepare&& prepare) noexcept {
    IDirect3DVertexBuffer9* savedStream0 = streamSrc_[0];
    if (savedStream0) savedStream0->AddRef();
    dxmt9::d3d9::pe::PeSwvpStreamOverrideGuard streamRestore(
        recorderState_.peBindingView.streams);

    HRESULT hr = S_FALSE;
    const auto allocation = dxmt9::d3d9::pe::runPublicAllocationPhase(
        [&] { hr = std::forward<Prepare>(prepare)(); });

    setRef(streamSrc_[0], savedStream0);
    streamRestore.restore();
    if (savedStream0) savedStream0->Release();
    if (allocation ==
        dxmt9::d3d9::pe::PublicAllocationResult::OutOfMemory) {
        return E_OUTOFMEMORY;
    }
    return hr;
}
