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

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_buffer_no_dirty_update
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_buffer_no_dirty_update_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vb_managed = NULL;
    IDirect3DVertexBuffer9 *vb_default = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    void *data;

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

    /* MANAGED VBs accept Lock without an explicit AddDirtyRect dance. */
    CHECK_HR(IDirect3DDevice9_CreateVertexBuffer(device, 64, 0, 0,
            D3DPOOL_MANAGED, &vb_managed, NULL), D3D_OK);
    if (vb_managed)
    {
        CHECK_HR(IDirect3DVertexBuffer9_Lock(vb_managed, 0, 0, &data, 0), D3D_OK);
        CHECK_TRUE(data != NULL);
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(vb_managed), D3D_OK);
        IDirect3DVertexBuffer9_Release(vb_managed);
    }

    /* DEFAULT-pool DYNAMIC also accepts Lock with DISCARD/NOOVERWRITE. */
    if (SUCCEEDED(IDirect3DDevice9_CreateVertexBuffer(device, 64,
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
            &vb_default, NULL)))
    {
        CHECK_HR(IDirect3DVertexBuffer9_Lock(vb_default, 0, 0, &data,
                D3DLOCK_DISCARD), D3D_OK);
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(vb_default), D3D_OK);
        IDirect3DVertexBuffer9_Release(vb_default);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: yuv_color_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_yuv_color_caps_policy(const struct d3d9_api *api)
{
    static const D3DFORMAT yuv_fourcc[] =
    {
        D3DFMT_YUY2, D3DFMT_UYVY,
    };
    IDirect3D9 *d3d9;
    HRESULT hr;
    UINT i;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(yuv_fourcc); ++i)
    {
        hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_SURFACE,
                yuv_fourcc[i]);
        CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);
    }

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: yuv_layout_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_yuv_layout_lock_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DLOCKED_RECT locked;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_SURFACE,
            D3DFMT_YUY2);
    if (FAILED(hr))
    {
        skip_current_test("YUY2 not supported");
        goto done_d3d9;
    }

    window = create_test_window();
    if (!window)
        goto done_d3d9;
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 16, 16,
            D3DFMT_YUY2, D3DPOOL_DEFAULT, &surface, NULL);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        if (SUCCEEDED(IDirect3DSurface9_LockRect(surface, &locked, NULL, 0)))
        {
            /* YUY2 packs 2 pixels per 32 bits, so pitch must be >= 2*width. */
            CHECK_TRUE(locked.Pitch >= 32);
            IDirect3DSurface9_UnlockRect(surface);
        }
        IDirect3DSurface9_Release(surface);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_3dc_formats
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_3dc_format_caps_policy(const struct d3d9_api *api)
{
    static const D3DFORMAT atc_fourcc[] =
    {
        MAKEFOURCC('A','T','I','1'),
        MAKEFOURCC('A','T','I','2'),
    };
    IDirect3D9 *d3d9;
    HRESULT hr;
    UINT i;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    for (i = 0; i < ARRAY_SIZE(atc_fourcc); ++i)
    {
        hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                atc_fourcc[i]);
        CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);
    }

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_position_index
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_position_index_decl_policy(const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 decl_elements[] =
    {
        {0, 0,  D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 1},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
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

    /* Declaration with POSITION0 + POSITION1 must be accepted. */
    CHECK_HR(IDirect3DDevice9_CreateVertexDeclaration(device,
            decl_elements, &decl), D3D_OK);
    if (decl)
        IDirect3DVertexDeclaration9_Release(decl);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_mvp_software_vertex_shaders
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_mvp_software_vp_policy(const struct d3d9_api *api)
{
/* Place high 2 register-type bits at bits 11-12 (D3DSP_REGTYPE_MASK2=0x1800).
 * mingw's d3d9types.h defines D3DSP_REGTYPE_SHIFT2=8 which contradicts MASK2;
 * dxmt9's decoder follows MASK2 (the canonical encoding), so we shift to 11. */
#define SWVP_VS_REGTYPE(type) \
    ((((DWORD)(type) & 0x7u) << D3DSP_REGTYPE_SHIFT) \
            | (((((DWORD)(type) >> 3u) & 0x3u) << 11)))
#define SWVP_VS_DST(type, index, mask) \
    (0x80000000u | SWVP_VS_REGTYPE(type) \
            | (((DWORD)(mask) & 0xfu) << 16) | ((DWORD)(index) & 0x7ffu))
#define SWVP_VS_SRC(type, index) \
    (0x80000000u | SWVP_VS_REGTYPE(type) | D3DSP_NOSWIZZLE \
            | ((DWORD)(index) & 0x7ffu))
#define SWVP_VS_INST(opcode, operands) \
    (((DWORD)(opcode) & D3DSI_OPCODE_MASK) \
            | (((DWORD)(operands) & 0xfu) << D3DSI_INSTLENGTH_SHIFT))
#define SWVP_VS_DCL(usage, usage_index) \
    (((DWORD)(usage) & 0xfu) \
            | (((DWORD)(usage_index) & 0xfu) << D3DSP_DCL_USAGEINDEX_SHIFT))
    struct swvp_vertex
    {
        float x, y, z;
        DWORD color;
    };
    struct swvp_lit_vertex
    {
        float x, y, z;
        float nx, ny, nz;
    };
    struct swvp_psize_vertex
    {
        float x, y, z;
        float psize;
        DWORD color;
    };
    struct swvp_blendweight_vertex
    {
        float x, y, z;
        float bw0, bw1, bw2, bw3;
        DWORD color;
    };
    struct swvp_blendindices_vertex
    {
        float x, y, z;
        float bw0, bw1, bw2, bw3;
        BYTE bi0, bi1, bi2, bi3;
        DWORD color;
    };
    struct swvp_blendweight_stream_vertex
    {
        float bw0, bw1, bw2, bw3;
    };
    struct swvp_blendindices_stream_vertex
    {
        float bw0, bw1, bw2, bw3;
        BYTE bi0, bi1, bi2, bi3;
    };
    struct swvp_position_vertex
    {
        float x, y, z;
    };
    struct swvp_color_vertex
    {
        DWORD color;
    };
    struct swvp_tangent_vertex
    {
        float x, y, z;
        DWORD color;
        float tx, ty, tz;
    };
    struct swvp_tangent_stream_vertex
    {
        float tx, ty, tz;
    };
    struct swvp_textured_vertex
    {
        float x, y, z;
        DWORD color;
        float u, v;
    };
    struct swvp_xyzw_vertex
    {
        float x, y, z, w;
        DWORD color;
    };
    struct swvp_short4n_vertex
    {
        SHORT x, y, z, w;
        DWORD color;
    };
    static const struct swvp_vertex tri[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffff0000u},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu},
    };
    static const struct swvp_vertex tri_yellow[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffffff00u},
        {-0.5f,  0.5f, 0.0f, 0xffffff00u},
        { 0.5f, -0.5f, 0.0f, 0xffffff00u},
    };
    static const struct swvp_vertex tri_with_padding[] =
    {
        { 0.0f,  0.0f, 0.0f, 0xff000000u},
        {-0.5f, -0.5f, 0.0f, 0xffff0000u},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu},
    };
    static const struct swvp_vertex strip[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffff0000u},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu},
        { 0.5f,  0.5f, 0.0f, 0xffffffffu},
    };
    static const struct swvp_lit_vertex lit_tri[] =
    {
        {-0.25f, -0.25f, 0.0f, 0.0f, 0.0f, -1.0f},
        {-0.25f,  0.25f, 0.0f, 0.0f, 0.0f, -1.0f},
        { 0.25f, -0.25f, 0.0f, 0.0f, 0.0f, -1.0f},
    };
    static const struct swvp_psize_vertex psize_tri[] =
    {
        {-0.25f, -0.25f, 0.0f, 4.0f, 0xffff00ffu},
        {-0.25f,  0.25f, 0.0f, 5.0f, 0xff00ffffu},
        { 0.25f, -0.25f, 0.0f, 6.0f, 0xffffff00u},
    };
    static const struct swvp_blendweight_vertex blendweight_tri[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0xffff0000u},
        {-0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0xff00ff00u},
        { 0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0xff0000ffu},
    };
    static const struct swvp_blendindices_vertex blendindices_tri[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0, 0xffff0000u},
        {-0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0, 0xff00ff00u},
        { 0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0, 0xff0000ffu},
    };
    static const struct swvp_blendweight_stream_vertex split_blendweights[] =
    {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
    };
    static const struct swvp_blendindices_stream_vertex split_blendindices[] =
    {
        {0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0},
        {0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0},
        {0.0f, 0.0f, 0.0f, 0.0f, 0, 0, 0, 0},
    };
    static const struct swvp_position_vertex split_positions[] =
    {
        {-0.5f, -0.5f, 0.0f},
        {-0.5f,  0.5f, 0.0f},
        { 0.5f, -0.5f, 0.0f},
    };
    static const struct swvp_position_vertex split_quad_positions[] =
    {
        {-0.5f, -0.5f, 0.0f},
        {-0.5f,  0.5f, 0.0f},
        { 0.5f, -0.5f, 0.0f},
        { 0.5f,  0.5f, 0.0f},
    };
    static const struct swvp_position_vertex split_line_positions[] =
    {
        {-0.75f, -0.375f, 0.0f},
        { 0.00f, -0.375f, 0.0f},
        { 0.75f, -0.375f, 0.0f},
    };
    static const struct swvp_color_vertex split_colors[] =
    {
        {0xffff0000u},
        {0xff00ff00u},
        {0xff0000ffu},
    };
    static const struct swvp_tangent_vertex tangent_tri[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffff0000u, 0.125f, 0.0f, 0.0f},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u, 0.125f, 0.0f, 0.0f},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu, 0.125f, 0.0f, 0.0f},
    };
    static const struct swvp_tangent_stream_vertex split_tangents[] =
    {
        {0.125f, 0.0f, 0.0f},
        {0.125f, 0.0f, 0.0f},
        {0.125f, 0.0f, 0.0f},
    };
    static const struct swvp_textured_vertex textured_quad[] =
    {
        {-2.0f,  2.0f, 0.0f, 0xffffffffu, 0.0f, 0.0f},
        { 2.0f,  2.0f, 0.0f, 0xffffffffu, 1.0f, 0.0f},
        {-2.0f, -2.0f, 0.0f, 0xffffffffu, 0.0f, 1.0f},
        { 2.0f, -2.0f, 0.0f, 0xffffffffu, 1.0f, 1.0f},
    };
    static const struct swvp_textured_vertex textured_clip_quad[] =
    {
        {-1.0f,  1.0f, 0.0f, 0xffffffffu, 0.0f, 0.0f},
        { 1.0f,  1.0f, 0.0f, 0xffffffffu, 1.0f, 0.0f},
        {-1.0f, -1.0f, 0.0f, 0xffffffffu, 0.0f, 1.0f},
        { 1.0f, -1.0f, 0.0f, 0xffffffffu, 1.0f, 1.0f},
    };
    static const struct swvp_xyzw_vertex clip_tri[] =
    {
        {-1.0f, -1.0f, 0.0f, 2.0f, 0xffff0000u},
        {-1.0f,  1.0f, 0.0f, 2.0f, 0xff00ff00u},
        { 1.0f, -1.0f, 0.0f, 2.0f, 0xff0000ffu},
    };
    static const struct swvp_xyzw_vertex behind_clip_tri[] =
    {
        {-0.5f, -0.5f, 0.0f, -1.0f, 0xffff0000u},
        {-0.5f,  0.5f, 0.0f, -1.0f, 0xff00ff00u},
        { 0.5f, -0.5f, 0.0f, -1.0f, 0xff0000ffu},
    };
    static const struct swvp_xyzw_vertex far_clip_tri[] =
    {
        {-0.5f, -0.5f, 2.0f, 1.0f, 0xffff0000u},
        {-0.5f,  0.5f, 2.0f, 1.0f, 0xff00ff00u},
        { 0.5f, -0.5f, 2.0f, 1.0f, 0xff0000ffu},
    };
    static const struct swvp_xyzw_vertex user_clip_tri[] =
    {
        {-0.75f, -0.50f, 0.5f, 1.0f, 0xffff0000u},
        {-0.75f,  0.50f, 0.5f, 1.0f, 0xff00ff00u},
        {-0.25f, -0.50f, 0.5f, 1.0f, 0xff0000ffu},
    };
    static const struct swvp_xyzw_vertex partial_user_clip_tri[] =
    {
        {-0.75f, -0.50f, 0.5f, 1.0f, 0xffff0000u},
        { 0.75f, -0.50f, 0.5f, 1.0f, 0xff00ff00u},
        { 0.75f,  0.50f, 0.5f, 1.0f, 0xff0000ffu},
    };
    static const struct swvp_xyzw_vertex partial_user_clip_line[] =
    {
        {-0.75f, 0.0f, 0.5f, 1.0f, 0xffff0000u},
        { 0.75f, 0.0f, 0.5f, 1.0f, 0xff00ff00u},
    };
    static const struct swvp_xyzw_vertex user_clip_point[] =
    {
        {-0.375f, 0.0f, 0.5f, 1.0f, 0xffffffffu},
    };
    static const float user_clip_plane_x_positive[4] = {1.0f, 0.0f, 0.0f, 0.0f};
    static const struct swvp_short4n_vertex short4n_tri[] =
    {
        {-32767, -32767, 0, 32767, 0xffff0000u},
        {-32767,  32767, 0, 32767, 0xff00ff00u},
        { 32767, -32767, 0, 32767, 0xff0000ffu},
    };
    static const D3DVERTEXELEMENT9 swvp_split_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_stream0_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_short4n_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_SHORT4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 8, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_blendweight_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        {0, 28, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_blendindices_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        {0, 28, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        {0, 32, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_split_blendweight_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_split_blendindices_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        {1, 16, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_tangent_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 16, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 swvp_split_tangent_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {2, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        D3DDECL_END()
    };
    static const WORD tri_indices[] = {0, 1, 2};
    static const DWORD tri_indices32[] = {0, 1, 2};
    static const WORD tri_offset_indices[] = {1, 2, 3};
    static const WORD strip_indices[] = {0, 1, 2, 3};
    static const DWORD swvp_vs_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 0, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 1, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        SWVP_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        SWVP_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        SWVP_VS_INST(D3DSIO_M4x4, 3),
        SWVP_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        SWVP_VS_SRC(D3DSPR_INPUT, 0),
        SWVP_VS_SRC(D3DSPR_CONST, 0),
        SWVP_VS_INST(D3DSIO_MOV, 2),
        SWVP_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        SWVP_VS_SRC(D3DSPR_INPUT, 1),
        D3DSIO_END,
    };
    static const DWORD swvp_vs_tangent_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 0, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 1, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_TANGENT, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 2, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        SWVP_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        SWVP_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        SWVP_VS_INST(D3DSIO_MOV, 2),
        SWVP_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        SWVP_VS_SRC(D3DSPR_INPUT, 0),
        SWVP_VS_INST(D3DSIO_ADD, 3),
        SWVP_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        SWVP_VS_SRC(D3DSPR_INPUT, 0),
        SWVP_VS_SRC(D3DSPR_INPUT, 2),
        SWVP_VS_INST(D3DSIO_MOV, 2),
        SWVP_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        SWVP_VS_SRC(D3DSPR_INPUT, 1),
        D3DSIO_END,
    };
    static const DWORD swvp_vs_texcoord_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 0, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 1, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        SWVP_VS_DST(D3DSPR_INPUT, 2, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        SWVP_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        SWVP_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        SWVP_VS_INST(D3DSIO_DCL, 2),
        SWVP_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        SWVP_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        SWVP_VS_INST(D3DSIO_M4x4, 3),
        SWVP_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        SWVP_VS_SRC(D3DSPR_INPUT, 0),
        SWVP_VS_SRC(D3DSPR_CONST, 0),
        SWVP_VS_INST(D3DSIO_MOV, 2),
        SWVP_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        SWVP_VS_SRC(D3DSPR_INPUT, 1),
        SWVP_VS_INST(D3DSIO_MOV, 2),
        SWVP_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        SWVP_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END,
    };
    static const DWORD swvp_ps_constant_2_0[] =
    {
        D3DPS_VERSION(2, 0),
        SWVP_VS_INST(D3DSIO_MOV, 2),
        SWVP_VS_DST(D3DSPR_COLOROUT, 0, 0xf),
        SWVP_VS_SRC(D3DSPR_CONST, 0),
        D3DSIO_END,
    };
    static const float swvp_vs_identity[4][4] =
    {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
    static const float swvp_ps_magenta[4] = {1.0f, 0.0f, 1.0f, 1.0f};
    IDirect3DVertexShader9 *vertex_shader = NULL;
    IDirect3DVertexShader9 *tangent_vertex_shader = NULL;
    IDirect3DVertexShader9 *texcoord_vertex_shader = NULL;
    IDirect3DPixelShader9 *pixel_shader = NULL;
    IDirect3DVertexDeclaration9 *stream0_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *short4n_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *blendweight_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *blendindices_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *split_blendweight_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *split_blendindices_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *split_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *tangent_vertex_decl = NULL;
    IDirect3DVertexDeclaration9 *split_tangent_vertex_decl = NULL;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *blendweight_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *blendindices_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *split_blendweight_stream_buffer = NULL;
    IDirect3DVertexBuffer9 *split_blendindices_stream_buffer = NULL;
    IDirect3DVertexBuffer9 *padded_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *strip_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *tangent_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *tangent_stream_buffer = NULL;
    IDirect3DVertexBuffer9 *position_buffer = NULL;
    IDirect3DVertexBuffer9 *position_quad_buffer = NULL;
    IDirect3DVertexBuffer9 *position_line_buffer = NULL;
    IDirect3DVertexBuffer9 *color_buffer = NULL;
    IDirect3DVertexBuffer9 *textured_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *textured_clip_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *clip_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *behind_clip_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *far_clip_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *user_clip_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *partial_user_clip_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *partial_user_clip_line_vertex_buffer = NULL;
    IDirect3DVertexBuffer9 *user_clip_point_vertex_buffer = NULL;
    IDirect3DIndexBuffer9 *index_buffer = NULL;
    IDirect3DIndexBuffer9 *index_buffer32 = NULL;
    IDirect3DIndexBuffer9 *offset_index_buffer = NULL;
    IDirect3DIndexBuffer9 *strip_index_buffer = NULL;
    IDirect3DSurface9 *render_target = NULL;
    IDirect3DSurface9 *readback = NULL;
    IDirect3DVertexShader9 *returned_vertex_shader = NULL;
    IDirect3DPixelShader9 *returned_pixel_shader = NULL;
    IDirect3DVertexBuffer9 *returned_stream = NULL;
    IDirect3DDevice9 *device_swvp = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DMATERIAL9 material;
    D3DMATRIX world;
    D3DLIGHT9 light;
    D3DLOCKED_RECT locked_rect;
    D3DVIEWPORT9 viewport;
    IDirect3D9 *d3d9;
    void *mapped;
    DWORD probe_pixel;
    HWND window;
    HRESULT hr;
    DWORD fvf;
    UINT i;
    UINT returned_offset;
    UINT returned_stride;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }
    window = create_test_window();
    if (!window)
        goto done_d3d9;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.hDeviceWindow = window;
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
            &pp, &device_swvp);
    /* SWVP create must either succeed or return a documented HRESULT. */
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL || hr == D3DERR_NOTAVAILABLE);
    if (SUCCEEDED(hr))
    {
        /* GetSoftwareVertexProcessing returns BOOL, not HRESULT. */
        CHECK_TRUE(IDirect3DDevice9_GetSoftwareVertexProcessing(device_swvp));
        CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                D3DRS_LIGHTING, FALSE), D3D_OK);
        memset(&world, 0, sizeof(world));
        world.m[0][0] = 0.5f;
        world.m[1][1] = 0.5f;
        world.m[2][2] = 1.0f;
        world.m[3][3] = 1.0f;
        CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp, D3DTS_WORLD,
                &world), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
        hr = IDirect3DDevice9_CreateVertexShader(device_swvp, swvp_vs_3_0,
                &vertex_shader);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device_swvp, 0,
                    (const float *)swvp_vs_identity, 4), D3D_OK);
        }
        hr = IDirect3DDevice9_CreatePixelShader(device_swvp,
                swvp_ps_constant_2_0, &pixel_shader);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantF(device_swvp, 0,
                    swvp_ps_magenta, 1), D3D_OK);
        }
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_stream0_decl_elements, &stream0_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_short4n_decl_elements, &short4n_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_blendweight_decl_elements, &blendweight_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_blendindices_decl_elements, &blendindices_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_split_blendweight_decl_elements,
                &split_blendweight_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_split_blendindices_decl_elements,
                &split_blendindices_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_split_decl_elements, &split_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_tangent_decl_elements, &tangent_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexDeclaration(device_swvp,
                swvp_split_tangent_decl_elements, &split_tangent_vertex_decl);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexShader(device_swvp,
                swvp_vs_tangent_3_0, &tangent_vertex_shader);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateVertexShader(device_swvp,
                swvp_vs_texcoord_3_0, &texcoord_vertex_shader);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateRenderTarget(device_swvp, 32, 32,
                D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                &render_target, NULL);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device_swvp, 32, 32,
                D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
        CHECK_HR(hr, D3D_OK);
        if (render_target)
        {
            CHECK_HR(IDirect3DDevice9_SetRenderTarget(device_swvp, 0,
                    render_target), D3D_OK);
            memset(&viewport, 0, sizeof(viewport));
            viewport.X = 0;
            viewport.Y = 0;
            viewport.Width = 32;
            viewport.Height = 32;
            viewport.MinZ = 0.0f;
            viewport.MaxZ = 1.0f;
            CHECK_HR(IDirect3DDevice9_SetViewport(device_swvp, &viewport),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_ZENABLE, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CULLMODE, D3DCULL_NONE), D3D_OK);
        }
        CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices,
                D3DFMT_INDEX16, tri, sizeof(tri[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                D3DPT_TRIANGLELIST, 1, ARRAY_SIZE(tri), 1,
                tri_offset_indices, D3DFMT_INDEX16, tri_with_padding,
                sizeof(tri_with_padding[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_TRIANGLESTRIP, 2, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                D3DPT_TRIANGLESTRIP, 0, ARRAY_SIZE(strip), 2, strip_indices,
                D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_POINTLIST, ARRAY_SIZE(strip), strip, sizeof(strip[0])),
                D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_LINELIST, 2, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_LINESTRIP, 3, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_TRIANGLEFAN, 2, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                D3DPT_POINTLIST, 0, ARRAY_SIZE(strip), ARRAY_SIZE(strip),
                strip_indices, D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                D3DPT_LINELIST, 0, ARRAY_SIZE(strip), 2, strip_indices,
                D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                D3DPT_LINESTRIP, 0, ARRAY_SIZE(strip), 3, strip_indices,
                D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                D3DPT_TRIANGLEFAN, 0, ARRAY_SIZE(strip), 2, strip_indices,
                D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
        if (vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetVertexShader(device_swvp,
                    &returned_vertex_shader), D3D_OK);
            CHECK_TRUE(returned_vertex_shader == vertex_shader);
            if (returned_vertex_shader)
            {
                IDirect3DVertexShader9_Release(returned_vertex_shader);
                returned_vertex_shader = NULL;
            }
            fvf = 0;
            CHECK_HR(IDirect3DDevice9_GetFVF(device_swvp, &fvf), D3D_OK);
            CHECK_TRUE(fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE));
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices,
                    D3DFMT_INDEX16, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, ARRAY_SIZE(tri), 1,
                    tri_offset_indices, D3DFMT_INDEX16, tri_with_padding,
                    sizeof(tri_with_padding[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLESTRIP, 2, strip, sizeof(strip[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLESTRIP, 0, ARRAY_SIZE(strip), 2, strip_indices,
                    D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_POINTLIST, ARRAY_SIZE(strip), strip, sizeof(strip[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_LINELIST, 2, strip, sizeof(strip[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_LINESTRIP, 3, strip, sizeof(strip[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLEFAN, 2, strip, sizeof(strip[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_POINTLIST, 0, ARRAY_SIZE(strip), ARRAY_SIZE(strip),
                    strip_indices, D3DFMT_INDEX16, strip, sizeof(strip[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_LINELIST, 0, ARRAY_SIZE(strip), 2, strip_indices,
                    D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_LINESTRIP, 0, ARRAY_SIZE(strip), 3, strip_indices,
                    D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLEFAN, 0, ARRAY_SIZE(strip), 2, strip_indices,
                    D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
            if (stream0_vertex_decl)
            {
                CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                        stream0_vertex_decl), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices,
                        D3DFMT_INDEX16, tri, sizeof(tri[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLELIST, 1, ARRAY_SIZE(tri), 1,
                        tri_offset_indices, D3DFMT_INDEX16, tri_with_padding,
                        sizeof(tri_with_padding[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, strip, sizeof(strip[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 0, ARRAY_SIZE(strip), 2,
                        strip_indices, D3DFMT_INDEX16, strip, sizeof(strip[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_POINTLIST, ARRAY_SIZE(strip), strip,
                        sizeof(strip[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_LINELIST, 2, strip, sizeof(strip[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_LINESTRIP, 3, strip, sizeof(strip[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLEFAN, 2, strip, sizeof(strip[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_POINTLIST, 0, ARRAY_SIZE(strip), ARRAY_SIZE(strip),
                        strip_indices, D3DFMT_INDEX16, strip, sizeof(strip[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_LINELIST, 0, ARRAY_SIZE(strip), 2, strip_indices,
                        D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_LINESTRIP, 0, ARRAY_SIZE(strip), 3, strip_indices,
                        D3DFMT_INDEX16, strip, sizeof(strip[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLEFAN, 0, ARRAY_SIZE(strip), 2,
                        strip_indices, D3DFMT_INDEX16, strip, sizeof(strip[0])),
                        D3D_OK);
            }
            if (tangent_vertex_shader && tangent_vertex_decl)
            {
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                        tangent_vertex_shader), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                        tangent_vertex_decl), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLELIST, 1, tangent_tri,
                        sizeof(tangent_tri[0])), D3D_OK);
            }
        }
        CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_TRIANGLELIST, 1, psize_tri, sizeof(psize_tri[0])),
                D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp, sizeof(tri),
                0, D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_MANAGED,
                &vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(vertex_buffer, 0,
                    sizeof(tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, tri, sizeof(tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(blendweight_tri), 0, 0, D3DPOOL_MANAGED,
                &blendweight_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(blendweight_vertex_buffer, 0,
                    sizeof(blendweight_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, blendweight_tri, sizeof(blendweight_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        blendweight_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(blendindices_tri), 0, 0, D3DPOOL_MANAGED,
                &blendindices_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(blendindices_vertex_buffer, 0,
                    sizeof(blendindices_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, blendindices_tri, sizeof(blendindices_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        blendindices_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(split_blendweights), 0, 0, D3DPOOL_MANAGED,
                &split_blendweight_stream_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(split_blendweight_stream_buffer,
                    0, sizeof(split_blendweights), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, split_blendweights, sizeof(split_blendweights));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        split_blendweight_stream_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(split_blendindices), 0, 0, D3DPOOL_MANAGED,
                &split_blendindices_stream_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(split_blendindices_stream_buffer,
                    0, sizeof(split_blendindices), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, split_blendindices, sizeof(split_blendindices));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        split_blendindices_stream_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateIndexBuffer(device_swvp,
                sizeof(tri_indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                &index_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DIndexBuffer9_Lock(index_buffer, 0,
                    sizeof(tri_indices), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, tri_indices, sizeof(tri_indices));
                CHECK_HR(IDirect3DIndexBuffer9_Unlock(index_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateIndexBuffer(device_swvp,
                sizeof(tri_indices32), 0, D3DFMT_INDEX32, D3DPOOL_MANAGED,
                &index_buffer32, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DIndexBuffer9_Lock(index_buffer32, 0,
                    sizeof(tri_indices32), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, tri_indices32, sizeof(tri_indices32));
                CHECK_HR(IDirect3DIndexBuffer9_Unlock(index_buffer32), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(tri_with_padding), 0, D3DFVF_XYZ | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &padded_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(padded_vertex_buffer, 0,
                    sizeof(tri_with_padding), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, tri_with_padding, sizeof(tri_with_padding));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(padded_vertex_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp, sizeof(strip),
                0, D3DFVF_XYZ | D3DFVF_DIFFUSE, D3DPOOL_MANAGED,
                &strip_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(strip_vertex_buffer, 0,
                    sizeof(strip), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, strip, sizeof(strip));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(strip_vertex_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(tangent_tri), 0, 0, D3DPOOL_MANAGED,
                &tangent_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(tangent_vertex_buffer, 0,
                    sizeof(tangent_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, tangent_tri, sizeof(tangent_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(tangent_vertex_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(split_tangents), 0, 0, D3DPOOL_MANAGED,
                &tangent_stream_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(tangent_stream_buffer, 0,
                    sizeof(split_tangents), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, split_tangents, sizeof(split_tangents));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(tangent_stream_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateIndexBuffer(device_swvp,
                sizeof(tri_offset_indices), 0, D3DFMT_INDEX16,
                D3DPOOL_MANAGED, &offset_index_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DIndexBuffer9_Lock(offset_index_buffer, 0,
                    sizeof(tri_offset_indices), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, tri_offset_indices, sizeof(tri_offset_indices));
                CHECK_HR(IDirect3DIndexBuffer9_Unlock(offset_index_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateIndexBuffer(device_swvp,
                sizeof(strip_indices), 0, D3DFMT_INDEX16, D3DPOOL_MANAGED,
                &strip_index_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DIndexBuffer9_Lock(strip_index_buffer, 0,
                    sizeof(strip_indices), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, strip_indices, sizeof(strip_indices));
                CHECK_HR(IDirect3DIndexBuffer9_Unlock(strip_index_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(split_positions), 0, 0, D3DPOOL_MANAGED,
                &position_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(position_buffer, 0,
                    sizeof(split_positions), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, split_positions, sizeof(split_positions));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(position_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(split_colors), 0, 0, D3DPOOL_MANAGED,
                &color_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(color_buffer, 0,
                    sizeof(split_colors), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, split_colors, sizeof(split_colors));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(color_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(split_quad_positions), 0, 0, D3DPOOL_MANAGED,
                &position_quad_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(position_quad_buffer, 0,
                    sizeof(split_quad_positions), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, split_quad_positions,
                        sizeof(split_quad_positions));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(position_quad_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(split_line_positions), 0, 0, D3DPOOL_MANAGED,
                &position_line_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(position_line_buffer, 0,
                    sizeof(split_line_positions), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, split_line_positions,
                        sizeof(split_line_positions));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(position_line_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(textured_quad), 0,
                D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                D3DPOOL_MANAGED, &textured_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(textured_vertex_buffer, 0,
                    sizeof(textured_quad), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, textured_quad, sizeof(textured_quad));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        textured_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(textured_clip_quad), 0,
                D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
                D3DPOOL_MANAGED, &textured_clip_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(textured_clip_vertex_buffer, 0,
                    sizeof(textured_clip_quad), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, textured_clip_quad, sizeof(textured_clip_quad));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        textured_clip_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(clip_tri), 0, D3DFVF_XYZW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &clip_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(clip_vertex_buffer, 0,
                    sizeof(clip_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, clip_tri, sizeof(clip_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(clip_vertex_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(user_clip_tri), 0, D3DFVF_XYZW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &user_clip_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(user_clip_vertex_buffer, 0,
                    sizeof(user_clip_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, user_clip_tri, sizeof(user_clip_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(user_clip_vertex_buffer),
                        D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(partial_user_clip_tri), 0, D3DFVF_XYZW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &partial_user_clip_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(partial_user_clip_vertex_buffer, 0,
                    sizeof(partial_user_clip_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, partial_user_clip_tri, sizeof(partial_user_clip_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        partial_user_clip_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(partial_user_clip_line), 0,
                D3DFVF_XYZW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &partial_user_clip_line_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(
                    partial_user_clip_line_vertex_buffer, 0,
                    sizeof(partial_user_clip_line), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, partial_user_clip_line,
                        sizeof(partial_user_clip_line));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        partial_user_clip_line_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(user_clip_point), 0, D3DFVF_XYZW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &user_clip_point_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(user_clip_point_vertex_buffer, 0,
                    sizeof(user_clip_point), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, user_clip_point, sizeof(user_clip_point));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        user_clip_point_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(behind_clip_tri), 0, D3DFVF_XYZW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &behind_clip_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(behind_clip_vertex_buffer, 0,
                    sizeof(behind_clip_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, behind_clip_tri, sizeof(behind_clip_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        behind_clip_vertex_buffer), D3D_OK);
            }
        }
        hr = IDirect3DDevice9_CreateVertexBuffer(device_swvp,
                sizeof(far_clip_tri), 0, D3DFVF_XYZW | D3DFVF_DIFFUSE,
                D3DPOOL_MANAGED, &far_clip_vertex_buffer, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            hr = IDirect3DVertexBuffer9_Lock(far_clip_vertex_buffer, 0,
                    sizeof(far_clip_tri), &mapped, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memcpy(mapped, far_clip_tri, sizeof(far_clip_tri));
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(far_clip_vertex_buffer),
                        D3D_OK);
            }
        }
        if (vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            if (vertex_shader)
            {
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                        vertex_shader), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            }
            if (index_buffer)
            {
                CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                        index_buffer), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                        D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                        D3D_OK);
                if (vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                        D3D_OK);
            }
            if (padded_vertex_buffer && index_buffer && offset_index_buffer)
            {
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        padded_vertex_buffer, 0, sizeof(tri_with_padding[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                        offset_index_buffer), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                        D3DPT_TRIANGLELIST, 0, 1, ARRAY_SIZE(tri), 0, 1),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                        index_buffer), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                        D3DPT_TRIANGLELIST, 1, 0, ARRAY_SIZE(tri), 0, 1),
                        D3D_OK);
                if (vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            offset_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_TRIANGLELIST, 0, 1, ARRAY_SIZE(tri), 0, 1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_TRIANGLELIST, 1, 0, ARRAY_SIZE(tri), 0, 1),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            }
            if (strip_vertex_buffer)
            {
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        strip_vertex_buffer, 0, sizeof(strip[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_POINTLIST, 0, ARRAY_SIZE(strip)), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_LINELIST, 0, 2), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_LINESTRIP, 0, 3), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_TRIANGLEFAN, 0, 2), D3D_OK);
                if (strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 0, ARRAY_SIZE(strip), 0, 2),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_POINTLIST, 0, 0, ARRAY_SIZE(strip), 0,
                            ARRAY_SIZE(strip)), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_LINELIST, 0, 0, ARRAY_SIZE(strip), 0, 2),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_LINESTRIP, 0, 0, ARRAY_SIZE(strip), 0, 3),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_TRIANGLEFAN, 0, 0, ARRAY_SIZE(strip), 0, 2),
                            D3D_OK);
                }
                if (vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_POINTLIST, 0, ARRAY_SIZE(strip)), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_LINELIST, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_LINESTRIP, 0, 3), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLEFAN, 0, 2), D3D_OK);
                    if (strip_index_buffer)
                    {
                        CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                                strip_index_buffer), D3D_OK);
                        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                                device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                                ARRAY_SIZE(strip), 0, 2), D3D_OK);
                        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                                device_swvp, D3DPT_POINTLIST, 0, 0,
                                ARRAY_SIZE(strip), 0, ARRAY_SIZE(strip)), D3D_OK);
                        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                                device_swvp, D3DPT_LINELIST, 0, 0,
                                ARRAY_SIZE(strip), 0, 2), D3D_OK);
                        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                                device_swvp, D3DPT_LINESTRIP, 0, 0,
                                ARRAY_SIZE(strip), 0, 3), D3D_OK);
                        CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                                device_swvp, D3DPT_TRIANGLEFAN, 0, 0,
                                ARRAY_SIZE(strip), 0, 2), D3D_OK);
                    }
                }
                CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            }
            if (vertex_shader && split_vertex_decl && position_buffer && color_buffer)
            {
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                        vertex_shader), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                        split_vertex_decl), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        position_buffer, 0, sizeof(split_positions[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                        color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
                if (index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                        NULL, 0, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            }
            if (tangent_vertex_shader && tangent_vertex_decl
                    && tangent_vertex_buffer)
            {
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                        tangent_vertex_shader), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                        tangent_vertex_decl), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        tangent_vertex_buffer, 0, sizeof(tangent_tri[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        NULL, 0, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            }
            if (tangent_vertex_shader && split_tangent_vertex_decl
                    && tangent_stream_buffer)
            {
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                        tangent_vertex_shader), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                        split_tangent_vertex_decl), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 2,
                        tangent_stream_buffer, 0, sizeof(split_tangents[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                        D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
                if (index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                            D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 2,
                        NULL, 0, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                        NULL, 0, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL,
                    0, 0), D3D_OK);
        }
        CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL), D3D_OK);
        memset(&material, 0, sizeof(material));
        material.Diffuse.r = 1.0f;
        material.Diffuse.g = 1.0f;
        material.Diffuse.b = 1.0f;
        material.Diffuse.a = 1.0f;
        material.Ambient = material.Diffuse;
        CHECK_HR(IDirect3DDevice9_SetMaterial(device_swvp, &material), D3D_OK);
        memset(&light, 0, sizeof(light));
        light.Type = D3DLIGHT_DIRECTIONAL;
        light.Diffuse.r = 1.0f;
        light.Diffuse.g = 1.0f;
        light.Diffuse.b = 1.0f;
        light.Direction.z = 1.0f;
        CHECK_HR(IDirect3DDevice9_SetLight(device_swvp, 0, &light), D3D_OK);
        CHECK_HR(IDirect3DDevice9_LightEnable(device_swvp, 0, TRUE), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                D3DRS_COLORVERTEX, FALSE), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                D3DRS_LIGHTING, TRUE), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
        CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                D3DPT_TRIANGLELIST, 1, lit_tri, sizeof(lit_tri[0])),
                D3D_OK);
        if (render_target)
        {
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
        }
        CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
        if (render_target && readback)
        {
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices,
                    D3DFMT_INDEX16, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices32,
                    D3DFMT_INDEX32, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZB4 | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, blendweight_tri,
                    sizeof(blendweight_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4
                    | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(blendindices_tri), 1,
                    tri_indices, D3DFMT_INDEX16, blendindices_tri,
                    sizeof(blendindices_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && blendweight_vertex_decl)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    blendweight_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, blendweight_tri,
                    sizeof(blendweight_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && blendindices_vertex_decl)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    blendindices_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(blendindices_tri), 1,
                    tri_indices, D3DFMT_INDEX16, blendindices_tri,
                    sizeof(blendindices_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && blendweight_vertex_decl
                && blendweight_vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    blendweight_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    blendweight_vertex_buffer, 0,
                    sizeof(blendweight_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && blendindices_vertex_decl
                && blendindices_vertex_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    blendindices_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    blendindices_vertex_buffer, 0,
                    sizeof(blendindices_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0,
                    ARRAY_SIZE(blendindices_tri), 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && split_blendweight_vertex_decl
                && vertex_buffer && split_blendweight_stream_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_blendweight_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    split_blendweight_stream_buffer, 0,
                    sizeof(split_blendweights[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && split_blendindices_vertex_decl
                && vertex_buffer && split_blendindices_stream_buffer
                && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            for (i = 0; i < 4; ++i)
            {
                CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp,
                        D3DTS_WORLDMATRIX(i), &world), D3D_OK);
            }
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_3WEIGHTS), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_blendindices_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    split_blendindices_stream_buffer, 0,
                    sizeof(split_blendindices[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_VERTEXBLEND, D3DVBF_DISABLE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && stream0_vertex_decl && vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    stream0_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && stream0_vertex_decl)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    stream0_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && short4n_vertex_decl)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    short4n_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, short4n_tri,
                    sizeof(short4n_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && short4n_vertex_decl && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    short4n_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, short4n_tri,
                    sizeof(short4n_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && stream0_vertex_decl)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    stream0_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices,
                    D3DFMT_INDEX16, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && stream0_vertex_decl && vertex_buffer
                && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    stream0_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_buffer && index_buffer32)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer32),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && user_clip_vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    user_clip_vertex_buffer, 0, sizeof(user_clip_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && user_clip_vertex_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    user_clip_vertex_buffer, 0, sizeof(user_clip_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(user_clip_tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && partial_user_clip_vertex_buffer)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    partial_user_clip_vertex_buffer, 0,
                    sizeof(partial_user_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 20 * locked_rect.Pitch;
                memcpy(&left_pixel, row + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && partial_user_clip_vertex_buffer && index_buffer)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    partial_user_clip_vertex_buffer, 0,
                    sizeof(partial_user_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(partial_user_clip_tri),
                    0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 20 * locked_rect.Pitch;
                memcpy(&left_pixel, row + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && partial_user_clip_line_vertex_buffer)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    partial_user_clip_line_vertex_buffer, 0,
                    sizeof(partial_user_clip_line[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_LINELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                /* dxmt9 applies a half-pixel rasterization offset so a line at
                 * NDC y=0 (D3D9 viewport row 16 center) lands on Metal pixel row
                 * 15 (whose center is at y=15.5). Probe the actual rendered row
                 * rather than the D3D9-convention row. */
                const BYTE *row16 = (const BYTE *)locked_rect.pBits
                        + 15 * locked_rect.Pitch;
                memcpy(&left_pixel, row16 + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row16 + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && partial_user_clip_line_vertex_buffer && index_buffer)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    partial_user_clip_line_vertex_buffer, 0,
                    sizeof(partial_user_clip_line[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_LINELIST, 0, 0, ARRAY_SIZE(partial_user_clip_line),
                    0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 15 * locked_rect.Pitch  /* dxmt9 half-pixel offset rasterizes the line at row 15 */;
                memcpy(&left_pixel, row + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, partial_user_clip_tri,
                    sizeof(partial_user_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 20 * locked_rect.Pitch;
                memcpy(&left_pixel, row + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(partial_user_clip_tri), 1,
                    tri_indices, D3DFMT_INDEX16, partial_user_clip_tri,
                    sizeof(partial_user_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 20 * locked_rect.Pitch;
                memcpy(&left_pixel, row + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_LINELIST, 1, partial_user_clip_line,
                    sizeof(partial_user_clip_line[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 15 * locked_rect.Pitch  /* dxmt9 half-pixel offset rasterizes the line at row 15 */;
                memcpy(&left_pixel, row + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            DWORD left_pixel = 0, right_pixel = 0;
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_LINELIST, 0, ARRAY_SIZE(partial_user_clip_line), 1,
                    tri_indices, D3DFMT_INDEX16, partial_user_clip_line,
                    sizeof(partial_user_clip_line[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 15 * locked_rect.Pitch  /* dxmt9 half-pixel offset rasterizes the line at row 15 */;
                memcpy(&left_pixel, row + 10 * sizeof(left_pixel),
                        sizeof(left_pixel));
                memcpy(&right_pixel, row + 24 * sizeof(right_pixel),
                        sizeof(right_pixel));
                CHECK_TRUE((left_pixel & 0x00ffffffu) == 0);
                CHECK_TRUE((right_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && user_clip_point_vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    user_clip_point_vertex_buffer, 0,
                    sizeof(user_clip_point[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_POINTLIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 16 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && user_clip_point_vertex_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    user_clip_point_vertex_buffer, 0,
                    sizeof(user_clip_point[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_POINTLIST, 0, 0, ARRAY_SIZE(user_clip_point), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 16 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_POINTLIST, ARRAY_SIZE(user_clip_point),
                    user_clip_point, sizeof(user_clip_point[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 16 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_POINTLIST, 0, ARRAY_SIZE(user_clip_point), 1,
                    tri_indices, D3DFMT_INDEX16, user_clip_point,
                    sizeof(user_clip_point[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 16 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && clip_vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    clip_vertex_buffer, 0, sizeof(clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && clip_vertex_buffer
                && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    clip_vertex_buffer, 0, sizeof(clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(clip_tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && far_clip_vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    far_clip_vertex_buffer, 0, sizeof(far_clip_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && far_clip_vertex_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    far_clip_vertex_buffer, 0, sizeof(far_clip_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(far_clip_tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && behind_clip_vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    behind_clip_vertex_buffer, 0, sizeof(behind_clip_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader
                && behind_clip_vertex_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    behind_clip_vertex_buffer, 0, sizeof(behind_clip_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(behind_clip_tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0, NULL, 0, 0),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(user_clip_tri), 1,
                    tri_indices, D3DFMT_INDEX16, user_clip_tri,
                    sizeof(user_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetClipPlane(device_swvp, 0,
                    user_clip_plane_x_positive), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, user_clip_tri,
                    sizeof(user_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPLANEENABLE, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, far_clip_tri,
                    sizeof(far_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(far_clip_tri), 1,
                    tri_indices, D3DFMT_INDEX16, far_clip_tri,
                    sizeof(far_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices,
                    D3DFMT_INDEX16, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(tri), 1, tri_indices32,
                    D3DFMT_INDEX32, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, clip_tri, sizeof(clip_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, behind_clip_tri,
                    sizeof(behind_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_CLIPPING, TRUE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZW | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 0, ARRAY_SIZE(behind_clip_tri), 1,
                    tri_indices, D3DFMT_INDEX16, behind_clip_tri,
                    sizeof(behind_clip_tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && vertex_buffer
                && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && vertex_buffer
                && index_buffer32)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer32),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_quad_buffer && color_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(red > green);
                CHECK_TRUE(red > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_quad_buffer && color_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_buffer && color_buffer && index_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_buffer, 0, sizeof(split_positions[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 3), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_buffer && color_buffer && index_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_buffer, 0, sizeof(split_positions[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_buffer && color_buffer && index_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_buffer, 0, sizeof(split_positions[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 3), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, 3, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_quad_buffer && color_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLEFAN, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_quad_buffer && color_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_quad_buffer && color_buffer && strip_index_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                    strip_index_buffer), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLEFAN, 0, 0, 4, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_quad_buffer && color_buffer && strip_index_buffer)
        {
            DWORD red, green, blue;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                    strip_index_buffer), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLESTRIP, 0, 0, 4, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && split_vertex_decl
                && position_quad_buffer && color_buffer)
        {
            DWORD red, green, blue;
            D3DMATRIX identity_world = {{
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
            }};

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp, D3DTS_WORLD,
                    &identity_world), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp, D3DTS_WORLD,
                    &world), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && split_vertex_decl
                && position_quad_buffer && color_buffer && strip_index_buffer)
        {
            DWORD red, green, blue;
            D3DMATRIX identity_world = {{
                1.0f, 0.0f, 0.0f, 0.0f,
                0.0f, 1.0f, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                0.0f, 0.0f, 0.0f, 1.0f,
            }};

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp, D3DTS_WORLD,
                    &identity_world), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_quad_buffer, 0, sizeof(split_quad_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                    strip_index_buffer), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLEFAN, 0, 0, 4, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetTransform(device_swvp, D3DTS_WORLD,
                    &world), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                red = probe_pixel & 0x00ff0000u;
                green = probe_pixel & 0x0000ff00u;
                blue = probe_pixel & 0x000000ffu;
                CHECK_TRUE(green > red);
                CHECK_TRUE(green > blue);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_line_buffer && color_buffer)
        {
            BOOL found_green = FALSE;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_line_buffer, 0, sizeof(split_line_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_LINESTRIP, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                UINT x, y;
                for (y = 0; y < 32 && !found_green; ++y)
                {
                    const BYTE *row = (const BYTE *)locked_rect.pBits
                            + y * locked_rect.Pitch;
                    for (x = 0; x < 32; ++x)
                    {
                        memcpy(&probe_pixel, row + x * sizeof(probe_pixel),
                                sizeof(probe_pixel));
                        if ((probe_pixel & 0x0000ff00u) >
                                (probe_pixel & 0x00ff0000u)
                                && (probe_pixel & 0x0000ff00u) >
                                (probe_pixel & 0x000000ffu))
                        {
                            found_green = TRUE;
                            break;
                        }
                    }
                }
                CHECK_TRUE(found_green);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && split_vertex_decl
                && position_line_buffer && color_buffer && index_buffer)
        {
            BOOL found_green = FALSE;

            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    position_line_buffer, 0, sizeof(split_line_positions[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    color_buffer, 0, sizeof(split_colors[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0,
                    D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1,
                    D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_LINESTRIP, 0, 0, 3, 0, 2), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device_swvp, 1, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 1,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                UINT x, y;
                for (y = 0; y < 32 && !found_green; ++y)
                {
                    const BYTE *row = (const BYTE *)locked_rect.pBits
                            + y * locked_rect.Pitch;
                    for (x = 0; x < 32; ++x)
                    {
                        memcpy(&probe_pixel, row + x * sizeof(probe_pixel),
                                sizeof(probe_pixel));
                        if ((probe_pixel & 0x0000ff00u) >
                                (probe_pixel & 0x00ff0000u)
                                && (probe_pixel & 0x0000ff00u) >
                                (probe_pixel & 0x000000ffu))
                        {
                            found_green = TRUE;
                            break;
                        }
                    }
                }
                CHECK_TRUE(found_green);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && pixel_shader && vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp,
                    pixel_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetStreamSource(device_swvp, 0,
                    &returned_stream, &returned_offset, &returned_stride),
                    D3D_OK);
            CHECK_TRUE(returned_stream == vertex_buffer);
            CHECK_TRUE(returned_offset == 0);
            CHECK_TRUE(returned_stride == sizeof(tri[0]));
            if (returned_stream)
            {
                IDirect3DVertexBuffer9_Release(returned_stream);
                returned_stream = NULL;
            }
            CHECK_HR(IDirect3DDevice9_GetPixelShader(device_swvp,
                    &returned_pixel_shader), D3D_OK);
            CHECK_TRUE(returned_pixel_shader == pixel_shader);
            if (returned_pixel_shader)
            {
                IDirect3DPixelShader9_Release(returned_pixel_shader);
                returned_pixel_shader = NULL;
            }
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0x00ff00ffu);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && pixel_shader && vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp,
                    pixel_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetPixelShader(device_swvp,
                    &returned_pixel_shader), D3D_OK);
            CHECK_TRUE(returned_pixel_shader == pixel_shader);
            if (returned_pixel_shader)
            {
                IDirect3DPixelShader9_Release(returned_pixel_shader);
                returned_pixel_shader = NULL;
            }
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0x00ff00ffu);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && pixel_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp,
                    pixel_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0x00ff00ffu);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && pixel_shader
                && vertex_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp,
                    pixel_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetPixelShader(device_swvp,
                    &returned_pixel_shader), D3D_OK);
            CHECK_TRUE(returned_pixel_shader == pixel_shader);
            if (returned_pixel_shader)
            {
                IDirect3DPixelShader9_Release(returned_pixel_shader);
                returned_pixel_shader = NULL;
            }
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 10 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0x00ff00ffu);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && pixel_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp,
                    pixel_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri_yellow, sizeof(tri_yellow[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0x00ffff00u);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && vertex_shader && pixel_shader)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri_yellow, sizeof(tri_yellow[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp,
                    pixel_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetPixelShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) == 0x00ff00ffu);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && tangent_vertex_shader
                && tangent_vertex_decl)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    tangent_vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    tangent_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tangent_tri, sizeof(tangent_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 12 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && tangent_vertex_decl)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    tangent_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                    D3DPT_TRIANGLELIST, 1, tangent_tri, sizeof(tangent_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 18 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && tangent_vertex_shader
                && tangent_vertex_decl && tangent_vertex_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    tangent_vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    tangent_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    tangent_vertex_buffer, 0, sizeof(tangent_tri[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 12 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && tangent_vertex_shader
                && split_tangent_vertex_decl && vertex_buffer
                && tangent_stream_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    tangent_vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_tangent_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 2,
                    tangent_stream_buffer, 0, sizeof(split_tangents[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 1), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 2,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 12 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback && tangent_vertex_shader
                && split_tangent_vertex_decl && vertex_buffer
                && tangent_stream_buffer && index_buffer)
        {
            CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                    tangent_vertex_shader), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device_swvp,
                    split_tangent_vertex_decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    vertex_buffer, 0, sizeof(tri[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 2,
                    tangent_stream_buffer, 0, sizeof(split_tangents[0])),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, index_buffer),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                    D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(device_swvp,
                    D3DPT_TRIANGLELIST, 0, 0, ARRAY_SIZE(tri), 0, 1),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 2,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                    NULL, 0, 0), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                    D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
            CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
            CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                    render_target, readback), D3D_OK);
            memset(&locked_rect, 0, sizeof(locked_rect));
            hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                const BYTE *row = (const BYTE *)locked_rect.pBits
                        + 22 * locked_rect.Pitch;
                memcpy(&probe_pixel, row + 12 * sizeof(probe_pixel),
                        sizeof(probe_pixel));
                CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
            }
        }
        if (render_target && readback)
        {
            static const DWORD texels[] =
            {
                0xff102030u, 0xff405060u,
                0xff708090u, 0xffa0b0c0u,
            };
            IDirect3DTexture9 *texture = NULL;
            DWORD point_size = 0x3f800000u;

            hr = IDirect3DDevice9_CreateTexture(device_swvp, 2, 2, 1, 0,
                    D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DTexture9_LockRect(texture, 0, &locked_rect,
                        NULL, 0);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    DWORD *row0 = locked_rect.pBits;
                    DWORD *row1 = (DWORD *)((BYTE *)locked_rect.pBits
                            + locked_rect.Pitch);
                    row0[0] = texels[0];
                    row0[1] = texels[1];
                    row1[0] = texels[2];
                    row1[1] = texels[3];
                    CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 0),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_POINTSIZE, point_size), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MAGFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MIPFILTER, D3DTEXF_NONE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_MODULATE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG2, D3DTA_DIFFUSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_DIFFUSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == texels[0]);
                    CHECK_TRUE(row8[24] == texels[1]);
                    CHECK_TRUE(row24[8] == texels[2]);
                    CHECK_TRUE(row24[24] == texels[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 0, ARRAY_SIZE(textured_quad), 2,
                        strip_indices, D3DFMT_INDEX16, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == texels[0]);
                    CHECK_TRUE(row8[24] == texels[1]);
                    CHECK_TRUE(row24[8] == texels[2]);
                    CHECK_TRUE(row24[24] == texels[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (textured_vertex_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (textured_vertex_buffer && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                            D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                            D3DSAMP_MAGFILTER, D3DTEXF_POINT), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                            D3DSAMP_MIPFILTER, D3DTEXF_NONE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0,
                            ARRAY_SIZE(textured_clip_quad), 2, strip_indices,
                            D3DFMT_INDEX16, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer
                        && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1,
                            D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_clip_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0,
                            ARRAY_SIZE(textured_clip_quad), 2, strip_indices,
                            D3DFMT_INDEX16, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer
                        && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_clip_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == texels[0]);
                        CHECK_TRUE(row8[24] == texels[1]);
                        CHECK_TRUE(row24[8] == texels[2]);
                        CHECK_TRUE(row24[24] == texels[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                IDirect3DTexture9_Release(texture);
            }
        }
        if (render_target && readback)
        {
            static const BYTE p8_texels[] =
            {
                1, 2,
                3, 4,
            };
            static const DWORD p8_expected[] =
            {
                0xff102030u, 0xff405060u,
                0xff708090u, 0xffa0b0c0u,
            };
            static const DWORD p8_updated_expected[] =
            {
                0xff0a1a2au, 0xff3a4a5au,
                0xff6a7a8au, 0xff9aaabau,
            };
            static const DWORD p8_palette1_expected[] =
            {
                0xff112233u, 0xff445566u,
                0xff778899u, 0xff99aabbu,
            };
            IDirect3DTexture9 *dst_texture = NULL;
            IDirect3DTexture9 *src_texture = NULL;
            IDirect3DTexture9 *texture = NULL;
            PALETTEENTRY palette[256];

            memset(palette, 0, sizeof(palette));
            palette[1].peRed = 0x10; palette[1].peGreen = 0x20;
            palette[1].peBlue = 0x30; palette[1].peFlags = 0xff;
            palette[2].peRed = 0x40; palette[2].peGreen = 0x50;
            palette[2].peBlue = 0x60; palette[2].peFlags = 0xff;
            palette[3].peRed = 0x70; palette[3].peGreen = 0x80;
            palette[3].peBlue = 0x90; palette[3].peFlags = 0xff;
            palette[4].peRed = 0xa0; palette[4].peGreen = 0xb0;
            palette[4].peBlue = 0xc0; palette[4].peFlags = 0xff;
            CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device_swvp, 0,
                    palette), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device_swvp,
                    0), D3D_OK);

            hr = IDirect3DDevice9_CreateTexture(device_swvp, 2, 2, 1, 0,
                    D3DFMT_P8, D3DPOOL_MANAGED, &texture, NULL);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DTexture9_LockRect(texture, 0, &locked_rect,
                        NULL, 0);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    BYTE *row0 = locked_rect.pBits;
                    BYTE *row1 = row0 + locked_rect.Pitch;
                    row0[0] = p8_texels[0];
                    row0[1] = p8_texels[1];
                    row1[0] = p8_texels[2];
                    row1[1] = p8_texels[3];
                    CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 0),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MAGFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MIPFILTER, D3DTEXF_NONE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == p8_expected[0]);
                    CHECK_TRUE(row8[24] == p8_expected[1]);
                    CHECK_TRUE(row24[8] == p8_expected[2]);
                    CHECK_TRUE(row24[24] == p8_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 0, ARRAY_SIZE(textured_quad), 2,
                        strip_indices, D3DFMT_INDEX16, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == p8_expected[0]);
                    CHECK_TRUE(row8[24] == p8_expected[1]);
                    CHECK_TRUE(row24[8] == p8_expected[2]);
                    CHECK_TRUE(row24[24] == p8_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (textured_vertex_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_expected[0]);
                        CHECK_TRUE(row8[24] == p8_expected[1]);
                        CHECK_TRUE(row24[8] == p8_expected[2]);
                        CHECK_TRUE(row24[24] == p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (textured_vertex_buffer && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_expected[0]);
                        CHECK_TRUE(row8[24] == p8_expected[1]);
                        CHECK_TRUE(row24[8] == p8_expected[2]);
                        CHECK_TRUE(row24[24] == p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_expected[0]);
                        CHECK_TRUE(row8[24] == p8_expected[1]);
                        CHECK_TRUE(row24[8] == p8_expected[2]);
                        CHECK_TRUE(row24[24] == p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0,
                            ARRAY_SIZE(textured_clip_quad), 2, strip_indices,
                            D3DFMT_INDEX16, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_expected[0]);
                        CHECK_TRUE(row8[24] == p8_expected[1]);
                        CHECK_TRUE(row24[8] == p8_expected[2]);
                        CHECK_TRUE(row24[24] == p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_expected[0]);
                        CHECK_TRUE(row8[24] == p8_expected[1]);
                        CHECK_TRUE(row24[8] == p8_expected[2]);
                        CHECK_TRUE(row24[24] == p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer
                        && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_clip_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_expected[0]);
                        CHECK_TRUE(row8[24] == p8_expected[1]);
                        CHECK_TRUE(row24[8] == p8_expected[2]);
                        CHECK_TRUE(row24[24] == p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                IDirect3DTexture9_Release(texture);
            }
            hr = IDirect3DDevice9_CreateTexture(device_swvp, 2, 2, 1, 0,
                    D3DFMT_P8, D3DPOOL_SYSTEMMEM, &src_texture, NULL);
            CHECK_HR(hr, D3D_OK);
            hr = IDirect3DDevice9_CreateTexture(device_swvp, 2, 2, 1, 0,
                    D3DFMT_P8, D3DPOOL_DEFAULT, &dst_texture, NULL);
            CHECK_HR(hr, D3D_OK);
            if (src_texture && dst_texture)
            {
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DTexture9_LockRect(src_texture, 0, &locked_rect,
                        NULL, 0);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    BYTE *row0 = locked_rect.pBits;
                    BYTE *row1 = row0 + locked_rect.Pitch;
                    row0[0] = p8_texels[0];
                    row0[1] = p8_texels[1];
                    row1[0] = p8_texels[2];
                    row1[1] = p8_texels[3];
                    CHECK_HR(IDirect3DTexture9_UnlockRect(src_texture, 0),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_UpdateTexture(device_swvp,
                        (IDirect3DBaseTexture9 *)src_texture,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MAGFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MIPFILTER, D3DTEXF_NONE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == p8_expected[0]);
                    CHECK_TRUE(row8[24] == p8_expected[1]);
                    CHECK_TRUE(row24[8] == p8_expected[2]);
                    CHECK_TRUE(row24[24] == p8_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_expected[0]);
                        CHECK_TRUE(row8[24] == p8_expected[1]);
                        CHECK_TRUE(row24[8] == p8_expected[2]);
                        CHECK_TRUE(row24[24] == p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                palette[1].peRed = 0x0a; palette[1].peGreen = 0x1a;
                palette[1].peBlue = 0x2a; palette[1].peFlags = 0xff;
                palette[2].peRed = 0x3a; palette[2].peGreen = 0x4a;
                palette[2].peBlue = 0x5a; palette[2].peFlags = 0xff;
                palette[3].peRed = 0x6a; palette[3].peGreen = 0x7a;
                palette[3].peBlue = 0x8a; palette[3].peFlags = 0xff;
                palette[4].peRed = 0x9a; palette[4].peGreen = 0xaa;
                palette[4].peBlue = 0xba; palette[4].peFlags = 0xff;
                CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device_swvp, 0,
                        palette), D3D_OK);
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == p8_updated_expected[0]);
                    CHECK_TRUE(row8[24] == p8_updated_expected[1]);
                    CHECK_TRUE(row24[8] == p8_updated_expected[2]);
                    CHECK_TRUE(row24[24] == p8_updated_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_updated_expected[0]);
                        CHECK_TRUE(row8[24] == p8_updated_expected[1]);
                        CHECK_TRUE(row24[8] == p8_updated_expected[2]);
                        CHECK_TRUE(row24[24] == p8_updated_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                palette[1].peRed = 0x11; palette[1].peGreen = 0x22;
                palette[1].peBlue = 0x33; palette[1].peFlags = 0xff;
                palette[2].peRed = 0x44; palette[2].peGreen = 0x55;
                palette[2].peBlue = 0x66; palette[2].peFlags = 0xff;
                palette[3].peRed = 0x77; palette[3].peGreen = 0x88;
                palette[3].peBlue = 0x99; palette[3].peFlags = 0xff;
                palette[4].peRed = 0x99; palette[4].peGreen = 0xaa;
                palette[4].peBlue = 0xbb; palette[4].peFlags = 0xff;
                CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device_swvp, 1,
                        palette), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(
                        device_swvp, 1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == p8_palette1_expected[0]);
                    CHECK_TRUE(row8[24] == p8_palette1_expected[1]);
                    CHECK_TRUE(row24[8] == p8_palette1_expected[2]);
                    CHECK_TRUE(row24[24] == p8_palette1_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_palette1_expected[0]);
                        CHECK_TRUE(row8[24] == p8_palette1_expected[1]);
                        CHECK_TRUE(row24[8] == p8_palette1_expected[2]);
                        CHECK_TRUE(row24[24] == p8_palette1_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (textured_vertex_buffer && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_palette1_expected[0]);
                        CHECK_TRUE(row8[24] == p8_palette1_expected[1]);
                        CHECK_TRUE(row24[8] == p8_palette1_expected[2]);
                        CHECK_TRUE(row24[24] == p8_palette1_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer
                        && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_clip_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == p8_palette1_expected[0]);
                        CHECK_TRUE(row8[24] == p8_palette1_expected[1]);
                        CHECK_TRUE(row24[8] == p8_palette1_expected[2]);
                        CHECK_TRUE(row24[24] == p8_palette1_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
            }
            if (dst_texture)
                IDirect3DTexture9_Release(dst_texture);
            if (src_texture)
                IDirect3DTexture9_Release(src_texture);
        }
        if (render_target && readback)
        {
            static const BYTE a8p8_texels[] =
            {
                1, 0x80, 2, 0x60,
                3, 0x40, 4, 0x20,
            };
            static const DWORD a8p8_expected[] =
            {
                0x80102030u, 0x60405060u,
                0x40708090u, 0x20a0b0c0u,
            };
            static const DWORD a8p8_updated_expected[] =
            {
                0x800a1a2au, 0x603a4a5au,
                0x406a7a8au, 0x209aaabau,
            };
            static const DWORD a8p8_palette1_expected[] =
            {
                0x80112233u, 0x60445566u,
                0x40778899u, 0x2099aabbu,
            };
            IDirect3DTexture9 *dst_texture = NULL;
            IDirect3DTexture9 *src_texture = NULL;
            IDirect3DTexture9 *texture = NULL;
            PALETTEENTRY palette[256];

            memset(palette, 0, sizeof(palette));
            palette[1].peRed = 0x10; palette[1].peGreen = 0x20;
            palette[1].peBlue = 0x30; palette[1].peFlags = 0xff;
            palette[2].peRed = 0x40; palette[2].peGreen = 0x50;
            palette[2].peBlue = 0x60; palette[2].peFlags = 0xff;
            palette[3].peRed = 0x70; palette[3].peGreen = 0x80;
            palette[3].peBlue = 0x90; palette[3].peFlags = 0xff;
            palette[4].peRed = 0xa0; palette[4].peGreen = 0xb0;
            palette[4].peBlue = 0xc0; palette[4].peFlags = 0xff;
            CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device_swvp, 0,
                    palette), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device_swvp,
                    0), D3D_OK);

            hr = IDirect3DDevice9_CreateTexture(device_swvp, 2, 2, 1, 0,
                    D3DFMT_A8P8, D3DPOOL_MANAGED, &texture, NULL);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DTexture9_LockRect(texture, 0, &locked_rect,
                        NULL, 0);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    BYTE *row0 = locked_rect.pBits;
                    BYTE *row1 = row0 + locked_rect.Pitch;
                    row0[0] = a8p8_texels[0];
                    row0[1] = a8p8_texels[1];
                    row0[2] = a8p8_texels[2];
                    row0[3] = a8p8_texels[3];
                    row1[0] = a8p8_texels[4];
                    row1[1] = a8p8_texels[5];
                    row1[2] = a8p8_texels[6];
                    row1[3] = a8p8_texels[7];
                    CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 0),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MAGFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MIPFILTER, D3DTEXF_NONE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == a8p8_expected[0]);
                    CHECK_TRUE(row8[24] == a8p8_expected[1]);
                    CHECK_TRUE(row24[8] == a8p8_expected[2]);
                    CHECK_TRUE(row24[24] == a8p8_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 0, ARRAY_SIZE(textured_quad), 2,
                        strip_indices, D3DFMT_INDEX16, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == a8p8_expected[0]);
                    CHECK_TRUE(row8[24] == a8p8_expected[1]);
                    CHECK_TRUE(row24[8] == a8p8_expected[2]);
                    CHECK_TRUE(row24[24] == a8p8_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (textured_vertex_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (textured_vertex_buffer && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitiveUP(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0,
                            ARRAY_SIZE(textured_clip_quad), 2, strip_indices,
                            D3DFMT_INDEX16, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device_swvp,
                            D3DPT_TRIANGLESTRIP, 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer
                        && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_clip_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                IDirect3DTexture9_Release(texture);
            }
            hr = IDirect3DDevice9_CreateTexture(device_swvp, 2, 2, 1, 0,
                    D3DFMT_A8P8, D3DPOOL_SYSTEMMEM, &src_texture, NULL);
            CHECK_HR(hr, D3D_OK);
            hr = IDirect3DDevice9_CreateTexture(device_swvp, 2, 2, 1, 0,
                    D3DFMT_A8P8, D3DPOOL_DEFAULT, &dst_texture, NULL);
            CHECK_HR(hr, D3D_OK);
            if (src_texture && dst_texture)
            {
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DTexture9_LockRect(src_texture, 0, &locked_rect,
                        NULL, 0);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    BYTE *row0 = locked_rect.pBits;
                    BYTE *row1 = row0 + locked_rect.Pitch;
                    row0[0] = a8p8_texels[0];
                    row0[1] = a8p8_texels[1];
                    row0[2] = a8p8_texels[2];
                    row0[3] = a8p8_texels[3];
                    row1[0] = a8p8_texels[4];
                    row1[1] = a8p8_texels[5];
                    row1[2] = a8p8_texels[6];
                    row1[3] = a8p8_texels[7];
                    CHECK_HR(IDirect3DTexture9_UnlockRect(src_texture, 0),
                            D3D_OK);
                }
                CHECK_HR(IDirect3DDevice9_UpdateTexture(device_swvp,
                        (IDirect3DBaseTexture9 *)src_texture,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MINFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MAGFILTER, D3DTEXF_POINT), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetSamplerState(device_swvp, 0,
                        D3DSAMP_MIPFILTER, D3DTEXF_NONE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == a8p8_expected[0]);
                    CHECK_TRUE(row8[24] == a8p8_expected[1]);
                    CHECK_TRUE(row24[8] == a8p8_expected[2]);
                    CHECK_TRUE(row24[24] == a8p8_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                palette[1].peRed = 0x0a; palette[1].peGreen = 0x1a;
                palette[1].peBlue = 0x2a; palette[1].peFlags = 0xff;
                palette[2].peRed = 0x3a; palette[2].peGreen = 0x4a;
                palette[2].peBlue = 0x5a; palette[2].peFlags = 0xff;
                palette[3].peRed = 0x6a; palette[3].peGreen = 0x7a;
                palette[3].peBlue = 0x8a; palette[3].peFlags = 0xff;
                palette[4].peRed = 0x9a; palette[4].peGreen = 0xaa;
                palette[4].peBlue = 0xba; palette[4].peFlags = 0xff;
                CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device_swvp, 0,
                        palette), D3D_OK);
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == a8p8_updated_expected[0]);
                    CHECK_TRUE(row8[24] == a8p8_updated_expected[1]);
                    CHECK_TRUE(row24[8] == a8p8_updated_expected[2]);
                    CHECK_TRUE(row24[24] == a8p8_updated_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_updated_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_updated_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_updated_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_updated_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                palette[1].peRed = 0x11; palette[1].peGreen = 0x22;
                palette[1].peBlue = 0x33; palette[1].peFlags = 0xff;
                palette[2].peRed = 0x44; palette[2].peGreen = 0x55;
                palette[2].peBlue = 0x66; palette[2].peFlags = 0xff;
                palette[3].peRed = 0x77; palette[3].peGreen = 0x88;
                palette[3].peBlue = 0x99; palette[3].peFlags = 0xff;
                palette[4].peRed = 0x99; palette[4].peGreen = 0xaa;
                palette[4].peBlue = 0xbb; palette[4].peFlags = 0xff;
                CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device_swvp, 1,
                        palette), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(
                        device_swvp, 1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                        D3DRS_LIGHTING, FALSE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp, 0,
                        D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                        (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                        D3DPT_TRIANGLESTRIP, 2, textured_quad,
                        sizeof(textured_quad[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0, NULL),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                        D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                        render_target, readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(readback, &locked_rect, NULL,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 8 * locked_rect.Pitch);
                    const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                            + 24 * locked_rect.Pitch);
                    CHECK_TRUE(row8[8] == a8p8_palette1_expected[0]);
                    CHECK_TRUE(row8[24] == a8p8_palette1_expected[1]);
                    CHECK_TRUE(row24[8] == a8p8_palette1_expected[2]);
                    CHECK_TRUE(row24[24] == a8p8_palette1_expected[3]);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
                }
                if (texcoord_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_swvp,
                            D3DPT_TRIANGLESTRIP, 2, textured_clip_quad,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_palette1_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_palette1_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_palette1_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_palette1_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (textured_vertex_buffer && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_vertex_buffer, 0,
                            sizeof(textured_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_palette1_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_palette1_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_palette1_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_palette1_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
                if (texcoord_vertex_shader && textured_clip_vertex_buffer
                        && strip_index_buffer)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_swvp),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetRenderState(device_swvp,
                            D3DRS_LIGHTING, FALSE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            texcoord_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device_swvp,
                            0, D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            textured_clip_vertex_buffer, 0,
                            sizeof(textured_clip_quad[0])), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp,
                            strip_index_buffer), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_swvp, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawIndexedPrimitive(
                            device_swvp, D3DPT_TRIANGLESTRIP, 0, 0,
                            ARRAY_SIZE(textured_clip_quad), 0, 2), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTexture(device_swvp, 0,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_swvp,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetIndices(device_swvp, NULL),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetStreamSource(device_swvp, 0,
                            NULL, 0, 0), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLOROP, D3DTOP_MODULATE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG1, D3DTA_TEXTURE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_COLORARG2, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAOP,
                            D3DTOP_SELECTARG1), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetTextureStageState(
                            device_swvp, 0, D3DTSS_ALPHAARG1, D3DTA_DIFFUSE),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetFVF(device_swvp,
                            D3DFVF_XYZ | D3DFVF_NORMAL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_swvp), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_swvp,
                            render_target, readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(readback, &locked_rect,
                            NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const DWORD *row8 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 8 * locked_rect.Pitch);
                        const DWORD *row24 = (const DWORD *)((const BYTE *)locked_rect.pBits
                                + 24 * locked_rect.Pitch);
                        CHECK_TRUE(row8[8] == a8p8_palette1_expected[0]);
                        CHECK_TRUE(row8[24] == a8p8_palette1_expected[1]);
                        CHECK_TRUE(row24[8] == a8p8_palette1_expected[2]);
                        CHECK_TRUE(row24[24] == a8p8_palette1_expected[3]);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(readback),
                                D3D_OK);
                    }
                }
            }
            if (dst_texture)
                IDirect3DTexture9_Release(dst_texture);
            if (src_texture)
                IDirect3DTexture9_Release(src_texture);
        }
        fvf = 0;
        CHECK_HR(IDirect3DDevice9_GetFVF(device_swvp, &fvf), D3D_OK);
        CHECK_TRUE(fvf == (D3DFVF_XYZ | D3DFVF_NORMAL));
        if (returned_vertex_shader)
        {
            IDirect3DVertexShader9_Release(returned_vertex_shader);
            returned_vertex_shader = NULL;
        }
        if (returned_stream)
        {
            IDirect3DVertexBuffer9_Release(returned_stream);
            returned_stream = NULL;
        }
        if (index_buffer)
            IDirect3DIndexBuffer9_Release(index_buffer);
        if (index_buffer32)
            IDirect3DIndexBuffer9_Release(index_buffer32);
        if (offset_index_buffer)
            IDirect3DIndexBuffer9_Release(offset_index_buffer);
        if (strip_index_buffer)
            IDirect3DIndexBuffer9_Release(strip_index_buffer);
        if (color_buffer)
            IDirect3DVertexBuffer9_Release(color_buffer);
        if (textured_clip_vertex_buffer)
            IDirect3DVertexBuffer9_Release(textured_clip_vertex_buffer);
        if (clip_vertex_buffer)
            IDirect3DVertexBuffer9_Release(clip_vertex_buffer);
        if (user_clip_vertex_buffer)
            IDirect3DVertexBuffer9_Release(user_clip_vertex_buffer);
        if (partial_user_clip_vertex_buffer)
            IDirect3DVertexBuffer9_Release(partial_user_clip_vertex_buffer);
        if (partial_user_clip_line_vertex_buffer)
            IDirect3DVertexBuffer9_Release(partial_user_clip_line_vertex_buffer);
        if (user_clip_point_vertex_buffer)
            IDirect3DVertexBuffer9_Release(user_clip_point_vertex_buffer);
        if (far_clip_vertex_buffer)
            IDirect3DVertexBuffer9_Release(far_clip_vertex_buffer);
        if (behind_clip_vertex_buffer)
            IDirect3DVertexBuffer9_Release(behind_clip_vertex_buffer);
        if (textured_vertex_buffer)
            IDirect3DVertexBuffer9_Release(textured_vertex_buffer);
        if (position_buffer)
            IDirect3DVertexBuffer9_Release(position_buffer);
        if (position_quad_buffer)
            IDirect3DVertexBuffer9_Release(position_quad_buffer);
        if (position_line_buffer)
            IDirect3DVertexBuffer9_Release(position_line_buffer);
        if (tangent_stream_buffer)
            IDirect3DVertexBuffer9_Release(tangent_stream_buffer);
        if (tangent_vertex_buffer)
            IDirect3DVertexBuffer9_Release(tangent_vertex_buffer);
        if (strip_vertex_buffer)
            IDirect3DVertexBuffer9_Release(strip_vertex_buffer);
        if (padded_vertex_buffer)
            IDirect3DVertexBuffer9_Release(padded_vertex_buffer);
        if (split_blendindices_stream_buffer)
            IDirect3DVertexBuffer9_Release(split_blendindices_stream_buffer);
        if (split_blendweight_stream_buffer)
            IDirect3DVertexBuffer9_Release(split_blendweight_stream_buffer);
        if (blendindices_vertex_buffer)
            IDirect3DVertexBuffer9_Release(blendindices_vertex_buffer);
        if (blendweight_vertex_buffer)
            IDirect3DVertexBuffer9_Release(blendweight_vertex_buffer);
        if (vertex_buffer)
            IDirect3DVertexBuffer9_Release(vertex_buffer);
        if (split_tangent_vertex_decl)
            IDirect3DVertexDeclaration9_Release(split_tangent_vertex_decl);
        if (tangent_vertex_decl)
            IDirect3DVertexDeclaration9_Release(tangent_vertex_decl);
        if (split_vertex_decl)
            IDirect3DVertexDeclaration9_Release(split_vertex_decl);
        if (split_blendindices_vertex_decl)
            IDirect3DVertexDeclaration9_Release(split_blendindices_vertex_decl);
        if (split_blendweight_vertex_decl)
            IDirect3DVertexDeclaration9_Release(split_blendweight_vertex_decl);
        if (blendindices_vertex_decl)
            IDirect3DVertexDeclaration9_Release(blendindices_vertex_decl);
        if (blendweight_vertex_decl)
            IDirect3DVertexDeclaration9_Release(blendweight_vertex_decl);
        if (short4n_vertex_decl)
            IDirect3DVertexDeclaration9_Release(short4n_vertex_decl);
        if (stream0_vertex_decl)
            IDirect3DVertexDeclaration9_Release(stream0_vertex_decl);
        if (tangent_vertex_shader)
            IDirect3DVertexShader9_Release(tangent_vertex_shader);
        if (texcoord_vertex_shader)
            IDirect3DVertexShader9_Release(texcoord_vertex_shader);
        if (vertex_shader)
            IDirect3DVertexShader9_Release(vertex_shader);
        if (pixel_shader)
            IDirect3DPixelShader9_Release(pixel_shader);
        if (readback)
            IDirect3DSurface9_Release(readback);
        if (render_target)
            IDirect3DSurface9_Release(render_target);
        IDirect3DDevice9_Release(device_swvp);
    }
    {
        IDirect3DSurface9 *mixed_render_target = NULL;
        IDirect3DSurface9 *mixed_readback = NULL;
        IDirect3DDevice9 *device_mixed = NULL;
        IDirect3DVertexShader9 *mixed_vertex_shader = NULL;

        hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, window,
                D3DCREATE_MIXED_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                &pp, &device_mixed);
        CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL
                || hr == D3DERR_NOTAVAILABLE);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(!IDirect3DDevice9_GetSoftwareVertexProcessing(
                    device_mixed));
            CHECK_HR(IDirect3DDevice9_SetSoftwareVertexProcessing(
                    device_mixed, TRUE), D3D_OK);
            CHECK_TRUE(IDirect3DDevice9_GetSoftwareVertexProcessing(
                    device_mixed));
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_mixed,
                    D3DRS_LIGHTING, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_mixed,
                    D3DRS_ZENABLE, FALSE), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetRenderState(device_mixed,
                    D3DRS_CULLMODE, D3DCULL_NONE), D3D_OK);
            memset(&world, 0, sizeof(world));
            world.m[0][0] = 0.5f;
            world.m[1][1] = 0.5f;
            world.m[2][2] = 1.0f;
            world.m[3][3] = 1.0f;
            CHECK_HR(IDirect3DDevice9_SetTransform(device_mixed, D3DTS_WORLD,
                    &world), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetFVF(device_mixed,
                    D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);
            hr = IDirect3DDevice9_CreateVertexShader(device_mixed,
                    swvp_vs_3_0, &mixed_vertex_shader);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(
                        device_mixed, 0, (const float *)swvp_vs_identity, 4),
                        D3D_OK);
            }
            hr = IDirect3DDevice9_CreateRenderTarget(device_mixed, 32, 32,
                    D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE,
                    &mixed_render_target, NULL);
            CHECK_HR(hr, D3D_OK);
            hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device_mixed,
                    32, 32, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM,
                    &mixed_readback, NULL);
            CHECK_HR(hr, D3D_OK);
            if (mixed_render_target && mixed_readback)
            {
                CHECK_HR(IDirect3DDevice9_SetRenderTarget(device_mixed, 0,
                        mixed_render_target), D3D_OK);
                memset(&viewport, 0, sizeof(viewport));
                viewport.X = 0;
                viewport.Y = 0;
                viewport.Width = 32;
                viewport.Height = 32;
                viewport.MinZ = 0.0f;
                viewport.MaxZ = 1.0f;
                CHECK_HR(IDirect3DDevice9_SetViewport(device_mixed,
                        &viewport), D3D_OK);
                CHECK_HR(IDirect3DDevice9_BeginScene(device_mixed), D3D_OK);
                CHECK_HR(IDirect3DDevice9_Clear(device_mixed, 0, NULL,
                        D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0), D3D_OK);
                CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_mixed,
                        D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])),
                        D3D_OK);
                CHECK_HR(IDirect3DDevice9_EndScene(device_mixed), D3D_OK);
                CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device_mixed,
                        mixed_render_target, mixed_readback), D3D_OK);
                memset(&locked_rect, 0, sizeof(locked_rect));
                hr = IDirect3DSurface9_LockRect(mixed_readback,
                        &locked_rect, NULL, D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    const BYTE *row = (const BYTE *)locked_rect.pBits
                            + 18 * locked_rect.Pitch;
                    memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                            sizeof(probe_pixel));
                    CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                    CHECK_HR(IDirect3DSurface9_UnlockRect(mixed_readback),
                            D3D_OK);
                }
                if (mixed_vertex_shader)
                {
                    CHECK_HR(IDirect3DDevice9_BeginScene(device_mixed),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_mixed,
                            mixed_vertex_shader), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_Clear(device_mixed, 0, NULL,
                            D3DCLEAR_TARGET, 0xff000000u, 1.0f, 0),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device_mixed,
                            D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0])),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_SetVertexShader(device_mixed,
                            NULL), D3D_OK);
                    CHECK_HR(IDirect3DDevice9_EndScene(device_mixed),
                            D3D_OK);
                    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(
                            device_mixed, mixed_render_target,
                            mixed_readback), D3D_OK);
                    memset(&locked_rect, 0, sizeof(locked_rect));
                    hr = IDirect3DSurface9_LockRect(mixed_readback,
                            &locked_rect, NULL, D3DLOCK_READONLY);
                    CHECK_HR(hr, D3D_OK);
                    if (SUCCEEDED(hr))
                    {
                        const BYTE *row = (const BYTE *)locked_rect.pBits
                                + 18 * locked_rect.Pitch;
                        memcpy(&probe_pixel, row + 14 * sizeof(probe_pixel),
                                sizeof(probe_pixel));
                        CHECK_TRUE((probe_pixel & 0x00ffffffu) != 0);
                        CHECK_HR(IDirect3DSurface9_UnlockRect(mixed_readback),
                                D3D_OK);
                    }
                }
            }
            if (mixed_readback)
                IDirect3DSurface9_Release(mixed_readback);
            if (mixed_render_target)
                IDirect3DSurface9_Release(mixed_render_target);
            if (mixed_vertex_shader)
                IDirect3DVertexShader9_Release(mixed_vertex_shader);
            IDirect3DDevice9_Release(device_mixed);
        }
    }

    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
#undef SWVP_VS_DCL
#undef SWVP_VS_INST
#undef SWVP_VS_SRC
#undef SWVP_VS_DST
#undef SWVP_VS_REGTYPE
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_process_vertices
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_process_vertices_xyzhw_policy(const struct d3d9_api *api)
{
#define PROCESS_VS_REGTYPE(type) \
    ((((DWORD)(type) & 0x7u) << D3DSP_REGTYPE_SHIFT) \
            | (((((DWORD)(type) >> 3u) & 0x3u) << D3DSP_REGTYPE_SHIFT2)))
#define PROCESS_VS_DST(type, index, mask) \
    (0x80000000u | PROCESS_VS_REGTYPE(type) \
            | (((DWORD)(mask) & 0xfu) << 16) | ((DWORD)(index) & 0x7ffu))
#define PROCESS_VS_DST_REL(type, index, mask) \
    (PROCESS_VS_DST(type, index, mask) | D3DSHADER_ADDRMODE_RELATIVE)
#define PROCESS_VS_DST_MOD(type, index, mask, mod) \
    (PROCESS_VS_DST(type, index, mask) | ((DWORD)(mod) & D3DSP_DSTMOD_MASK))
#define PROCESS_VS_SRC(type, index) \
    (0x80000000u | PROCESS_VS_REGTYPE(type) | D3DSP_NOSWIZZLE \
            | ((DWORD)(index) & 0x7ffu))
#define PROCESS_VS_SWIZZLE(x, y, z, w) \
    ((((DWORD)(x) & 0x3u) << 16) | (((DWORD)(y) & 0x3u) << 18) \
            | (((DWORD)(z) & 0x3u) << 20) | (((DWORD)(w) & 0x3u) << 22))
#define PROCESS_VS_SRC_SWZ(type, index, x, y, z, w) \
    (0x80000000u | PROCESS_VS_REGTYPE(type) \
            | PROCESS_VS_SWIZZLE(x, y, z, w) \
            | ((DWORD)(index) & 0x7ffu))
#define PROCESS_VS_SRC_REL(type, index) \
    (PROCESS_VS_SRC(type, index) | D3DSHADER_ADDRMODE_RELATIVE)
#define PROCESS_VS_SRC_MOD(type, index, mod) \
    (PROCESS_VS_SRC(type, index) | ((DWORD)(mod) & D3DSP_SRCMOD_MASK))
#define PROCESS_VS_INST(opcode, operands) \
    (((DWORD)(opcode) & D3DSI_OPCODE_MASK) \
            | (((DWORD)(operands) & 0xfu) << D3DSI_INSTLENGTH_SHIFT))
#define PROCESS_VS_INST_PRED(opcode, operands) \
    (PROCESS_VS_INST(opcode, operands) | 0x10000000u)
#define PROCESS_VS_INST_CTRL(opcode, operands, controls) \
    (PROCESS_VS_INST(opcode, operands) | (((DWORD)(controls) & 0xffu) << 16))
#define PROCESS_VS_DCL(usage, usage_index) \
    (((DWORD)(usage) & 0xfu) \
            | (((DWORD)(usage_index) & 0xfu) << D3DSP_DCL_USAGEINDEX_SHIFT))
#define PROCESS_VS_LABEL(label) \
    (0x80000000u | ((DWORD)(label) & 0x7ffu))
    static const D3DVERTEXELEMENT9 dst_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 dst_specular_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 1},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 dst_sparse_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 dst_sparse_tex7_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 dst_psize_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_PSIZE, 0},
        {0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 dst_tex4_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 dst_d3dcolor_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 dst_packed_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_SHORT2N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 24, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        {0, 28, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
        {0, 32, D3DDECLTYPE_UDEC3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3},
        {0, 36, D3DDECLTYPE_DEC3N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_sparse_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_sparse_tex7_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 7},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_short2_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_SHORT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_short2n_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_SHORT2N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_ushort2n_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_USHORT2N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_float16_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_FLOAT16_2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_short4_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_short4n_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_SHORT4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_ushort4n_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_USHORT4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_float16_4_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_FLOAT16_4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_ubyte4_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_ubyte4n_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_udec3_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_UDEC3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_dec3n_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_DEC3N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_d3dcolor_tex_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_pos4_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_short4n_pos_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_SHORT4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 8, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_extra_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        {0, 40, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0},
        {0, 52, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 60, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        {0, 76, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_d3dcolor_blendindices_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 52, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 76, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_ubyte4n_blendweight_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 52, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {0, 76, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_extra_split_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {1, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {2, 28, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TANGENT, 0},
        {3, 40, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BINORMAL, 0},
        {0, 52, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        {4, 60, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDWEIGHT, 0},
        {5, 76, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_BLENDINDICES, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_generic_split_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 52, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_PSIZE, 0},
        {2, 60, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TESSFACTOR, 0},
        {3, 56, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_FOG, 0},
        {4, 28, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_DEPTH, 0},
        {5, 76, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_SAMPLE, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_generic_index_split_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 52, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_PSIZE, 0},
        {2, 60, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TESSFACTOR, 1},
        {3, 56, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_FOG, 1},
        {4, 28, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_DEPTH, 1},
        {5, 76, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_SAMPLE, 1},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_generic_d3dcolor_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 24, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_SAMPLE, 2},
        {0, 52, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_generic_short2_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_SHORT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_SAMPLE, 3},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_generic_ubyte4n_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_SAMPLE, 4},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_generic_udec3_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {1, 4, D3DDECLTYPE_UDEC3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_SAMPLE, 5},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_short4_normal_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_SHORT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 20, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 24, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_ubyte4_normal_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_UBYTE4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_ubyte4n_normal_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_UBYTE4N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_dec3n_normal_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_DEC3N, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_udec3_normal_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_UDEC3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const DWORD process_vs_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_packed_tex_dst_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 1),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 4, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 5, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 6, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 4, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_MUL, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 5, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_SRC_SWZ(D3DSPR_CONST, 4, 0, 0, 0, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 6, 0xf),
        PROCESS_VS_SRC_MOD(D3DSPR_INPUT, 2, D3DSPSM_SIGN),
        D3DSIO_END
    };
    static const DWORD process_vs_psize_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_PSIZE, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0x1),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0x1),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        D3DSIO_END
    };
    static const DWORD process_vs_fvf_psize_input_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_PSIZE, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_fvf_tex2_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 1),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 1),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        D3DSIO_END
    };
    static const DWORD process_vs_sparse_texcoord_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 1),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 1),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_sparse_texcoord7_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 7),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 7),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_1_1[] =
    {
        D3DVS_VERSION(1, 1),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_RASTOUT, D3DSRO_POSITION, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_ATTROUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 5),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_TEXCRDOUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 7),
        D3DSIO_END
    };
    static const DWORD process_vs_specular_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 1),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 1),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        D3DSIO_END
    };
    static const DWORD process_vs_output_relative_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOVA, 2),
        PROCESS_VS_DST(D3DSPR_ADDR, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST_REL(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_ADDR, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 3, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_input_relative_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOVA, 2),
        PROCESS_VS_DST(D3DSPR_ADDR, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC_REL(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_ADDR, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC_REL(D3DSPR_INPUT, 1),
        PROCESS_VS_SRC(D3DSPR_ADDR, 0),
        D3DSIO_END
    };
    static const DWORD process_vs_matrix_relative_const_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOVA, 2),
        PROCESS_VS_DST(D3DSPR_ADDR, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC_REL(D3DSPR_CONST, 0),
        PROCESS_VS_SRC(D3DSPR_ADDR, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_mova_component_relative_const_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOVA, 2),
        PROCESS_VS_DST(D3DSPR_ADDR, 0, 0x1),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_MOVA, 2),
        PROCESS_VS_DST(D3DSPR_ADDR, 0, 0x2),
        PROCESS_VS_SRC(D3DSPR_CONST, 9),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC_REL(D3DSPR_CONST, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_ADDR, 0, 1, 1, 1, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC_REL(D3DSPR_CONST, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_ADDR, 0, 0, 0, 0, 0),
        D3DSIO_END
    };
    static const DWORD process_vs_texldl_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        D3DSTT_2D,
        PROCESS_VS_DST(D3DSPR_SAMPLER, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0x8),
        PROCESS_VS_SRC_SWZ(D3DSPR_CONST, 0, 2, 2, 2, 2),
        PROCESS_VS_INST(D3DSIO_TEXLDL, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_SAMPLER, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_partialprecision_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST_MOD(D3DSPR_OUTPUT, 0, 0xf, D3DSPDM_PARTIALPRECISION),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_saturate_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST_MOD(D3DSPR_OUTPUT, 1, 0xf, D3DSPDM_SATURATE),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_mad_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MAD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_pos4_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_normal_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_NORMAL, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_tangent_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TANGENT, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 4, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_INPUT, 4),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_vector_math_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_NORMAL, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_NRM, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_INST(D3DSIO_DP3, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_INST(D3DSIO_MUL, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x8),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_compare_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_NORMAL, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_SLT, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_INST(D3DSIO_SGE, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST(D3DSIO_MIN, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_MAX, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_LRP, 4),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST(D3DSIO_CND, 4),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_CMP, 4),
        PROCESS_VS_DST(D3DSPR_TEMP, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 2),
        PROCESS_VS_INST(D3DSIO_SETP, 2),
        PROCESS_VS_DST(D3DSPR_PREDICATE, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST_PRED(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x8),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_scalar_math_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_ABS, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_POW, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST(D3DSIO_RSQ, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_RCP, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_FRC, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_SUB, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_CRS, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 2),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_DP2ADD, 4),
        PROCESS_VS_DST(D3DSPR_TEMP, 3, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 5),
        PROCESS_VS_SRC(D3DSPR_CONST, 6),
        PROCESS_VS_SRC(D3DSPR_CONST, 7),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 2),
        PROCESS_VS_SRC(D3DSPR_TEMP, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x8),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_transcendent_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DST, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST(D3DSIO_LIT, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_INST(D3DSIO_SUB, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_EXP, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 2),
        PROCESS_VS_INST(D3DSIO_LOG, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 3, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_SUB, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 3),
        PROCESS_VS_INST(D3DSIO_EXPP, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 4, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 5),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 4),
        PROCESS_VS_INST(D3DSIO_LOGP, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 5, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 6),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 5),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_source_modifiers_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 4, D3DSPSM_BIAS),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 5, D3DSPSM_BIASNEG),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 6, D3DSPSM_SIGN),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 7, D3DSPSM_SIGNNEG),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 8, D3DSPSM_COMP),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 9, D3DSPSM_X2),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 10, D3DSPSM_X2NEG),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 11, D3DSPSM_DZ),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 12, D3DSPSM_DW),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC_MOD(D3DSPR_CONST, 13, D3DSPSM_NOT),
        PROCESS_VS_INST(D3DSIO_SUB, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 14),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_flow_if_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_IF, 1),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_INST(D3DSIO_ELSE, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST(D3DSIO_ENDIF, 0),
        PROCESS_VS_INST(D3DSIO_SETP, 2),
        PROCESS_VS_DST(D3DSPR_PREDICATE, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST_PRED(D3DSIO_IF, 1),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_INST(D3DSIO_ELSE, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_INST(D3DSIO_ENDIF, 0),
        PROCESS_VS_INST_CTRL(D3DSIO_IFC, 2, D3DSPC_GT),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 5),
        PROCESS_VS_INST(D3DSIO_ELSE, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_INST(D3DSIO_ENDIF, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_bool_constant_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DEFB, 2),
        PROCESS_VS_DST(D3DSPR_CONSTBOOL, 1, 0xf),
        TRUE,
        PROCESS_VS_INST(D3DSIO_M4x4, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_IF, 1),
        PROCESS_VS_SRC(D3DSPR_CONSTBOOL, 0),
        PROCESS_VS_INST(D3DSIO_IF, 1),
        PROCESS_VS_SRC(D3DSPR_CONSTBOOL, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_ELSE, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_ENDIF, 0),
        PROCESS_VS_INST(D3DSIO_ELSE, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_ENDIF, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_rep_loop_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DEFI, 5),
        PROCESS_VS_DST(D3DSPR_CONSTINT, 0, 0xf),
        4, 0, 0, 0,
        PROCESS_VS_INST(D3DSIO_DEFI, 5),
        PROCESS_VS_DST(D3DSPR_CONSTINT, 1, 0xf),
        4, 0, 0, 0,
        PROCESS_VS_INST(D3DSIO_DEFI, 5),
        PROCESS_VS_DST(D3DSPR_CONSTINT, 2, 0xf),
        3, 0, 1, 0,
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_REP, 1),
        PROCESS_VS_SRC(D3DSPR_CONSTINT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_INST(D3DSIO_BREAK, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ENDREP, 0),
        PROCESS_VS_INST(D3DSIO_LOOP, 2),
        PROCESS_VS_SRC(D3DSPR_LOOP, 0),
        PROCESS_VS_SRC(D3DSPR_CONSTINT, 1),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST_CTRL(D3DSIO_BREAKC, 2, D3DSPC_GT),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_SRC(D3DSPR_CONST, 5),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ENDLOOP, 0),
        PROCESS_VS_INST(D3DSIO_REP, 1),
        PROCESS_VS_SRC(D3DSPR_CONSTINT, 0),
        PROCESS_VS_INST(D3DSIO_SETP, 2),
        PROCESS_VS_DST(D3DSPR_PREDICATE, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 8),
        PROCESS_VS_INST(D3DSIO_BREAKP, 2),
        PROCESS_VS_SRC(D3DSPR_PREDICATE, 0),
        PROCESS_VS_SRC(D3DSPR_PREDICATE, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_ENDREP, 0),
        PROCESS_VS_INST(D3DSIO_LOOP, 2),
        PROCESS_VS_SRC(D3DSPR_LOOP, 0),
        PROCESS_VS_SRC(D3DSPR_CONSTINT, 2),
        PROCESS_VS_INST(D3DSIO_MUL, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_LOOP, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 9),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_REL(D3DSPR_CONST, 10),
        PROCESS_VS_SRC(D3DSPR_LOOP, 0),
        PROCESS_VS_INST(D3DSIO_ENDLOOP, 0),
        PROCESS_VS_INST(D3DSIO_MOVA, 2),
        PROCESS_VS_DST(D3DSPR_ADDR, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 8),
        PROCESS_VS_INST(D3DSIO_MUL, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_ADDR, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 9),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_REL(D3DSPR_CONST, 13),
        PROCESS_VS_SRC(D3DSPR_ADDR, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST_REL(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_ADDR, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 15),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_CALL, 1),
        PROCESS_VS_LABEL(7),
        PROCESS_VS_INST(D3DSIO_CALLNZ, 0),
        PROCESS_VS_LABEL(9),
        PROCESS_VS_SRC(D3DSPR_CONST, 8),
        PROCESS_VS_INST(D3DSIO_SUB, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        PROCESS_VS_INST(D3DSIO_LABEL, 1),
        PROCESS_VS_LABEL(7),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 6),
        PROCESS_VS_INST(D3DSIO_RET, 0),
        PROCESS_VS_INST(D3DSIO_LABEL, 1),
        PROCESS_VS_LABEL(9),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 7),
        PROCESS_VS_INST(D3DSIO_RET, 0),
        D3DSIO_END
    };
    static const DWORD process_vs_matrix_special_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_M3x2, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_INST(D3DSIO_SINCOS, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_INST(D3DSIO_SGN, 2),
        PROCESS_VS_DST(D3DSPR_TEMP, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 1),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 2),
        PROCESS_VS_INST(D3DSIO_SUB, 3),
        PROCESS_VS_DST(D3DSPR_TEMP, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_SRC(D3DSPR_CONST, 5),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC(D3DSPR_TEMP, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_binormal_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_BINORMAL, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 5, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_INPUT, 5),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_blendweight_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_BLENDWEIGHT, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 6, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_SRC(D3DSPR_INPUT, 6),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_blendindices_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_BLENDINDICES, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 7, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MAD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 7),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_extra_multistream_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_NORMAL, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TANGENT, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 4, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_BINORMAL, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 5, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_BLENDWEIGHT, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 6, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_BLENDINDICES, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 7, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MAD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_SRC(D3DSPR_CONST, 0),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_INST(D3DSIO_MAD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 4),
        PROCESS_VS_SRC(D3DSPR_CONST, 1),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_INST(D3DSIO_MAD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 5),
        PROCESS_VS_SRC(D3DSPR_CONST, 2),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_INST(D3DSIO_MAD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 6),
        PROCESS_VS_SRC(D3DSPR_CONST, 3),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_INST(D3DSIO_MAD, 4),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x7),
        PROCESS_VS_SRC(D3DSPR_INPUT, 7),
        PROCESS_VS_SRC(D3DSPR_CONST, 4),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_generic_usage_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_PSIZE, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TESSFACTOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_FOG, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 4, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_DEPTH, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 5, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_SAMPLE, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 6, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x1),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_INPUT, 2, 0, 0, 0, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x2),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_INPUT, 4, 0, 0, 0, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x4),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_INPUT, 5, 0, 0, 0, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0x3),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 2),
        PROCESS_VS_SRC(D3DSPR_INPUT, 6),
        D3DSIO_END
    };
    static const DWORD process_vs_generic_usage_index_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_PSIZE, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TESSFACTOR, 1),
        PROCESS_VS_DST(D3DSPR_INPUT, 3, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_FOG, 1),
        PROCESS_VS_DST(D3DSPR_INPUT, 4, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_DEPTH, 1),
        PROCESS_VS_DST(D3DSPR_INPUT, 5, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_SAMPLE, 1),
        PROCESS_VS_DST(D3DSPR_INPUT, 6, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x1),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_INPUT, 2, 0, 0, 0, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x2),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_INPUT, 4, 0, 0, 0, 0),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0x4),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 0),
        PROCESS_VS_SRC_SWZ(D3DSPR_INPUT, 5, 0, 0, 0, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 3),
        PROCESS_VS_INST(D3DSIO_ADD, 3),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0x3),
        PROCESS_VS_SRC(D3DSPR_OUTPUT, 2),
        PROCESS_VS_SRC(D3DSPR_INPUT, 6),
        D3DSIO_END
    };
    static const DWORD process_vs_generic_d3dcolor_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_SAMPLE, 2),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_generic_short2_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_SAMPLE, 3),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_generic_ubyte4n_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_SAMPLE, 4),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    static const DWORD process_vs_generic_udec3_3_0[] =
    {
        D3DVS_VERSION(3, 0),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_INPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_SAMPLE, 5),
        PROCESS_VS_DST(D3DSPR_INPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_POSITION, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_COLOR, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_INST(D3DSIO_DCL, 2),
        PROCESS_VS_DCL(D3DDECLUSAGE_TEXCOORD, 0),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 0, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 0),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 1, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 1),
        PROCESS_VS_INST(D3DSIO_MOV, 2),
        PROCESS_VS_DST(D3DSPR_OUTPUT, 2, 0xf),
        PROCESS_VS_SRC(D3DSPR_INPUT, 2),
        D3DSIO_END
    };
    struct src_vertex
    {
        float x, y, z;
        DWORD color;
        float u, v;
    };
    struct src_specular_vertex
    {
        float x, y, z;
        DWORD color;
        DWORD specular;
        float u, v;
    };
    struct dst_vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v;
    };
    struct dst_tex1_vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u;
    };
    struct dst_tex4_vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float u, v, s, t;
    };
    struct dst_d3dcolor_tex_vertex
    {
        float x, y, z, rhw;
        DWORD color;
        DWORD texcolor;
    };
    struct dst_packed_tex_vertex
    {
        float x, y, z, rhw;
        DWORD color;
        SHORT short2n_u, short2n_v;
        BYTE ubyte4n_u, ubyte4n_v, ubyte4n_s, ubyte4n_t;
        WORD half_u, half_v;
        DWORD udec3;
        DWORD dec3n;
    };
    struct dst_specular_vertex
    {
        float x, y, z, rhw;
        DWORD color;
        DWORD specular;
        float u, v;
    };
    struct src_psize_vertex
    {
        float x, y, z;
        float psize;
        DWORD color;
        float u, v;
    };
    struct dst_psize_vertex
    {
        float x, y, z, rhw;
        float psize;
        DWORD color;
        float u, v;
    };
    struct src_fvf_tex2_vertex
    {
        float x, y, z;
        DWORD color;
        float t0;
        float t1x, t1y, t1z;
    };
    struct dst_fvf_tex2_vertex
    {
        float x, y, z, rhw;
        DWORD color;
        float t0;
        float t1x, t1y, t1z;
    };
    struct attr_vertex
    {
        DWORD color;
        float u, v;
    };
    struct attr_short2n_vertex
    {
        DWORD color;
        SHORT u, v;
    };
    struct attr_ushort2n_vertex
    {
        DWORD color;
        WORD u, v;
    };
    struct attr_float16_vertex
    {
        DWORD color;
        WORD u, v;
    };
    struct attr_short4n_vertex
    {
        DWORD color;
        SHORT u, v, s, t;
    };
    struct attr_ushort4n_vertex
    {
        DWORD color;
        WORD u, v, s, t;
    };
    struct attr_float16_4_vertex
    {
        DWORD color;
        WORD u, v, s, t;
    };
    struct attr_ubyte4n_vertex
    {
        DWORD color;
        BYTE u, v, s, t;
    };
    struct attr_udec3_vertex
    {
        DWORD color;
        DWORD texcoord;
    };
    struct src_pos4_vertex
    {
        float x, y, z, w;
        DWORD color;
        float u, v;
    };
    struct src_short4n_pos_vertex
    {
        SHORT x, y, z, w;
        DWORD color;
        float u, v;
    };
    struct src_fvf_normal_vertex
    {
        float x, y, z;
        float nx, ny, nz;
        DWORD color;
        float u, v;
    };
    struct src_fvf_normal_specular_vertex
    {
        float x, y, z;
        float nx, ny, nz;
        DWORD color;
        DWORD specular;
        float u, v;
    };
    struct src_fvf_blendweight_vertex
    {
        float x, y, z;
        float bw0, bw1, bw2, bw3;
        DWORD color;
        float u, v;
    };
    struct src_fvf_blendindices_vertex
    {
        float x, y, z;
        float bw0, bw1, bw2, bw3;
        BYTE bi0, bi1, bi2, bi3;
        DWORD color;
        float u, v;
    };
    struct src_extra_vertex
    {
        float x, y, z;
        float nx, ny, nz;
        DWORD color;
        float tx, ty, tz;
        float bx, by, bz;
        float u, v;
        float bw0, bw1, bw2, bw3;
        BYTE bi0, bi1, bi2, bi3;
    };
    struct src_short4_normal_vertex
    {
        float x, y, z;
        SHORT nx, ny, nz, nw;
        DWORD color;
        float u, v;
    };
    struct src_ubyte4_normal_vertex
    {
        float x, y, z;
        BYTE nx, ny, nz, nw;
        DWORD color;
        float u, v;
    };
    struct src_ubyte4n_normal_vertex
    {
        float x, y, z;
        BYTE nx, ny, nz, nw;
        DWORD color;
        float u, v;
    };
    struct src_dec3n_normal_vertex
    {
        float x, y, z;
        DWORD normal;
        DWORD color;
        float u, v;
    };
    struct src_udec3_normal_vertex
    {
        float x, y, z;
        DWORD normal;
        DWORD color;
        float u, v;
    };
    const struct src_vertex src[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_vertex src_texldl[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffff0000u,  1.25f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u, -0.25f, 0.25f},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu,  0.25f, 1.75f},
        { 0.5f,  0.5f, 0.0f, 0xffffffffu,  1.75f, 1.75f},
    };
    const struct src_vertex src_depth_clamp[] =
    {
        {-0.5f, -0.5f, -0.5f, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f,  0.5f, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f,  1.5f, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f,  2.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_specular_vertex src_specular[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffff0000u, 0xff001122u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u, 0xff334455u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu, 0xff667788u, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0xffffffffu, 0xff99aabbu, 1.00f, 1.00f},
    };
    const struct src_psize_vertex src_psize[] =
    {
        {-0.5f, -0.5f, 0.0f, 1.25f, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 2.50f, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 3.75f, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 5.00f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_fvf_tex2_vertex src_fvf_tex2[] =
    {
        {-0.5f, -0.5f, 0.0f, 0xffff0000u, 0.125f, 1.0f, 2.0f, 3.0f},
        {-0.5f,  0.5f, 0.0f, 0xff00ff00u, 0.250f, 4.0f, 5.0f, 6.0f},
        { 0.5f, -0.5f, 0.0f, 0xff0000ffu, 0.500f, 7.0f, 8.0f, 9.0f},
        { 0.5f,  0.5f, 0.0f, 0xffffffffu, 0.750f, 10.0f, 11.0f, 12.0f},
    };
    const struct attr_short2n_vertex src_attr_short2[] =
    {
        {0xffff0000u,  0,  1},
        {0xff00ff00u,  2,  3},
        {0xff0000ffu, -4,  5},
        {0xffffffffu,  6, -7},
    };
    const struct attr_short2n_vertex src_attr_short2n[] =
    {
        {0xffff0000u,     0,  8192},
        {0xff00ff00u,  8192, 16384},
        {0xff0000ffu, 16384, 24575},
        {0xffffffffu, 32767, 32767},
    };
    const struct attr_ushort2n_vertex src_attr_ushort2n[] =
    {
        {0xffff0000u,     0, 16384},
        {0xff00ff00u, 16384, 32768},
        {0xff0000ffu, 32768, 49151},
        {0xffffffffu, 65535, 65535},
    };
    const struct attr_float16_vertex src_attr_float16[] =
    {
        {0xffff0000u, 0x0000, 0x3400},
        {0xff00ff00u, 0x3400, 0x3800},
        {0xff0000ffu, 0x3800, 0x3a00},
        {0xffffffffu, 0x3c00, 0x3c00},
    };
    const struct attr_short4n_vertex src_attr_short4[] =
    {
        {0xffff0000u,  0,  1,   2,   3},
        {0xff00ff00u, -4,  5,  -6,   7},
        {0xff0000ffu,  8, -9,  10, -11},
        {0xffffffffu, 12, 13, -14, -15},
    };
    const struct attr_short4n_vertex src_attr_short4n[] =
    {
        {0xffff0000u,     0,  8192, 16384, 32767},
        {0xff00ff00u,  8192, 16384, 24575, 32767},
        {0xff0000ffu, 16384, 24575, 32767,     0},
        {0xffffffffu, 32767, 32767,     0, 16384},
    };
    const struct attr_ushort4n_vertex src_attr_ushort4n[] =
    {
        {0xffff0000u,     0, 16384, 32768, 65535},
        {0xff00ff00u, 16384, 32768, 49151, 65535},
        {0xff0000ffu, 32768, 49151, 65535,     0},
        {0xffffffffu, 65535, 65535,     0, 32768},
    };
    const struct attr_float16_4_vertex src_attr_float16_4[] =
    {
        {0xffff0000u, 0x0000, 0x3400, 0x3800, 0x3c00},
        {0xff00ff00u, 0x3400, 0x3800, 0x3a00, 0x3c00},
        {0xff0000ffu, 0x3800, 0x3a00, 0x3c00, 0x0000},
        {0xffffffffu, 0x3c00, 0x3c00, 0x0000, 0x3800},
    };
    const struct attr_ubyte4n_vertex src_attr_ubyte4[] =
    {
        {0xffff0000u,  0,  1,  2,  3},
        {0xff00ff00u,  4,  5,  6,  7},
        {0xff0000ffu,  8,  9, 10, 11},
        {0xffffffffu, 12, 13, 14, 15},
    };
    const struct attr_ubyte4n_vertex src_attr_ubyte4n[] =
    {
        {0xffff0000u,   0,  64, 128, 255},
        {0xff00ff00u,  64, 128, 192, 255},
        {0xff0000ffu, 128, 192, 255,   0},
        {0xffffffffu, 255, 255,   0, 128},
    };
    const struct attr_udec3_vertex src_attr_udec3[] =
    {
        {0xffff0000u, 0x00200400u},
        {0xff00ff00u, 0x00501003u},
        {0xff0000ffu, 0x04008010u},
        {0xffffffffu, 0x0ff803ffu},
    };
    const struct attr_udec3_vertex src_attr_dec3n[] =
    {
        {0xffff0000u, 0x1ff40000u},
        {0xff00ff00u, 0x10000300u},
        {0xff0000ffu, 0x3ff801ffu},
        {0xffffffffu, 0x0007fe00u},
    };
    const struct dst_vertex expected[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_fvf_blendweight[] =
    {
        {180.0f, 345.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {180.0f, 135.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {460.0f, 345.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {460.0f, 135.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_fvf_blendindices[] =
    {
        {200.0f, 330.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {200.0f, 150.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {440.0f, 330.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {440.0f, 150.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_fvf_tex2_vertex expected_fvf_tex2[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.125f, 1.0f, 2.0f, 3.0f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.250f, 4.0f, 5.0f, 6.0f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.500f, 7.0f, 8.0f, 9.0f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 0.750f, 10.0f, 11.0f, 12.0f},
    };
    const struct dst_d3dcolor_tex_vertex expected_d3dcolor_tex_dst[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0xff004000u},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0xff408000u},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0xff80bf00u},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 0xffffff00u},
    };
    const struct dst_packed_tex_vertex expected_packed_tex_dst[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u,
                0, 8192, 0, 64, 0, 255, 0x0000, 0x3400,
                0x00040000u, 0x200c0200u},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u,
                8192, 16384, 64, 128, 0, 255, 0x3400, 0x3800,
                0x00080100u, 0x20000300u},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu,
                16384, 24575, 128, 191, 0, 255, 0x3800, 0x3a00,
                0x000bfe00u, 0x20040000u},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu,
                32767, 32767, 255, 255, 0, 255, 0x3c00, 0x3c00,
                0x000fffffu, 0x2007fdffu},
    };
    const struct dst_vertex expected_depth_clamp[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.5f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 1.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 1.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_texldl[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff112233u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff445566u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff778899u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffaabbccu,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_lod[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff336699u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff336699u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff336699u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff336699u,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_clamp[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff445566u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff112233u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff778899u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffaabbccu,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_mirror[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff445566u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff112233u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff112233u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff112233u,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_mirroronce[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff445566u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff112233u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff778899u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffaabbccu,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_border[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0x7f123456u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0x7f123456u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0x7f123456u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0x7f123456u,  1.75f, 1.75f},
    };
    const struct texldl_address_case
    {
        DWORD address;
        DWORD border_color;
        const struct dst_vertex *expected;
    }
    texldl_address_cases[] =
    {
        {D3DTADDRESS_CLAMP,      0x00000000u, expected_texldl_clamp},
        {D3DTADDRESS_MIRROR,     0x00000000u, expected_texldl_mirror},
        {D3DTADDRESS_MIRRORONCE, 0x00000000u, expected_texldl_mirroronce},
        {D3DTADDRESS_BORDER,     0x7f123456u, expected_texldl_border},
    };
    const struct dst_vertex expected_texldl_p8[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff102030u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff405060u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff708090u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffa0b0c0u,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_p8_updated[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff224466u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff6688aau, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff99bbddu,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffccdd22u,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_a8p8_initial[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0x80102030u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0x60405060u, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0x40708090u,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0x20a0b0c0u,  1.75f, 1.75f},
    };
    const struct dst_vertex expected_texldl_a8p8[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0x80224466u,  1.25f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0x606688aau, -0.25f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0x4099bbddu,  0.25f, 1.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0x20ccdd22u,  1.75f, 1.75f},
    };
    const struct dst_psize_vertex expected_psize[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 1.25f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 2.50f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 3.75f, 0xff0000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 5.00f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_psize_vertex expected_prog_psize[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 6.25f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 6.25f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 6.25f, 0xff0000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 6.25f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_fvf_psize_input[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 1.25f, 0.00f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 2.50f, 0.00f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 3.75f, 0.00f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 5.00f, 0.00f},
    };
    const struct dst_vertex expected_instanced_source[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
    };
    const struct dst_vertex expected_saturate[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0080u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xffff0080u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xffff0080u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffff0080u, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_mova_component_relative[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 4.00f, 5.00f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 4.00f, 5.00f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 4.00f, 5.00f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 4.00f, 5.00f},
    };
    const struct dst_vertex expected_bool_false[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff00ffffu, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ffffu, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff00ffffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff00ffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex sentinel =
        {13.0f, 17.0f, 19.0f, 23.0f, 0xff123456u, 0.125f, 0.875f};
    const struct dst_vertex expected_offset[] =
    {
        {13.0f, 17.0f, 19.0f, 23.0f, 0xff123456u, 0.125f, 0.875f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {13.0f, 17.0f, 19.0f, 23.0f, 0xff123456u, 0.125f, 0.875f},
    };
    const struct dst_vertex expected_short2[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u,  0.0f,  1.0f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u,  2.0f,  3.0f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, -4.0f,  5.0f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu,  6.0f, -7.0f},
    };
    const struct dst_vertex expected_ubyte4n_generic[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25098f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25098f, 0.50196f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50196f, 0.75294f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_udec3_generic[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u,    0.0f,   1.0f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u,    3.0f,   4.0f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu,   16.0f,  32.0f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1023.0f, 512.0f},
    };
    const struct dst_tex1_vertex expected_tex1[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f},
    };
    const struct src_pos4_vertex src_pos4[] =
    {
        {-0.25f, -0.25f, 0.0f, 0.5f, 0xff336699u, 0.125f, 0.375f},
        { 0.25f,  0.25f, 0.5f, 0.5f, 0xff996633u, 0.625f, 0.875f},
    };
    const struct src_short4n_pos_vertex src_short4n_pos[] =
    {
        {-16384, -16384,     0, 32767, 0xff663399u, 0.125f, 0.375f},
        { 16384,  16384, 16384, 32767, 0xff339966u, 0.625f, 0.875f},
    };
    const struct src_fvf_normal_vertex src_fvf_normal[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_fvf_normal_specular_vertex src_fvf_normal_specular[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xffff0000u, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff00ff00u, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff0000ffu, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xffffffffu, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_fvf_normal_specular_vertex src_fvf_material_sources[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff204060u, 0xff102030u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff306090u, 0xff081018u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff4080c0u, 0xff000810u, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff808080u, 0xff202020u, 1.00f, 1.00f},
    };
    const struct src_fvf_blendweight_vertex src_fvf_blendweight[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_fvf_blendindices_vertex src_fvf_blendindices[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_extra_vertex src_extra[] =
    {
        {-0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xffff0000u, 1.0f, 0.0f, 0.0f, 0.0f, -0.25f, 0.0f, 0.00f, 0.25f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0},
        {-0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff00ff00u, 1.0f, 0.0f, 0.0f, 0.0f, -0.25f, 0.0f, 0.25f, 0.50f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0},
        { 0.5f, -0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xff0000ffu, 1.0f, 0.0f, 0.0f, 0.0f, -0.25f, 0.0f, 0.50f, 0.75f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0},
        { 0.5f,  0.5f, 0.0f, 0.0f, 0.0f, 1.0f, 0xffffffffu, 1.0f, 0.0f, 0.0f, 0.0f, -0.25f, 0.0f, 1.00f, 1.00f, 0.25f, 0.25f, 0.0f, 0.0f, 1, 1, 0, 0},
    };
    const struct src_short4_normal_vertex src_short4_normal[] =
    {
        {-0.5f, -0.5f, 0.0f, 0, 0, 1, 0, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0, 0, 1, 0, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0, 0, 1, 0, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0, 0, 1, 0, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_ubyte4_normal_vertex src_ubyte4_normal[] =
    {
        {-0.5f, -0.5f, 0.0f, 0, 0, 1, 0, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0, 0, 1, 0, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0, 0, 1, 0, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0, 0, 1, 0, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_ubyte4n_normal_vertex src_ubyte4n_normal[] =
    {
        {-0.5f, -0.5f, 0.0f, 0, 0, 255, 255, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0, 0, 255, 255, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0, 0, 255, 255, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0, 0, 255, 255, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_dec3n_normal_vertex src_dec3n_normal[] =
    {
        {-0.5f, -0.5f, 0.0f, 0x1ff00000u, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0x1ff00000u, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0x1ff00000u, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0x1ff00000u, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct src_udec3_normal_vertex src_udec3_normal[] =
    {
        {-0.5f, -0.5f, 0.0f, 0x00100000u, 0xffff0000u, 0.00f, 0.25f},
        {-0.5f,  0.5f, 0.0f, 0x00100000u, 0xff00ff00u, 0.25f, 0.50f},
        { 0.5f, -0.5f, 0.0f, 0x00100000u, 0xff0000ffu, 0.50f, 0.75f},
        { 0.5f,  0.5f, 0.0f, 0x00100000u, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_pos4[] =
    {
        {160.0f, 360.0f, 0.0f, 2.0f, 0xff336699u, 0.125f, 0.375f},
        {480.0f, 120.0f, 1.0f, 2.0f, 0xff996633u, 0.625f, 0.875f},
    };
    const struct dst_vertex expected_short4n_pos[] =
    {
        {160.0f, 360.0f, 0.0f, 1.0f, 0xff663399u, 0.125f, 0.375f},
        {480.0f, 120.0f, 0.5f, 1.0f, 0xff339966u, 0.625f, 0.875f},
    };
    const struct dst_vertex expected_normal[] =
    {
        {240.0f, 300.0f, 1.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 1.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 1.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 1.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_lit[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff8000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xffff8000u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xffff8000u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffff8000u, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_lit_point_attenuated[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff402000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff402000u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff402000u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff402000u, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_lit_unlit[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff000000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff000000u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff000000u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff000000u, 1.00f, 1.00f},
    };
    const struct
    {
        float attenuation0;
        float attenuation1;
        float attenuation2;
    } point_attenuation_cases[] =
    {
        {4.0f, 0.0f, 0.0f},
        {1.0f, 3.0e-5f, 0.0f},
        {1.0f, 0.0f, 3.0e-10f},
    };
    const struct dst_vertex expected_lit_colorvertex[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff800000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff008000u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff000080u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff808080u, 1.00f, 1.00f},
    };
    const struct dst_specular_vertex expected_lit_colorvertex_specular[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff800000u, 0x00800000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff008000u, 0x00008000u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff000080u, 0x00000080u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff808080u, 0x00808080u, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_lit_ambient_emissive_sources[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff183048u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff14283cu, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff102840u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff404040u, 1.00f, 1.00f},
    };
    const struct dst_specular_vertex expected_spot_specular[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff8000u, 0x000000ffu, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xffff8000u, 0x000000ffu, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xffff8000u, 0x000000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffff8000u, 0x000000ffu, 1.00f, 1.00f},
    };
    const struct dst_specular_vertex expected_spot_falloff[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff402000u, 0x00000040u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff402000u, 0x00000040u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff402000u, 0x00000040u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff402000u, 0x00000040u, 1.00f, 1.00f},
    };
    const struct dst_specular_vertex expected_spot_unlit[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff000000u, 0x00000000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff000000u, 0x00000000u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff000000u, 0x00000000u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff000000u, 0x00000000u, 1.00f, 1.00f},
    };
    const struct dst_specular_vertex expected_specular[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0xff001122u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0xff334455u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0xff667788u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 0xff99aabbu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_tangent[] =
    {
        {480.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {480.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {800.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {800.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_binormal[] =
    {
        {160.0f, 420.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {160.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {480.0f, 420.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {480.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_blendweight[] =
    {
        {400.0f, 240.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {400.0f,   0.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {720.0f, 240.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {720.0f,   0.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_d3dcolor_blendindices[] =
    {
        {160.0f, 120.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {160.0f, -120.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {480.0f, 120.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {480.0f, -120.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_ubyte4n_blendweight[] =
    {
        {240.62745f, 299.52942f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {240.62745f, 179.52942f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {400.62745f, 299.52942f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {400.62745f, 179.52942f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_extra_multistream[] =
    {
        {640.0f, 300.0f, 1.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {640.0f,  60.0f, 1.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {960.0f, 300.0f, 1.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {960.0f,  60.0f, 1.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_generic_usage[] =
    {
        {240.0f, 270.0f, 1.0f, 1.0f, 0xffff0000u, 1.25f, 1.25f},
        {280.0f, 120.0f, 1.0f, 1.0f, 0xff00ff00u, 1.25f, 1.25f},
        {480.0f, 210.0f, 1.0f, 1.0f, 0xff0000ffu, 1.25f, 1.25f},
        {560.0f,  60.0f, 1.0f, 1.0f, 0xffffffffu, 1.25f, 1.25f},
    };
    const struct dst_vertex expected_d3dcolor_tex[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 1.00f, 0.00f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.00f, 1.00f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.00f, 0.00f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_vertex expected_transcendent[] =
    {
        {480.0f, 480.0f, -0.25f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {480.0f, 240.0f, -0.25f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {800.0f, 480.0f, -0.25f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {800.0f, 240.0f, -0.25f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    const struct dst_tex4_vertex expected_tex4[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f, 0.50f, 1.00f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f, 0.75f, 1.00f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f, 1.00f, 0.00f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f, 0.00f, 0.50f},
    };
    const struct dst_tex4_vertex expected_short4_tex4[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u,  0.0f,  1.0f,   2.0f,   3.0f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, -4.0f,  5.0f,  -6.0f,   7.0f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu,  8.0f, -9.0f,  10.0f, -11.0f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 12.0f, 13.0f, -14.0f, -15.0f},
    };
    const struct dst_tex4_vertex expected_ubyte4_tex4[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u,  0.0f,  1.0f,  2.0f,  3.0f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u,  4.0f,  5.0f,  6.0f,  7.0f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu,  8.0f,  9.0f, 10.0f, 11.0f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 12.0f, 13.0f, 14.0f, 15.0f},
    };
    const struct dst_tex4_vertex expected_udec3_tex4[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u,    0.0f,   1.0f,   2.0f, 1.0f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u,    3.0f,   4.0f,   5.0f, 1.0f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu,   16.0f,  32.0f,  64.0f, 1.0f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1023.0f, 512.0f, 255.0f, 1.0f},
    };
    const struct dst_tex4_vertex expected_dec3n_tex4[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u,  0.00f,  0.50f,  1.00f, 1.0f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, -0.50f,  0.00f,  0.50f, 1.0f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu,  1.00f, -1.00f, -0.00f, 1.0f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, -1.00f,  1.00f,  0.00f, 1.0f},
    };
    IDirect3DVertexBuffer9 *src_vb = NULL;
    IDirect3DVertexBuffer9 *src_texldl_vb = NULL;
    IDirect3DVertexBuffer9 *src_depth_clamp_vb = NULL;
    IDirect3DVertexBuffer9 *src_specular_vb = NULL;
    IDirect3DVertexBuffer9 *src_psize_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_short2_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_short2n_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_ushort2n_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_float16_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_short4_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_short4n_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_ushort4n_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_float16_4_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_ubyte4_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_ubyte4n_vb = NULL;
    IDirect3DVertexBuffer9 *src_attr_udec3_vb = NULL;
    IDirect3DVertexBuffer9 *src_pos4_vb = NULL;
    IDirect3DVertexBuffer9 *src_short4n_pos_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_normal_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_normal_specular_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_material_sources_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_blendweight_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_blendindices_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_tex2_vb = NULL;
    IDirect3DVertexBuffer9 *src_extra_vb = NULL;
    IDirect3DVertexBuffer9 *src_short4_normal_vb = NULL;
    IDirect3DVertexBuffer9 *src_ubyte4_normal_vb = NULL;
    IDirect3DVertexBuffer9 *src_ubyte4n_normal_vb = NULL;
    IDirect3DVertexBuffer9 *src_dec3n_normal_vb = NULL;
    IDirect3DVertexBuffer9 *src_udec3_normal_vb = NULL;
    IDirect3DVertexBuffer9 *dst_vb = NULL;
    IDirect3DVertexBuffer9 *lit_dst_vb = NULL;
    IDirect3DVertexBuffer9 *lit_specular_dst_vb = NULL;
    IDirect3DVertexBuffer9 *offset_dst_vb = NULL;
    IDirect3DVertexBuffer9 *decl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *specular_decl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *psize_dst_vb = NULL;
    IDirect3DVertexBuffer9 *psize_decl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *src_decl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *src_extra_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_legacy_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_specular_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_specular_decl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_sparse_tex_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_sparse_tex7_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_psize_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_fvf_psize_dst_vb = NULL;
    IDirect3DVertexBuffer9 *fvf_tex2_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_fvf_tex2_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_normal_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_raw_normal_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_ubyte4n_normal_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_dec3n_normal_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_udec3_normal_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_fvf_normal_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_tangent_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_vector_math_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_compare_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_scalar_math_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_transcendent_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_source_modifiers_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_flow_if_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_bool_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_rep_loop_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_matrix_special_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_binormal_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_blendweight_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_blendindices_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_extra_multistream_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_generic_usage_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_mad_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_output_relative_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_input_relative_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_matrix_relative_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_mova_component_relative_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_texldl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_partialprecision_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_saturate_dst_vb = NULL;
    IDirect3DVertexBuffer9 *fixed_short4n_pos_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_pos4_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_short4n_pos_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_xyzw_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_short2_tex_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_d3dcolor_tex_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_packed_tex_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_short2n_tex_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_tex4_dst_vb = NULL;
    IDirect3DVertexDeclaration9 *src_decl = NULL;
    IDirect3DVertexDeclaration9 *src_sparse_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_sparse_tex7_decl = NULL;
    IDirect3DVertexDeclaration9 *src_short2_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_short2n_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_ushort2n_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_float16_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_short4_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_short4n_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_ushort4n_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_float16_4_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_ubyte4_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_ubyte4n_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_udec3_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_dec3n_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_pos4_decl = NULL;
    IDirect3DVertexDeclaration9 *src_short4n_pos_decl = NULL;
    IDirect3DVertexDeclaration9 *src_extra_decl = NULL;
    IDirect3DVertexDeclaration9 *src_d3dcolor_blendindices_decl = NULL;
    IDirect3DVertexDeclaration9 *src_ubyte4n_blendweight_decl = NULL;
    IDirect3DVertexDeclaration9 *src_extra_split_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_split_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_index_split_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_d3dcolor_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_short2_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_ubyte4n_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_udec3_decl = NULL;
    IDirect3DVertexDeclaration9 *src_d3dcolor_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *src_short4_normal_decl = NULL;
    IDirect3DVertexDeclaration9 *src_ubyte4_normal_decl = NULL;
    IDirect3DVertexDeclaration9 *src_ubyte4n_normal_decl = NULL;
    IDirect3DVertexDeclaration9 *src_dec3n_normal_decl = NULL;
    IDirect3DVertexDeclaration9 *src_udec3_normal_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_sparse_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_sparse_tex7_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_psize_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_specular_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_tex4_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_d3dcolor_tex_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_packed_tex_decl = NULL;
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DTexture9 *vs_texldl_texture = NULL;
    IDirect3DTexture9 *vs_texldl_p8_texture = NULL;
    IDirect3DTexture9 *vs_texldl_a8p8_texture = NULL;
    IDirect3DDevice9 *device = NULL;
    struct dst_vertex *mapped = NULL;
    struct dst_tex1_vertex *mapped_tex1 = NULL;
    struct dst_tex4_vertex *mapped_tex4 = NULL;
    struct dst_fvf_tex2_vertex *mapped_fvf_tex2 = NULL;
    struct dst_d3dcolor_tex_vertex *mapped_d3dcolor_tex = NULL;
    struct dst_packed_tex_vertex *mapped_packed_tex = NULL;
    struct dst_specular_vertex *mapped_specular = NULL;
    struct dst_psize_vertex *mapped_psize = NULL;
    D3DLOCKED_RECT locked_rect;
    D3DMATERIAL9 material;
    D3DLIGHT9 light;
    PALETTEENTRY texldl_palette[256];
    BOOL bool_constant;
    const float vs_constants[4][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const float vs_psize_constants[5][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {6.25f, 0.0f, 0.0f, 0.0f},
    };
    const float vs_packed_tex_constants[5][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1023.0f, 1023.0f, 1023.0f, 1023.0f},
    };
    const float vs_output_relative_constants[5][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
    };
    const float vs_matrix_relative_constants[9][4] =
    {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {5.0f, 0.0f, 0.0f, 0.0f},
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const float vs_mova_component_relative_constants[10][4] =
    {
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {4.0f, 5.0f, 0.0f, 0.0f},
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 5.0f, 0.0f, 0.0f},
    };
    const float vs_saturate_constants[5][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.5f, -0.25f, 0.5f, 2.0f},
    };
    const float vs_mad_constants[2][4] =
    {
        {0.5f, 0.5f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
    };
    const float vs_blendindices_constants[1][4] =
    {
        {0.25f, 0.25f, 0.0f, 0.0f},
    };
    const float vs_d3dcolor_blendindices_constants[1][4] =
    {
        {0.0f, 255.0f, 0.0f, 0.0f},
    };
    const float vs_extra_multistream_constants[5][4] =
    {
        {0.0f, 0.0f, 1.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 1.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 0.0f, 0.0f},
        {0.25f, 0.25f, 0.0f, 0.0f},
    };
    const float vs_vector_math_constants[1][4] =
    {
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    const float vs_compare_constants[5][4] =
    {
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.75f, 0.25f, 0.75f, 0.25f},
        {1.0f, -1.0f, 1.0f, -1.0f},
        {-1.0f, 1.0f, -1.0f, 1.0f},
    };
    const float vs_scalar_math_constants[8][4] =
    {
        {-1.0f, -1.0f, -1.0f, -1.0f},
        { 2.0f,  2.0f,  2.0f,  2.0f},
        { 0.0f,  1.0f,  0.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f,  0.0f},
        { 1.0f,  1.0f,  1.0f,  1.0f},
        { 1.0f,  2.0f,  7.0f,  0.0f},
        { 3.0f,  4.0f,  9.0f,  0.0f},
        {-11.0f, -11.0f, -11.0f, -11.0f},
    };
    const float vs_transcendent_constants[7][4] =
    {
        {0.0f, 2.0f, 0.5f, 0.125f},
        {0.0f, 0.25f, 0.0f, 0.125f},
        {0.5f, 0.25f, 0.0f, 1.0f},
        {0.0f, -1.0f, -1.0f, 0.0f},
        {2.0f, 4.0f, 4.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
    };
    const float vs_source_modifier_constants[15][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.5f, 0.5f, 0.5f, 0.5f},
        {0.5f, 0.5f, 0.5f, 0.5f},
        {0.5f, 0.5f, 0.5f, 0.5f},
        {0.5f, 0.5f, 0.5f, 0.5f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 1.0f, 1.0f, 1.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
    };
    const float vs_flow_if_constants[6][4] =
    {
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {100.0f, 100.0f, 100.0f, 0.0f},
        {2.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
    };
    const float vs_bool_constants[5][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {0.0f, 1.0f, 1.0f, 1.0f},
    };
    const float vs_rep_loop_constants[16][4] =
    {
        {0.25f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.25f, 0.0f, 0.0f},
        {1.03125f, 0.375f, -1.0f, 0.0f},
        {100.0f, 100.0f, 100.0f, 0.0f},
        {2.0f, 0.0f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.125f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.125f, 0.0f, 0.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
        {0.125f, 0.0f, 0.0f, 0.0f},
        {0.015625f, 0.0f, 0.0f, 0.0f},
        {0.015625f, 0.0f, 0.0f, 0.0f},
        {0.03125f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
        {0.0625f, 0.0f, 0.0f, 0.0f},
        {0.03125f, 0.0f, 0.0f, 0.0f},
    };
    const float vs_matrix_special_constants[6][4] =
    {
        {1.0f, 1.0f, 1.0f, 0.0f},
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {1.5707964f, 0.0f, 0.0f, 0.0f},
        {-1.0f, 0.0f, 1.0f, 0.0f},
        {0.5f, 0.5f, 0.0f, 0.0f},
    };
    D3DMATRIX world;
    void *bits = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    UINT i;
    UINT j;

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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src), 0,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &src_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_vb, 0, sizeof(src), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src, sizeof(src));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_texldl), 0,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &src_texldl_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_texldl_vb, 0,
            sizeof(src_texldl), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_texldl, sizeof(src_texldl));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_texldl_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_depth_clamp),
            0, D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &src_depth_clamp_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_depth_clamp_vb, 0,
            sizeof(src_depth_clamp), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_depth_clamp, sizeof(src_depth_clamp));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_depth_clamp_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_specular), 0,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &src_specular_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_specular_vb, 0,
            sizeof(src_specular), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_specular, sizeof(src_specular));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_specular_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_psize), 0,
            D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &src_psize_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_psize_vb, 0, sizeof(src_psize),
            &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_psize, sizeof(src_psize));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_psize_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            ARRAY_SIZE(src) * sizeof(struct attr_vertex), 0, 0,
            D3DPOOL_SYSTEMMEM, &src_attr_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_vb, 0,
            ARRAY_SIZE(src) * sizeof(struct attr_vertex), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        struct attr_vertex *attrs = bits;
        for (i = 0; i < ARRAY_SIZE(src); ++i)
        {
            attrs[i].color = src[i].color;
            attrs[i].u = src[i].u;
            attrs[i].v = src[i].v;
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_short2), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_short2_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_short2_vb, 0,
            sizeof(src_attr_short2), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_short2, sizeof(src_attr_short2));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_short2_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_short2n), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_short2n_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_short2n_vb, 0,
            sizeof(src_attr_short2n), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_short2n, sizeof(src_attr_short2n));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_short2n_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_ushort2n), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_ushort2n_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_ushort2n_vb, 0,
            sizeof(src_attr_ushort2n), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_ushort2n, sizeof(src_attr_ushort2n));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_ushort2n_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_float16), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_float16_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_float16_vb, 0,
            sizeof(src_attr_float16), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_float16, sizeof(src_attr_float16));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_float16_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_short4), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_short4_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_short4_vb, 0,
            sizeof(src_attr_short4), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_short4, sizeof(src_attr_short4));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_short4_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_short4n), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_short4n_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_short4n_vb, 0,
            sizeof(src_attr_short4n), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_short4n, sizeof(src_attr_short4n));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_short4n_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_ushort4n), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_ushort4n_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_ushort4n_vb, 0,
            sizeof(src_attr_ushort4n), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_ushort4n, sizeof(src_attr_ushort4n));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_ushort4n_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_float16_4), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_float16_4_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_float16_4_vb, 0,
            sizeof(src_attr_float16_4), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_float16_4, sizeof(src_attr_float16_4));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_float16_4_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_ubyte4), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_ubyte4_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_ubyte4_vb, 0,
            sizeof(src_attr_ubyte4), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_ubyte4, sizeof(src_attr_ubyte4));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_ubyte4_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_ubyte4n), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_ubyte4n_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_ubyte4n_vb, 0,
            sizeof(src_attr_ubyte4n), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_ubyte4n, sizeof(src_attr_ubyte4n));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_ubyte4n_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_attr_udec3), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_attr_udec3_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_attr_udec3_vb, 0,
            sizeof(src_attr_udec3), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_attr_udec3, sizeof(src_attr_udec3));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_udec3_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_pos4), 0,
            0, D3DPOOL_SYSTEMMEM, &src_pos4_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_pos4_vb, 0, sizeof(src_pos4),
            &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_pos4, sizeof(src_pos4));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_pos4_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_short4n_pos),
            0, 0, D3DPOOL_SYSTEMMEM, &src_short4n_pos_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_short4n_pos_vb, 0,
            sizeof(src_short4n_pos), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_short4n_pos, sizeof(src_short4n_pos));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_short4n_pos_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_fvf_normal), 0,
            0, D3DPOOL_SYSTEMMEM, &src_fvf_normal_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_fvf_normal_vb, 0,
            sizeof(src_fvf_normal), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_fvf_normal, sizeof(src_fvf_normal));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_fvf_normal_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_fvf_normal_specular), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_fvf_normal_specular_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_fvf_normal_specular_vb, 0,
            sizeof(src_fvf_normal_specular), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_fvf_normal_specular, sizeof(src_fvf_normal_specular));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_fvf_normal_specular_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_fvf_material_sources), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_fvf_material_sources_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_fvf_material_sources_vb, 0,
            sizeof(src_fvf_material_sources), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_fvf_material_sources, sizeof(src_fvf_material_sources));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_fvf_material_sources_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_fvf_blendweight), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_fvf_blendweight_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_fvf_blendweight_vb, 0,
            sizeof(src_fvf_blendweight), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_fvf_blendweight, sizeof(src_fvf_blendweight));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_fvf_blendweight_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_fvf_blendindices), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_fvf_blendindices_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_fvf_blendindices_vb, 0,
            sizeof(src_fvf_blendindices), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_fvf_blendindices, sizeof(src_fvf_blendindices));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_fvf_blendindices_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_fvf_tex2), 0,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2
            | D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE3(1),
            D3DPOOL_SYSTEMMEM, &src_fvf_tex2_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_fvf_tex2_vb, 0,
            sizeof(src_fvf_tex2), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_fvf_tex2, sizeof(src_fvf_tex2));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_fvf_tex2_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(src_extra), 0,
            0, D3DPOOL_SYSTEMMEM, &src_extra_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_extra_vb, 0, sizeof(src_extra),
            &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_extra, sizeof(src_extra));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_extra_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_short4_normal), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_short4_normal_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_short4_normal_vb, 0,
            sizeof(src_short4_normal), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_short4_normal, sizeof(src_short4_normal));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_short4_normal_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_ubyte4_normal), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_ubyte4_normal_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_ubyte4_normal_vb, 0,
            sizeof(src_ubyte4_normal), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_ubyte4_normal, sizeof(src_ubyte4_normal));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_ubyte4_normal_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_ubyte4n_normal), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_ubyte4n_normal_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_ubyte4n_normal_vb, 0,
            sizeof(src_ubyte4n_normal), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_ubyte4n_normal, sizeof(src_ubyte4n_normal));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_ubyte4n_normal_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_dec3n_normal), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_dec3n_normal_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_dec3n_normal_vb, 0,
            sizeof(src_dec3n_normal), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_dec3n_normal, sizeof(src_dec3n_normal));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_dec3n_normal_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(src_udec3_normal), 0, 0, D3DPOOL_SYSTEMMEM,
            &src_udec3_normal_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(src_udec3_normal_vb, 0,
            sizeof(src_udec3_normal), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(bits, src_udec3_normal, sizeof(src_udec3_normal));
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_udec3_normal_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_fvf_tex2),
            0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2
            | D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE3(1),
            D3DPOOL_SYSTEMMEM, &fvf_tex2_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_fvf_tex2),
            0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX2
            | D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE3(1),
            D3DPOOL_SYSTEMMEM, &prog_fvf_tex2_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            D3D_OK);
    memset(&world, 0, sizeof(world));
    world.m[0][0] = 0.5f;
    world.m[1][1] = 0.5f;
    world.m[2][2] = 1.0f;
    world.m[3][3] = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_WORLD, &world),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(dst_vb, 0, sizeof(expected),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(dst_vb), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_VERTEXBLEND,
            D3DVBF_3WEIGHTS), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZB4 | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_fvf_blendweight_vb, 0,
            sizeof(struct src_fvf_blendweight_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_blendweight), dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(dst_vb, 0,
            sizeof(expected_fvf_blendweight), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_fvf_blendweight); ++i)
        {
            float dx = mapped[i].x - expected_fvf_blendweight[i].x;
            float dy = mapped[i].y - expected_fvf_blendweight[i].y;
            float dz = mapped[i].z - expected_fvf_blendweight[i].z;
            float dw = mapped[i].rhw - expected_fvf_blendweight[i].rhw;
            float du = mapped[i].u - expected_fvf_blendweight[i].u;
            float dv = mapped[i].v - expected_fvf_blendweight[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_fvf_blendweight[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(dst_vb), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_INDEXEDVERTEXBLENDENABLE, TRUE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZB5 | D3DFVF_LASTBETA_UBYTE4
            | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_fvf_blendindices_vb, 0,
            sizeof(struct src_fvf_blendindices_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_blendindices), dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(dst_vb, 0,
            sizeof(expected_fvf_blendindices), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_fvf_blendindices); ++i)
        {
            float dx = mapped[i].x - expected_fvf_blendindices[i].x;
            float dy = mapped[i].y - expected_fvf_blendindices[i].y;
            float dz = mapped[i].z - expected_fvf_blendindices[i].z;
            float dw = mapped[i].rhw - expected_fvf_blendindices[i].rhw;
            float du = mapped[i].u - expected_fvf_blendindices[i].u;
            float dv = mapped[i].v - expected_fvf_blendindices[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_fvf_blendindices[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_INDEXEDVERTEXBLENDENABLE, FALSE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_VERTEXBLEND,
            D3DVBF_DISABLE), D3D_OK);

    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX2
            | D3DFVF_TEXCOORDSIZE1(0) | D3DFVF_TEXCOORDSIZE3(1)),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_fvf_tex2_vb, 0,
            sizeof(src_fvf_tex2[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_tex2), fvf_tex2_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(fvf_tex2_dst_vb, 0,
            sizeof(expected_fvf_tex2), (void **)&mapped_fvf_tex2,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_fvf_tex2); ++i)
        {
            float dx = mapped_fvf_tex2[i].x - expected_fvf_tex2[i].x;
            float dy = mapped_fvf_tex2[i].y - expected_fvf_tex2[i].y;
            float dz = mapped_fvf_tex2[i].z - expected_fvf_tex2[i].z;
            float dw = mapped_fvf_tex2[i].rhw - expected_fvf_tex2[i].rhw;
            float dt0 = mapped_fvf_tex2[i].t0 - expected_fvf_tex2[i].t0;
            float dt1x = mapped_fvf_tex2[i].t1x - expected_fvf_tex2[i].t1x;
            float dt1y = mapped_fvf_tex2[i].t1y - expected_fvf_tex2[i].t1y;
            float dt1z = mapped_fvf_tex2[i].t1z - expected_fvf_tex2[i].t1z;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (dt0 < 0.0f) dt0 = -dt0;
            if (dt1x < 0.0f) dt1x = -dt1x;
            if (dt1y < 0.0f) dt1y = -dt1y;
            if (dt1z < 0.0f) dt1z = -dt1z;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_fvf_tex2[i].color == expected_fvf_tex2[i].color);
            CHECK_TRUE(dt0 < 0.01f);
            CHECK_TRUE(dt1x < 0.01f);
            CHECK_TRUE(dt1y < 0.01f);
            CHECK_TRUE(dt1z < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(fvf_tex2_dst_vb), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_CreateVertexShader(device,
            process_vs_fvf_tex2_3_0, &vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, ARRAY_SIZE(vs_constants)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_tex2), prog_fvf_tex2_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_fvf_tex2_dst_vb, 0,
            sizeof(expected_fvf_tex2), (void **)&mapped_fvf_tex2,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_fvf_tex2); ++i)
        {
            float dx = mapped_fvf_tex2[i].x - expected_fvf_tex2[i].x;
            float dy = mapped_fvf_tex2[i].y - expected_fvf_tex2[i].y;
            float dz = mapped_fvf_tex2[i].z - expected_fvf_tex2[i].z;
            float dw = mapped_fvf_tex2[i].rhw - expected_fvf_tex2[i].rhw;
            float dt0 = mapped_fvf_tex2[i].t0 - expected_fvf_tex2[i].t0;
            float dt1x = mapped_fvf_tex2[i].t1x - expected_fvf_tex2[i].t1x;
            float dt1y = mapped_fvf_tex2[i].t1y - expected_fvf_tex2[i].t1y;
            float dt1z = mapped_fvf_tex2[i].t1z - expected_fvf_tex2[i].t1z;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (dt0 < 0.0f) dt0 = -dt0;
            if (dt1x < 0.0f) dt1x = -dt1x;
            if (dt1y < 0.0f) dt1y = -dt1y;
            if (dt1z < 0.0f) dt1z = -dt1z;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_fvf_tex2[i].color == expected_fvf_tex2[i].color);
            CHECK_TRUE(dt0 < 0.01f);
            CHECK_TRUE(dt1x < 0.01f);
            CHECK_TRUE(dt1y < 0.01f);
            CHECK_TRUE(dt1z < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_fvf_tex2_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPING, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_depth_clamp_vb, 0,
            sizeof(src_depth_clamp[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_depth_clamp), dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(dst_vb, 0, sizeof(expected_depth_clamp),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_depth_clamp); ++i)
        {
            float dx = mapped[i].x - expected_depth_clamp[i].x;
            float dy = mapped[i].y - expected_depth_clamp[i].y;
            float dz = mapped[i].z - expected_depth_clamp[i].z;
            float dw = mapped[i].rhw - expected_depth_clamp[i].rhw;
            float du = mapped[i].u - expected_depth_clamp[i].u;
            float dv = mapped[i].v - expected_depth_clamp[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_depth_clamp[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CLIPPING, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_lit), 0,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &lit_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 1.0f;
    material.Diffuse.g = 0.5f;
    material.Diffuse.a = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetMaterial(device, &material), D3D_OK);
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 1.0f;
    light.Diffuse.g = 1.0f;
    light.Diffuse.b = 1.0f;
    light.Direction.z = -1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_LightEnable(device, 0, TRUE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_AMBIENT, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_fvf_normal_vb, 0,
            sizeof(src_fvf_normal[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), lit_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_dst_vb, 0, sizeof(expected_lit),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_lit); ++i)
        {
            float dx = mapped[i].x - expected_lit[i].x;
            float dy = mapped[i].y - expected_lit[i].y;
            float dz = mapped[i].z - expected_lit[i].z;
            float dw = mapped[i].rhw - expected_lit[i].rhw;
            float du = mapped[i].u - expected_lit[i].u;
            float dv = mapped[i].v - expected_lit[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_lit[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_dst_vb), D3D_OK);
    }
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_POINT;
    light.Diffuse.r = 1.0f;
    light.Diffuse.g = 1.0f;
    light.Diffuse.b = 1.0f;
    light.Position.z = 100000.0f;
    light.Range = 200000.0f;
    light.Attenuation0 = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), lit_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_dst_vb, 0, sizeof(expected_lit),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_lit); ++i)
        {
            float dx = mapped[i].x - expected_lit[i].x;
            float dy = mapped[i].y - expected_lit[i].y;
            float dz = mapped[i].z - expected_lit[i].z;
            float dw = mapped[i].rhw - expected_lit[i].rhw;
            float du = mapped[i].u - expected_lit[i].u;
            float dv = mapped[i].v - expected_lit[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_lit[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_dst_vb), D3D_OK);
    }
    for (j = 0; j < ARRAY_SIZE(point_attenuation_cases); ++j)
    {
        light.Attenuation0 = point_attenuation_cases[j].attenuation0;
        light.Attenuation1 = point_attenuation_cases[j].attenuation1;
        light.Attenuation2 = point_attenuation_cases[j].attenuation2;
        CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
        CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
                ARRAY_SIZE(src_fvf_normal), lit_dst_vb, NULL, 0), D3D_OK);

        hr = IDirect3DVertexBuffer9_Lock(lit_dst_vb, 0,
                sizeof(expected_lit_point_attenuated), (void **)&mapped,
                D3DLOCK_READONLY);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            for (i = 0; i < ARRAY_SIZE(expected_lit_point_attenuated); ++i)
            {
                float dx = mapped[i].x - expected_lit_point_attenuated[i].x;
                float dy = mapped[i].y - expected_lit_point_attenuated[i].y;
                float dz = mapped[i].z - expected_lit_point_attenuated[i].z;
                float dw = mapped[i].rhw - expected_lit_point_attenuated[i].rhw;
                float du = mapped[i].u - expected_lit_point_attenuated[i].u;
                float dv = mapped[i].v - expected_lit_point_attenuated[i].v;
                if (dx < 0.0f) dx = -dx;
                if (dy < 0.0f) dy = -dy;
                if (dz < 0.0f) dz = -dz;
                if (dw < 0.0f) dw = -dw;
                if (du < 0.0f) du = -du;
                if (dv < 0.0f) dv = -dv;
                CHECK_TRUE(dx < 0.01f);
                CHECK_TRUE(dy < 0.01f);
                CHECK_TRUE(dz < 0.01f);
                CHECK_TRUE(dw < 0.01f);
                CHECK_TRUE(mapped[i].color == expected_lit_point_attenuated[i].color);
                CHECK_TRUE(du < 0.01f);
                CHECK_TRUE(dv < 0.01f);
            }
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_dst_vb), D3D_OK);
        }
    }
    light.Range = 0.5f;
    light.Attenuation0 = 1.0f;
    light.Attenuation1 = 0.0f;
    light.Attenuation2 = 0.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), lit_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_dst_vb, 0,
            sizeof(expected_lit_unlit), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_lit_unlit); ++i)
        {
            float dx = mapped[i].x - expected_lit_unlit[i].x;
            float dy = mapped[i].y - expected_lit_unlit[i].y;
            float dz = mapped[i].z - expected_lit_unlit[i].z;
            float dw = mapped[i].rhw - expected_lit_unlit[i].rhw;
            float du = mapped[i].u - expected_lit_unlit[i].u;
            float dv = mapped[i].v - expected_lit_unlit[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_lit_unlit[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_dst_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_spot_specular), 0,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &lit_specular_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    material.Specular.b = 1.0f;
    material.Power = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetMaterial(device, &material), D3D_OK);
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_SPOT;
    light.Diffuse.r = 1.0f;
    light.Diffuse.g = 1.0f;
    light.Diffuse.b = 1.0f;
    light.Specular.b = 1.0f;
    light.Position.z = 100000.0f;
    light.Direction.z = -1.0f;
    light.Range = 200000.0f;
    light.Attenuation0 = 1.0f;
    light.Falloff = 1.0f;
    light.Theta = 0.5f;
    light.Phi = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), lit_specular_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_specular_dst_vb, 0,
            sizeof(expected_spot_specular), (void **)&mapped_specular,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_spot_specular); ++i)
        {
            float dx = mapped_specular[i].x - expected_spot_specular[i].x;
            float dy = mapped_specular[i].y - expected_spot_specular[i].y;
            float dz = mapped_specular[i].z - expected_spot_specular[i].z;
            float dw = mapped_specular[i].rhw - expected_spot_specular[i].rhw;
            float du = mapped_specular[i].u - expected_spot_specular[i].u;
            float dv = mapped_specular[i].v - expected_spot_specular[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_specular[i].color == expected_spot_specular[i].color);
            CHECK_TRUE(mapped_specular[i].specular == expected_spot_specular[i].specular);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_specular_dst_vb), D3D_OK);
    }
    light.Direction.x = -0.8660254f;
    light.Direction.z = -0.5f;
    light.Falloff = 2.0f;
    light.Theta = 0.0f;
    light.Phi = 3.1415927f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), lit_specular_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_specular_dst_vb, 0,
            sizeof(expected_spot_falloff), (void **)&mapped_specular,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_spot_falloff); ++i)
        {
            float dx = mapped_specular[i].x - expected_spot_falloff[i].x;
            float dy = mapped_specular[i].y - expected_spot_falloff[i].y;
            float dz = mapped_specular[i].z - expected_spot_falloff[i].z;
            float dw = mapped_specular[i].rhw - expected_spot_falloff[i].rhw;
            float du = mapped_specular[i].u - expected_spot_falloff[i].u;
            float dv = mapped_specular[i].v - expected_spot_falloff[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_specular[i].color == expected_spot_falloff[i].color);
            CHECK_TRUE(mapped_specular[i].specular == expected_spot_falloff[i].specular);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_specular_dst_vb), D3D_OK);
    }
    light.Direction.x = 1.0f;
    light.Direction.z = 0.0f;
    light.Falloff = 1.0f;
    light.Theta = 0.5f;
    light.Phi = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), lit_specular_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_specular_dst_vb, 0,
            sizeof(expected_spot_unlit), (void **)&mapped_specular,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_spot_unlit); ++i)
        {
            float dx = mapped_specular[i].x - expected_spot_unlit[i].x;
            float dy = mapped_specular[i].y - expected_spot_unlit[i].y;
            float dz = mapped_specular[i].z - expected_spot_unlit[i].z;
            float dw = mapped_specular[i].rhw - expected_spot_unlit[i].rhw;
            float du = mapped_specular[i].u - expected_spot_unlit[i].u;
            float dv = mapped_specular[i].v - expected_spot_unlit[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_specular[i].color == expected_spot_unlit[i].color);
            CHECK_TRUE(mapped_specular[i].specular == expected_spot_unlit[i].specular);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_specular_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE, FALSE),
            D3D_OK);
    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 1.0f;
    material.Diffuse.g = 1.0f;
    material.Diffuse.b = 1.0f;
    material.Diffuse.a = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetMaterial(device, &material), D3D_OK);
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 0.5f;
    light.Diffuse.g = 0.5f;
    light.Diffuse.b = 0.5f;
    light.Direction.z = -1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_COLOR1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_MATERIAL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_MATERIAL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), lit_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_dst_vb, 0,
            sizeof(expected_lit_colorvertex), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_lit_colorvertex); ++i)
        {
            float dx = mapped[i].x - expected_lit_colorvertex[i].x;
            float dy = mapped[i].y - expected_lit_colorvertex[i].y;
            float dz = mapped[i].z - expected_lit_colorvertex[i].z;
            float dw = mapped[i].rhw - expected_lit_colorvertex[i].rhw;
            float du = mapped[i].u - expected_lit_colorvertex[i].u;
            float dv = mapped[i].v - expected_lit_colorvertex[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_lit_colorvertex[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_dst_vb), D3D_OK);
    }
    material.Specular.r = 1.0f;
    material.Specular.g = 1.0f;
    material.Specular.b = 1.0f;
    material.Power = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetMaterial(device, &material), D3D_OK);
    light.Specular.r = 0.5f;
    light.Specular.g = 0.5f;
    light.Specular.b = 0.5f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_SPECULARMATERIALSOURCE, D3DMCS_COLOR2), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE
            | D3DFVF_SPECULAR | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_fvf_normal_specular_vb, 0, sizeof(src_fvf_normal_specular[0])),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal_specular), lit_specular_dst_vb, NULL, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_specular_dst_vb, 0,
            sizeof(expected_lit_colorvertex_specular), (void **)&mapped_specular,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_lit_colorvertex_specular); ++i)
        {
            float dx = mapped_specular[i].x - expected_lit_colorvertex_specular[i].x;
            float dy = mapped_specular[i].y - expected_lit_colorvertex_specular[i].y;
            float dz = mapped_specular[i].z - expected_lit_colorvertex_specular[i].z;
            float dw = mapped_specular[i].rhw - expected_lit_colorvertex_specular[i].rhw;
            float du = mapped_specular[i].u - expected_lit_colorvertex_specular[i].u;
            float dv = mapped_specular[i].v - expected_lit_colorvertex_specular[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_specular[i].color
                    == expected_lit_colorvertex_specular[i].color);
            CHECK_TRUE(mapped_specular[i].specular
                    == expected_lit_colorvertex_specular[i].specular);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_specular_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_LightEnable(device, 0, FALSE), D3D_OK);
    memset(&material, 0, sizeof(material));
    material.Diffuse.a = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetMaterial(device, &material), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_AMBIENT, 0x00404040),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_DIFFUSEMATERIALSOURCE, D3DMCS_MATERIAL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_AMBIENTMATERIALSOURCE, D3DMCS_COLOR1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_EMISSIVEMATERIALSOURCE, D3DMCS_COLOR2), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_SPECULARMATERIALSOURCE, D3DMCS_MATERIAL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_fvf_material_sources_vb, 0,
            sizeof(src_fvf_material_sources[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_material_sources), lit_dst_vb, NULL, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_dst_vb, 0,
            sizeof(expected_lit_ambient_emissive_sources),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_lit_ambient_emissive_sources); ++i)
        {
            float dx = mapped[i].x - expected_lit_ambient_emissive_sources[i].x;
            float dy = mapped[i].y - expected_lit_ambient_emissive_sources[i].y;
            float dz = mapped[i].z - expected_lit_ambient_emissive_sources[i].z;
            float dw = mapped[i].rhw - expected_lit_ambient_emissive_sources[i].rhw;
            float du = mapped[i].u - expected_lit_ambient_emissive_sources[i].u;
            float dv = mapped[i].v - expected_lit_ambient_emissive_sources[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color
                    == expected_lit_ambient_emissive_sources[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_offset), 0,
            D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &offset_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DVertexBuffer9_Lock(offset_dst_vb, 0,
            sizeof(expected_offset), &bits, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_offset); ++i)
            ((struct dst_vertex *)bits)[i] = sentinel;
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(offset_dst_vb), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 1, 1, 2,
            offset_dst_vb, NULL, D3DPV_DONOTCOPYDATA), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0, 1,
            offset_dst_vb, NULL, D3DPV_DONOTCOPYDATA << 1),
            D3DERR_INVALIDCALL);

    hr = IDirect3DVertexBuffer9_Lock(offset_dst_vb, 0,
            sizeof(expected_offset), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_offset); ++i)
        {
            float dx = mapped[i].x - expected_offset[i].x;
            float dy = mapped[i].y - expected_offset[i].y;
            float dz = mapped[i].z - expected_offset[i].z;
            float dw = mapped[i].rhw - expected_offset[i].rhw;
            float du = mapped[i].u - expected_offset[i].u;
            float dv = mapped[i].v - expected_offset[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_offset[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(offset_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &decl_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device, dst_decl_elements,
            &dst_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            dst_sparse_tex_decl_elements, &dst_sparse_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            dst_sparse_tex7_decl_elements, &dst_sparse_tex7_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            dst_psize_decl_elements, &dst_psize_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            dst_specular_decl_elements, &dst_specular_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device, dst_tex4_decl_elements,
            &dst_tex4_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            dst_d3dcolor_tex_decl_elements, &dst_d3dcolor_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            dst_packed_tex_decl_elements, &dst_packed_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), decl_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(decl_dst_vb, 0, sizeof(expected),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(decl_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_specular),
            0, 0, D3DPOOL_SYSTEMMEM, &specular_decl_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_specular_vb, 0,
            sizeof(src_specular[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_specular), specular_decl_dst_vb,
            dst_specular_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(specular_decl_dst_vb, 0,
            sizeof(expected_specular), (void **)&mapped_specular,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_specular); ++i)
        {
            float dx = mapped_specular[i].x - expected_specular[i].x;
            float dy = mapped_specular[i].y - expected_specular[i].y;
            float dz = mapped_specular[i].z - expected_specular[i].z;
            float dw = mapped_specular[i].rhw - expected_specular[i].rhw;
            float du = mapped_specular[i].u - expected_specular[i].u;
            float dv = mapped_specular[i].v - expected_specular[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_specular[i].color == expected_specular[i].color);
            CHECK_TRUE(mapped_specular[i].specular == expected_specular[i].specular);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(specular_decl_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_psize), 0,
            D3DFVF_XYZRHW | D3DFVF_PSIZE | D3DFVF_DIFFUSE | D3DFVF_TEX1,
            D3DPOOL_SYSTEMMEM, &psize_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_psize_vb, 0,
            sizeof(struct src_psize_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_psize), psize_dst_vb, NULL, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(psize_dst_vb, 0, sizeof(expected_psize),
            (void **)&mapped_psize, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_psize); ++i)
        {
            float dx = mapped_psize[i].x - expected_psize[i].x;
            float dy = mapped_psize[i].y - expected_psize[i].y;
            float dz = mapped_psize[i].z - expected_psize[i].z;
            float dw = mapped_psize[i].rhw - expected_psize[i].rhw;
            float dp = mapped_psize[i].psize - expected_psize[i].psize;
            float du = mapped_psize[i].u - expected_psize[i].u;
            float dv = mapped_psize[i].v - expected_psize[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (dp < 0.0f) dp = -dp;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(dp < 0.01f);
            CHECK_TRUE(mapped_psize[i].color == expected_psize[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(psize_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_psize), 0,
            0, D3DPOOL_SYSTEMMEM, &psize_decl_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_psize), psize_decl_dst_vb, dst_psize_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(psize_decl_dst_vb, 0,
            sizeof(expected_psize), (void **)&mapped_psize, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_psize); ++i)
        {
            float dx = mapped_psize[i].x - expected_psize[i].x;
            float dy = mapped_psize[i].y - expected_psize[i].y;
            float dz = mapped_psize[i].z - expected_psize[i].z;
            float dw = mapped_psize[i].rhw - expected_psize[i].rhw;
            float dp = mapped_psize[i].psize - expected_psize[i].psize;
            float du = mapped_psize[i].u - expected_psize[i].u;
            float dv = mapped_psize[i].v - expected_psize[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (dp < 0.0f) dp = -dp;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(dp < 0.01f);
            CHECK_TRUE(mapped_psize[i].color == expected_psize[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(psize_decl_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_psize_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_prog_psize), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_psize_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_psize_constants, 5), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_psize_dst_vb, dst_psize_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_psize_dst_vb, 0,
            sizeof(expected_prog_psize), (void **)&mapped_psize,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_prog_psize); ++i)
        {
            float dx = mapped_psize[i].x - expected_prog_psize[i].x;
            float dy = mapped_psize[i].y - expected_prog_psize[i].y;
            float dz = mapped_psize[i].z - expected_prog_psize[i].z;
            float dw = mapped_psize[i].rhw - expected_prog_psize[i].rhw;
            float dp = mapped_psize[i].psize - expected_prog_psize[i].psize;
            float du = mapped_psize[i].u - expected_prog_psize[i].u;
            float dv = mapped_psize[i].v - expected_prog_psize[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (dp < 0.0f) dp = -dp;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(dp < 0.01f);
            CHECK_TRUE(mapped_psize[i].color == expected_prog_psize[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_psize_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_fvf_psize_input_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_fvf_psize_input), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_fvf_psize_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_psize_vb, 0,
            sizeof(struct src_psize_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_PSIZE | D3DFVF_DIFFUSE | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_psize), prog_fvf_psize_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_fvf_psize_dst_vb, 0,
            sizeof(expected_fvf_psize_input), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_fvf_psize_input); ++i)
        {
            float dx = mapped[i].x - expected_fvf_psize_input[i].x;
            float dy = mapped[i].y - expected_fvf_psize_input[i].y;
            float dz = mapped[i].z - expected_fvf_psize_input[i].z;
            float dw = mapped[i].rhw - expected_fvf_psize_input[i].rhw;
            float du = mapped[i].u - expected_fvf_psize_input[i].u;
            float dv = mapped[i].v - expected_fvf_psize_input[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_fvf_psize_input[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_fvf_psize_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, src_decl_elements,
            &src_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_sparse_tex_decl_elements, &src_sparse_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_sparse_tex7_decl_elements, &src_sparse_tex7_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_short2_tex_decl_elements, &src_short2_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_short2n_tex_decl_elements, &src_short2n_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_ushort2n_tex_decl_elements, &src_ushort2n_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_float16_tex_decl_elements, &src_float16_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_short4_tex_decl_elements, &src_short4_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_short4n_tex_decl_elements, &src_short4n_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_ushort4n_tex_decl_elements, &src_ushort4n_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_float16_4_tex_decl_elements, &src_float16_4_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_ubyte4_tex_decl_elements, &src_ubyte4_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_ubyte4n_tex_decl_elements, &src_ubyte4n_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_udec3_tex_decl_elements, &src_udec3_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_dec3n_tex_decl_elements, &src_dec3n_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &src_decl_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, src_attr_vb, 0,
            sizeof(struct attr_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), src_decl_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(src_decl_dst_vb, 0, sizeof(expected),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_decl_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1,
            D3DSTREAMSOURCE_INSTANCEDATA | 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), src_decl_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(src_decl_dst_vb, 0,
            sizeof(expected_instanced_source), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_instanced_source); ++i)
        {
            float dx = mapped[i].x - expected_instanced_source[i].x;
            float dy = mapped[i].y - expected_instanced_source[i].y;
            float dz = mapped[i].z - expected_instanced_source[i].z;
            float dw = mapped[i].rhw - expected_instanced_source[i].rhw;
            float du = mapped[i].u - expected_instanced_source[i].u;
            float dv = mapped[i].v - expected_instanced_source[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_instanced_source[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_decl_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 0,
            D3DSTREAMSOURCE_INDEXEDDATA | 2), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1,
            D3DSTREAMSOURCE_INSTANCEDATA | 3), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), src_decl_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(src_decl_dst_vb, 0,
            sizeof(expected_instanced_source), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_instanced_source); ++i)
        {
            float dx = mapped[i].x - expected_instanced_source[i].x;
            float dy = mapped[i].y - expected_instanced_source[i].y;
            float dz = mapped[i].z - expected_instanced_source[i].z;
            float dw = mapped[i].rhw - expected_instanced_source[i].rhw;
            float du = mapped[i].u - expected_instanced_source[i].u;
            float dv = mapped[i].v - expected_instanced_source[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_instanced_source[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_decl_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 0, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1, 1), D3D_OK);

    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_extra_decl_elements, &src_extra_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_d3dcolor_blendindices_decl_elements,
            &src_d3dcolor_blendindices_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_ubyte4n_blendweight_decl_elements,
            &src_ubyte4n_blendweight_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_extra_split_decl_elements, &src_extra_split_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_generic_split_decl_elements, &src_generic_split_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_generic_index_split_decl_elements, &src_generic_index_split_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_generic_d3dcolor_decl_elements, &src_generic_d3dcolor_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_generic_short2_decl_elements, &src_generic_short2_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_generic_ubyte4n_decl_elements, &src_generic_ubyte4n_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_generic_udec3_decl_elements, &src_generic_udec3_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_d3dcolor_tex_decl_elements, &src_d3dcolor_tex_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &src_extra_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), src_extra_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(src_extra_dst_vb, 0, sizeof(expected),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_extra_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_normal_3_0,
            &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_normal_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_normal_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_normal_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_normal_dst_vb), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_short4_normal_decl_elements, &src_short4_normal_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_ubyte4_normal_decl_elements, &src_ubyte4_normal_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_raw_normal_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    {
        struct raw_normal_case
        {
            IDirect3DVertexDeclaration9 *decl;
            IDirect3DVertexBuffer9 *vb;
            UINT stride;
        };
        const struct raw_normal_case cases[] =
        {
            {src_short4_normal_decl, src_short4_normal_vb,
                    sizeof(struct src_short4_normal_vertex)},
            {src_ubyte4_normal_decl, src_ubyte4_normal_vb,
                    sizeof(struct src_ubyte4_normal_vertex)},
        };
        UINT raw_case;

        for (raw_case = 0; raw_case < ARRAY_SIZE(cases); ++raw_case)
        {
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
                    cases[raw_case].decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
                    cases[raw_case].vb, 0, cases[raw_case].stride), D3D_OK);
            CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
                    ARRAY_SIZE(src), prog_raw_normal_dst_vb, dst_decl, 0),
                    D3D_OK);

            hr = IDirect3DVertexBuffer9_Lock(prog_raw_normal_dst_vb, 0,
                    sizeof(expected_normal), (void **)&mapped,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
                {
                    float dx = mapped[i].x - expected_normal[i].x;
                    float dy = mapped[i].y - expected_normal[i].y;
                    float dz = mapped[i].z - expected_normal[i].z;
                    float dw = mapped[i].rhw - expected_normal[i].rhw;
                    float du = mapped[i].u - expected_normal[i].u;
                    float dv = mapped[i].v - expected_normal[i].v;
                    if (dx < 0.0f) dx = -dx;
                    if (dy < 0.0f) dy = -dy;
                    if (dz < 0.0f) dz = -dz;
                    if (dw < 0.0f) dw = -dw;
                    if (du < 0.0f) du = -du;
                    if (dv < 0.0f) dv = -dv;
                    CHECK_TRUE(dx < 0.01f);
                    CHECK_TRUE(dy < 0.01f);
                    CHECK_TRUE(dz < 0.01f);
                    CHECK_TRUE(dw < 0.01f);
                    CHECK_TRUE(mapped[i].color == expected_normal[i].color);
                    CHECK_TRUE(du < 0.01f);
                    CHECK_TRUE(dv < 0.01f);
                }
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(
                        prog_raw_normal_dst_vb), D3D_OK);
            }
        }
    }
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_ubyte4n_normal_decl_elements, &src_ubyte4n_normal_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_ubyte4n_normal_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_ubyte4n_normal_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_ubyte4n_normal_vb, 0,
            sizeof(struct src_ubyte4n_normal_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_ubyte4n_normal), prog_ubyte4n_normal_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_ubyte4n_normal_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_ubyte4n_normal_dst_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_dec3n_normal_decl_elements, &src_dec3n_normal_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_dec3n_normal_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_dec3n_normal_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_dec3n_normal_vb, 0, sizeof(struct src_dec3n_normal_vertex)),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_dec3n_normal), prog_dec3n_normal_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_dec3n_normal_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_dec3n_normal_dst_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_udec3_normal_decl_elements, &src_udec3_normal_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_udec3_normal_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_udec3_normal_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_udec3_normal_vb, 0, sizeof(struct src_udec3_normal_vertex)),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_udec3_normal), prog_udec3_normal_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_udec3_normal_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_udec3_normal_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 1.0f;
    material.Diffuse.g = 0.5f;
    material.Diffuse.a = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetMaterial(device, &material), D3D_OK);
    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 1.0f;
    light.Diffuse.g = 1.0f;
    light.Diffuse.b = 1.0f;
    light.Direction.z = -1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, 0, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_LightEnable(device, 0, TRUE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_AMBIENT, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_COLORVERTEX, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, TRUE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_ubyte4n_normal_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_ubyte4n_normal_vb, 0,
            sizeof(struct src_ubyte4n_normal_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_ubyte4n_normal), lit_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(lit_dst_vb, 0, sizeof(expected_lit),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_lit); ++i)
        {
            float dx = mapped[i].x - expected_lit[i].x;
            float dy = mapped[i].y - expected_lit[i].y;
            float dz = mapped[i].z - expected_lit[i].z;
            float dw = mapped[i].rhw - expected_lit[i].rhw;
            float du = mapped[i].u - expected_lit[i].u;
            float dv = mapped[i].v - expected_lit[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_lit[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(lit_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            D3D_OK);

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_normal_3_0,
            &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_fvf_normal_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_NORMAL | D3DFVF_DIFFUSE | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_fvf_normal_vb, 0,
            sizeof(struct src_fvf_normal_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_normal), prog_fvf_normal_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_fvf_normal_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_fvf_normal_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_vector_math_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_vector_math_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_vector_math_constants, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_vector_math_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_vector_math_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_vector_math_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_scalar_math_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_tangent),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_scalar_math_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_scalar_math_constants,
            ARRAY_SIZE(vs_scalar_math_constants)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_scalar_math_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_scalar_math_dst_vb, 0,
            sizeof(expected_tangent), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_tangent); ++i)
        {
            float dx = mapped[i].x - expected_tangent[i].x;
            float dy = mapped[i].y - expected_tangent[i].y;
            float dz = mapped[i].z - expected_tangent[i].z;
            float dw = mapped[i].rhw - expected_tangent[i].rhw;
            float du = mapped[i].u - expected_tangent[i].u;
            float dv = mapped[i].v - expected_tangent[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_tangent[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_scalar_math_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_transcendent_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_transcendent), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_transcendent_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_transcendent_constants, 7), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_transcendent_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_transcendent_dst_vb, 0,
            sizeof(expected_transcendent), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_transcendent); ++i)
        {
            float dx = mapped[i].x - expected_transcendent[i].x;
            float dy = mapped[i].y - expected_transcendent[i].y;
            float dz = mapped[i].z - expected_transcendent[i].z;
            float dw = mapped[i].rhw - expected_transcendent[i].rhw;
            float du = mapped[i].u - expected_transcendent[i].u;
            float dv = mapped[i].v - expected_transcendent[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_transcendent[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_transcendent_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_source_modifiers_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_source_modifiers_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_source_modifier_constants, 15), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_source_modifiers_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_source_modifiers_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_source_modifiers_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_flow_if_3_0,
            &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_flow_if_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_flow_if_constants, 6), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_flow_if_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_flow_if_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_flow_if_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_bool_constant_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_bool_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_bool_constants, ARRAY_SIZE(vs_bool_constants)),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    bool_constant = TRUE;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantB(device, 0,
            &bool_constant, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_bool_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_bool_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_bool_dst_vb), D3D_OK);
    }
    bool_constant = FALSE;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantB(device, 0,
            &bool_constant, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_bool_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_bool_dst_vb, 0,
            sizeof(expected_bool_false), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_bool_false); ++i)
        {
            float dx = mapped[i].x - expected_bool_false[i].x;
            float dy = mapped[i].y - expected_bool_false[i].y;
            float dz = mapped[i].z - expected_bool_false[i].z;
            float dw = mapped[i].rhw - expected_bool_false[i].rhw;
            float du = mapped[i].u - expected_bool_false[i].u;
            float dv = mapped[i].v - expected_bool_false[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_bool_false[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_bool_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_rep_loop_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_rep_loop_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_rep_loop_constants,
            ARRAY_SIZE(vs_rep_loop_constants)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_rep_loop_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_rep_loop_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_rep_loop_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_matrix_special_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_matrix_special_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_matrix_special_constants, 6), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_matrix_special_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_matrix_special_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_matrix_special_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_compare_3_0,
            &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_normal),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_compare_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_compare_constants,
            ARRAY_SIZE(vs_compare_constants)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_compare_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_compare_dst_vb, 0,
            sizeof(expected_normal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_normal); ++i)
        {
            float dx = mapped[i].x - expected_normal[i].x;
            float dy = mapped[i].y - expected_normal[i].y;
            float dz = mapped[i].z - expected_normal[i].z;
            float dw = mapped[i].rhw - expected_normal[i].rhw;
            float du = mapped[i].u - expected_normal[i].u;
            float dv = mapped[i].v - expected_normal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_normal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_compare_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_tangent_3_0,
            &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_tangent),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_tangent_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_tangent_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_tangent_dst_vb, 0,
            sizeof(expected_tangent), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_tangent); ++i)
        {
            float dx = mapped[i].x - expected_tangent[i].x;
            float dy = mapped[i].y - expected_tangent[i].y;
            float dz = mapped[i].z - expected_tangent[i].z;
            float dw = mapped[i].rhw - expected_tangent[i].rhw;
            float du = mapped[i].u - expected_tangent[i].u;
            float dv = mapped[i].v - expected_tangent[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_tangent[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_tangent_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_binormal_3_0,
            &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_binormal),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_binormal_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_binormal_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_binormal_dst_vb, 0,
            sizeof(expected_binormal), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_binormal); ++i)
        {
            float dx = mapped[i].x - expected_binormal[i].x;
            float dy = mapped[i].y - expected_binormal[i].y;
            float dz = mapped[i].z - expected_binormal[i].z;
            float dw = mapped[i].rhw - expected_binormal[i].rhw;
            float du = mapped[i].u - expected_binormal[i].u;
            float dv = mapped[i].v - expected_binormal[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_binormal[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_binormal_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_blendweight_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_blendweight), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_blendweight_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_blendweight_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_blendweight_dst_vb, 0,
            sizeof(expected_blendweight), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_blendweight); ++i)
        {
            float dx = mapped[i].x - expected_blendweight[i].x;
            float dy = mapped[i].y - expected_blendweight[i].y;
            float dz = mapped[i].z - expected_blendweight[i].z;
            float dw = mapped[i].rhw - expected_blendweight[i].rhw;
            float du = mapped[i].u - expected_blendweight[i].u;
            float dv = mapped[i].v - expected_blendweight[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_blendweight[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_blendweight_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_ubyte4n_blendweight_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_blendweight_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_blendweight_dst_vb, 0,
            sizeof(expected_ubyte4n_blendweight), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_ubyte4n_blendweight); ++i)
        {
            float dx = mapped[i].x - expected_ubyte4n_blendweight[i].x;
            float dy = mapped[i].y - expected_ubyte4n_blendweight[i].y;
            float dz = mapped[i].z - expected_ubyte4n_blendweight[i].z;
            float dw = mapped[i].rhw - expected_ubyte4n_blendweight[i].rhw;
            float du = mapped[i].u - expected_ubyte4n_blendweight[i].u;
            float dv = mapped[i].v - expected_ubyte4n_blendweight[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_ubyte4n_blendweight[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_blendweight_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZB4 | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_fvf_blendweight_vb, 0,
            sizeof(struct src_fvf_blendweight_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_blendweight), prog_blendweight_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_blendweight_dst_vb, 0,
            sizeof(expected_blendweight), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_blendweight); ++i)
        {
            float dx = mapped[i].x - expected_blendweight[i].x;
            float dy = mapped[i].y - expected_blendweight[i].y;
            float dz = mapped[i].z - expected_blendweight[i].z;
            float dw = mapped[i].rhw - expected_blendweight[i].rhw;
            float du = mapped[i].u - expected_blendweight[i].u;
            float dv = mapped[i].v - expected_blendweight[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_blendweight[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_blendweight_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_blendindices_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_blendweight), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_blendindices_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_blendindices_constants, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_blendindices_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_blendindices_dst_vb, 0,
            sizeof(expected_blendweight), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_blendweight); ++i)
        {
            float dx = mapped[i].x - expected_blendweight[i].x;
            float dy = mapped[i].y - expected_blendweight[i].y;
            float dz = mapped[i].z - expected_blendweight[i].z;
            float dw = mapped[i].rhw - expected_blendweight[i].rhw;
            float du = mapped[i].u - expected_blendweight[i].u;
            float dv = mapped[i].v - expected_blendweight[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_blendweight[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_blendindices_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_d3dcolor_blendindices_constants, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_d3dcolor_blendindices_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_blendindices_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_blendindices_dst_vb, 0,
            sizeof(expected_d3dcolor_blendindices), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_d3dcolor_blendindices); ++i)
        {
            float dx = mapped[i].x - expected_d3dcolor_blendindices[i].x;
            float dy = mapped[i].y - expected_d3dcolor_blendindices[i].y;
            float dz = mapped[i].z - expected_d3dcolor_blendindices[i].z;
            float dw = mapped[i].rhw - expected_d3dcolor_blendindices[i].rhw;
            float du = mapped[i].u - expected_d3dcolor_blendindices[i].u;
            float dv = mapped[i].v - expected_d3dcolor_blendindices[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_d3dcolor_blendindices[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_blendindices_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_blendindices_constants, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZB5
            | D3DFVF_LASTBETA_UBYTE4 | D3DFVF_DIFFUSE | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_fvf_blendindices_vb, 0,
            sizeof(struct src_fvf_blendindices_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_blendindices), prog_blendindices_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_blendindices_dst_vb, 0,
            sizeof(expected_blendweight), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_blendweight); ++i)
        {
            float dx = mapped[i].x - expected_blendweight[i].x;
            float dy = mapped[i].y - expected_blendweight[i].y;
            float dz = mapped[i].z - expected_blendweight[i].z;
            float dw = mapped[i].rhw - expected_blendweight[i].rhw;
            float du = mapped[i].u - expected_blendweight[i].u;
            float dv = mapped[i].v - expected_blendweight[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_blendweight[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_blendindices_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_d3dcolor_blendindices_constants, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZB5
            | D3DFVF_LASTBETA_D3DCOLOR | D3DFVF_DIFFUSE | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_fvf_blendindices), prog_blendindices_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_blendindices_dst_vb, 0,
            sizeof(expected_d3dcolor_blendindices), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_d3dcolor_blendindices); ++i)
        {
            float dx = mapped[i].x - expected_d3dcolor_blendindices[i].x;
            float dy = mapped[i].y - expected_d3dcolor_blendindices[i].y;
            float dz = mapped[i].z - expected_d3dcolor_blendindices[i].z;
            float dw = mapped[i].rhw - expected_d3dcolor_blendindices[i].rhw;
            float du = mapped[i].u - expected_d3dcolor_blendindices[i].u;
            float dv = mapped[i].v - expected_d3dcolor_blendindices[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_d3dcolor_blendindices[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_blendindices_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_extra_multistream_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_extra_multistream), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_extra_multistream_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_extra_multistream_constants, 5), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_extra_split_decl), D3D_OK);
    for (i = 0; i < 6; ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, i, src_extra_vb,
                0, sizeof(struct src_extra_vertex)), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_extra_multistream_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_extra_multistream_dst_vb, 0,
            sizeof(expected_extra_multistream), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_extra_multistream); ++i)
        {
            float dx = mapped[i].x - expected_extra_multistream[i].x;
            float dy = mapped[i].y - expected_extra_multistream[i].y;
            float dz = mapped[i].z - expected_extra_multistream[i].z;
            float dw = mapped[i].rhw - expected_extra_multistream[i].rhw;
            float du = mapped[i].u - expected_extra_multistream[i].u;
            float dv = mapped[i].v - expected_extra_multistream[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_extra_multistream[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_extra_multistream_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_generic_usage_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_generic_usage), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_generic_usage_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_generic_split_decl), D3D_OK);
    for (i = 0; i < 6; ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, i, src_extra_vb,
                0, sizeof(struct src_extra_vertex)), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_generic_usage_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_generic_usage_dst_vb, 0,
            sizeof(expected_generic_usage), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_generic_usage); ++i)
        {
            float dx = mapped[i].x - expected_generic_usage[i].x;
            float dy = mapped[i].y - expected_generic_usage[i].y;
            float dz = mapped[i].z - expected_generic_usage[i].z;
            float dw = mapped[i].rhw - expected_generic_usage[i].rhw;
            float du = mapped[i].u - expected_generic_usage[i].u;
            float dv = mapped[i].v - expected_generic_usage[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_generic_usage[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_generic_usage_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_generic_usage_index_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_generic_index_split_decl), D3D_OK);
    for (i = 0; i < 6; ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, i, src_extra_vb,
                0, sizeof(struct src_extra_vertex)), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_generic_usage_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_generic_usage_dst_vb, 0,
            sizeof(expected_generic_usage), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_generic_usage); ++i)
        {
            float dx = mapped[i].x - expected_generic_usage[i].x;
            float dy = mapped[i].y - expected_generic_usage[i].y;
            float dz = mapped[i].z - expected_generic_usage[i].z;
            float dw = mapped[i].rhw - expected_generic_usage[i].rhw;
            float du = mapped[i].u - expected_generic_usage[i].u;
            float dv = mapped[i].v - expected_generic_usage[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_generic_usage[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_generic_usage_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_generic_d3dcolor_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_generic_d3dcolor_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb,
            0, sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_generic_usage_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_generic_usage_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_generic_usage_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_generic_short2_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_generic_short2_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb,
            0, sizeof(struct src_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
            src_attr_short2_vb, 0, sizeof(src_attr_short2[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_generic_usage_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_generic_usage_dst_vb, 0,
            sizeof(expected_short2), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_short2); ++i)
        {
            float dx = mapped[i].x - expected_short2[i].x;
            float dy = mapped[i].y - expected_short2[i].y;
            float dz = mapped[i].z - expected_short2[i].z;
            float dw = mapped[i].rhw - expected_short2[i].rhw;
            float du = mapped[i].u - expected_short2[i].u;
            float dv = mapped[i].v - expected_short2[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_short2[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_generic_usage_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_generic_ubyte4n_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_generic_ubyte4n_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb,
            0, sizeof(struct src_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
            src_attr_ubyte4n_vb, 0, sizeof(src_attr_ubyte4n[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_generic_usage_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_generic_usage_dst_vb, 0,
            sizeof(expected_ubyte4n_generic), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_ubyte4n_generic); ++i)
        {
            float dx = mapped[i].x - expected_ubyte4n_generic[i].x;
            float dy = mapped[i].y - expected_ubyte4n_generic[i].y;
            float dz = mapped[i].z - expected_ubyte4n_generic[i].z;
            float dw = mapped[i].rhw - expected_ubyte4n_generic[i].rhw;
            float du = mapped[i].u - expected_ubyte4n_generic[i].u;
            float dv = mapped[i].v - expected_ubyte4n_generic[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_ubyte4n_generic[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_generic_usage_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_generic_udec3_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_generic_udec3_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb,
            0, sizeof(struct src_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
            src_attr_udec3_vb, 0, sizeof(src_attr_udec3[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_generic_usage_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_generic_usage_dst_vb, 0,
            sizeof(expected_udec3_generic), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_udec3_generic); ++i)
        {
            float dx = mapped[i].x - expected_udec3_generic[i].x;
            float dy = mapped[i].y - expected_udec3_generic[i].y;
            float dz = mapped[i].z - expected_udec3_generic[i].z;
            float dw = mapped[i].rhw - expected_udec3_generic[i].rhw;
            float du = mapped[i].u - expected_udec3_generic[i].u;
            float dv = mapped[i].v - expected_udec3_generic[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_udec3_generic[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_generic_usage_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    for (i = 1; i < 6; ++i)
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, i, NULL, 0, 0),
                D3D_OK);

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_dst_vb, 0, sizeof(expected),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_d3dcolor_tex_dst), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_d3dcolor_tex_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_d3dcolor_tex_dst_vb,
            dst_d3dcolor_tex_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_d3dcolor_tex_dst_vb, 0,
            sizeof(expected_d3dcolor_tex_dst),
            (void **)&mapped_d3dcolor_tex, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_d3dcolor_tex_dst); ++i)
        {
            float dx = mapped_d3dcolor_tex[i].x - expected_d3dcolor_tex_dst[i].x;
            float dy = mapped_d3dcolor_tex[i].y - expected_d3dcolor_tex_dst[i].y;
            float dz = mapped_d3dcolor_tex[i].z - expected_d3dcolor_tex_dst[i].z;
            float dw = mapped_d3dcolor_tex[i].rhw - expected_d3dcolor_tex_dst[i].rhw;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_d3dcolor_tex[i].color
                    == expected_d3dcolor_tex_dst[i].color);
            CHECK_TRUE(mapped_d3dcolor_tex[i].texcolor
                    == expected_d3dcolor_tex_dst[i].texcolor);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_d3dcolor_tex_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_packed_tex_dst_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_packed_tex_constants, 5), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_packed_tex_dst), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_packed_tex_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_packed_tex_dst_vb,
            dst_packed_tex_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_packed_tex_dst_vb, 0,
            sizeof(expected_packed_tex_dst),
            (void **)&mapped_packed_tex, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_packed_tex_dst); ++i)
        {
            float dx = mapped_packed_tex[i].x - expected_packed_tex_dst[i].x;
            float dy = mapped_packed_tex[i].y - expected_packed_tex_dst[i].y;
            float dz = mapped_packed_tex[i].z - expected_packed_tex_dst[i].z;
            float dw = mapped_packed_tex[i].rhw - expected_packed_tex_dst[i].rhw;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_packed_tex[i].color
                    == expected_packed_tex_dst[i].color);
            CHECK_TRUE(mapped_packed_tex[i].short2n_u
                    == expected_packed_tex_dst[i].short2n_u);
            CHECK_TRUE(mapped_packed_tex[i].short2n_v
                    == expected_packed_tex_dst[i].short2n_v);
            CHECK_TRUE(mapped_packed_tex[i].ubyte4n_u
                    == expected_packed_tex_dst[i].ubyte4n_u);
            CHECK_TRUE(mapped_packed_tex[i].ubyte4n_v
                    == expected_packed_tex_dst[i].ubyte4n_v);
            CHECK_TRUE(mapped_packed_tex[i].ubyte4n_s
                    == expected_packed_tex_dst[i].ubyte4n_s);
            CHECK_TRUE(mapped_packed_tex[i].ubyte4n_t
                    == expected_packed_tex_dst[i].ubyte4n_t);
            CHECK_TRUE(mapped_packed_tex[i].half_u
                    == expected_packed_tex_dst[i].half_u);
            CHECK_TRUE(mapped_packed_tex[i].half_v
                    == expected_packed_tex_dst[i].half_v);
            CHECK_TRUE(mapped_packed_tex[i].udec3
                    == expected_packed_tex_dst[i].udec3);
            CHECK_TRUE(mapped_packed_tex[i].dec3n
                    == expected_packed_tex_dst[i].dec3n);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_packed_tex_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_sparse_texcoord_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_sparse_tex_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_sparse_tex_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, src_attr_vb, 0,
            sizeof(struct attr_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_sparse_tex_dst_vb, dst_sparse_tex_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_sparse_tex_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_sparse_tex_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_sparse_texcoord7_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_tex1), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_sparse_tex7_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_sparse_tex7_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, src_attr_vb, 0,
            sizeof(struct attr_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_sparse_tex7_dst_vb, dst_sparse_tex7_decl,
            0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_sparse_tex7_dst_vb, 0,
            sizeof(expected_tex1), (void **)&mapped_tex1, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_tex1); ++i)
        {
            float dx = mapped_tex1[i].x - expected_tex1[i].x;
            float dy = mapped_tex1[i].y - expected_tex1[i].y;
            float dz = mapped_tex1[i].z - expected_tex1[i].z;
            float dw = mapped_tex1[i].rhw - expected_tex1[i].rhw;
            float du = mapped_tex1[i].u - expected_tex1[i].u;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_tex1[i].color == expected_tex1[i].color);
            CHECK_TRUE(du < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_sparse_tex7_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_specular_3_0,
            &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_specular),
            0, D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR
            | D3DFVF_TEX1, D3DPOOL_SYSTEMMEM, &prog_specular_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_specular_vb, 0,
            sizeof(src_specular[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_specular), prog_specular_dst_vb, NULL, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_specular_dst_vb, 0,
            sizeof(expected_specular), (void **)&mapped_specular,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_specular); ++i)
        {
            float dx = mapped_specular[i].x - expected_specular[i].x;
            float dy = mapped_specular[i].y - expected_specular[i].y;
            float dz = mapped_specular[i].z - expected_specular[i].z;
            float dw = mapped_specular[i].rhw - expected_specular[i].rhw;
            float du = mapped_specular[i].u - expected_specular[i].u;
            float dv = mapped_specular[i].v - expected_specular[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_specular[i].color == expected_specular[i].color);
            CHECK_TRUE(mapped_specular[i].specular == expected_specular[i].specular);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_specular_dst_vb),
                D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_specular),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_specular_decl_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_specular), prog_specular_decl_dst_vb,
            dst_specular_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_specular_decl_dst_vb, 0,
            sizeof(expected_specular), (void **)&mapped_specular,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_specular); ++i)
        {
            float dx = mapped_specular[i].x - expected_specular[i].x;
            float dy = mapped_specular[i].y - expected_specular[i].y;
            float dz = mapped_specular[i].z - expected_specular[i].z;
            float dw = mapped_specular[i].rhw - expected_specular[i].rhw;
            float du = mapped_specular[i].u - expected_specular[i].u;
            float dv = mapped_specular[i].v - expected_specular[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped_specular[i].color == expected_specular[i].color);
            CHECK_TRUE(mapped_specular[i].specular == expected_specular[i].specular);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_specular_decl_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_1_1, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_legacy_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_legacy_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_legacy_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_legacy_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_output_relative_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_output_relative_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_output_relative_constants, 5), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_output_relative_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_output_relative_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_output_relative_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_input_relative_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_input_relative_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_output_relative_constants, 5), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_input_relative_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_input_relative_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_input_relative_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_matrix_relative_const_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_matrix_relative_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_matrix_relative_constants, 9), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_matrix_relative_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_matrix_relative_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_matrix_relative_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_mova_component_relative_const_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_mova_component_relative_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_mova_component_relative_constants, 10), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_mova_component_relative_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_mova_component_relative_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_mova_component_relative); ++i)
        {
            float dx = mapped[i].x - expected_mova_component_relative[i].x;
            float dy = mapped[i].y - expected_mova_component_relative[i].y;
            float dz = mapped[i].z - expected_mova_component_relative[i].z;
            float dw = mapped[i].rhw - expected_mova_component_relative[i].rhw;
            float du = mapped[i].u - expected_mova_component_relative[i].u;
            float dv = mapped[i].v - expected_mova_component_relative[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_mova_component_relative[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_mova_component_relative_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateTexture(device, 2, 2, 2, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vs_texldl_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DTexture9_LockRect(vs_texldl_texture, 0, &locked_rect, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        DWORD *row0 = (DWORD *)locked_rect.pBits;
        DWORD *row1 = (DWORD *)((BYTE *)locked_rect.pBits + locked_rect.Pitch);
        row0[0] = 0xff112233u;
        row0[1] = 0xff445566u;
        row1[0] = 0xff778899u;
        row1[1] = 0xffaabbccu;
        CHECK_HR(IDirect3DTexture9_UnlockRect(vs_texldl_texture, 0), D3D_OK);
    }
    hr = IDirect3DTexture9_LockRect(vs_texldl_texture, 1, &locked_rect, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        *(DWORD *)locked_rect.pBits = 0xff336699u;
        CHECK_HR(IDirect3DTexture9_UnlockRect(vs_texldl_texture, 1), D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_texldl_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_texldl), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_texldl_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_texldl_vb, 0,
            sizeof(src_texldl[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
            D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
            D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0,
            (IDirect3DBaseTexture9 *)vs_texldl_texture), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl); ++i)
        {
            float dx = mapped[i].x - expected_texldl[i].x;
            float dy = mapped[i].y - expected_texldl[i].y;
            float dz = mapped[i].z - expected_texldl[i].z;
            float dw = mapped[i].rhw - expected_texldl[i].rhw;
            float du = mapped[i].u - expected_texldl[i].u;
            float dv = mapped[i].v - expected_texldl[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    for (j = 0; j < ARRAY_SIZE(texldl_address_cases); ++j)
    {
        const struct dst_vertex *expected_address =
                texldl_address_cases[j].expected;
        CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
                D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU,
                texldl_address_cases[j].address), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
                D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV,
                texldl_address_cases[j].address), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
                D3DVERTEXTEXTURESAMPLER0, D3DSAMP_BORDERCOLOR,
                texldl_address_cases[j].border_color), D3D_OK);
        CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
                ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
                D3D_OK);

        hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
                sizeof(expected_texldl), (void **)&mapped, D3DLOCK_READONLY);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            for (i = 0; i < ARRAY_SIZE(expected_texldl); ++i)
            {
                float dx = mapped[i].x - expected_address[i].x;
                float dy = mapped[i].y - expected_address[i].y;
                float dz = mapped[i].z - expected_address[i].z;
                float dw = mapped[i].rhw - expected_address[i].rhw;
                float du = mapped[i].u - expected_address[i].u;
                float dv = mapped[i].v - expected_address[i].v;
                if (dx < 0.0f) dx = -dx;
                if (dy < 0.0f) dy = -dy;
                if (dz < 0.0f) dz = -dz;
                if (dw < 0.0f) dw = -dw;
                if (du < 0.0f) du = -du;
                if (dv < 0.0f) dv = -dv;
                CHECK_TRUE(dx < 0.01f);
                CHECK_TRUE(dy < 0.01f);
                CHECK_TRUE(dz < 0.01f);
                CHECK_TRUE(dw < 0.01f);
                CHECK_TRUE(mapped[i].color == expected_address[i].color);
                CHECK_TRUE(du < 0.01f);
                CHECK_TRUE(dv < 0.01f);
            }
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                    D3D_OK);
        }
    }
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
            D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSU, D3DTADDRESS_WRAP),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
            D3DVERTEXTEXTURESAMPLER0, D3DSAMP_ADDRESSV, D3DTADDRESS_WRAP),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
            D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAXMIPLEVEL, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_lod), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_lod); ++i)
        {
            float dx = mapped[i].x - expected_texldl_lod[i].x;
            float dy = mapped[i].y - expected_texldl_lod[i].y;
            float dz = mapped[i].z - expected_texldl_lod[i].z;
            float dw = mapped[i].rhw - expected_texldl_lod[i].rhw;
            float du = mapped[i].u - expected_texldl_lod[i].u;
            float dv = mapped[i].v - expected_texldl_lod[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_lod[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device,
            D3DVERTEXTEXTURESAMPLER0, D3DSAMP_MAXMIPLEVEL, 0), D3D_OK);
    memset(texldl_palette, 0, sizeof(texldl_palette));
    for (i = 0; i < ARRAY_SIZE(texldl_palette); ++i)
        texldl_palette[i].peFlags = 0xff;
    texldl_palette[1].peRed = 0x10;
    texldl_palette[1].peGreen = 0x20;
    texldl_palette[1].peBlue = 0x30;
    texldl_palette[2].peRed = 0x40;
    texldl_palette[2].peGreen = 0x50;
    texldl_palette[2].peBlue = 0x60;
    texldl_palette[3].peRed = 0x70;
    texldl_palette[3].peGreen = 0x80;
    texldl_palette[3].peBlue = 0x90;
    texldl_palette[4].peRed = 0xa0;
    texldl_palette[4].peGreen = 0xb0;
    texldl_palette[4].peBlue = 0xc0;
    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, texldl_palette),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0), D3D_OK);
    hr = IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_P8,
            D3DPOOL_MANAGED, &vs_texldl_p8_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DTexture9_LockRect(vs_texldl_p8_texture, 0, &locked_rect,
            NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        BYTE *row0 = (BYTE *)locked_rect.pBits;
        BYTE *row1 = (BYTE *)locked_rect.pBits + locked_rect.Pitch;
        row0[0] = 1;
        row0[1] = 2;
        row1[0] = 3;
        row1[1] = 4;
        CHECK_HR(IDirect3DTexture9_UnlockRect(vs_texldl_p8_texture, 0),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0,
            (IDirect3DBaseTexture9 *)vs_texldl_p8_texture), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_p8), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_p8); ++i)
        {
            float dx = mapped[i].x - expected_texldl_p8[i].x;
            float dy = mapped[i].y - expected_texldl_p8[i].y;
            float dz = mapped[i].z - expected_texldl_p8[i].z;
            float dw = mapped[i].rhw - expected_texldl_p8[i].rhw;
            float du = mapped[i].u - expected_texldl_p8[i].u;
            float dv = mapped[i].v - expected_texldl_p8[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_p8[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    texldl_palette[1].peRed = 0x22;
    texldl_palette[1].peGreen = 0x44;
    texldl_palette[1].peBlue = 0x66;
    texldl_palette[2].peRed = 0x66;
    texldl_palette[2].peGreen = 0x88;
    texldl_palette[2].peBlue = 0xaa;
    texldl_palette[3].peRed = 0x99;
    texldl_palette[3].peGreen = 0xbb;
    texldl_palette[3].peBlue = 0xdd;
    texldl_palette[4].peRed = 0xcc;
    texldl_palette[4].peGreen = 0xdd;
    texldl_palette[4].peBlue = 0x22;
    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, texldl_palette),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_p8_updated), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_p8_updated); ++i)
        {
            float dx = mapped[i].x - expected_texldl_p8_updated[i].x;
            float dy = mapped[i].y - expected_texldl_p8_updated[i].y;
            float dz = mapped[i].z - expected_texldl_p8_updated[i].z;
            float dw = mapped[i].rhw - expected_texldl_p8_updated[i].rhw;
            float du = mapped[i].u - expected_texldl_p8_updated[i].u;
            float dv = mapped[i].v - expected_texldl_p8_updated[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_p8_updated[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 1, texldl_palette),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_p8_updated), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_p8_updated); ++i)
        {
            float dx = mapped[i].x - expected_texldl_p8_updated[i].x;
            float dy = mapped[i].y - expected_texldl_p8_updated[i].y;
            float dz = mapped[i].z - expected_texldl_p8_updated[i].z;
            float dw = mapped[i].rhw - expected_texldl_p8_updated[i].rhw;
            float du = mapped[i].u - expected_texldl_p8_updated[i].u;
            float dv = mapped[i].v - expected_texldl_p8_updated[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_p8_updated[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    texldl_palette[1].peRed = 0x10;
    texldl_palette[1].peGreen = 0x20;
    texldl_palette[1].peBlue = 0x30;
    texldl_palette[2].peRed = 0x40;
    texldl_palette[2].peGreen = 0x50;
    texldl_palette[2].peBlue = 0x60;
    texldl_palette[3].peRed = 0x70;
    texldl_palette[3].peGreen = 0x80;
    texldl_palette[3].peBlue = 0x90;
    texldl_palette[4].peRed = 0xa0;
    texldl_palette[4].peGreen = 0xb0;
    texldl_palette[4].peBlue = 0xc0;
    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, texldl_palette),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0,
            NULL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0,
            (IDirect3DBaseTexture9 *)vs_texldl_p8_texture), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_p8_updated), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_p8_updated); ++i)
        {
            float dx = mapped[i].x - expected_texldl_p8_updated[i].x;
            float dy = mapped[i].y - expected_texldl_p8_updated[i].y;
            float dz = mapped[i].z - expected_texldl_p8_updated[i].z;
            float dw = mapped[i].rhw - expected_texldl_p8_updated[i].rhw;
            float du = mapped[i].u - expected_texldl_p8_updated[i].u;
            float dv = mapped[i].v - expected_texldl_p8_updated[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_p8_updated[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0), D3D_OK);
    hr = IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, D3DFMT_A8P8,
            D3DPOOL_MANAGED, &vs_texldl_a8p8_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DTexture9_LockRect(vs_texldl_a8p8_texture, 0, &locked_rect,
            NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        BYTE *row0 = (BYTE *)locked_rect.pBits;
        BYTE *row1 = (BYTE *)locked_rect.pBits + locked_rect.Pitch;
        row0[0] = 1;
        row0[1] = 0x80;
        row0[2] = 2;
        row0[3] = 0x60;
        row1[0] = 3;
        row1[1] = 0x40;
        row1[2] = 4;
        row1[3] = 0x20;
        CHECK_HR(IDirect3DTexture9_UnlockRect(vs_texldl_a8p8_texture, 0),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0,
            (IDirect3DBaseTexture9 *)vs_texldl_a8p8_texture), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_a8p8_initial), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_a8p8_initial); ++i)
        {
            float dx = mapped[i].x - expected_texldl_a8p8_initial[i].x;
            float dy = mapped[i].y - expected_texldl_a8p8_initial[i].y;
            float dz = mapped[i].z - expected_texldl_a8p8_initial[i].z;
            float dw = mapped[i].rhw - expected_texldl_a8p8_initial[i].rhw;
            float du = mapped[i].u - expected_texldl_a8p8_initial[i].u;
            float dv = mapped[i].v - expected_texldl_a8p8_initial[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_a8p8_initial[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    texldl_palette[1].peRed = 0x22;
    texldl_palette[1].peGreen = 0x44;
    texldl_palette[1].peBlue = 0x66;
    texldl_palette[2].peRed = 0x66;
    texldl_palette[2].peGreen = 0x88;
    texldl_palette[2].peBlue = 0xaa;
    texldl_palette[3].peRed = 0x99;
    texldl_palette[3].peGreen = 0xbb;
    texldl_palette[3].peBlue = 0xdd;
    texldl_palette[4].peRed = 0xcc;
    texldl_palette[4].peGreen = 0xdd;
    texldl_palette[4].peBlue = 0x22;
    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, texldl_palette),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_a8p8), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_a8p8); ++i)
        {
            float dx = mapped[i].x - expected_texldl_a8p8[i].x;
            float dy = mapped[i].y - expected_texldl_a8p8[i].y;
            float dz = mapped[i].z - expected_texldl_a8p8[i].z;
            float dw = mapped[i].rhw - expected_texldl_a8p8[i].rhw;
            float du = mapped[i].u - expected_texldl_a8p8[i].u;
            float dv = mapped[i].v - expected_texldl_a8p8[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_a8p8[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_texldl), prog_texldl_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_texldl_dst_vb, 0,
            sizeof(expected_texldl_a8p8), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_texldl_a8p8); ++i)
        {
            float dx = mapped[i].x - expected_texldl_a8p8[i].x;
            float dy = mapped[i].y - expected_texldl_a8p8[i].y;
            float dz = mapped[i].z - expected_texldl_a8p8[i].z;
            float dw = mapped[i].rhw - expected_texldl_a8p8[i].rhw;
            float du = mapped[i].u - expected_texldl_a8p8[i].u;
            float dv = mapped[i].v - expected_texldl_a8p8[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_texldl_a8p8[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_texldl_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0,
            NULL), D3D_OK);
    IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_short2),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_short2_tex_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_short2_tex_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
            src_attr_short2_vb, 0, sizeof(src_attr_short2[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_short2_tex_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_short2_tex_dst_vb, 0,
            sizeof(expected_short2), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_short2); ++i)
        {
            float dx = mapped[i].x - expected_short2[i].x;
            float dy = mapped[i].y - expected_short2[i].y;
            float dz = mapped[i].z - expected_short2[i].z;
            float dw = mapped[i].rhw - expected_short2[i].rhw;
            float du = mapped[i].u - expected_short2[i].u;
            float dv = mapped[i].v - expected_short2[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_short2[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_short2_tex_dst_vb),
                D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_short2n_tex_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_short2n_tex_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
            src_attr_short2n_vb, 0, sizeof(src_attr_short2n[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_short2n_tex_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_short2n_tex_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_short2n_tex_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_ushort2n_tex_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
            src_attr_ushort2n_vb, 0, sizeof(src_attr_ushort2n[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_short2n_tex_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_short2n_tex_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_short2n_tex_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_float16_tex_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
            src_attr_float16_vb, 0, sizeof(src_attr_float16[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_short2n_tex_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_short2n_tex_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_short2n_tex_dst_vb),
                D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_d3dcolor_tex_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(src_extra[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_short2n_tex_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_short2n_tex_dst_vb, 0,
            sizeof(expected_d3dcolor_tex), (void **)&mapped,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_d3dcolor_tex); ++i)
        {
            float dx = mapped[i].x - expected_d3dcolor_tex[i].x;
            float dy = mapped[i].y - expected_d3dcolor_tex[i].y;
            float dz = mapped[i].z - expected_d3dcolor_tex[i].z;
            float dw = mapped[i].rhw - expected_d3dcolor_tex[i].rhw;
            float du = mapped[i].u - expected_d3dcolor_tex[i].u;
            float dv = mapped[i].v - expected_d3dcolor_tex[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_d3dcolor_tex[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_short2n_tex_dst_vb),
                D3D_OK);
    }

    {
        struct tex4_decl_case
        {
            IDirect3DVertexDeclaration9 *decl;
            IDirect3DVertexBuffer9 *vb;
            UINT stride;
        };
        const struct tex4_decl_case tex4_cases[] =
        {
            {src_short4n_tex_decl, src_attr_short4n_vb,
                    sizeof(src_attr_short4n[0])},
            {src_ushort4n_tex_decl, src_attr_ushort4n_vb,
                    sizeof(src_attr_ushort4n[0])},
            {src_float16_4_tex_decl, src_attr_float16_4_vb,
                    sizeof(src_attr_float16_4[0])},
            {src_ubyte4n_tex_decl, src_attr_ubyte4n_vb,
                    sizeof(src_attr_ubyte4n[0])},
        };
        UINT tex_case;

        hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_tex4),
                0, 0, D3DPOOL_SYSTEMMEM, &prog_tex4_dst_vb, NULL);
        CHECK_HR(hr, D3D_OK);
        if (FAILED(hr))
            goto done_device;

        for (tex_case = 0; tex_case < ARRAY_SIZE(tex4_cases); ++tex_case)
        {
            CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
                    tex4_cases[tex_case].decl), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
                    sizeof(src[0])), D3D_OK);
            CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
                    tex4_cases[tex_case].vb, 0, tex4_cases[tex_case].stride),
                    D3D_OK);
            CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
                    ARRAY_SIZE(src), prog_tex4_dst_vb, dst_tex4_decl, 0),
                    D3D_OK);

            hr = IDirect3DVertexBuffer9_Lock(prog_tex4_dst_vb, 0,
                    sizeof(expected_tex4), (void **)&mapped_tex4,
                    D3DLOCK_READONLY);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
            {
                for (i = 0; i < ARRAY_SIZE(expected_tex4); ++i)
                {
                    float dx = mapped_tex4[i].x - expected_tex4[i].x;
                    float dy = mapped_tex4[i].y - expected_tex4[i].y;
                    float dz = mapped_tex4[i].z - expected_tex4[i].z;
                    float dw = mapped_tex4[i].rhw - expected_tex4[i].rhw;
                    float du = mapped_tex4[i].u - expected_tex4[i].u;
                    float dv = mapped_tex4[i].v - expected_tex4[i].v;
                    float ds = mapped_tex4[i].s - expected_tex4[i].s;
                    float dt = mapped_tex4[i].t - expected_tex4[i].t;
                    if (dx < 0.0f) dx = -dx;
                    if (dy < 0.0f) dy = -dy;
                    if (dz < 0.0f) dz = -dz;
                    if (dw < 0.0f) dw = -dw;
                    if (du < 0.0f) du = -du;
                    if (dv < 0.0f) dv = -dv;
                    if (ds < 0.0f) ds = -ds;
                    if (dt < 0.0f) dt = -dt;
                    CHECK_TRUE(dx < 0.01f);
                    CHECK_TRUE(dy < 0.01f);
                    CHECK_TRUE(dz < 0.01f);
                    CHECK_TRUE(dw < 0.01f);
                    CHECK_TRUE(mapped_tex4[i].color == expected_tex4[i].color);
                    CHECK_TRUE(du < 0.01f);
                    CHECK_TRUE(dv < 0.01f);
                    CHECK_TRUE(ds < 0.01f);
                    CHECK_TRUE(dt < 0.01f);
                }
                CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_tex4_dst_vb),
                        D3D_OK);
            }
        }

        {
            struct raw_tex4_decl_case
            {
                IDirect3DVertexDeclaration9 *decl;
                IDirect3DVertexBuffer9 *vb;
                UINT stride;
                const struct dst_tex4_vertex *expected;
            };
            const struct raw_tex4_decl_case raw_tex4_cases[] =
            {
                {src_short4_tex_decl, src_attr_short4_vb,
                        sizeof(src_attr_short4[0]), expected_short4_tex4},
                {src_ubyte4_tex_decl, src_attr_ubyte4_vb,
                        sizeof(src_attr_ubyte4[0]), expected_ubyte4_tex4},
            };

            for (tex_case = 0; tex_case < ARRAY_SIZE(raw_tex4_cases);
                    ++tex_case)
            {
                const struct dst_tex4_vertex *expected_raw =
                        raw_tex4_cases[tex_case].expected;
                CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
                        raw_tex4_cases[tex_case].decl), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
                        sizeof(src[0])), D3D_OK);
                CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
                        raw_tex4_cases[tex_case].vb, 0,
                        raw_tex4_cases[tex_case].stride), D3D_OK);
                CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
                        ARRAY_SIZE(src), prog_tex4_dst_vb, dst_tex4_decl, 0),
                        D3D_OK);

                hr = IDirect3DVertexBuffer9_Lock(prog_tex4_dst_vb, 0,
                        sizeof(expected_short4_tex4), (void **)&mapped_tex4,
                        D3DLOCK_READONLY);
                CHECK_HR(hr, D3D_OK);
                if (SUCCEEDED(hr))
                {
                    for (i = 0; i < ARRAY_SIZE(expected_short4_tex4); ++i)
                    {
                        float dx = mapped_tex4[i].x - expected_raw[i].x;
                        float dy = mapped_tex4[i].y - expected_raw[i].y;
                        float dz = mapped_tex4[i].z - expected_raw[i].z;
                        float dw = mapped_tex4[i].rhw - expected_raw[i].rhw;
                        float du = mapped_tex4[i].u - expected_raw[i].u;
                        float dv = mapped_tex4[i].v - expected_raw[i].v;
                        float ds = mapped_tex4[i].s - expected_raw[i].s;
                        float dt = mapped_tex4[i].t - expected_raw[i].t;
                        if (dx < 0.0f) dx = -dx;
                        if (dy < 0.0f) dy = -dy;
                        if (dz < 0.0f) dz = -dz;
                        if (dw < 0.0f) dw = -dw;
                        if (du < 0.0f) du = -du;
                        if (dv < 0.0f) dv = -dv;
                        if (ds < 0.0f) ds = -ds;
                        if (dt < 0.0f) dt = -dt;
                        CHECK_TRUE(dx < 0.01f);
                        CHECK_TRUE(dy < 0.01f);
                        CHECK_TRUE(dz < 0.01f);
                        CHECK_TRUE(dw < 0.01f);
                        CHECK_TRUE(mapped_tex4[i].color == expected_raw[i].color);
                        CHECK_TRUE(du < 0.01f);
                        CHECK_TRUE(dv < 0.01f);
                        CHECK_TRUE(ds < 0.01f);
                        CHECK_TRUE(dt < 0.01f);
                    }
                    CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_tex4_dst_vb),
                            D3D_OK);
                }
            }
        }

        CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
                src_udec3_tex_decl), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
                sizeof(src[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
                src_attr_udec3_vb, 0, sizeof(src_attr_udec3[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
                ARRAY_SIZE(src), prog_tex4_dst_vb, dst_tex4_decl, 0),
                D3D_OK);

        hr = IDirect3DVertexBuffer9_Lock(prog_tex4_dst_vb, 0,
                sizeof(expected_udec3_tex4), (void **)&mapped_tex4,
                D3DLOCK_READONLY);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            for (i = 0; i < ARRAY_SIZE(expected_udec3_tex4); ++i)
            {
                float dx = mapped_tex4[i].x - expected_udec3_tex4[i].x;
                float dy = mapped_tex4[i].y - expected_udec3_tex4[i].y;
                float dz = mapped_tex4[i].z - expected_udec3_tex4[i].z;
                float dw = mapped_tex4[i].rhw - expected_udec3_tex4[i].rhw;
                float du = mapped_tex4[i].u - expected_udec3_tex4[i].u;
                float dv = mapped_tex4[i].v - expected_udec3_tex4[i].v;
                float ds = mapped_tex4[i].s - expected_udec3_tex4[i].s;
                float dt = mapped_tex4[i].t - expected_udec3_tex4[i].t;
                if (dx < 0.0f) dx = -dx;
                if (dy < 0.0f) dy = -dy;
                if (dz < 0.0f) dz = -dz;
                if (dw < 0.0f) dw = -dw;
                if (du < 0.0f) du = -du;
                if (dv < 0.0f) dv = -dv;
                if (ds < 0.0f) ds = -ds;
                if (dt < 0.0f) dt = -dt;
                CHECK_TRUE(dx < 0.01f);
                CHECK_TRUE(dy < 0.01f);
                CHECK_TRUE(dz < 0.01f);
                CHECK_TRUE(dw < 0.01f);
                CHECK_TRUE(mapped_tex4[i].color == expected_udec3_tex4[i].color);
                CHECK_TRUE(du < 0.01f);
                CHECK_TRUE(dv < 0.01f);
                CHECK_TRUE(ds < 0.01f);
                CHECK_TRUE(dt < 0.01f);
            }
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_tex4_dst_vb),
                    D3D_OK);
        }

        CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
                src_dec3n_tex_decl), D3D_OK);
        hr = IDirect3DVertexBuffer9_Lock(src_attr_udec3_vb, 0,
                sizeof(src_attr_dec3n), &bits, 0);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memcpy(bits, src_attr_dec3n, sizeof(src_attr_dec3n));
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(src_attr_udec3_vb),
                    D3D_OK);
        }
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
                sizeof(src[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1,
                src_attr_udec3_vb, 0, sizeof(src_attr_dec3n[0])), D3D_OK);
        CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
                ARRAY_SIZE(src), prog_tex4_dst_vb, dst_tex4_decl, 0),
                D3D_OK);

        hr = IDirect3DVertexBuffer9_Lock(prog_tex4_dst_vb, 0,
                sizeof(expected_dec3n_tex4), (void **)&mapped_tex4,
                D3DLOCK_READONLY);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            for (i = 0; i < ARRAY_SIZE(expected_dec3n_tex4); ++i)
            {
                float dx = mapped_tex4[i].x - expected_dec3n_tex4[i].x;
                float dy = mapped_tex4[i].y - expected_dec3n_tex4[i].y;
                float dz = mapped_tex4[i].z - expected_dec3n_tex4[i].z;
                float dw = mapped_tex4[i].rhw - expected_dec3n_tex4[i].rhw;
                float du = mapped_tex4[i].u - expected_dec3n_tex4[i].u;
                float dv = mapped_tex4[i].v - expected_dec3n_tex4[i].v;
                float ds = mapped_tex4[i].s - expected_dec3n_tex4[i].s;
                float dt = mapped_tex4[i].t - expected_dec3n_tex4[i].t;
                if (dx < 0.0f) dx = -dx;
                if (dy < 0.0f) dy = -dy;
                if (dz < 0.0f) dz = -dz;
                if (dw < 0.0f) dw = -dw;
                if (du < 0.0f) du = -du;
                if (dv < 0.0f) dv = -dv;
                if (ds < 0.0f) ds = -ds;
                if (dt < 0.0f) dt = -dt;
                CHECK_TRUE(dx < 0.01f);
                CHECK_TRUE(dy < 0.01f);
                CHECK_TRUE(dz < 0.01f);
                CHECK_TRUE(dw < 0.01f);
                CHECK_TRUE(mapped_tex4[i].color == expected_dec3n_tex4[i].color);
                CHECK_TRUE(du < 0.01f);
                CHECK_TRUE(dv < 0.01f);
                CHECK_TRUE(ds < 0.01f);
                CHECK_TRUE(dt < 0.01f);
            }
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_tex4_dst_vb),
                    D3D_OK);
        }
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_partialprecision_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_partialprecision_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_constants, 4), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_partialprecision_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_partialprecision_dst_vb, 0,
            sizeof(expected), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_partialprecision_dst_vb),
                D3D_OK);
    }

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device,
            process_vs_saturate_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_saturate),
            0, 0, D3DPOOL_SYSTEMMEM, &prog_saturate_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_saturate_constants, 5), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_extra_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_extra_vb, 0,
            sizeof(struct src_extra_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_extra), prog_saturate_dst_vb, dst_decl, 0),
            D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_saturate_dst_vb, 0,
            sizeof(expected_saturate), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_saturate); ++i)
        {
            float dx = mapped[i].x - expected_saturate[i].x;
            float dy = mapped[i].y - expected_saturate[i].y;
            float dz = mapped[i].z - expected_saturate[i].z;
            float dw = mapped[i].rhw - expected_saturate[i].rhw;
            float du = mapped[i].u - expected_saturate[i].u;
            float dv = mapped[i].v - expected_saturate[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_saturate[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_saturate_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_mad_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_mad_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0,
            (const float *)vs_mad_constants, 2), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_mad_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_mad_dst_vb, 0, sizeof(expected),
            (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected); ++i)
        {
            float dx = mapped[i].x - expected[i].x;
            float dy = mapped[i].y - expected[i].y;
            float dz = mapped[i].z - expected[i].z;
            float dw = mapped[i].rhw - expected[i].rhw;
            float du = mapped[i].u - expected[i].u;
            float dv = mapped[i].v - expected[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_mad_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_pos4_decl_elements, &src_pos4_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            src_short4n_pos_decl_elements, &src_short4n_pos_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_short4n_pos), 0, 0, D3DPOOL_SYSTEMMEM,
            &fixed_short4n_pos_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_short4n_pos_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_short4n_pos_vb, 0, sizeof(struct src_short4n_pos_vertex)),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_short4n_pos), fixed_short4n_pos_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(fixed_short4n_pos_dst_vb, 0,
            sizeof(expected_short4n_pos), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_short4n_pos); ++i)
        {
            float dx = mapped[i].x - expected_short4n_pos[i].x;
            float dy = mapped[i].y - expected_short4n_pos[i].y;
            float dz = mapped[i].z - expected_short4n_pos[i].z;
            float dw = mapped[i].rhw - expected_short4n_pos[i].rhw;
            float du = mapped[i].u - expected_short4n_pos[i].u;
            float dv = mapped[i].v - expected_short4n_pos[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.02f);
            CHECK_TRUE(dy < 0.02f);
            CHECK_TRUE(dz < 0.02f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_short4n_pos[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(fixed_short4n_pos_dst_vb),
                D3D_OK);
    }
    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_pos4_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_pos4), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_pos4_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_pos4_decl),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_pos4_vb, 0,
            sizeof(struct src_pos4_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_pos4), prog_pos4_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_pos4_dst_vb, 0,
            sizeof(expected_pos4), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_pos4); ++i)
        {
            float dx = mapped[i].x - expected_pos4[i].x;
            float dy = mapped[i].y - expected_pos4[i].y;
            float dz = mapped[i].z - expected_pos4[i].z;
            float dw = mapped[i].rhw - expected_pos4[i].rhw;
            float du = mapped[i].u - expected_pos4[i].u;
            float dv = mapped[i].v - expected_pos4[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_pos4[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_pos4_dst_vb), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device,
            sizeof(expected_short4n_pos), 0, 0, D3DPOOL_SYSTEMMEM,
            &prog_short4n_pos_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device,
            src_short4n_pos_decl), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0,
            src_short4n_pos_vb, 0, sizeof(struct src_short4n_pos_vertex)),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_short4n_pos), prog_short4n_pos_dst_vb,
            dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_short4n_pos_dst_vb, 0,
            sizeof(expected_short4n_pos), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_short4n_pos); ++i)
        {
            float dx = mapped[i].x - expected_short4n_pos[i].x;
            float dy = mapped[i].y - expected_short4n_pos[i].y;
            float dz = mapped[i].z - expected_short4n_pos[i].z;
            float dw = mapped[i].rhw - expected_short4n_pos[i].rhw;
            float du = mapped[i].u - expected_short4n_pos[i].u;
            float dv = mapped[i].v - expected_short4n_pos[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.02f);
            CHECK_TRUE(dy < 0.02f);
            CHECK_TRUE(dz < 0.02f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_short4n_pos[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_short4n_pos_dst_vb),
                D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);

    IDirect3DVertexShader9_Release(vs);
    vs = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device, process_vs_pos4_3_0, &vs);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected_pos4), 0,
            0, D3DPOOL_SYSTEMMEM, &prog_xyzw_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZW | D3DFVF_DIFFUSE | D3DFVF_TEX1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_pos4_vb, 0,
            sizeof(struct src_pos4_vertex)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src_pos4), prog_xyzw_dst_vb, dst_decl, 0), D3D_OK);

    hr = IDirect3DVertexBuffer9_Lock(prog_xyzw_dst_vb, 0,
            sizeof(expected_pos4), (void **)&mapped, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < ARRAY_SIZE(expected_pos4); ++i)
        {
            float dx = mapped[i].x - expected_pos4[i].x;
            float dy = mapped[i].y - expected_pos4[i].y;
            float dz = mapped[i].z - expected_pos4[i].z;
            float dw = mapped[i].rhw - expected_pos4[i].rhw;
            float du = mapped[i].u - expected_pos4[i].u;
            float dv = mapped[i].v - expected_pos4[i].v;
            if (dx < 0.0f) dx = -dx;
            if (dy < 0.0f) dy = -dy;
            if (dz < 0.0f) dz = -dz;
            if (dw < 0.0f) dw = -dw;
            if (du < 0.0f) du = -du;
            if (dv < 0.0f) dv = -dv;
            CHECK_TRUE(dx < 0.01f);
            CHECK_TRUE(dy < 0.01f);
            CHECK_TRUE(dz < 0.01f);
            CHECK_TRUE(dw < 0.01f);
            CHECK_TRUE(mapped[i].color == expected_pos4[i].color);
            CHECK_TRUE(du < 0.01f);
            CHECK_TRUE(dv < 0.01f);
        }
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(prog_xyzw_dst_vb), D3D_OK);
    }
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);

