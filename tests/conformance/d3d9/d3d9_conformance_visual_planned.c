/*
 * Planned-item conformance scaffolds derived from Wine's
 * dlls/d3d9/tests/visual.c oracle. These scaffolds absorb the
 * remaining specs/wine_test.plan.md section 5.1 planned/EXP-route
 * rows whose observable contract is expressible at the public D3D9
 * ABI level without a full runtime probe.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/visual.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Wine is referenced as an LGPL behavioral oracle only; no Wine
 * implementation, control flow, or table data is copied here.
 */

#include "d3d9_conformance_fixtures.h"

#ifndef D3DFMT_NULL
#define D3DFMT_NULL ((D3DFORMAT)MAKEFOURCC('N','U','L','L'))
#endif

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_specular_lighting
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_specular_lighting_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DMATERIAL9 material;
    D3DMATERIAL9 out;
    HWND window;
    DWORD specular_enable;

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

    /* Default specular enable is FALSE. */
    specular_enable = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SPECULARENABLE,
            &specular_enable), D3D_OK);
    CHECK_TRUE(specular_enable == FALSE);

    /* Round-trip TRUE then back to FALSE. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE,
            TRUE), D3D_OK);
    specular_enable = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SPECULARENABLE,
            &specular_enable), D3D_OK);
    CHECK_TRUE(specular_enable == TRUE);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SPECULARENABLE,
            FALSE), D3D_OK);
    specular_enable = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SPECULARENABLE,
            &specular_enable), D3D_OK);
    CHECK_TRUE(specular_enable == FALSE);

    /* Default material: black specular, power 0.0f. */
    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetMaterial(device, &out), D3D_OK);
    CHECK_TRUE(out.Specular.r == 0.0f);
    CHECK_TRUE(out.Specular.g == 0.0f);
    CHECK_TRUE(out.Specular.b == 0.0f);
    CHECK_TRUE(out.Specular.a == 0.0f);
    CHECK_TRUE(out.Power == 0.0f);

    memset(&material, 0, sizeof(material));
    material.Diffuse.r = 0.25f;
    material.Specular.r = 0.5f;
    material.Specular.g = 0.75f;
    material.Specular.b = 1.0f;
    material.Specular.a = 1.0f;
    material.Power = 25.0f;
    CHECK_HR(IDirect3DDevice9_SetMaterial(device, &material), D3D_OK);

    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetMaterial(device, &out), D3D_OK);
    CHECK_TRUE(out.Specular.r == 0.5f);
    CHECK_TRUE(out.Specular.g == 0.75f);
    CHECK_TRUE(out.Specular.b == 1.0f);
    CHECK_TRUE(out.Specular.a == 1.0f);
    CHECK_TRUE(out.Power == 25.0f);
    CHECK_TRUE(out.Diffuse.r == 0.25f);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_max_index16
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_max_index16_draw_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    HWND window;
    HRESULT hr;
    void *data = NULL;
    WORD *indices;
    float *verts;
    unsigned int i;

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

    memset(&caps, 0, sizeof(caps));
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &caps), D3D_OK);
    CHECK_TRUE(caps.MaxVertexIndex >= 0xfffe);

    hr = IDirect3DDevice9_CreateIndexBuffer(device, sizeof(WORD) * 4,
            D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DIndexBuffer9_Lock(ib, 0, 0, &data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        indices = (WORD *)data;
        indices[0] = 0;
        indices[1] = (WORD)0xfffd;
        indices[2] = (WORD)0xfffe;
        indices[3] = 0;
        CHECK_HR(IDirect3DIndexBuffer9_Unlock(ib), D3D_OK);
    }

    /* Small VB so MinVertexIndex/NumVertices declares 0..0xfffe. */
    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(float) * 3 * 3,
            D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_ib;

    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, &data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        verts = (float *)data;
        for (i = 0; i < 9; ++i)
            verts[i] = 0.0f;
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(vb), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vb, 0,
            sizeof(float) * 3), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetIndices(device, ib), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ), D3D_OK);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    /* Accept either S_OK or D3DERR_INVALIDCALL; must not crash. */
    hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
            0, 0, 0xffff, 0, 1);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);

    IDirect3DVertexBuffer9_Release(vb);
