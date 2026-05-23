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

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_stretch_rect
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine's test_stretch_rect exercises StretchRect HRESULT contracts
 * across many surface format / pool / filter / rect combos. This
 * scaffold pins a minimal subset of the public-ABI policy: NULL
 * src or NULL dst returns D3DERR_INVALIDCALL; a rect with
 * non-positive (zero-area or negative) width/height also returns
 * D3DERR_INVALIDCALL. Format / pool / MSAA matrix is covered by
 * sibling test_visual_blit_format_conversion_policy.
 */
void test_stretch_rect_null_and_degenerate_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *src = NULL;
    IDirect3DSurface9 *dst = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    RECT degen = {10, 10, 10, 10};
    RECT neg = {20, 30, 10, 5};

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

    /* StretchRect requires DEFAULT-pool surfaces; use that for both so the
     * success leg is unambiguous. The pool/format matrix is exercised in
     * test_visual_blit_format_conversion_policy. */
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &src, NULL);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateOffscreenPlainSurface src failed with %s",
                hr_buffer);
        goto done_device;
    }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst, NULL);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateOffscreenPlainSurface dst failed with %s",
                hr_buffer);
        goto done_src;
    }

    /* NULL src -> D3DERR_INVALIDCALL. */
    hr = IDirect3DDevice9_StretchRect(device, NULL, NULL, dst, NULL,
            D3DTEXF_NONE);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    /* NULL dst -> D3DERR_INVALIDCALL. */
    hr = IDirect3DDevice9_StretchRect(device, src, NULL, NULL, NULL,
            D3DTEXF_NONE);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    /* Both non-NULL, matching format / dims / pool, default whole-surface
     * rects -> S_OK. */
    hr = IDirect3DDevice9_StretchRect(device, src, NULL, dst, NULL,
            D3DTEXF_NONE);
    CHECK_HR(hr, D3D_OK);

    /* Zero-area src rect -> D3DERR_INVALIDCALL. */
    hr = IDirect3DDevice9_StretchRect(device, src, &degen, dst, NULL,
            D3DTEXF_NONE);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    /* Negative-extent src rect (right < left, bottom < top) ->
     * D3DERR_INVALIDCALL. */
    hr = IDirect3DDevice9_StretchRect(device, src, &neg, dst, NULL,
            D3DTEXF_NONE);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    IDirect3DSurface9_Release(dst);
done_src:
    IDirect3DSurface9_Release(src);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: depth_blit_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine's depth_blit_test exercises StretchRect between two depth-stencil
 * surfaces and verifies that depth samples round-trip through the blit.
 * The pixel round-trip portion is EXP-route (needs depth-sample readback).
 * This PE scaffold pins the public-ABI policy: matching-format depth-to-
 * depth StretchRect in DEFAULT pool returns S_OK or D3DERR_INVALIDCALL
 * (dxmt9 may legitimately reject depth-to-depth blit), and cross-format
 * depth blit (D24S8 -> D16) must return D3DERR_INVALIDCALL.
 */
void test_stretch_rect_depth_stencil_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *src_ds = NULL;
    IDirect3DSurface9 *dst_ds = NULL;
    IDirect3DSurface9 *mismatch_ds = NULL;
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

    /* Two matching-format DS surfaces. D24S8 is the conformant baseline
     * format Wine's depth_blit_test uses; DEFAULT pool is implicit for
     * CreateDepthStencilSurface. Discard=FALSE so the surface is eligible
     * as a StretchRect source. */
    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 64, 64,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &src_ds, NULL);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateDepthStencilSurface src failed with %s",
                hr_buffer);
        goto done_device;
    }

    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 64, 64,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &dst_ds, NULL);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateDepthStencilSurface dst failed with %s",
                hr_buffer);
        goto done_src;
    }

    /* Matching-format depth-to-depth StretchRect. D3D9 historically
     * permits this only for DEFAULT-pool same-format depth surfaces;
     * some drivers (and dxmt9, depending on backend support) may reject
     * it with D3DERR_INVALIDCALL. Either outcome is conformant. */
    hr = IDirect3DDevice9_StretchRect(device, src_ds, NULL, dst_ds, NULL,
            D3DTEXF_NONE);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);

    /* Cross-format depth blit (D24S8 source -> D16 destination). D3D9
     * requires source and destination depth surfaces to match format;
     * this must always be rejected. CreateDepthStencilSurface for D16
     * may fail on some configurations -- only run the negative test if
     * the mismatched surface was actually created. */
    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 64, 64,
            D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &mismatch_ds, NULL);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DDevice9_StretchRect(device, src_ds, NULL, mismatch_ds,
                NULL, D3DTEXF_NONE);
        CHECK_HR(hr, D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(mismatch_ds);
    }

    IDirect3DSurface9_Release(dst_ds);
