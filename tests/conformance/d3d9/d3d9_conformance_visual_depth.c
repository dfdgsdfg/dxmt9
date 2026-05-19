/*
 * Depth-stencil shaped conformance cases derived from Wine's
 * dlls/d3d9/tests/visual.c oracle. Each test exercises a depth-stencil
 * or depth-related render-state ABI surface that the visual suite
 * presupposes before issuing rendered probe quads.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/visual.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Wine is referenced as an LGPL behavioral oracle only; no Wine
 * implementation, control flow, or table data is copied here.
 */

#include "d3d9_conformance_fixtures.h"

#ifndef D3DFMT_NVDB
#define D3DFMT_NVDB ((D3DFORMAT)MAKEFOURCC('N','V','D','B'))
#endif

/*
 * Build a windowed presentation parameter block with an auto depth
 * stencil. The visual.c oracle assumes a D24S8 (or D16) backbuffer
 * companion so that depth-related state can be observed.
 */
static D3DPRESENT_PARAMETERS depth_present_parameters(HWND window,
        D3DFORMAT depth_format)
{
    D3DPRESENT_PARAMETERS pp = default_present_parameters(window);

    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = depth_format;
    return pp;
}

static IDirect3DDevice9 *create_depth_device(IDirect3D9 *d3d9, HWND window,
        D3DFORMAT depth_format)
{
    D3DPRESENT_PARAMETERS pp = depth_present_parameters(window, depth_format);
    IDirect3DDevice9 *device = NULL;
    HRESULT hr;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
            &pp, &device);
    if (FAILED(hr))
    {
        /* Some hosts only expose software VP; fall back so the harness
         * still observes the depth-state policy. */
        pp = depth_present_parameters(window, depth_format);
        hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                &pp, &device);
    }
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateDevice with depth failed with %s", hr_buffer);
        return NULL;
    }
    return device;
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: z_range_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_z_range_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    D3DVIEWPORT9 vp = {0};
    D3DVIEWPORT9 vp_read = {0};
    DWORD zenable = 0xdeadbeef;
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

    device = create_depth_device(d3d9, window, D3DFMT_D24S8);
    if (!device)
        goto done_window;

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE,
            D3DZB_TRUE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZENABLE,
            &zenable), D3D_OK);
    CHECK_TRUE(zenable == (DWORD)D3DZB_TRUE);

    vp.X = 0;
    vp.Y = 0;
    vp.Width = 640;
    vp.Height = 480;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetViewport(device, &vp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetViewport(device, &vp_read), D3D_OK);
    CHECK_TRUE(vp_read.MinZ == 0.0f);
    CHECK_TRUE(vp_read.MaxZ == 1.0f);

    /* D3D9 accepts out-of-[0,1] MinZ/MaxZ and round-trips them. */
    vp.MinZ = -0.5f;
    vp.MaxZ = 1.5f;
    hr = IDirect3DDevice9_SetViewport(device, &vp);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&vp_read, 0, sizeof(vp_read));
        CHECK_HR(IDirect3DDevice9_GetViewport(device, &vp_read), D3D_OK);
        CHECK_TRUE(vp_read.MinZ == -0.5f);
        CHECK_TRUE(vp_read.MaxZ == 1.5f);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: ds_size_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_ds_smaller_than_rt_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *rt = NULL;
    IDirect3DSurface9 *ds_small = NULL;
    IDirect3DSurface9 *ds_large = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr_small;
    HRESULT hr_large;
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

    device = create_depth_device(d3d9, window, D3DFMT_D24S8);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 32, 32,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds_small, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_rt;

    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 128, 128,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds_large, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_ds_small;

    CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, rt), D3D_OK);

    /* DS smaller than RT: D3D9 either returns D3DERR_INVALIDCALL or
     * succeeds with a runtime warning. Record either as valid. */
    hr_small = IDirect3DDevice9_SetDepthStencilSurface(device, ds_small);
    CHECK_TRUE(hr_small == D3D_OK || hr_small == D3DERR_INVALIDCALL);

    /* DS larger than RT is always accepted. */
    hr_large = IDirect3DDevice9_SetDepthStencilSurface(device, ds_large);
    CHECK_HR(hr_large, D3D_OK);

    /* Restore null DS so device teardown does not retain ds_large. */
    IDirect3DDevice9_SetDepthStencilSurface(device, NULL);

    IDirect3DSurface9_Release(ds_large);
done_ds_small:
    IDirect3DSurface9_Release(ds_small);