done_ib:
    IDirect3DIndexBuffer9_Release(ib);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * dxmt9-specific policy test (no Wine oracle function).
 *
 * Pins the HRESULT dxmt9 returns for an indexed draw whose index range runs
 * past the end of the bound index buffer, ON A HARDWARE-VP DEVICE.
 *
 * Why this needs its own test. Until 83a0b085 the two SWVP fallback probes ran
 * on every indexed draw and read the whole index buffer BEFORE checking whether
 * software vertex processing was enabled; readSoftwareFfpAdjustedIndices'
 * bounds check therefore rejected out-of-range draws on every device, hardware
 * VP included, as a side effect of a probe that could not apply. Hoisting the
 * applicability gate above the read (a 22.6%-of-frame win on 3DMark05 GT2,
 * docs/perfomance/state-churn-encode/state-churn-encode-append-decomposition.09)
 * removed that incidental validation.
 *
 * Retail D3D9 does not validate index ranges outside the debug runtime, so
 * D3D_OK is the faithful answer. The point of the test is that nothing else can
 * see this: create_base_device() and the whole visual_mvp_software_vp_policy
 * oracle use D3DCREATE_SOFTWARE_VERTEXPROCESSING, where the hoisted gate
 * evaluates true and the code is bit-identical. A hardware-VP device is the
 * only configuration in which the behaviour changed.
 *
 * NOT YET A PIN -- the assertion below is deliberately tolerant.
 * Attempting to tighten it to CHECK_HR(hr, D3D_OK) exposed a harness problem
 * that has to be fixed first: this suite could not be made to observe ANY
 * change to d3d9.dll. Forcing DrawIndexedPrimitive to return D3DERR_INVALIDCALL
 * unconditionally, rebuilding, and restaging still produced D3D_OK here, while
 * the entry point's own debug log (which sits AFTER the forced return) kept
 * appearing -- so the module Wine loads is not the module meson builds, even
 * though tmp/conformance-prefix/drive_c/windows/system32/d3d9.dll and the
 * exe-adjacent copy both md5-match the build output. Separately, that prefix
 * was found holding a d3d9.dll two weeks stale (2026-07-18), so it is staged by
 * hand and nothing refreshes it.
 *
 * Until a conformance run is demonstrated to fail when the code under test
 * changes, a strict expectation here would be decoration: it would pass
 * whatever the runtime does. Tighten this to CHECK_HR(hr, D3D_OK) and flip the
 * manifest status once the loader/staging path is understood.
 */
void test_visual_indexed_draw_out_of_range_hwvp_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vb = NULL;
    IDirect3DIndexBuffer9 *ib = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    void *data = NULL;
    WORD *indices;
    float *verts;
    unsigned int i;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }
    window = create_test_window();
    if (!window)
        goto done_d3d9;

    /* Hardware VP specifically -- software VP cannot observe the change. */
    pp = default_present_parameters(window);
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
    {
        skip_current_test("no hardware vertex processing device");
        goto done_window;
    }

    /* 3 indices only; the draw below asks for indices [2..4]. */
    hr = IDirect3DDevice9_CreateIndexBuffer(device, sizeof(WORD) * 3,
            D3DUSAGE_WRITEONLY, D3DFMT_INDEX16, D3DPOOL_DEFAULT, &ib, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DIndexBuffer9_Lock(ib, 0, 0, &data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        indices = (WORD *)data;
        indices[0] = 0;
        indices[1] = 1;
        indices[2] = 2;
        CHECK_HR(IDirect3DIndexBuffer9_Unlock(ib), D3D_OK);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(float) * 3 * 3,
            D3DUSAGE_WRITEONLY, D3DFVF_XYZ, D3DPOOL_DEFAULT, &vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_ib;

    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, &data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        verts = (float *)data;
        for (i = 0; i < 9; ++i)
            verts[i] = 0.0f;
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(vb), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vb, 0,
            sizeof(float) * 3), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetIndices(device, ib), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ), D3D_OK);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    /* StartIndex 2 + 3 indices needs [2..4] from a 3-index buffer. Tolerant
     * until the harness is shown to detect a change -- see the header. */
    hr = IDirect3DDevice9_DrawIndexedPrimitive(device, D3DPT_TRIANGLELIST,
            0, 0, 3, 2, 1);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);

    /* The draw is now recorded rather than rejected, so it reaches the backend.
     * It must not lose or wedge the device. */
    CHECK_HR(IDirect3DDevice9_TestCooperativeLevel(device), D3D_OK);

    IDirect3DVertexBuffer9_Release(vb);
