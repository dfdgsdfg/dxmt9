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
    IDirect3DDevice9 *device_swvp = NULL;
    D3DPRESENT_PARAMETERS pp;
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
        IDirect3DDevice9_Release(device_swvp);
    }

    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
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
    static const D3DVERTEXELEMENT9 src_pos4_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
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
    const struct dst_vertex expected[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
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
        {240.0f, 300.0f, 0.0f, 1.0f, 0xff336699u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff336699u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff336699u, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xff336699u, 1.00f, 1.00f},
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
    IDirect3DVertexBuffer9 *src_vb = NULL;
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
    IDirect3DVertexBuffer9 *src_fvf_normal_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_normal_specular_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_material_sources_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_blendweight_vb = NULL;
    IDirect3DVertexBuffer9 *src_fvf_blendindices_vb = NULL;
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
    IDirect3DVertexBuffer9 *prog_texldl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_partialprecision_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_saturate_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_pos4_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_xyzw_dst_vb = NULL;
    IDirect3DVertexBuffer9 *prog_short2_tex_dst_vb = NULL;
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
    IDirect3DVertexDeclaration9 *src_pos4_decl = NULL;
    IDirect3DVertexDeclaration9 *src_extra_decl = NULL;
    IDirect3DVertexDeclaration9 *src_extra_split_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_split_decl = NULL;
    IDirect3DVertexDeclaration9 *src_generic_index_split_decl = NULL;
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
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DTexture9 *vs_texldl_texture = NULL;
    IDirect3DDevice9 *device = NULL;
    struct dst_vertex *mapped = NULL;
    struct dst_tex1_vertex *mapped_tex1 = NULL;
    struct dst_tex4_vertex *mapped_tex4 = NULL;
    struct dst_specular_vertex *mapped_specular = NULL;
    struct dst_psize_vertex *mapped_psize = NULL;
    D3DLOCKED_RECT locked_rect;
    D3DMATERIAL9 material;
    D3DLIGHT9 light;
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
    const float vs_output_relative_constants[5][4] =
    {
        {0.5f, 0.0f, 0.0f, 0.0f},
        {0.0f, 0.5f, 0.0f, 0.0f},
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 1.0f},
        {1.0f, 0.0f, 0.0f, 0.0f},
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
    const float vs_compare_constants[2][4] =
    {
        {0.0f, 0.0f, 1.0f, 0.0f},
        {0.0f, 0.0f, 0.0f, 0.0f},
    };
    const float vs_scalar_math_constants[5][4] =
    {
        {-1.0f, -1.0f, -1.0f, -1.0f},
        { 2.0f,  2.0f,  2.0f,  2.0f},
        { 0.0f,  1.0f,  0.0f,  0.0f},
        { 0.0f,  0.0f,  1.0f,  0.0f},
        { 1.0f,  1.0f,  1.0f,  1.0f},
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
            (const float *)vs_scalar_math_constants, 5), D3D_OK);
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
            (const float *)vs_compare_constants, 2), D3D_OK);
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
    hr = IDirect3DDevice9_CreateTexture(device, 1, 1, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &vs_texldl_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DTexture9_LockRect(vs_texldl_texture, 0, &locked_rect, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        *(DWORD *)locked_rect.pBits = 0xff336699u;
        CHECK_HR(IDirect3DTexture9_UnlockRect(vs_texldl_texture, 0), D3D_OK);
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
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, src_vb, 0,
            sizeof(src[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, D3DVERTEXTEXTURESAMPLER0,
            (IDirect3DBaseTexture9 *)vs_texldl_texture), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, vs), D3D_OK);
    CHECK_HR(IDirect3DDevice9_ProcessVertices(device, 0, 0,
            ARRAY_SIZE(src), prog_texldl_dst_vb, dst_decl, 0),
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
    if (src_generic_split_decl) IDirect3DVertexDeclaration9_Release(src_generic_split_decl);
    if (src_extra_split_decl) IDirect3DVertexDeclaration9_Release(src_extra_split_decl);
    if (src_extra_decl) IDirect3DVertexDeclaration9_Release(src_extra_decl);
    if (src_short4_normal_decl) IDirect3DVertexDeclaration9_Release(src_short4_normal_decl);
    if (src_ubyte4_normal_decl) IDirect3DVertexDeclaration9_Release(src_ubyte4_normal_decl);
    if (src_ubyte4n_normal_decl) IDirect3DVertexDeclaration9_Release(src_ubyte4n_normal_decl);
    if (src_dec3n_normal_decl) IDirect3DVertexDeclaration9_Release(src_dec3n_normal_decl);
    if (src_udec3_normal_decl) IDirect3DVertexDeclaration9_Release(src_udec3_normal_decl);
    if (src_pos4_decl) IDirect3DVertexDeclaration9_Release(src_pos4_decl);
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
    if (dst_tex4_decl) IDirect3DVertexDeclaration9_Release(dst_tex4_decl);
    if (dst_specular_decl) IDirect3DVertexDeclaration9_Release(dst_specular_decl);
    if (dst_psize_decl) IDirect3DVertexDeclaration9_Release(dst_psize_decl);
    if (dst_sparse_tex7_decl) IDirect3DVertexDeclaration9_Release(dst_sparse_tex7_decl);
    if (dst_sparse_tex_decl) IDirect3DVertexDeclaration9_Release(dst_sparse_tex_decl);
    if (dst_decl) IDirect3DVertexDeclaration9_Release(dst_decl);
    if (prog_xyzw_dst_vb) IDirect3DVertexBuffer9_Release(prog_xyzw_dst_vb);
    if (prog_tex4_dst_vb) IDirect3DVertexBuffer9_Release(prog_tex4_dst_vb);
    if (prog_short2n_tex_dst_vb) IDirect3DVertexBuffer9_Release(prog_short2n_tex_dst_vb);
    if (prog_short2_tex_dst_vb) IDirect3DVertexBuffer9_Release(prog_short2_tex_dst_vb);
    if (prog_pos4_dst_vb) IDirect3DVertexBuffer9_Release(prog_pos4_dst_vb);
    if (prog_partialprecision_dst_vb) IDirect3DVertexBuffer9_Release(prog_partialprecision_dst_vb);
    if (prog_saturate_dst_vb) IDirect3DVertexBuffer9_Release(prog_saturate_dst_vb);
    if (prog_texldl_dst_vb) IDirect3DVertexBuffer9_Release(prog_texldl_dst_vb);
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
    if (dst_vb) IDirect3DVertexBuffer9_Release(dst_vb);
    if (src_extra_vb) IDirect3DVertexBuffer9_Release(src_extra_vb);
    if (src_dec3n_normal_vb) IDirect3DVertexBuffer9_Release(src_dec3n_normal_vb);
    if (src_udec3_normal_vb) IDirect3DVertexBuffer9_Release(src_udec3_normal_vb);
    if (src_ubyte4n_normal_vb) IDirect3DVertexBuffer9_Release(src_ubyte4n_normal_vb);
    if (src_ubyte4_normal_vb) IDirect3DVertexBuffer9_Release(src_ubyte4_normal_vb);
    if (src_short4_normal_vb) IDirect3DVertexBuffer9_Release(src_short4_normal_vb);
    if (src_fvf_blendindices_vb) IDirect3DVertexBuffer9_Release(src_fvf_blendindices_vb);
    if (src_fvf_blendweight_vb) IDirect3DVertexBuffer9_Release(src_fvf_blendweight_vb);
    if (src_fvf_material_sources_vb) IDirect3DVertexBuffer9_Release(src_fvf_material_sources_vb);
    if (src_fvf_normal_specular_vb) IDirect3DVertexBuffer9_Release(src_fvf_normal_specular_vb);
    if (src_fvf_normal_vb) IDirect3DVertexBuffer9_Release(src_fvf_normal_vb);
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
    if (src_vb) IDirect3DVertexBuffer9_Release(src_vb);
    if (vs_texldl_texture) IDirect3DTexture9_Release(vs_texldl_texture);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
#undef PROCESS_VS_DCL
#undef PROCESS_VS_INST_CTRL
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
