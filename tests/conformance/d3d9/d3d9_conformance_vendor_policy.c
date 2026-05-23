/*
 * Vendor FOURCC / legacy SM1 dependent-texture deferred-policy probes.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/visual.c (texbem_test, texdepth_test, intz_test,
 *   test_fetch4, resz_test, test_mipmap_upload)
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Each probe pins dxmt9's deliberate "accept / reject / map-as" choice
 * for a Wine vendor-format or SM1-feature surface that cannot yet be
 * runtime-verified end-to-end. Bodies only validate the relevant public
 * CheckDeviceFormat / D3DCAPS9 HRESULT / cap-bit contract; full
 * Lock/Draw/Readback validation is deferred until the corresponding
 * NATIVE oracle or shader-runner fixture exists.
 *
 * Wine source is referenced as LGPL behavioral oracle only; no Wine
 * implementation code is copied into dxmt9 project code.
 */

#include "d3d9_conformance_fixtures.h"

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: texbem_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vendor_policy_texbem_unsupported(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    /*
     * Wine gates BEM/TEXBEM behavior on D3DTEXOPCAPS_BUMPENVMAP; the public
     * CheckDeviceFormat surface for D3DFMT_V8U8 with QUERY_LEGACYBUMPMAP
     * (the D3D9-public cap-query equivalent of "BEM-capable signed format")
     * must return either S_OK (BEM advertised) or D3DERR_NOTAVAILABLE (BEM
     * not advertised). dxmt9 does not lower the TEXBEM opcode today, so the
     * runtime BEM-output equality oracle is deferred to a NATIVE follow-up;
     * here we only pin the cap-query contract.
     */
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_LEGACYBUMPMAP,
            D3DRTYPE_TEXTURE, D3DFMT_V8U8);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: texdepth_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vendor_policy_texdepth_unsupported(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3DDevice9_GetDeviceCaps(device, &caps);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        /*
         * texdepth_test exercises ps_1_4 TEXDEPTH; the surface contract
         * we pin here is that ps_1_4 is exposed and PixelShader1xMaxValue
         * is finite. TEXDEPTH opcode lowering is deferred until the SM1
         * depth-readback oracle exists.
         */
        CHECK_TRUE(caps.PixelShaderVersion >= D3DPS_VERSION(1, 4));
        CHECK_TRUE(caps.PixelShader1xMaxValue >= 1.0f);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: intz_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vendor_policy_intz_caps(const struct d3d9_api *api)
{
    static const D3DFORMAT intz_format = (D3DFORMAT)MAKEFOURCC('I','N','T','Z');
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    /*
     * dxmt9 now exposes INTZ as a sampleable depth-stencil texture
     * format via the Metal Depth32Float_Stencil8 path (commit 36935fb
     * closed gap_d3d9.md §C.5 row INTZ via track G1-3). The
     * CheckDeviceFormat query reports S_OK for the
     * USAGE_DEPTHSTENCIL + RTYPE_TEXTURE pair; a dedicated readback
     * oracle for INTZ depth-as-sampler readback lives in the
     * `legacy_sm1/dxmt9_ps14_texdepth_*` corpus entries (already
     * passing) and a future INTZ-specific runtime probe.
     */
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
            D3DRTYPE_TEXTURE, intz_format);
    CHECK_HR(hr, D3D_OK);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_fetch4
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vendor_policy_fetch4_caps(const struct d3d9_api *api)
{
    static const D3DFORMAT fetch4_format = (D3DFORMAT)MAKEFOURCC('G','E','T','4');
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    /*
     * AMD FETCH4 enable token query: dxmt9 reports unsupported for the
     * paired D24S8 sampleable-depth + GET4 cap query. Both must return
     * D3DERR_NOTAVAILABLE to prevent apps from enabling a FETCH4 path
     * that dxmt9 does not implement.
     */
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
            D3DRTYPE_TEXTURE, D3DFMT_D24S8);
    CHECK_HR(hr, D3DERR_NOTAVAILABLE);

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
            D3DRTYPE_TEXTURE, fetch4_format);
    CHECK_HR(hr, D3DERR_NOTAVAILABLE);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: resz_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vendor_policy_resz_caps(const struct d3d9_api *api)
{
    static const D3DFORMAT resz_format = (D3DFORMAT)MAKEFOURCC('R','E','S','Z');
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    /*
     * RESZ is the AMD multisample-depth-resolve enable token. dxmt9 uses
     * a different path on Metal (resolve attachments + dedicated blit),
     * so the public RESZ cap query must report D3DERR_NOTAVAILABLE.
     * The resolved-depth readback oracle is deferred to a NATIVE
     * follow-up that pins the resolve path explicitly.
     */
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0,
            D3DRTYPE_SURFACE, resz_format);
    CHECK_HR(hr, D3DERR_NOTAVAILABLE);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_mipmap_upload
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vendor_policy_mipmap_upload_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    memset(&caps, 0, sizeof(caps));
    hr = IDirect3DDevice9_GetDeviceCaps(device, &caps);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        /*
         * test_mipmap_upload uploads each mip level via per-level Lock
         * and asserts the readback equals the uploaded data. The public
         * cap surface that gates the test is mipmap support + at least
         * one texture blend stage and one simultaneous texture. The
         * "upload via Lock per level" path itself is deferred to a
         * NATIVE spec that pins the per-mip Lock/Unlock layout against
         * the Wine oracle without depending on a runtime probe lane.
         */
        CHECK_TRUE((caps.TextureCaps & D3DPTEXTURECAPS_MIPMAP) != 0);
        CHECK_TRUE(caps.MaxTextureBlendStages >= 1);
        CHECK_TRUE(caps.MaxSimultaneousTextures >= 1);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_miptree_layout
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * The deterministic block/byte layout math is covered by the native
 * spec tests/native/core/core_d3d9_miptree_layout_spec.cpp; this
 * scaffold pins the public-ABI portion (per-level GetLevelDesc dims
 * + LockRect.Pitch) for an A8R8G8B8 mip chain.
 */
void test_miptree_layout_lock_pitch_policy(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    UINT level;
    UINT levels;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }
    window = create_test_window();
    if (!window)
        goto done_d3d9;
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    CHECK_HR(IDirect3DDevice9_CreateTexture(device, 64, 64, 0, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL), D3D_OK);
    if (!texture)
        goto done_device;

    levels = IDirect3DTexture9_GetLevelCount(texture);
    CHECK_TRUE(levels == 7);

    for (level = 0; level < levels; ++level)
    {
        D3DSURFACE_DESC desc;
        D3DLOCKED_RECT locked;
        UINT expected_dim = 64u >> level;
        if (!expected_dim) expected_dim = 1;

        memset(&desc, 0xcc, sizeof(desc));
        CHECK_HR(IDirect3DTexture9_GetLevelDesc(texture, level, &desc), D3D_OK);
        CHECK_TRUE(desc.Width == expected_dim);
        CHECK_TRUE(desc.Height == expected_dim);
        CHECK_TRUE(desc.Format == D3DFMT_A8R8G8B8);

        memset(&locked, 0xcc, sizeof(locked));
        CHECK_HR(IDirect3DTexture9_LockRect(texture, level, &locked, NULL, 0), D3D_OK);
        CHECK_TRUE(locked.pBits != NULL);
        /* A8R8G8B8 = 4 bytes/pel; pitch must accommodate the level row width. */
        CHECK_TRUE(locked.Pitch >= (INT)(expected_dim * 4u));
        CHECK_HR(IDirect3DTexture9_UnlockRect(texture, level), D3D_OK);
    }

    IDirect3DTexture9_Release(texture);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
