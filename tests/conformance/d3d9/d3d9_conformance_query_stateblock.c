/*
 * StateBlock-shaped conformance cases split from d3d9_conformance.c.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/stateblock.c (creation/record/apply paths)
 * - dlls/d3d9/tests/device.c (invalid-call recording transitions)
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 */

#include "d3d9_conformance_fixtures.h"

static void set_scale_matrix(D3DMATRIX *matrix, float scale)
{
    memset(matrix, 0, sizeof(*matrix));
    matrix->m[0][0] = scale;
    matrix->m[1][1] = scale;
    matrix->m[2][2] = scale;
    matrix->m[3][3] = scale;
}

static void check_matrix_equals(const D3DMATRIX *actual,
        const D3DMATRIX *expected)
{
    CHECK_TRUE(memcmp(actual, expected, sizeof(*actual)) == 0);
}

/*
 * Wine stateblock.c covers stateblock creation/record/apply paths. Wine
 * device.c adds the invalid-call recording transitions kept in this slice.
 */
void test_stateblock_invalid_type_recording_invalid_calls(
        const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    IDirect3DDevice9 *device = NULL;
    IDirect3DStateBlock9 *stateblock = NULL;
    IDirect3DStateBlock9 *stateblock2 = NULL;
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

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    stateblock = (IDirect3DStateBlock9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateStateBlock(device, (D3DSTATEBLOCKTYPE)0,
            &stateblock);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(stateblock == (IDirect3DStateBlock9 *)0xdeadbeef
            || stateblock == NULL);
    if (SUCCEEDED(hr) && stateblock
            && stateblock != (IDirect3DStateBlock9 *)0xdeadbeef)
        IDirect3DStateBlock9_Release(stateblock);

    stateblock = (IDirect3DStateBlock9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateStateBlock(device, D3DSBT_FORCE_DWORD,
            &stateblock);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(stateblock == (IDirect3DStateBlock9 *)0xdeadbeef
            || stateblock == NULL);
    if (SUCCEEDED(hr) && stateblock
            && stateblock != (IDirect3DStateBlock9 *)0xdeadbeef)
        IDirect3DStateBlock9_Release(stateblock);

    stateblock = NULL;
    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), S_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            S_OK);
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(stateblock != NULL);

    stateblock2 = (IDirect3DStateBlock9 *)0xdeadbeef;
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock2);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(stateblock2 == (IDirect3DStateBlock9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && stateblock2
            && stateblock2 != (IDirect3DStateBlock9 *)0xdeadbeef)
        IDirect3DStateBlock9_Release(stateblock2);

    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_LIGHTING, &value),
            S_OK);
    CHECK_TRUE(value == TRUE);

    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), S_OK);
    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3DERR_INVALIDCALL);
    if (stateblock)
    {
        CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3DERR_INVALIDCALL);
        CHECK_HR(IDirect3DStateBlock9_Capture(stateblock), D3DERR_INVALIDCALL);
    }

    stateblock2 = (IDirect3DStateBlock9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &stateblock2);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(stateblock2 == (IDirect3DStateBlock9 *)0xdeadbeef
            || stateblock2 == NULL);
    if (SUCCEEDED(hr) && stateblock2
            && stateblock2 != (IDirect3DStateBlock9 *)0xdeadbeef)
        IDirect3DStateBlock9_Release(stateblock2);

    stateblock2 = NULL;
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock2);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(stateblock2 != NULL);
    if (stateblock2)
        CHECK_HR(IDirect3DStateBlock9_Apply(stateblock2), S_OK);

    if (stateblock)
        IDirect3DStateBlock9_Release(stateblock);
    if (stateblock2)
        IDirect3DStateBlock9_Release(stateblock2);
    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/stateblock.c
 * function: test_shader_constant_apply()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_shader_constant_apply(const struct d3d9_api *api)
{
    static const float vs_a[4] = {1.0f, 2.0f, 3.0f, 4.0f};
    static const float vs_b[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
    static const int ps_i_a[4] = {10, 20, 30, 40};
    static const int ps_i_b[4] = {-10, -20, -30, -40};
    static const BOOL ps_b_a[2] = {TRUE, FALSE};
    static const BOOL ps_b_b[2] = {FALSE, TRUE};
    IDirect3DStateBlock9 *stateblock = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    float out_f[4];
    int out_i[4];
    BOOL out_b[2];
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

    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, vs_a, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, ps_i_a, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, ps_b_a, 2),
            D3D_OK);
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(stateblock != NULL);
    if (FAILED(hr) || !stateblock)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, vs_b, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, ps_i_b, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, ps_b_b, 2),
            D3D_OK);

    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);

    memset(out_f, 0xcc, sizeof(out_f));
    hr = IDirect3DDevice9_GetVertexShaderConstantF(device, 0, out_f, 1);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_f, vs_a, sizeof(vs_a)) == 0);

    memset(out_i, 0xcc, sizeof(out_i));
    hr = IDirect3DDevice9_GetPixelShaderConstantI(device, 0, out_i, 1);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_i, ps_i_a, sizeof(ps_i_a)) == 0);

    memset(out_b, 0xcc, sizeof(out_b));
    hr = IDirect3DDevice9_GetPixelShaderConstantB(device, 0, out_b, 2);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_b, ps_b_a, sizeof(ps_b_a)) == 0);

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, vs_b, 1),
            D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Capture(stateblock), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, vs_a, 1),
            D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);
    memset(out_f, 0xcc, sizeof(out_f));
    hr = IDirect3DDevice9_GetVertexShaderConstantF(device, 0, out_f, 1);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_f, vs_b, sizeof(vs_b)) == 0);

    IDirect3DStateBlock9_Release(stateblock);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_shader_constant_apply()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_shader_constant_stateblock_cross_stage(const struct d3d9_api *api)
{
    static const float vs_f_a[8] =
    {
        1.0f, 2.0f, 3.0f, 4.0f,
        5.0f, 6.0f, 7.0f, 8.0f
    };
    static const float ps_f_a[4] = {-1.0f, -2.0f, -3.0f, -4.0f};
    static const int vs_i_a[4] = {1, 2, 3, 4};
    static const int ps_i_a[4] = {-1, -2, -3, -4};
    static const BOOL vs_b_a[3] = {TRUE, FALSE, TRUE};
    static const BOOL ps_b_a[3] = {FALSE, TRUE, FALSE};
    static const float vs_f_b[8] =
    {
        8.0f, 7.0f, 6.0f, 5.0f,
        4.0f, 3.0f, 2.0f, 1.0f
    };
    static const float ps_f_b[4] = {4.0f, 3.0f, 2.0f, 1.0f};
    static const int vs_i_b[4] = {10, 20, 30, 40};
    static const int ps_i_b[4] = {-10, -20, -30, -40};
    static const BOOL vs_b_b[3] = {FALSE, FALSE, TRUE};
    static const BOOL ps_b_b[3] = {TRUE, TRUE, FALSE};
    IDirect3DStateBlock9 *stateblock = NULL;
    IDirect3DDevice9 *device = NULL;
    float out_f[8];
    int out_i[4];
    BOOL out_b[3];
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

    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 2, vs_f_a, 2),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantF(device, 1, ps_f_a, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantI(device, 1, vs_i_a, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantI(device, 1, ps_i_a, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantB(device, 1, vs_b_a, 3),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantB(device, 1, ps_b_a, 3),
            D3D_OK);
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(stateblock != NULL);
    if (FAILED(hr) || !stateblock)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 2, vs_f_b, 2),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantF(device, 1, ps_f_b, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantI(device, 1, vs_i_b, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantI(device, 1, ps_i_b, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantB(device, 1, vs_b_b, 3),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantB(device, 1, ps_b_b, 3),
            D3D_OK);

    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);

    memset(out_f, 0xcc, sizeof(out_f));
    hr = IDirect3DDevice9_GetVertexShaderConstantF(device, 2, out_f, 2);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_f, vs_f_a, sizeof(vs_f_a)) == 0);

    memset(out_f, 0xcc, sizeof(out_f));
    hr = IDirect3DDevice9_GetPixelShaderConstantF(device, 1, out_f, 1);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_f, ps_f_a, sizeof(ps_f_a)) == 0);

    memset(out_i, 0xcc, sizeof(out_i));
    hr = IDirect3DDevice9_GetVertexShaderConstantI(device, 1, out_i, 1);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_i, vs_i_a, sizeof(vs_i_a)) == 0);

    memset(out_i, 0xcc, sizeof(out_i));
    hr = IDirect3DDevice9_GetPixelShaderConstantI(device, 1, out_i, 1);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_i, ps_i_a, sizeof(ps_i_a)) == 0);

    memset(out_b, 0xcc, sizeof(out_b));
    hr = IDirect3DDevice9_GetVertexShaderConstantB(device, 1, out_b, 3);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_b, vs_b_a, sizeof(vs_b_a)) == 0);

    memset(out_b, 0xcc, sizeof(out_b));
    hr = IDirect3DDevice9_GetPixelShaderConstantB(device, 1, out_b, 3);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_b, ps_b_a, sizeof(ps_b_a)) == 0);