done_ib:
    IDirect3DIndexBuffer9_Release(ib);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_null_format
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_null_format_caps_policy(const struct d3d9_api *api)
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

    /* CheckDeviceFormat at the factory level — record whichever HRESULT
     * the host reports. NULL render-target surfaces are an optional cap. */
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
            D3DRTYPE_SURFACE, D3DFMT_NULL);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);

    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_NULL render targets are not supported");
        goto done_d3d9;
    }

    window = create_test_window();
    if (!window)
        goto done_d3d9;
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64, D3DFMT_NULL,
            D3DMULTISAMPLE_NONE, 0, FALSE, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&desc, 0xcc, sizeof(desc));
        CHECK_HR(IDirect3DSurface9_GetDesc(surface, &desc), D3D_OK);
        /* Format round-trips. Some hosts normalize to the canonical
         * D3DFMT_NULL fourcc — accept either equality form. */
        CHECK_TRUE(desc.Format == D3DFMT_NULL);
        CHECK_TRUE(desc.Width == 64);
        CHECK_TRUE(desc.Height == 64);
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
 * function: test_sample_mask
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_sample_mask_render_state_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    DWORD value;

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

    /* Default multisample mask is all-ones. */
    value = 0;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_MULTISAMPLEMASK,
            &value), D3D_OK);
    CHECK_TRUE(value == 0xffffffff);

    /* Round-trip an arbitrary 32-bit mask byte-equal. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_MULTISAMPLEMASK,
            0xdeadbeef), D3D_OK);
    value = 0;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_MULTISAMPLEMASK,
            &value), D3D_OK);
    CHECK_TRUE(value == 0xdeadbeef);

    /* Default MULTISAMPLEANTIALIAS is TRUE. */
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_MULTISAMPLEANTIALIAS, &value), D3D_OK);
    CHECK_TRUE(value == TRUE);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_MULTISAMPLEANTIALIAS, FALSE), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_MULTISAMPLEANTIALIAS, &value), D3D_OK);
    CHECK_TRUE(value == FALSE);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_MULTISAMPLEANTIALIAS, TRUE), D3D_OK);
    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device,
            D3DRS_MULTISAMPLEANTIALIAS, &value), D3D_OK);
    CHECK_TRUE(value == TRUE);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_depth_stencil_init
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_depth_stencil_init_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *ds = NULL;
    IDirect3DSurface9 *out = NULL;
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

    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 64, 64,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&desc, 0xcc, sizeof(desc));
    CHECK_HR(IDirect3DSurface9_GetDesc(ds, &desc), D3D_OK);
    CHECK_TRUE(desc.Width == 64);
    CHECK_TRUE(desc.Height == 64);
    CHECK_TRUE(desc.Format == D3DFMT_D24S8);
    CHECK_TRUE(desc.Pool == D3DPOOL_DEFAULT);
    CHECK_TRUE(desc.Type == D3DRTYPE_SURFACE);

    /* SetDepthStencilSurface(ds) then GetDepthStencilSurface returns the
     * same surface AddRef'd. */
    CHECK_HR(IDirect3DDevice9_SetDepthStencilSurface(device, ds), D3D_OK);
    out = NULL;
    CHECK_HR(IDirect3DDevice9_GetDepthStencilSurface(device, &out), D3D_OK);
    CHECK_TRUE(out == ds);
    if (out)
        IDirect3DSurface9_Release(out);

    /* SetDepthStencilSurface(NULL) then GetDepthStencilSurface yields
     * D3DERR_NOTFOUND with NULL out-pointer. */
    CHECK_HR(IDirect3DDevice9_SetDepthStencilSurface(device, NULL), D3D_OK);
    out = (IDirect3DSurface9 *)0xdeadbeef;
    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &out);
    CHECK_HR(hr, D3DERR_NOTFOUND);
    CHECK_TRUE(out == NULL);

    IDirect3DSurface9_Release(ds);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
