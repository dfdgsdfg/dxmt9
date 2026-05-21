/*
 * D3D9 PE conformance — narrow public-ABI scaffolds for visual.c oracle
 * entries whose full rasterization aspect requires shader-runner DSL /
 * oracle infrastructure that is not yet present (see specs/gap.md).
 *
 * Each scaffold captures only the portion of the contract that is
 * observable through the public D3D9 ABI: state round-trips, declaration
 * round-trips, shader-create validation, and caps surface. The pixel /
 * rasterization comparison that the Wine oracle performs requires SR
 * runner DSL features (TEXBEM runner support, XYZRHW pretransformed
 * varying fixture, VFACE input fixture, NaN/Inf tolerance policy) and
 * remains deferred.
 *
 * Wine source is referenced as LGPL behavioral oracle only; no Wine
 * implementation code is copied into this file.
 */

#include "d3d9_conformance_fixtures.h"

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: fixed_function_bumpmap_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Note: the full rasterization aspect of this oracle requires SR
 * runner DSL / oracle infrastructure that is not yet present
 * (see specs/gap.md). This PE-side scaffold captures the public
 * ABI portion of the contract — D3DTOP_BUMPENVMAP /
 * D3DTOP_BUMPENVMAPLUMINANCE TSS round-trip, BUMPENVMAT/LSCALE/LOFFSET
 * DWORD-encoded float round-trip, and the TextureOpCaps surface.
 */
void test_visual_bumpenvmap_tss_policy(const struct d3d9_api *api)
{
    static const struct
    {
        D3DTEXTURESTAGESTATETYPE state;
        DWORD value;
    } matrix_cases[] =
    {
        {D3DTSS_BUMPENVMAT00, 0x3f800000}, /* 1.0f */
        {D3DTSS_BUMPENVMAT01, 0x00000000}, /* 0.0f */
        {D3DTSS_BUMPENVMAT10, 0x00000000}, /* 0.0f */
        {D3DTSS_BUMPENVMAT11, 0xbf800000}, /* -1.0f */
        {D3DTSS_BUMPENVLSCALE, 0x40000000}, /* 2.0f */
        {D3DTSS_BUMPENVLOFFSET, 0x3f000000}, /* 0.5f */
    };
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    DWORD value;
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
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    memset(&caps, 0, sizeof(caps));
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &caps), D3D_OK);
    /* Cap surface query: accept whatever value is reported per device. */
    (void)(caps.TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAP);
    (void)(caps.TextureOpCaps & D3DTEXOPCAPS_BUMPENVMAPLUMINANCE);

    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_COLOROP, D3DTOP_BUMPENVMAP), D3D_OK);
    value = 0xdeadbeef;
    hr = IDirect3DDevice9_GetTextureStageState(device, 0, D3DTSS_COLOROP,
            &value);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(value == D3DTOP_BUMPENVMAP);

    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_COLOROP, D3DTOP_BUMPENVMAPLUMINANCE), D3D_OK);
    value = 0xdeadbeef;
    hr = IDirect3DDevice9_GetTextureStageState(device, 0, D3DTSS_COLOROP,
            &value);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(value == D3DTOP_BUMPENVMAPLUMINANCE);

    for (i = 0; i < ARRAY_SIZE(matrix_cases); ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
                matrix_cases[i].state, matrix_cases[i].value), D3D_OK);
        value = 0xdeadbeef;
        hr = IDirect3DDevice9_GetTextureStageState(device, 0,
                matrix_cases[i].state, &value);
        CHECK_HR(hr, D3D_OK);
        /* DWORD-encoded floats round-trip byte-equal. */
        CHECK_TRUE(value == matrix_cases[i].value);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: pretransformed_varying_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Note: the full rasterization aspect of this oracle requires SR
 * runner DSL / oracle infrastructure that is not yet present
 * (see specs/gap.md). This PE-side scaffold captures the public
 * ABI portion of the contract — CreateVertexDeclaration acceptance
 * of D3DDECLUSAGE_POSITIONT, GetDeclaration round-trip, and the
 * implicit FVF→decl translation for D3DFVF_XYZRHW.
 */
