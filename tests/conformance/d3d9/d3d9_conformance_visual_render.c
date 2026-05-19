/*
 * Render-target, clear, and surface conformance scaffolds split from
 * d3d9_conformance.c. Each scaffold validates the observable D3D9
 * surface for the indicated Wine visual.c oracle without copying
 * the Wine implementation.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/visual.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Covers depth-bias render state, Clear flag/rect policy, smaller
 * render-target clear policy, ColorFill format/pool policy,
 * offscreen plain surface creation policy, stencil format caps,
 * UpdateSurface pool/format policy, and swapchain Present/Flip
 * policy.
 */

#include "d3d9_conformance_fixtures.h"

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: depth_clamp_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_depth_clamp_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DVIEWPORT9 vp;
    DWORD bias;
    HWND window;

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

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_DEPTHBIAS, 0x3c800000), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_DEPTHBIAS, &bias), D3D_OK);
    CHECK_TRUE(bias == 0x3c800000);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_SLOPESCALEDEPTHBIAS, 0x40000000), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_SLOPESCALEDEPTHBIAS, &bias), D3D_OK);
    CHECK_TRUE(bias == 0x40000000);

    memset(&vp, 0, sizeof(vp));
    vp.Width = 64;
    vp.Height = 64;
    vp.MinZ = -0.5f;
    vp.MaxZ = 1.5f;
    CHECK_HR(IDirect3DDevice9_SetViewport(device, &vp), D3D_OK);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: clear_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_clear_color_only_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DRECT rect;
    HWND window;

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

    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff112233, 0.0f, 0), D3D_OK);

    rect.x1 = 0; rect.y1 = 0; rect.x2 = 16; rect.y2 = 16;
    CHECK_HR(IDirect3DDevice9_Clear(device, 1, &rect, D3DCLEAR_TARGET,
            0xff112233, 0.0f, 0), D3D_OK);

    CHECK_HR(IDirect3DDevice9_Clear(device, 0, &rect, D3DCLEAR_TARGET,
            0xff112233, 0.0f, 0), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_Clear(device, 1, NULL, D3DCLEAR_TARGET,
            0xff112233, 0.0f, 0), D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_clear_different_size_surfaces
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_clear_smaller_rt_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *rt = NULL;
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
    if (!window)
        goto done_d3d9;
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateRenderTarget(device, 32, 32,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, rt), D3D_OK);
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff445566, 0.0f, 0);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);

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
 * function: color_fill_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_colorfill_format_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *plain_default = NULL;
    IDirect3DSurface9 *plain_dxt1 = NULL;
    IDirect3DSurface9 *plain_sysmem = NULL;
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
    if (!window)
        goto done_d3d9;
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &plain_default, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_HR(IDirect3DDevice9_ColorFill(device, plain_default, NULL,
                0xff112233), D3D_OK);

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32,
            D3DFMT_DXT1, D3DPOOL_DEFAULT, &plain_dxt1, NULL);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_ColorFill(device, plain_dxt1, NULL,
                0xff112233), D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(plain_dxt1);
    }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &plain_sysmem, NULL);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_ColorFill(device, plain_sysmem, NULL,
                0xff112233), D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(plain_sysmem);
    }

    if (plain_default)
        IDirect3DSurface9_Release(plain_default);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: offscreen_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_offscreen_surface_creation_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DSURFACE_DESC desc;
    HWND window;
    HRESULT hr;

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

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&desc, 0xcc, sizeof(desc));
        CHECK_HR(IDirect3DSurface9_GetDesc(surface, &desc), D3D_OK);
        CHECK_TRUE(desc.Width == 64);
        CHECK_TRUE(desc.Height == 64);
        CHECK_TRUE(desc.Format == D3DFMT_A8R8G8B8);
        CHECK_TRUE(desc.Type == D3DRTYPE_SURFACE);
        IDirect3DSurface9_Release(surface);
        surface = NULL;
    }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &surface, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    if (SUCCEEDED(hr))
        IDirect3DSurface9_Release(surface);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: stencil_cull_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_stencil_cull_caps_policy(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    HRESULT s8_hr;
    HRESULT d24s8_hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    s8_hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
            D3DRTYPE_SURFACE, D3DFMT_S8_LOCKABLE);
    CHECK_TRUE(s8_hr == D3D_OK || s8_hr == D3DERR_NOTAVAILABLE);

    d24s8_hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
            D3DRTYPE_SURFACE, D3DFMT_D24S8);
    CHECK_HR(d24s8_hr, D3D_OK);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: update_surface_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_update_surface_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *src_sysmem = NULL;
    IDirect3DSurface9 *dst_default = NULL;
    IDirect3DSurface9 *dst_sysmem = NULL;
    IDirect3DSurface9 *dst_xrgb = NULL;
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
    if (!window)
        goto done_d3d9;
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &src_sysmem, NULL);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &dst_default, NULL);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_UpdateSurface(device, src_sysmem, NULL,
            dst_default, NULL), D3D_OK);

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &dst_sysmem, NULL);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_UpdateSurface(device, src_sysmem, NULL,
                dst_sysmem, NULL), D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(dst_sysmem);
    }

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 32, 32,
            D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &dst_xrgb, NULL);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_UpdateSurface(device, src_sysmem, NULL,
                dst_xrgb, NULL), D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(dst_xrgb);
    }

done_device:
    if (dst_default)
        IDirect3DSurface9_Release(dst_default);
    if (src_sysmem)
        IDirect3DSurface9_Release(src_sysmem);
    if (device)
        IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_flip
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_swapchain_flip_present_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DSURFACE_DESC desc;
    HWND window;
    HRESULT hr;

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

    CHECK_HR(IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL), D3D_OK);

    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
            &backbuffer);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&desc, 0xcc, sizeof(desc));
        CHECK_HR(IDirect3DSurface9_GetDesc(backbuffer, &desc), D3D_OK);
        CHECK_TRUE(desc.Width > 0);
        CHECK_TRUE(desc.Height > 0);
        IDirect3DSurface9_Release(backbuffer);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