done_src:
    IDirect3DSurface9_Release(src_ds);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: stretchrect_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine's stretchrect_test exercises StretchRect across many surface
 * format combinations. This PE scaffold pins the public-ABI policy
 * for the format matrix: matching-format DEFAULT-pool offscreen
 * surfaces in the common backbuffer-capable formats (A8R8G8B8,
 * X8R8G8B8, R5G6B5) must accept a same-format whole-surface blit
 * with S_OK. Formats that the runtime cannot create as
 * CreateOffscreenPlainSurface are skipped (continue to the next
 * format) rather than failing the test.
 */
void test_stretch_rect_format_matrix_policy(const struct d3d9_api *api)
{
    static const D3DFORMAT formats[] = {
        D3DFMT_A8R8G8B8,
        D3DFMT_X8R8G8B8,
        D3DFMT_R5G6B5,
    };
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    size_t i;

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

    for (i = 0; i < sizeof(formats) / sizeof(formats[0]); ++i)
    {
        IDirect3DSurface9 *src = NULL;
        IDirect3DSurface9 *dst = NULL;

        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64,
                formats[i], D3DPOOL_DEFAULT, &src, NULL);
        if (FAILED(hr))
            continue;

        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64,
                formats[i], D3DPOOL_DEFAULT, &dst, NULL);
        if (FAILED(hr))
        {
            IDirect3DSurface9_Release(src);
            continue;
        }

        /* Matching-format / matching-dim whole-surface blit -> S_OK. */
        hr = IDirect3DDevice9_StretchRect(device, src, NULL, dst, NULL,
                D3DTEXF_LINEAR);
        CHECK_HR(hr, D3D_OK);

        IDirect3DSurface9_Release(dst);
        IDirect3DSurface9_Release(src);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: np2_stretch_rect_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727 (visual.c:12487)
 *
 * Wine's np2_stretch_rect_test exercises StretchRect on non-power-of-two
 * (NPOT) surfaces. This PE scaffold pins the public-ABI policy: NPOT
 * D3DPOOL_DEFAULT D3DFMT_A8R8G8B8 offscreen surfaces must accept
 * StretchRect with D3DTEXF_LINEAR and return S_OK, including NPOT->NPOT
 * (matching), NPOT->POT (non-square scaling), and asymmetric NPOT->NPOT
 * combinations. Pairs where CreateOffscreenPlainSurface itself fails on
 * the runtime are skipped rather than failing the test.
 */
void test_stretch_rect_npot_dimension_policy(const struct d3d9_api *api)
{
    static const struct {
        UINT src_w;
        UINT src_h;
        UINT dst_w;
        UINT dst_h;
    } pairs[] = {
        { 33, 33, 33, 33 },  /* NPOT -> same NPOT */
        { 33, 33, 64, 64 },  /* NPOT -> POT, non-square scaling */
        { 15,  7, 33, 17 },  /* asymmetric NPOT -> NPOT */
    };
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    size_t i;

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

    for (i = 0; i < sizeof(pairs) / sizeof(pairs[0]); ++i)
    {
        IDirect3DSurface9 *src = NULL;
        IDirect3DSurface9 *dst = NULL;

        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device,
                pairs[i].src_w, pairs[i].src_h, D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT, &src, NULL);
        if (FAILED(hr))
            continue;

        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device,
                pairs[i].dst_w, pairs[i].dst_h, D3DFMT_A8R8G8B8,
                D3DPOOL_DEFAULT, &dst, NULL);
        if (FAILED(hr))
        {
            IDirect3DSurface9_Release(src);
            continue;
        }

        /* NPOT whole-surface blit -> S_OK. */
        hr = IDirect3DDevice9_StretchRect(device, src, NULL, dst, NULL,
                D3DTEXF_LINEAR);
        CHECK_HR(hr, D3D_OK);

        IDirect3DSurface9_Release(dst);
        IDirect3DSurface9_Release(src);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
