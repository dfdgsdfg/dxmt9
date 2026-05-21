/*
 * D3D9 PE conformance — visual raster / line-aa / blit policy scaffolds.
 *
 * These tests absorb the public-ABI portion of three Wine D3D9 visual.c
 * pixel-level tests. The full pixel-coverage / blended-line / blit
 * conversion probe is EXP-route (needs a working Metal probe to read back
 * rasterized pixels); the PE-route scaffolds here only verify the state
 * surface and HRESULT decision matrix that those tests assume.
 *
 * Wine source is referenced as LGPL behavioral oracle only — no Wine
 * implementation code is copied here.
 */

#include "d3d9_conformance_fixtures.h"

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_filling_convention
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine's test_filling_convention rasterizes triangles in CW/CCW/NONE cull
 * modes and reads back pixels to check coverage. The pixel probe is
 * EXP-route. Here we only assert that the cull-mode / line-aa cap and
 * render-state surface defaults and round-trips correctly.
 */
void test_visual_filling_convention_caps_policy(const struct d3d9_api *api)
{
    static const DWORD cull_values[] =
    {
        D3DCULL_NONE,
        D3DCULL_CW,
        D3DCULL_CCW,
    };
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    DWORD value;
    HWND window;
    UINT i;

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
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &caps), D3D_OK);

    /* All three cull modes must be reported as supported on any
     * conformant D3D9 device. */
    CHECK_TRUE((caps.PrimitiveMiscCaps & D3DPMISCCAPS_CULLNONE) != 0);
    CHECK_TRUE((caps.PrimitiveMiscCaps & D3DPMISCCAPS_CULLCW) != 0);
    CHECK_TRUE((caps.PrimitiveMiscCaps & D3DPMISCCAPS_CULLCCW) != 0);

    /* Line-antialias cap is queried but not required — we only record it.
     * The EXP-route line-aa pixel probe gates on this bit. */
    (void)(caps.LineCaps & D3DLINECAPS_ANTIALIAS);

    /* Default cull mode is CCW per D3D9 spec. */
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_CULLMODE, &value),
            D3D_OK);
    CHECK_TRUE(value == D3DCULL_CCW);

    /* Cull mode round-trips for NONE/CW/CCW. */
    for (i = 0; i < ARRAY_SIZE(cull_values); ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE,
                cull_values[i]), D3D_OK);
        value = 0xdeadbeef;
        CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_CULLMODE,
                &value), D3D_OK);
        CHECK_TRUE(value == cull_values[i]);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_line_antialiasing_blending
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine's test_line_antialiasing_blending draws antialiased lines with
 * SRCALPHA/INVSRCALPHA blending and reads back the framebuffer. The pixel
 * probe is EXP-route. Here we only validate that the render-state surface
 * defaults match D3D9 spec and that ABLE / blend-op / blend-factor states
 * round-trip.
 */
void test_visual_line_antialiasing_blending_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    DWORD value;
    HWND window;

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

    /* Antialiased-line state defaults to FALSE per D3D9 spec. */
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_ANTIALIASEDLINEENABLE, &value), D3D_OK);
    CHECK_TRUE(value == FALSE);

    /* ABLE defaults to FALSE per D3D9 spec. */
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_ALPHABLENDENABLE, &value), D3D_OK);
    CHECK_TRUE(value == FALSE);

    /* Antialias-line enable round-trips both directions. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_ANTIALIASEDLINEENABLE, TRUE), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_ANTIALIASEDLINEENABLE, &value), D3D_OK);
    CHECK_TRUE(value == TRUE);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_ANTIALIASEDLINEENABLE, FALSE), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_ANTIALIASEDLINEENABLE, &value), D3D_OK);
    CHECK_TRUE(value == FALSE);

    /* Classic SRCALPHA / INVSRCALPHA / ADD blend pipeline round-trips. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_BLENDOP,
            D3DBLENDOP_ADD), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_BLENDOP,
            &value), D3D_OK);
    CHECK_TRUE(value == D3DBLENDOP_ADD);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND,
            D3DBLEND_SRCALPHA), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SRCBLEND,
            &value), D3D_OK);
    CHECK_TRUE(value == D3DBLEND_SRCALPHA);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_DESTBLEND,
            D3DBLEND_INVSRCALPHA), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_DESTBLEND,
            &value), D3D_OK);
    CHECK_TRUE(value == D3DBLEND_INVSRCALPHA);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_blit_format_conversion
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine's test_blit_format_conversion StretchRects across surface format
 * combinations and reads back pixels to check the conversion. The
 * pixel-correctness probe is EXP-route. Here we only validate the
 * HRESULT decision matrix: same-format succeeds, X8 vs A8 is accepted
 * with documented latitude, MSAA mismatch is recorded.
 */
void test_visual_blit_format_conversion_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *src_argb = NULL;
    IDirect3DSurface9 *dst_argb = NULL;
    IDirect3DSurface9 *dst_xrgb = NULL;
    IDirect3DSurface9 *ms_surface = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
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

    /* Same-format A8R8G8B8 RT-to-RT in DEFAULT pool must succeed. */
    hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &src_argb,
            NULL);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &dst_argb,
            NULL);
    CHECK_HR(hr, D3D_OK);

    if (src_argb && dst_argb)
    {
        hr = IDirect3DDevice9_StretchRect(device, src_argb, NULL, dst_argb,
                NULL, D3DTEXF_NONE);
        CHECK_SUCCEEDED(hr);
    }

    /* A8R8G8B8 -> X8R8G8B8 same dims/pool — record HRESULT, accept either
     * S_OK (Wine permissive) or D3DERR_INVALIDCALL (strict refcard). */
    hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64,
            D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &dst_xrgb,
            NULL);
    CHECK_HR(hr, D3D_OK);
    if (src_argb && dst_xrgb)
    {
        hr = IDirect3DDevice9_StretchRect(device, src_argb, NULL, dst_xrgb,
                NULL, D3DTEXF_NONE);
        CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
    }

    /* Compatible RT formats but mismatched MSAA — record HRESULT.
     * Skip if the host does not support 2x MSAA. */
    if (SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d9,
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
            D3DMULTISAMPLE_2_SAMPLES, NULL)))
    {
        hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64,
                D3DFMT_A8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE,
                &ms_surface, NULL);
        if (SUCCEEDED(hr) && ms_surface && src_argb)
        {
            hr = IDirect3DDevice9_StretchRect(device, src_argb, NULL,
                    ms_surface, NULL, D3DTEXF_NONE);
            CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
            IDirect3DSurface9_Release(ms_surface);
        }
    }

    if (dst_xrgb)
        IDirect3DSurface9_Release(dst_xrgb);
    if (dst_argb)
        IDirect3DSurface9_Release(dst_argb);
    if (src_argb)
        IDirect3DSurface9_Release(src_argb);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
