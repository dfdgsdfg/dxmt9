/*
 * Shading, lighting, transform, and resource lifetime conformance
 * scaffolds split from d3d9_conformance.c. Each scaffold validates
 * the observable D3D9 surface for the indicated Wine visual.c oracle
 * without copying the Wine implementation.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/visual.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Covers shademode render-state policy, fixed-function lighting
 * default and SetLight round-trip, world/view/projection transform
 * round-trip, internal-refcount-from-bind release policy,
 * EvictManagedResources, AddDirtyRect pool/rect policy,
 * GetFrontBufferData policy, and multisample RT/DS mismatch.
 */

#include "d3d9_conformance_fixtures.h"

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_shademode
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_shademode_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    DWORD mode;

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

    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SHADEMODE, &mode), D3D_OK);
    CHECK_TRUE(mode == D3DSHADE_GOURAUD);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SHADEMODE,
            D3DSHADE_FLAT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SHADEMODE, &mode), D3D_OK);
    CHECK_TRUE(mode == D3DSHADE_FLAT);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SHADEMODE,
            D3DSHADE_PHONG), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SHADEMODE, &mode), D3D_OK);
    CHECK_TRUE(mode == D3DSHADE_PHONG);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: lighting_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_lighting_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DLIGHT9 light;
    D3DLIGHT9 out;
    HWND window;
    DWORD on;

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

    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_LIGHTING, &on), D3D_OK);
    CHECK_TRUE(on == TRUE);

    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 1.0f;
    light.Direction.z = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_LightEnable(device, 0, TRUE), D3D_OK);

    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetLight(device, 0, &out), D3D_OK);
    CHECK_TRUE(out.Type == D3DLIGHT_DIRECTIONAL);
    CHECK_TRUE(out.Diffuse.r == 1.0f);

    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, NULL), D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_lighting_matrices
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_lighting_world_view_matrix_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DMATRIX in;
    D3DMATRIX out;
    HWND window;
    int i;

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

    memset(&in, 0, sizeof(in));
    for (i = 0; i < 16; ++i)
        ((float *)&in)[i] = (float)(i + 1);

    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &in), D3D_OK);
    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetTransform(device, D3DTS_WORLD, &out), D3D_OK);
    CHECK_TRUE(memcmp(&in, &out, sizeof(in)) == 0);

    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &in), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetTransform(device, D3DTS_VIEW, &out), D3D_OK);
    CHECK_TRUE(memcmp(&in, &out, sizeof(in)) == 0);

    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_PROJECTION, &in), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetTransform(device, D3DTS_PROJECTION, &out), D3D_OK);
    CHECK_TRUE(memcmp(&in, &out, sizeof(in)) == 0);

    /* D3D9 accepts unknown TRANSFORMSTATETYPE indices silently. */
    CHECK_HR(IDirect3DDevice9_SetTransform(device,
            (D3DTRANSFORMSTATETYPE)0xdead, &in), D3D_OK);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: release_buffer_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_release_buffer_bound_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DVertexBuffer9 *out_vb = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    UINT offset;
    UINT stride;

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

    CHECK_HR(IDirect3DDevice9_CreateVertexBuffer(device, 64, 0, 0,
            D3DPOOL_MANAGED, &vb, NULL), D3D_OK);
    if (!vb)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, 12), D3D_OK);
    /* Drop our reference. Device must keep its internal reference. */
    IDirect3DVertexBuffer9_Release(vb);

    CHECK_HR(IDirect3DDevice9_GetStreamSource(device, 0, &out_vb, &offset, &stride),
            D3D_OK);
    CHECK_TRUE(out_vb == vb);
    CHECK_TRUE(offset == 0);
    CHECK_TRUE(stride == 12);

    if (out_vb)
        IDirect3DVertexBuffer9_Release(out_vb);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_evict_bound_resources
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_evict_managed_resources_policy(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DSURFACE_DESC desc;
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

    CHECK_HR(IDirect3DDevice9_CreateTexture(device, 32, 32, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL), D3D_OK);
    if (!texture)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0,
            (IDirect3DBaseTexture9 *)texture), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EvictManagedResources(device), D3D_OK);

    /* Managed eviction drops the GPU mirror but keeps the COM object live. */
    memset(&desc, 0xcc, sizeof(desc));
    CHECK_HR(IDirect3DTexture9_GetLevelDesc(texture, 0, &desc), D3D_OK);
    CHECK_TRUE(desc.Width == 32);
    CHECK_TRUE(desc.Height == 32);

    IDirect3DDevice9_SetTexture(device, 0, NULL);
    IDirect3DTexture9_Release(texture);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: add_dirty_rect_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_add_dirty_rect_policy(const struct d3d9_api *api)
{
    IDirect3DTexture9 *managed = NULL;
    IDirect3DTexture9 *defaultpool = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    RECT rect;
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

    CHECK_HR(IDirect3DDevice9_CreateTexture(device, 64, 64, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &managed, NULL), D3D_OK);
    if (!managed)
        goto done_device;

    CHECK_HR(IDirect3DTexture9_AddDirtyRect(managed, NULL), D3D_OK);

    rect.left = 0; rect.top = 0; rect.right = 16; rect.bottom = 16;
    CHECK_HR(IDirect3DTexture9_AddDirtyRect(managed, &rect), D3D_OK);

    rect.left = 16; rect.right = 8;  /* inverted */
    CHECK_HR(IDirect3DTexture9_AddDirtyRect(managed, &rect), D3DERR_INVALIDCALL);

    IDirect3DTexture9_Release(managed);
    managed = NULL;

    if (SUCCEEDED(IDirect3DDevice9_CreateTexture(device, 64, 64, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &defaultpool, NULL)))
    {
        rect.left = 0; rect.top = 0; rect.right = 16; rect.bottom = 16;
        CHECK_HR(IDirect3DTexture9_AddDirtyRect(defaultpool, &rect),
                D3DERR_INVALIDCALL);
        IDirect3DTexture9_Release(defaultpool);
    }

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_multisample_get_front_buffer_data
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_multisample_get_front_buffer_data_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *front = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DDISPLAYMODE mode;
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

    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &mode);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device,
            mode.Width, mode.Height, D3DFMT_A8R8G8B8,
            D3DPOOL_SYSTEMMEM, &front, NULL);
    if (FAILED(hr))
    {
        skip_current_test("Could not create matching front-buffer surface");
        goto done_device;
    }

    hr = IDirect3DDevice9_GetFrontBufferData(device, 0, front);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL
            || hr == D3DERR_DEVICELOST);

    CHECK_HR(IDirect3DDevice9_GetFrontBufferData(device, 0, NULL),
            D3DERR_INVALIDCALL);

    IDirect3DSurface9_Release(front);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_multisample_mismatch
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_multisample_rt_ds_mismatch_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *rt = NULL;
    IDirect3DSurface9 *ds = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    DWORD quality;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }
    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_D24S8, TRUE,
            D3DMULTISAMPLE_2_SAMPLES, &quality);
    if (FAILED(hr))
    {
        skip_current_test("2x multisample depth-stencil not supported");
        goto done_d3d9;
    }

    window = create_test_window();
    if (!window)
        goto done_d3d9;
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateRenderTarget(device, 32, 32,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 32, 32,
            D3DFMT_D24S8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE, &ds, NULL);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, rt), D3D_OK);
    hr = IDirect3DDevice9_SetDepthStencilSurface(device, ds);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

done_device:
    if (ds)
        IDirect3DSurface9_Release(ds);
    if (rt)
        IDirect3DSurface9_Release(rt);
    if (device)
        IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