done_device:
    if (stateblock)
        IDirect3DStateBlock9_Release(stateblock);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/stateblock.c
 * function: test_vdecl_apply()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vdecl_apply(const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 position_color[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 position_texcoord[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *decl_a = NULL;
    IDirect3DVertexDeclaration9 *decl_b = NULL;
    IDirect3DVertexDeclaration9 *current = NULL;
    IDirect3DStateBlock9 *stateblock = NULL;
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

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, position_color,
            &decl_a);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device, position_texcoord,
            &decl_b);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, decl_a), D3D_OK);
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(stateblock != NULL);
    if (FAILED(hr) || !stateblock)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, decl_b), D3D_OK);
    current = NULL;
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == decl_b);
    if (current)
        IDirect3DVertexDeclaration9_Release(current);

    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);
    current = NULL;
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == decl_a);
    if (current)
        IDirect3DVertexDeclaration9_Release(current);

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, decl_b), D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Capture(stateblock), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, decl_a), D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);
    current = NULL;
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == decl_b);
    if (current)
        IDirect3DVertexDeclaration9_Release(current);

done_device:
    if (stateblock)
        IDirect3DStateBlock9_Release(stateblock);
    if (decl_b)
        IDirect3DVertexDeclaration9_Release(decl_b);
    if (decl_a)
        IDirect3DVertexDeclaration9_Release(decl_a);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/stateblock.c
 * function: transform state in test_state_management()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_stateblock_transform_capture_apply(const struct d3d9_api *api)
{
    IDirect3DStateBlock9 *stateblock = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DMATRIX view_a, view_b, out;
    D3DMATRIX world_a, world_b;
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

    set_scale_matrix(&view_a, 2.0f);
    set_scale_matrix(&view_b, 3.0f);
    set_scale_matrix(&world_a, 4.0f);
    set_scale_matrix(&world_b, 5.0f);

    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &view_a),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_WORLDMATRIX(255),
            &world_a), D3D_OK);
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(stateblock != NULL);
    if (FAILED(hr) || !stateblock)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &view_b),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_WORLDMATRIX(255),
            &world_b), D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);

    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetTransform(device, D3DTS_VIEW, &out),
            D3D_OK);
    check_matrix_equals(&out, &view_a);
    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetTransform(device, D3DTS_WORLDMATRIX(255),
            &out), D3D_OK);
    check_matrix_equals(&out, &world_a);

    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &view_b),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_WORLDMATRIX(255),
            &world_b), D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Capture(stateblock), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_VIEW, &view_a),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTransform(device, D3DTS_WORLDMATRIX(255),
            &world_a), D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);

    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetTransform(device, D3DTS_VIEW, &out),
            D3D_OK);
    check_matrix_equals(&out, &view_b);
    memset(&out, 0xcc, sizeof(out));
    CHECK_HR(IDirect3DDevice9_GetTransform(device, D3DTS_WORLDMATRIX(255),
            &out), D3D_OK);
    check_matrix_equals(&out, &world_b);