void test_visual_pretransformed_vertex_declaration_policy(
        const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 positiont_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITIONT, 0},
        {0, 16, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    D3DVERTEXELEMENT9 actual[MAXD3DDECLLENGTH + 1];
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexDeclaration9 *fvf_decl = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT count;
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
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    /* POSITIONT (= 9) trailing element accepts via CreateVertexDeclaration. */
    hr = IDirect3DDevice9_CreateVertexDeclaration(device,
            positiont_elements, &decl);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(decl != NULL);
    if (!decl)
        goto done_device;

    count = 0;
    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, NULL, &count);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(count == ARRAY_SIZE(positiont_elements));

    count = ARRAY_SIZE(actual);
    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, actual, &count);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(count == ARRAY_SIZE(positiont_elements));
    for (i = 0; i < count; ++i)
        CHECK_TRUE(memcmp(&actual[i], &positiont_elements[i],
                sizeof(actual[i])) == 0);

    /* Implicit FVF→decl translation places POSITIONT in slot 0 for
     * D3DFVF_XYZRHW. */
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW), D3D_OK);
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &fvf_decl);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(fvf_decl != NULL);
    if (fvf_decl)
    {
        count = ARRAY_SIZE(actual);
        hr = IDirect3DVertexDeclaration9_GetDeclaration(fvf_decl, actual,
                &count);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(count >= 1);
        if (count >= 1)
            CHECK_TRUE(actual[0].Usage == D3DDECLUSAGE_POSITIONT);
        IDirect3DVertexDeclaration9_Release(fvf_decl);
    }

    IDirect3DVertexDeclaration9_Release(decl);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: vface_register_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Note: the full rasterization aspect of this oracle requires SR
 * runner DSL / oracle infrastructure that is not yet present
 * (see specs/gap.md). This PE-side scaffold captures the public
 * ABI portion of the contract — CreatePixelShader rejects NULL,
 * accepts a minimal ps_3_0 bytecode when the device caps report
 * ps_3_0, and exposes the PS20Caps surface without crashing.
 */
void test_visual_vface_pixel_shader_create_policy(const struct d3d9_api *api)
{
    static const DWORD minimal_ps_3_0[] =
    {
        0xffff0300, /* ps_3_0 header */
        0x0000ffff  /* end token */
    };
    IDirect3DPixelShader9 *shader = NULL;
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
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &caps), D3D_OK);
    /* PS20Caps surface readable without crashing. */
    (void)caps.PS20Caps.Caps;
    (void)caps.PS20Caps.DynamicFlowControlDepth;
    (void)caps.PS20Caps.NumTemps;

    /* NULL bytecode is rejected. */
    shader = (IDirect3DPixelShader9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreatePixelShader(device, NULL, &shader);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(shader == NULL || shader == (IDirect3DPixelShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && shader && shader != (IDirect3DPixelShader9 *)0xdeadbeef)
        IDirect3DPixelShader9_Release(shader);

    /* ps_3_0 minimal stream — accept iff device caps report ps_3_0. */
    if (caps.PixelShaderVersion >= D3DPS_VERSION(3, 0))
    {
        shader = NULL;
        hr = IDirect3DDevice9_CreatePixelShader(device, minimal_ps_3_0,
                &shader);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(shader != NULL);
        if (shader)
            IDirect3DPixelShader9_Release(shader);
    }
    else
    {
        skip_current_test("device does not advertise ps_3_0");
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: fp_special_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Note: the full rasterization aspect of this oracle requires SR
 * runner DSL / oracle infrastructure that is not yet present
 * (see specs/gap.md). This PE-side scaffold captures the public
 * ABI portion of the contract — FP-precision-adjacent caps surface
 * (PS20Caps.NumInstructionSlots, MaxPixelShader30InstructionSlots,
 * PrimitiveMiscCaps & D3DPMISCCAPS_FOGANDSPECULARALPHA). NaN/Inf
 * rasterization behavior cannot be probed through the public ABI.
 */
void test_visual_fp_special_caps_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
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

    memset(&caps, 0, sizeof(caps));
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &caps), D3D_OK);

    /* PS20Caps.NumInstructionSlots > 0 whenever ps_2_0+ is advertised. */
    if (caps.PixelShaderVersion >= D3DPS_VERSION(2, 0))
        CHECK_TRUE(caps.PS20Caps.NumInstructionSlots > 0);

    /* MaxPixelShader30InstructionSlots is exposed whenever ps_3_0+ is
     * advertised. */
    if (caps.PixelShaderVersion >= D3DPS_VERSION(3, 0))
        CHECK_TRUE(caps.MaxPixelShader30InstructionSlots > 0);

    /* Fog-related cap reported per device; accept whatever value is
     * reported (FOGINFVF in older SDKs is not exposed by mingw d3d9caps.h
     * — the comparable FP-precision-adjacent fog cap is
     * D3DPMISCCAPS_FOGANDSPECULARALPHA). */
    (void)(caps.PrimitiveMiscCaps & D3DPMISCCAPS_FOGANDSPECULARALPHA);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
