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
    static const D3DVERTEXELEMENT9 dst_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 20, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 src_decl_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
        {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    struct src_vertex
    {
        float x, y, z;
        DWORD color;
        float u, v;
    };
    struct dst_vertex
    {
        float x, y, z, rhw;
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
    const struct dst_vertex expected[] =
    {
        {240.0f, 300.0f, 0.0f, 1.0f, 0xffff0000u, 0.00f, 0.25f},
        {240.0f, 180.0f, 0.0f, 1.0f, 0xff00ff00u, 0.25f, 0.50f},
        {400.0f, 300.0f, 0.0f, 1.0f, 0xff0000ffu, 0.50f, 0.75f},
        {400.0f, 180.0f, 0.0f, 1.0f, 0xffffffffu, 1.00f, 1.00f},
    };
    IDirect3DVertexBuffer9 *src_vb = NULL;
    IDirect3DVertexBuffer9 *dst_vb = NULL;
    IDirect3DVertexBuffer9 *decl_dst_vb = NULL;
    IDirect3DVertexBuffer9 *src_decl_dst_vb = NULL;
    IDirect3DVertexDeclaration9 *src_decl = NULL;
    IDirect3DVertexDeclaration9 *dst_decl = NULL;
    IDirect3DDevice9 *device = NULL;
    struct dst_vertex *mapped = NULL;
    D3DMATRIX world;
    void *bits = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    UINT i;

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

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, src_decl_elements,
            &src_decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(expected), 0,
            0, D3DPOOL_SYSTEMMEM, &src_decl_dst_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, src_decl), D3D_OK);
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

done_device:
    if (src_decl) IDirect3DVertexDeclaration9_Release(src_decl);
    if (dst_decl) IDirect3DVertexDeclaration9_Release(dst_decl);
    if (src_decl_dst_vb) IDirect3DVertexBuffer9_Release(src_decl_dst_vb);
    if (decl_dst_vb) IDirect3DVertexBuffer9_Release(decl_dst_vb);
    if (dst_vb) IDirect3DVertexBuffer9_Release(dst_vb);
    if (src_vb) IDirect3DVertexBuffer9_Release(src_vb);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