done_rt:
    IDirect3DSurface9_Release(rt);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: depth_buffer_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_depth_buffer_clear_policy(const struct d3d9_api *api)
{
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

    device = create_depth_device(d3d9, window, D3DFMT_D24S8);
    if (!device)
        goto done_window;

    /* With auto depth-stencil bound, depth clear is OK. */
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER,
            0xff000000, 1.0f, 0);
    CHECK_HR(hr, D3D_OK);

    /* Color-only clear is always OK irrespective of DS binding. */
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff000000, 1.0f, 0);
    CHECK_HR(hr, D3D_OK);

    /* Remove the depth-stencil binding, then a depth clear must fail. */
    CHECK_HR(IDirect3DDevice9_SetDepthStencilSurface(device, NULL), D3D_OK);
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_ZBUFFER,
            0xff000000, 1.0f, 0);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: depth_buffer2_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_depth_buffer_reset_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *ds_query = (IDirect3DSurface9 *)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DPRESENT_PARAMETERS pp;
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

    device = create_depth_device(d3d9, window, D3DFMT_D24S8);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &ds_query);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr) && ds_query)
        IDirect3DSurface9_Release(ds_query);

    /* Reset without auto depth-stencil. */
    pp = default_present_parameters(window);
    pp.EnableAutoDepthStencil = FALSE;
    pp.AutoDepthStencilFormat = D3DFMT_UNKNOWN;
    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    ds_query = (IDirect3DSurface9 *)0xdeadbeef;
    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &ds_query);
    CHECK_HR(hr, D3DERR_NOTFOUND);
    CHECK_TRUE(ds_query == NULL);

    /* Reset back with auto depth-stencil so the device leaves the test
     * in a state matching the next case's expectations. */
    pp = depth_present_parameters(window, D3DFMT_D24S8);
    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_HR(hr, D3D_OK);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: depth_bounds_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_depth_bounds_caps_policy(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    /* NVDB is a vendor-defined fourcc used by NVIDIA to advertise the
     * depth-bounds extension. The conformance contract is that the
     * factory must either expose it (S_OK) or cleanly reject it
     * (D3DERR_NOTAVAILABLE) — never crash or return a different code. */
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
            D3DRTYPE_SURFACE, D3DFMT_NVDB);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);

    /* Querying with a TEXTURE resource type must still resolve to a
     * non-crashing HRESULT even if NVDB is unsupported. */
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
            D3DRTYPE_TEXTURE, D3DFMT_NVDB);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: zenable_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_zenable_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    DWORD value = 0xdeadbeef;
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

    device = create_depth_device(d3d9, window, D3DFMT_D24S8);
    if (!device)
        goto done_window;

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE,
            D3DZB_FALSE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZENABLE,
            &value), D3D_OK);
    CHECK_TRUE(value == (DWORD)D3DZB_FALSE);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE,
            D3DZB_TRUE), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZENABLE,
            &value), D3D_OK);
    CHECK_TRUE(value == (DWORD)D3DZB_TRUE);

    /* D3D9 does not validate render-state *values* server-side; any
     * dword survives a Set/Get round-trip including D3DZB_USEW and
     * arbitrary fourth bits. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE,
            D3DZB_USEW), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZENABLE,
            &value), D3D_OK);
    CHECK_TRUE(value == (DWORD)D3DZB_USEW);

    hr = IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, 0xdeadbeefu);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        value = 0;
        CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZENABLE,
                &value), D3D_OK);
        CHECK_TRUE(value == 0xdeadbeefu);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: zwriteenable_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_zwriteenable_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    DWORD value = 0xdeadbeef;

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

    device = create_depth_device(d3d9, window, D3DFMT_D24S8);
    if (!device)
        goto done_window;

    /* Default after CreateDevice is TRUE per visual.c oracle. */
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZWRITEENABLE,
            &value), D3D_OK);
    CHECK_TRUE(value == TRUE);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE,
            FALSE), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZWRITEENABLE,
            &value), D3D_OK);
    CHECK_TRUE(value == FALSE);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZWRITEENABLE,
            TRUE), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_ZWRITEENABLE,
            &value), D3D_OK);
    CHECK_TRUE(value == TRUE);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: multisampled_depth_buffer_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_multisampled_depth_buffer_caps_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *ds = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    DWORD quality = 0xdeadbeef;
    HRESULT cap_hr;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    cap_hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_D24S8, FALSE, D3DMULTISAMPLE_2_SAMPLES,
            &quality);
    CHECK_TRUE(cap_hr == D3D_OK || cap_hr == D3DERR_NOTAVAILABLE);
    if (cap_hr == D3D_OK)
        CHECK_TRUE(quality >= 1);

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_depth_device(d3d9, window, D3DFMT_D24S8);
    if (!device)
        goto done_window;

    /* If multisampled D24S8 is reported as available, CreateDepthStencilSurface
     * with that sample type must succeed. If reported unavailable, the
     * runtime must still surface a clean D3DERR_INVALIDCALL or
     * D3DERR_NOTAVAILABLE rather than crashing. */
    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 64, 64,
            D3DFMT_D24S8, D3DMULTISAMPLE_2_SAMPLES, 0, TRUE, &ds, NULL);
    if (cap_hr == D3D_OK)
    {
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr) && ds)
            IDirect3DSurface9_Release(ds);
    }
    else
    {
        CHECK_TRUE(hr == D3DERR_INVALIDCALL || hr == D3DERR_NOTAVAILABLE);
        CHECK_TRUE(ds == NULL);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