done_device:
    if (stateblock)
        IDirect3DStateBlock9_Release(stateblock);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_multiply_transform()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_stateblock_multiply_transform_capture(const struct d3d9_api *api)
{
    static const D3DTRANSFORMSTATETYPE tests[] =
    {
        D3DTS_VIEW,
        D3DTS_PROJECTION,
        D3DTS_TEXTURE0,
        D3DTS_TEXTURE7,
        D3DTS_WORLD,
        D3DTS_WORLDMATRIX(255),
    };
    IDirect3DStateBlock9 *stateblock = NULL;
    D3DMATRIX identity, scale, out;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    unsigned int i;
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

    set_scale_matrix(&identity, 1.0f);
    set_scale_matrix(&scale, 2.0f);

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
    {
        if (stateblock)
        {
            IDirect3DStateBlock9_Release(stateblock);
            stateblock = NULL;
        }

        CHECK_HR(IDirect3DDevice9_SetTransform(device, tests[i], &identity),
                D3D_OK);
        CHECK_HR(IDirect3DDevice9_MultiplyTransform(device, tests[i], &scale),
                D3D_OK);
        memset(&out, 0xcc, sizeof(out));
        CHECK_HR(IDirect3DDevice9_GetTransform(device, tests[i], &out),
                D3D_OK);
        check_matrix_equals(&out, &scale);

        CHECK_HR(IDirect3DDevice9_SetTransform(device, tests[i], &identity),
                D3D_OK);
        CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3D_OK);
        CHECK_HR(IDirect3DDevice9_MultiplyTransform(device, tests[i], &scale),
                D3D_OK);
        hr = IDirect3DDevice9_EndStateBlock(device, &stateblock);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(stateblock != NULL);
        if (FAILED(hr) || !stateblock)
            break;

        memset(&out, 0xcc, sizeof(out));
        CHECK_HR(IDirect3DDevice9_GetTransform(device, tests[i], &out),
                D3D_OK);
        check_matrix_equals(&out, &scale);

        CHECK_HR(IDirect3DStateBlock9_Capture(stateblock), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetTransform(device, tests[i], &identity),
                D3D_OK);
        CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);
        memset(&out, 0xcc, sizeof(out));
        CHECK_HR(IDirect3DDevice9_GetTransform(device, tests[i], &out),
                D3D_OK);
        check_matrix_equals(&out, &identity);
    }

    if (stateblock)
        IDirect3DStateBlock9_Release(stateblock);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/stateblock.c
 * function: test_state_management (D3DSBT_ALL capture/apply matrix)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_state_management_all_capture_apply_matrix(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    IDirect3DDevice9 *device = NULL;
    IDirect3DStateBlock9 *stateblock = NULL;
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

    /* Snapshot known-default state, mutate a few entries, verify
     * D3DSBT_ALL apply restores them. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE,
            D3DFILL_SOLID), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE,
            D3DCULL_CCW), D3D_OK);

    CHECK_HR(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL,
            &stateblock), D3D_OK);
    if (!stateblock)
        goto done_device;

    /* Mutate and apply to restore. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_FILLMODE,
            D3DFILL_WIREFRAME), D3D_OK);
    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_FILLMODE, &value),
            D3D_OK);
    CHECK_TRUE(value == D3DFILL_SOLID);

    IDirect3DStateBlock9_Release(stateblock);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/stateblock.c
 * function: test_state_management (D3DSBT_PIXELSTATE capture/apply slice)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_state_management_pixel_capture_apply_slice(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    IDirect3DDevice9 *device = NULL;
    IDirect3DStateBlock9 *stateblock = NULL;
    HWND window;
    DWORD blend_value;
    DWORD cull_value;

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

    /* Set pixel-state and a vertex-state value; pixel stateblock must
     * restore the first but not the second. */
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND,
            D3DBLEND_ONE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE,
            D3DCULL_CCW), D3D_OK);

    CHECK_HR(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_PIXELSTATE,
            &stateblock), D3D_OK);
    if (!stateblock)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND,
            D3DBLEND_ZERO), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE,
            D3DCULL_NONE), D3D_OK);

    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);

    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SRCBLEND,
            &blend_value), D3D_OK);
    CHECK_TRUE(blend_value == D3DBLEND_ONE);

    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_CULLMODE,
            &cull_value), D3D_OK);
    /* D3DRS_CULLMODE is a vertex-state render-state — must remain mutated. */
    CHECK_TRUE(cull_value == D3DCULL_NONE);

    IDirect3DStateBlock9_Release(stateblock);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/stateblock.c
 * function: test_state_management (D3DSBT_VERTEXSTATE capture/apply slice)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_state_management_vertex_capture_apply_slice(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    IDirect3DDevice9 *device = NULL;
    IDirect3DStateBlock9 *stateblock = NULL;
    HWND window;
    DWORD blend_value;
    DWORD cull_value;

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

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND,
            D3DBLEND_ONE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE,
            D3DCULL_CCW), D3D_OK);

    CHECK_HR(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_VERTEXSTATE,
            &stateblock), D3D_OK);
    if (!stateblock)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_SRCBLEND,
            D3DBLEND_ZERO), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_CULLMODE,
            D3DCULL_NONE), D3D_OK);

    CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3D_OK);

    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_SRCBLEND,
            &blend_value), D3D_OK);
    /* D3DRS_SRCBLEND is a pixel-state render-state — must remain mutated. */
    CHECK_TRUE(blend_value == D3DBLEND_ZERO);

    CHECK_HR(IDirect3DDevice9_GetRenderState(device, D3DRS_CULLMODE,
            &cull_value), D3D_OK);
    CHECK_TRUE(cull_value == D3DCULL_CCW);

    IDirect3DStateBlock9_Release(stateblock);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