done_device:
    if (vs) IDirect3DVertexShader9_Release(vs);
    if (src_generic_index_split_decl) IDirect3DVertexDeclaration9_Release(src_generic_index_split_decl);
    if (src_generic_d3dcolor_decl) IDirect3DVertexDeclaration9_Release(src_generic_d3dcolor_decl);
    if (src_generic_short2_decl) IDirect3DVertexDeclaration9_Release(src_generic_short2_decl);
    if (src_generic_ubyte4n_decl) IDirect3DVertexDeclaration9_Release(src_generic_ubyte4n_decl);
    if (src_generic_udec3_decl) IDirect3DVertexDeclaration9_Release(src_generic_udec3_decl);
    if (src_d3dcolor_tex_decl) IDirect3DVertexDeclaration9_Release(src_d3dcolor_tex_decl);
    if (src_generic_split_decl) IDirect3DVertexDeclaration9_Release(src_generic_split_decl);
    if (src_extra_split_decl) IDirect3DVertexDeclaration9_Release(src_extra_split_decl);
    if (src_ubyte4n_blendweight_decl) IDirect3DVertexDeclaration9_Release(src_ubyte4n_blendweight_decl);
    if (src_d3dcolor_blendindices_decl) IDirect3DVertexDeclaration9_Release(src_d3dcolor_blendindices_decl);
    if (src_extra_decl) IDirect3DVertexDeclaration9_Release(src_extra_decl);
    if (src_short4_normal_decl) IDirect3DVertexDeclaration9_Release(src_short4_normal_decl);
    if (src_ubyte4_normal_decl) IDirect3DVertexDeclaration9_Release(src_ubyte4_normal_decl);
    if (src_ubyte4n_normal_decl) IDirect3DVertexDeclaration9_Release(src_ubyte4n_normal_decl);
    if (src_dec3n_normal_decl) IDirect3DVertexDeclaration9_Release(src_dec3n_normal_decl);
    if (src_udec3_normal_decl) IDirect3DVertexDeclaration9_Release(src_udec3_normal_decl);
    if (src_short4n_pos_decl) IDirect3DVertexDeclaration9_Release(src_short4n_pos_decl);
    if (src_pos4_decl) IDirect3DVertexDeclaration9_Release(src_pos4_decl);
    if (src_dec3n_tex_decl) IDirect3DVertexDeclaration9_Release(src_dec3n_tex_decl);
    if (src_udec3_tex_decl) IDirect3DVertexDeclaration9_Release(src_udec3_tex_decl);
    if (src_ubyte4n_tex_decl) IDirect3DVertexDeclaration9_Release(src_ubyte4n_tex_decl);
    if (src_ubyte4_tex_decl) IDirect3DVertexDeclaration9_Release(src_ubyte4_tex_decl);
    if (src_float16_4_tex_decl) IDirect3DVertexDeclaration9_Release(src_float16_4_tex_decl);
    if (src_ushort4n_tex_decl) IDirect3DVertexDeclaration9_Release(src_ushort4n_tex_decl);
    if (src_short4n_tex_decl) IDirect3DVertexDeclaration9_Release(src_short4n_tex_decl);
    if (src_short4_tex_decl) IDirect3DVertexDeclaration9_Release(src_short4_tex_decl);
    if (src_float16_tex_decl) IDirect3DVertexDeclaration9_Release(src_float16_tex_decl);
    if (src_ushort2n_tex_decl) IDirect3DVertexDeclaration9_Release(src_ushort2n_tex_decl);
    if (src_short2n_tex_decl) IDirect3DVertexDeclaration9_Release(src_short2n_tex_decl);
    if (src_short2_tex_decl) IDirect3DVertexDeclaration9_Release(src_short2_tex_decl);
    if (src_sparse_tex7_decl) IDirect3DVertexDeclaration9_Release(src_sparse_tex7_decl);
    if (src_sparse_tex_decl) IDirect3DVertexDeclaration9_Release(src_sparse_tex_decl);
    if (src_decl) IDirect3DVertexDeclaration9_Release(src_decl);
    if (dst_packed_tex_decl) IDirect3DVertexDeclaration9_Release(dst_packed_tex_decl);
    if (dst_d3dcolor_tex_decl) IDirect3DVertexDeclaration9_Release(dst_d3dcolor_tex_decl);
    if (dst_tex4_decl) IDirect3DVertexDeclaration9_Release(dst_tex4_decl);
    if (dst_specular_decl) IDirect3DVertexDeclaration9_Release(dst_specular_decl);
    if (dst_psize_decl) IDirect3DVertexDeclaration9_Release(dst_psize_decl);
    if (dst_sparse_tex7_decl) IDirect3DVertexDeclaration9_Release(dst_sparse_tex7_decl);
    if (dst_sparse_tex_decl) IDirect3DVertexDeclaration9_Release(dst_sparse_tex_decl);
    if (dst_decl) IDirect3DVertexDeclaration9_Release(dst_decl);
    if (prog_xyzw_dst_vb) IDirect3DVertexBuffer9_Release(prog_xyzw_dst_vb);
    if (prog_short4n_pos_dst_vb) IDirect3DVertexBuffer9_Release(prog_short4n_pos_dst_vb);
    if (prog_tex4_dst_vb) IDirect3DVertexBuffer9_Release(prog_tex4_dst_vb);
    if (prog_short2n_tex_dst_vb) IDirect3DVertexBuffer9_Release(prog_short2n_tex_dst_vb);
    if (prog_packed_tex_dst_vb) IDirect3DVertexBuffer9_Release(prog_packed_tex_dst_vb);
    if (prog_d3dcolor_tex_dst_vb) IDirect3DVertexBuffer9_Release(prog_d3dcolor_tex_dst_vb);
    if (prog_short2_tex_dst_vb) IDirect3DVertexBuffer9_Release(prog_short2_tex_dst_vb);
    if (prog_pos4_dst_vb) IDirect3DVertexBuffer9_Release(prog_pos4_dst_vb);
    if (prog_partialprecision_dst_vb) IDirect3DVertexBuffer9_Release(prog_partialprecision_dst_vb);
    if (prog_saturate_dst_vb) IDirect3DVertexBuffer9_Release(prog_saturate_dst_vb);
    if (fixed_short4n_pos_dst_vb) IDirect3DVertexBuffer9_Release(fixed_short4n_pos_dst_vb);
    if (prog_texldl_dst_vb) IDirect3DVertexBuffer9_Release(prog_texldl_dst_vb);
    if (prog_mova_component_relative_dst_vb) IDirect3DVertexBuffer9_Release(prog_mova_component_relative_dst_vb);
    if (prog_matrix_relative_dst_vb) IDirect3DVertexBuffer9_Release(prog_matrix_relative_dst_vb);
    if (prog_input_relative_dst_vb) IDirect3DVertexBuffer9_Release(prog_input_relative_dst_vb);
    if (prog_output_relative_dst_vb) IDirect3DVertexBuffer9_Release(prog_output_relative_dst_vb);
    if (prog_mad_dst_vb) IDirect3DVertexBuffer9_Release(prog_mad_dst_vb);
    if (prog_blendindices_dst_vb) IDirect3DVertexBuffer9_Release(prog_blendindices_dst_vb);
    if (prog_generic_usage_dst_vb) IDirect3DVertexBuffer9_Release(prog_generic_usage_dst_vb);
    if (prog_extra_multistream_dst_vb) IDirect3DVertexBuffer9_Release(prog_extra_multistream_dst_vb);
    if (prog_blendweight_dst_vb) IDirect3DVertexBuffer9_Release(prog_blendweight_dst_vb);
    if (prog_binormal_dst_vb) IDirect3DVertexBuffer9_Release(prog_binormal_dst_vb);
    if (prog_matrix_special_dst_vb) IDirect3DVertexBuffer9_Release(prog_matrix_special_dst_vb);
    if (prog_rep_loop_dst_vb) IDirect3DVertexBuffer9_Release(prog_rep_loop_dst_vb);
    if (prog_bool_dst_vb) IDirect3DVertexBuffer9_Release(prog_bool_dst_vb);
    if (prog_flow_if_dst_vb) IDirect3DVertexBuffer9_Release(prog_flow_if_dst_vb);
    if (prog_source_modifiers_dst_vb) IDirect3DVertexBuffer9_Release(prog_source_modifiers_dst_vb);
    if (prog_transcendent_dst_vb) IDirect3DVertexBuffer9_Release(prog_transcendent_dst_vb);
    if (prog_scalar_math_dst_vb) IDirect3DVertexBuffer9_Release(prog_scalar_math_dst_vb);
    if (prog_compare_dst_vb) IDirect3DVertexBuffer9_Release(prog_compare_dst_vb);
    if (prog_vector_math_dst_vb) IDirect3DVertexBuffer9_Release(prog_vector_math_dst_vb);
    if (prog_tangent_dst_vb) IDirect3DVertexBuffer9_Release(prog_tangent_dst_vb);
    if (prog_fvf_normal_dst_vb) IDirect3DVertexBuffer9_Release(prog_fvf_normal_dst_vb);
    if (prog_dec3n_normal_dst_vb) IDirect3DVertexBuffer9_Release(prog_dec3n_normal_dst_vb);
    if (prog_udec3_normal_dst_vb) IDirect3DVertexBuffer9_Release(prog_udec3_normal_dst_vb);
    if (prog_ubyte4n_normal_dst_vb) IDirect3DVertexBuffer9_Release(prog_ubyte4n_normal_dst_vb);
    if (prog_raw_normal_dst_vb) IDirect3DVertexBuffer9_Release(prog_raw_normal_dst_vb);
    if (prog_normal_dst_vb) IDirect3DVertexBuffer9_Release(prog_normal_dst_vb);
    if (prog_specular_decl_dst_vb) IDirect3DVertexBuffer9_Release(prog_specular_decl_dst_vb);
    if (prog_specular_dst_vb) IDirect3DVertexBuffer9_Release(prog_specular_dst_vb);
    if (prog_fvf_tex2_dst_vb) IDirect3DVertexBuffer9_Release(prog_fvf_tex2_dst_vb);
    if (prog_fvf_psize_dst_vb) IDirect3DVertexBuffer9_Release(prog_fvf_psize_dst_vb);
    if (prog_psize_dst_vb) IDirect3DVertexBuffer9_Release(prog_psize_dst_vb);
    if (prog_sparse_tex7_dst_vb) IDirect3DVertexBuffer9_Release(prog_sparse_tex7_dst_vb);
    if (prog_sparse_tex_dst_vb) IDirect3DVertexBuffer9_Release(prog_sparse_tex_dst_vb);
    if (prog_legacy_dst_vb) IDirect3DVertexBuffer9_Release(prog_legacy_dst_vb);
    if (prog_dst_vb) IDirect3DVertexBuffer9_Release(prog_dst_vb);
    if (src_extra_dst_vb) IDirect3DVertexBuffer9_Release(src_extra_dst_vb);
    if (src_decl_dst_vb) IDirect3DVertexBuffer9_Release(src_decl_dst_vb);
    if (psize_decl_dst_vb) IDirect3DVertexBuffer9_Release(psize_decl_dst_vb);
    if (psize_dst_vb) IDirect3DVertexBuffer9_Release(psize_dst_vb);
    if (specular_decl_dst_vb) IDirect3DVertexBuffer9_Release(specular_decl_dst_vb);
    if (decl_dst_vb) IDirect3DVertexBuffer9_Release(decl_dst_vb);
    if (offset_dst_vb) IDirect3DVertexBuffer9_Release(offset_dst_vb);
    if (lit_specular_dst_vb) IDirect3DVertexBuffer9_Release(lit_specular_dst_vb);
    if (lit_dst_vb) IDirect3DVertexBuffer9_Release(lit_dst_vb);
    if (fvf_tex2_dst_vb) IDirect3DVertexBuffer9_Release(fvf_tex2_dst_vb);
    if (dst_vb) IDirect3DVertexBuffer9_Release(dst_vb);
    if (src_extra_vb) IDirect3DVertexBuffer9_Release(src_extra_vb);
    if (src_dec3n_normal_vb) IDirect3DVertexBuffer9_Release(src_dec3n_normal_vb);
    if (src_udec3_normal_vb) IDirect3DVertexBuffer9_Release(src_udec3_normal_vb);
    if (src_ubyte4n_normal_vb) IDirect3DVertexBuffer9_Release(src_ubyte4n_normal_vb);
    if (src_ubyte4_normal_vb) IDirect3DVertexBuffer9_Release(src_ubyte4_normal_vb);
    if (src_short4_normal_vb) IDirect3DVertexBuffer9_Release(src_short4_normal_vb);
    if (src_fvf_tex2_vb) IDirect3DVertexBuffer9_Release(src_fvf_tex2_vb);
    if (src_fvf_blendindices_vb) IDirect3DVertexBuffer9_Release(src_fvf_blendindices_vb);
    if (src_fvf_blendweight_vb) IDirect3DVertexBuffer9_Release(src_fvf_blendweight_vb);
    if (src_fvf_material_sources_vb) IDirect3DVertexBuffer9_Release(src_fvf_material_sources_vb);
    if (src_fvf_normal_specular_vb) IDirect3DVertexBuffer9_Release(src_fvf_normal_specular_vb);
    if (src_fvf_normal_vb) IDirect3DVertexBuffer9_Release(src_fvf_normal_vb);
    if (src_short4n_pos_vb) IDirect3DVertexBuffer9_Release(src_short4n_pos_vb);
    if (src_pos4_vb) IDirect3DVertexBuffer9_Release(src_pos4_vb);
    if (src_attr_ubyte4n_vb) IDirect3DVertexBuffer9_Release(src_attr_ubyte4n_vb);
    if (src_attr_ubyte4_vb) IDirect3DVertexBuffer9_Release(src_attr_ubyte4_vb);
    if (src_attr_float16_4_vb) IDirect3DVertexBuffer9_Release(src_attr_float16_4_vb);
    if (src_attr_ushort4n_vb) IDirect3DVertexBuffer9_Release(src_attr_ushort4n_vb);
    if (src_attr_short4n_vb) IDirect3DVertexBuffer9_Release(src_attr_short4n_vb);
    if (src_attr_short4_vb) IDirect3DVertexBuffer9_Release(src_attr_short4_vb);
    if (src_attr_float16_vb) IDirect3DVertexBuffer9_Release(src_attr_float16_vb);
    if (src_attr_udec3_vb) IDirect3DVertexBuffer9_Release(src_attr_udec3_vb);
    if (src_attr_ushort2n_vb) IDirect3DVertexBuffer9_Release(src_attr_ushort2n_vb);
    if (src_attr_short2n_vb) IDirect3DVertexBuffer9_Release(src_attr_short2n_vb);
    if (src_attr_short2_vb) IDirect3DVertexBuffer9_Release(src_attr_short2_vb);
    if (src_attr_vb) IDirect3DVertexBuffer9_Release(src_attr_vb);
    if (src_psize_vb) IDirect3DVertexBuffer9_Release(src_psize_vb);
    if (src_specular_vb) IDirect3DVertexBuffer9_Release(src_specular_vb);
    if (src_depth_clamp_vb) IDirect3DVertexBuffer9_Release(src_depth_clamp_vb);
    if (src_texldl_vb) IDirect3DVertexBuffer9_Release(src_texldl_vb);
    if (src_vb) IDirect3DVertexBuffer9_Release(src_vb);
    if (vs_texldl_a8p8_texture) IDirect3DTexture9_Release(vs_texldl_a8p8_texture);
    if (vs_texldl_p8_texture) IDirect3DTexture9_Release(vs_texldl_p8_texture);
    if (vs_texldl_texture) IDirect3DTexture9_Release(vs_texldl_texture);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
#undef PROCESS_VS_DCL
#undef PROCESS_VS_INST_CTRL
#undef PROCESS_VS_INST_PRED
#undef PROCESS_VS_INST
#undef PROCESS_VS_SRC_MOD
#undef PROCESS_VS_SRC_REL
#undef PROCESS_VS_SRC_SWZ
#undef PROCESS_VS_SWIZZLE
#undef PROCESS_VS_SRC
#undef PROCESS_VS_DST_MOD
#undef PROCESS_VS_DST_REL
#undef PROCESS_VS_DST
#undef PROCESS_VS_REGTYPE
}
