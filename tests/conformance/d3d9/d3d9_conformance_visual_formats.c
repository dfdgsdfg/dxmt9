/*
 * Format-cap and texture-creation conformance scaffolds split from
 * d3d9_conformance.c. Each scaffold validates the observable D3D9
 * surface for the indicated Wine visual.c oracle without copying
 * the Wine implementation.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/visual.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Covers float / G16R16 / V16U16 texture pitch policy, sRGB read/write
 * format caps, volume sRGB caps, DXT1 volume pitch policy, and the
 * V8U8/V16U16/Q8W8V8U8 signed-format cap matrix.
 */

#include "d3d9_conformance_fixtures.h"

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: float_texture_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_float_texture_format_policy(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_RECT locked;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
            D3DFMT_R32F);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_R32F textures are not supported");
        goto done_d3d9;
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 1, 0, D3DFMT_R32F,
            D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.Pitch >= 16);
        CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 0), D3D_OK);
    }

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
 * function: g16r16_texture_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_g16r16_texture_format_policy(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_RECT locked;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
            D3DFMT_G16R16);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_G16R16 textures are not supported");
        goto done_d3d9;
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 1, 0, D3DFMT_G16R16,
            D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.Pitch >= 16);
        CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 0), D3D_OK);
    }

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
 * function: volume_v16u16_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_volume_v16u16_format_policy(const struct d3d9_api *api)
{
    IDirect3DVolumeTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_BOX locked;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_VOLUMETEXTURE,
            D3DFMT_V16U16);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_V16U16 volume textures are not supported");
        goto done_d3d9;
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 2, 1, 0,
            D3DFMT_V16U16, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.RowPitch > 0);
        CHECK_TRUE(locked.SlicePitch > 0);
        CHECK_TRUE((locked.RowPitch % 4) == 0);
        CHECK_TRUE((locked.SlicePitch % 4) == 0);
        CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0), D3D_OK);
    }

    IDirect3DVolumeTexture9_Release(texture);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: srgbtexture_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_srgb_texture_caps_policy(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_SRGBREAD,
            D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: srgbwrite_format_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_srgb_write_caps_policy(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_SRGBWRITE,
            D3DRTYPE_TEXTURE, D3DFMT_A8R8G8B8);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: volume_srgb_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_volume_srgb_caps_policy(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_SRGBREAD,
            D3DRTYPE_VOLUMETEXTURE, D3DFMT_A8R8G8B8);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: volume_dxtn_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_volume_dxtn_format_policy(const struct d3d9_api *api)
{
    IDirect3DVolumeTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_BOX locked;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_VOLUMETEXTURE,
            D3DFMT_DXT1);
    if (hr != D3D_OK)
    {
        skip_current_test("DXT1 volume textures are not supported");
        goto done_d3d9;
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0,
            D3DFMT_DXT1, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.RowPitch >= 8);
        CHECK_TRUE(locked.SlicePitch >= locked.RowPitch);
        CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0), D3D_OK);
    }

    IDirect3DVolumeTexture9_Release(texture);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_signed_formats
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_signed_formats_caps_policy(const struct d3d9_api *api)
{
    static const D3DFORMAT formats[] =
    {
        D3DFMT_V8U8,
        D3DFMT_V16U16,
        D3DFMT_Q8W8V8U8,
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

    for (i = 0; i < ARRAY_SIZE(formats); ++i)
    {
        hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
                formats[i]);
        CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);
    }

    IDirect3D9_Release(d3d9);
}

#define P8_PS_REGTYPE(type) \
    ((((DWORD)(type) & 0x7u) << D3DSP_REGTYPE_SHIFT) \
            | (((((DWORD)(type) >> 3u) & 0x3u) << D3DSP_REGTYPE_SHIFT2)))
#define P8_PS_DST(type, index, mask) \
    (0x80000000u | P8_PS_REGTYPE(type) \
            | (((DWORD)(mask) & 0xfu) << 16) | ((DWORD)(index) & 0x7ffu))
#define P8_PS_SRC(type, index) \
    (0x80000000u | P8_PS_REGTYPE(type) | D3DSP_NOSWIZZLE \
            | ((DWORD)(index) & 0x7ffu))
#define P8_PS_INST(opcode, operands) \
    (((DWORD)(opcode) & D3DSI_OPCODE_MASK) \
            | (((DWORD)(operands) & 0xfu) << D3DSI_INSTLENGTH_SHIFT))

static const DWORD p8_sample_ps_2_0[] =
{
    D3DPS_VERSION(2, 0),
    P8_PS_INST(D3DSIO_DCL, 2),
    D3DDECLUSAGE_TEXCOORD,
    P8_PS_DST(D3DSPR_TEXTURE, 0, 0x3),
    P8_PS_INST(D3DSIO_DCL, 2),
    D3DSTT_2D,
    P8_PS_DST(D3DSPR_SAMPLER, 0, 0xf),
    P8_PS_INST(D3DSIO_TEX, 3),
    P8_PS_DST(D3DSPR_TEMP, 0, 0xf),
    P8_PS_SRC(D3DSPR_TEXTURE, 0),
    P8_PS_SRC(D3DSPR_SAMPLER, 0),
    P8_PS_INST(D3DSIO_MOV, 2),
    P8_PS_DST(D3DSPR_COLOROUT, 0, 0xf),
    P8_PS_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

static const DWORD p8_cube_sample_ps_2_0[] =
{
    D3DPS_VERSION(2, 0),
    P8_PS_INST(D3DSIO_DCL, 2),
    D3DDECLUSAGE_TEXCOORD,
    P8_PS_DST(D3DSPR_TEXTURE, 0, 0x7),
    P8_PS_INST(D3DSIO_DCL, 2),
    D3DSTT_CUBE,
    P8_PS_DST(D3DSPR_SAMPLER, 0, 0xf),
    P8_PS_INST(D3DSIO_TEX, 3),
    P8_PS_DST(D3DSPR_TEMP, 0, 0xf),
    P8_PS_SRC(D3DSPR_TEXTURE, 0),
    P8_PS_SRC(D3DSPR_SAMPLER, 0),
    P8_PS_INST(D3DSIO_MOV, 2),
    P8_PS_DST(D3DSPR_COLOROUT, 0, 0xf),
    P8_PS_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

static const DWORD p8_volume_sample_ps_2_0[] =
{
    D3DPS_VERSION(2, 0),
    P8_PS_INST(D3DSIO_DCL, 2),
    D3DDECLUSAGE_TEXCOORD,
    P8_PS_DST(D3DSPR_TEXTURE, 0, 0x7),
    P8_PS_INST(D3DSIO_DCL, 2),
    D3DSTT_VOLUME,
    P8_PS_DST(D3DSPR_SAMPLER, 0, 0xf),
    P8_PS_INST(D3DSIO_TEX, 3),
    P8_PS_DST(D3DSPR_TEMP, 0, 0xf),
    P8_PS_SRC(D3DSPR_TEXTURE, 0),
    P8_PS_SRC(D3DSPR_SAMPLER, 0),
    P8_PS_INST(D3DSIO_MOV, 2),
    P8_PS_DST(D3DSPR_COLOROUT, 0, 0xf),
    P8_PS_SRC(D3DSPR_TEMP, 0),
    D3DSIO_END
};

#undef P8_PS_INST
#undef P8_PS_SRC
#undef P8_PS_DST
#undef P8_PS_REGTYPE

static void check_visual_palettized_texture_sampler(IDirect3DDevice9 *device,
        D3DFORMAT format, const BYTE *texels, UINT texel_bytes,
        const DWORD *expected, BOOL programmable_ps, const BYTE *lod_texels,
        const DWORD *lod_expected, const PALETTEENTRY *updated_palette,
        const DWORD *updated_expected, UINT updated_palette_index,
        BOOL update_palette_before_bind)
{
    struct textured_point
    {
        float x, y, z, rhw;
        float u, v;
    };
    static const struct textured_point points[] =
    {
        {0.5f, 0.5f, 0.0f, 1.0f, 0.25f, 0.25f},
        {1.5f, 0.5f, 0.0f, 1.0f, 0.75f, 0.25f},
        {0.5f, 1.5f, 0.0f, 1.0f, 0.25f, 0.75f},
        {1.5f, 1.5f, 0.0f, 1.0f, 0.75f, 0.75f},
    };
    IDirect3DSurface9 *readback = NULL;
    IDirect3DSurface9 *rt = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    D3DLOCKED_RECT locked;
    DWORD point_size;
    D3DVIEWPORT9 vp;
    HRESULT hr;

    hr = IDirect3DDevice9_CreateTexture(device, 2, 2, lod_texels ? 2 : 1,
            0, format,
            D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_texture;
    if (SUCCEEDED(hr))
    {
        INT row_bytes = (INT)(2 * texel_bytes);
        BYTE *row0 = locked.pBits;
        BYTE *row1 = row0 + locked.Pitch;
        CHECK_TRUE(locked.Pitch >= row_bytes);
        if (locked.Pitch >= row_bytes)
        {
            memcpy(row0, texels, row_bytes);
            memcpy(row1, texels + row_bytes, row_bytes);
        }
        CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 0), D3D_OK);
    }
    if (lod_texels)
    {
        memset(&locked, 0xcc, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(texture, 1, &locked, NULL, 0);
        CHECK_HR(hr, D3D_OK);
        if (FAILED(hr))
            goto done_texture;
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(locked.Pitch >= (INT)texel_bytes);
            if (locked.Pitch >= (INT)texel_bytes)
                memcpy(locked.pBits, lod_texels, texel_bytes);
            CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 1), D3D_OK);
        }
        CHECK_TRUE(IDirect3DTexture9_SetLOD(texture, 1) == 0);
        expected = lod_expected;
    }

    hr = IDirect3DDevice9_CreateRenderTarget(device, 2, 2,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_texture;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 2, 2,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_rt;

    CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, rt), D3D_OK);
    vp.X = 0;
    vp.Y = 0;
    vp.Width = 2;
    vp.Height = 2;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetViewport(device, &vp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE),
            D3D_OK);
    point_size = 0x3f800000u;
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE,
            point_size), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER,
            D3DTEXF_NONE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
    if (updated_palette && update_palette_before_bind)
    {
        CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device,
                updated_palette_index,
                updated_palette), D3D_OK);
        if (updated_palette_index)
            CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device,
                    updated_palette_index), D3D_OK);
        expected = updated_expected;
    }
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0,
            (IDirect3DBaseTexture9 *)texture), D3D_OK);
    if (updated_palette && !update_palette_before_bind)
    {
        CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device,
                updated_palette_index,
                updated_palette), D3D_OK);
        if (updated_palette_index)
            CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device,
                    updated_palette_index), D3D_OK);
        expected = updated_expected;
    }
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1),
            D3D_OK);
    if (programmable_ps)
    {
        hr = IDirect3DDevice9_CreatePixelShader(device, p8_sample_ps_2_0,
                &ps);
        CHECK_HR(hr, D3D_OK);
        if (FAILED(hr))
            goto done_texture_bind;
        CHECK_HR(IDirect3DDevice9_SetPixelShader(device, ps), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff000000u, 0.0f, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST,
            ARRAY_SIZE(points), points, sizeof(points[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device, rt, readback),
            D3D_OK);

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(readback, &locked, NULL,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        const DWORD *row0 = locked.pBits;
        const DWORD *row1 = (const DWORD *)((const BYTE *)locked.pBits
                + locked.Pitch);
        CHECK_TRUE(row0[0] == expected[0]);
        CHECK_TRUE(row0[1] == expected[1]);
        CHECK_TRUE(row1[0] == expected[2]);
        CHECK_TRUE(row1[1] == expected[3]);
        CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
    }

    if (programmable_ps)
        CHECK_HR(IDirect3DDevice9_SetPixelShader(device, NULL), D3D_OK);
    if (ps) IDirect3DPixelShader9_Release(ps);
done_texture_bind:
    if (updated_palette && updated_palette_index)
        CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0),
                D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0, NULL), D3D_OK);
    IDirect3DSurface9_Release(readback);
done_rt:
    IDirect3DSurface9_Release(rt);
done_texture:
    IDirect3DTexture9_Release(texture);
done:
    return;
}

static void check_visual_palettized_update_texture_sampler(
        IDirect3DDevice9 *device, D3DFORMAT format, const BYTE *texels,
        UINT texel_bytes, const DWORD *expected,
        const PALETTEENTRY *updated_palette, const DWORD *updated_expected,
        BOOL programmable_ps)
{
    struct textured_point
    {
        float x, y, z, rhw;
        float u, v;
    };
    static const struct textured_point points[] =
    {
        {0.5f, 0.5f, 0.0f, 1.0f, 0.25f, 0.25f},
        {1.5f, 0.5f, 0.0f, 1.0f, 0.75f, 0.25f},
        {0.5f, 1.5f, 0.0f, 1.0f, 0.25f, 0.75f},
        {1.5f, 1.5f, 0.0f, 1.0f, 0.75f, 0.75f},
    };
    IDirect3DSurface9 *readback = NULL;
    IDirect3DTexture9 *dst_texture = NULL;
    IDirect3DTexture9 *src_texture = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    IDirect3DSurface9 *rt = NULL;
    D3DLOCKED_RECT locked;
    DWORD point_size;
    D3DVIEWPORT9 vp;
    HRESULT hr;

    hr = IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, format,
            D3DPOOL_SYSTEMMEM, &src_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done;
    hr = IDirect3DDevice9_CreateTexture(device, 2, 2, 1, 0, format,
            D3DPOOL_DEFAULT, &dst_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_src_texture;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(src_texture, 0, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_dst_texture;
    if (SUCCEEDED(hr))
    {
        INT row_bytes = (INT)(2 * texel_bytes);
        BYTE *row0 = locked.pBits;
        BYTE *row1 = row0 + locked.Pitch;
        CHECK_TRUE(locked.Pitch >= row_bytes);
        if (locked.Pitch >= row_bytes)
        {
            memcpy(row0, texels, row_bytes);
            memcpy(row1, texels + row_bytes, row_bytes);
        }
        CHECK_HR(IDirect3DTexture9_UnlockRect(src_texture, 0), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_UpdateTexture(device,
            (IDirect3DBaseTexture9 *)src_texture,
            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);

    hr = IDirect3DDevice9_CreateRenderTarget(device, 2, 2,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_dst_texture;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 2, 2,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_rt;

    CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, rt), D3D_OK);
    vp.X = 0;
    vp.Y = 0;
    vp.Width = 2;
    vp.Height = 2;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetViewport(device, &vp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE),
            D3D_OK);
    point_size = 0x3f800000u;
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE,
            point_size), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER,
            D3DTEXF_NONE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_COLOROP, D3DTOP_SELECTARG1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_COLORARG1, D3DTA_TEXTURE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_ALPHAOP, D3DTOP_SELECTARG1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            D3DTSS_ALPHAARG1, D3DTA_TEXTURE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0,
            (IDirect3DBaseTexture9 *)dst_texture), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1),
            D3D_OK);
    if (programmable_ps)
    {
        hr = IDirect3DDevice9_CreatePixelShader(device, p8_sample_ps_2_0,
                &ps);
        CHECK_HR(hr, D3D_OK);
        if (FAILED(hr))
            goto done_texture_bind;
        CHECK_HR(IDirect3DDevice9_SetPixelShader(device, ps), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff000000u, 0.0f, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST,
            ARRAY_SIZE(points), points, sizeof(points[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device, rt, readback),
            D3D_OK);

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(readback, &locked, NULL,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        const DWORD *row0 = locked.pBits;
        const DWORD *row1 = (const DWORD *)((const BYTE *)locked.pBits
                + locked.Pitch);
        CHECK_TRUE(row0[0] == expected[0]);
        CHECK_TRUE(row0[1] == expected[1]);
        CHECK_TRUE(row1[0] == expected[2]);
        CHECK_TRUE(row1[1] == expected[3]);
        CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 1, updated_palette),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff000000u, 0.0f, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST,
            ARRAY_SIZE(points), points, sizeof(points[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device, rt, readback),
            D3D_OK);

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(readback, &locked, NULL,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        const DWORD *row0 = locked.pBits;
        const DWORD *row1 = (const DWORD *)((const BYTE *)locked.pBits
                + locked.Pitch);
        CHECK_TRUE(row0[0] == updated_expected[0]);
        CHECK_TRUE(row0[1] == updated_expected[1]);
        CHECK_TRUE(row1[0] == updated_expected[2]);
        CHECK_TRUE(row1[1] == updated_expected[3]);
        CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
    }

    if (programmable_ps)
        CHECK_HR(IDirect3DDevice9_SetPixelShader(device, NULL), D3D_OK);
    if (ps) IDirect3DPixelShader9_Release(ps);
done_texture_bind:
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0, NULL), D3D_OK);
    IDirect3DSurface9_Release(readback);
done_rt:
    IDirect3DSurface9_Release(rt);
done_dst_texture:
    IDirect3DTexture9_Release(dst_texture);
done_src_texture:
    IDirect3DTexture9_Release(src_texture);
done:
    return;
}

static void check_visual_palettized_volume_texture_sampler(
        IDirect3DDevice9 *device, D3DFORMAT format, const BYTE *texels,
        UINT texel_bytes, const DWORD *expected)
{
    struct volume_point
    {
        float x, y, z, rhw;
        float u, v, w;
    };
    static const struct volume_point points[] =
    {
        {0.5f, 0.5f, 0.0f, 1.0f, 0.5f, 0.5f, 0.125f},
        {1.5f, 0.5f, 0.0f, 1.0f, 0.5f, 0.5f, 0.375f},
        {2.5f, 0.5f, 0.0f, 1.0f, 0.5f, 0.5f, 0.625f},
        {3.5f, 0.5f, 0.0f, 1.0f, 0.5f, 0.5f, 0.875f},
    };
    IDirect3DVolumeTexture9 *texture = NULL;
    IDirect3DSurface9 *readback = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    IDirect3DSurface9 *rt = NULL;
    D3DLOCKED_BOX locked;
    D3DLOCKED_RECT rb;
    D3DVIEWPORT9 vp;
    DWORD point_size;
    HRESULT hr;
    UINT z;

    hr = IDirect3DDevice9_CreateVolumeTexture(device, 1, 1, 4, 1, 0,
            format, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_texture;
    CHECK_TRUE(locked.RowPitch >= (INT)texel_bytes);
    CHECK_TRUE(locked.SlicePitch >= locked.RowPitch);
    if (locked.RowPitch >= (INT)texel_bytes
            && locked.SlicePitch >= locked.RowPitch)
    {
        BYTE *base = locked.pBits;
        for (z = 0; z < 4; ++z)
            memcpy(base + z * locked.SlicePitch, texels + z * texel_bytes,
                    texel_bytes);
    }
    CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0), D3D_OK);

    hr = IDirect3DDevice9_CreateRenderTarget(device, 4, 1,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_texture;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 4, 1,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_rt;

    CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, rt), D3D_OK);
    vp.X = 0;
    vp.Y = 0;
    vp.Width = 4;
    vp.Height = 1;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetViewport(device, &vp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE),
            D3D_OK);
    point_size = 0x3f800000u;
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE,
            point_size), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER,
            D3DTEXF_NONE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0,
            (IDirect3DBaseTexture9 *)texture), D3D_OK);
    hr = IDirect3DDevice9_CreatePixelShader(device, p8_volume_sample_ps_2_0,
            &ps);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_texture_bind;
    CHECK_HR(IDirect3DDevice9_SetPixelShader(device, ps), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1
            | D3DFVF_TEXCOORDSIZE3(0)), D3D_OK);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff000000u, 0.0f, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST,
            ARRAY_SIZE(points), points, sizeof(points[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device, rt, readback),
            D3D_OK);

    memset(&rb, 0xcc, sizeof(rb));
    hr = IDirect3DSurface9_LockRect(readback, &rb, NULL, D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        const DWORD *row = rb.pBits;
        CHECK_TRUE(row[0] == expected[0]);
        CHECK_TRUE(row[1] == expected[1]);
        CHECK_TRUE(row[2] == expected[2]);
        CHECK_TRUE(row[3] == expected[3]);
        CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetPixelShader(device, NULL), D3D_OK);
    if (ps) IDirect3DPixelShader9_Release(ps);
done_texture_bind:
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0, NULL), D3D_OK);
    IDirect3DSurface9_Release(readback);
done_rt:
    IDirect3DSurface9_Release(rt);
done_texture:
    IDirect3DVolumeTexture9_Release(texture);
done:
    return;
}

static void check_visual_palettized_cube_texture_sampler(
        IDirect3DDevice9 *device, D3DFORMAT format, const BYTE *texels,
        UINT texel_bytes, const DWORD *expected)
{
    static const D3DCUBEMAP_FACES faces[] =
    {
        D3DCUBEMAP_FACE_POSITIVE_X,
        D3DCUBEMAP_FACE_NEGATIVE_X,
        D3DCUBEMAP_FACE_POSITIVE_Y,
        D3DCUBEMAP_FACE_NEGATIVE_Y,
        D3DCUBEMAP_FACE_POSITIVE_Z,
        D3DCUBEMAP_FACE_NEGATIVE_Z,
    };
    struct cube_point
    {
        float x, y, z, rhw;
        float u, v, w;
    };
    static const struct cube_point points[] =
    {
        {0.5f, 0.5f, 0.0f, 1.0f,  1.0f,  0.0f,  0.0f},
        {1.5f, 0.5f, 0.0f, 1.0f, -1.0f,  0.0f,  0.0f},
        {2.5f, 0.5f, 0.0f, 1.0f,  0.0f,  1.0f,  0.0f},
        {0.5f, 1.5f, 0.0f, 1.0f,  0.0f, -1.0f,  0.0f},
        {1.5f, 1.5f, 0.0f, 1.0f,  0.0f,  0.0f,  1.0f},
        {2.5f, 1.5f, 0.0f, 1.0f,  0.0f,  0.0f, -1.0f},
    };
    IDirect3DSurface9 *readback = NULL;
    IDirect3DSurface9 *rt = NULL;
    IDirect3DCubeTexture9 *texture = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    D3DLOCKED_RECT locked;
    DWORD point_size;
    D3DVIEWPORT9 vp;
    HRESULT hr;
    UINT i;

    hr = IDirect3DDevice9_CreateCubeTexture(device, 1, 1, 0, format,
            D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done;

    for (i = 0; i < ARRAY_SIZE(faces); ++i)
    {
        memset(&locked, 0xcc, sizeof(locked));
        hr = IDirect3DCubeTexture9_LockRect(texture, faces[i], 0,
                &locked, NULL, 0);
        CHECK_HR(hr, D3D_OK);
        if (FAILED(hr))
            goto done_texture;
        CHECK_TRUE(locked.Pitch >= (INT)texel_bytes);
        if (locked.Pitch >= (INT)texel_bytes)
            memcpy(locked.pBits, texels + i * texel_bytes, texel_bytes);
        CHECK_HR(IDirect3DCubeTexture9_UnlockRect(texture, faces[i], 0),
                D3D_OK);
    }

    hr = IDirect3DDevice9_CreateRenderTarget(device, 3, 2,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_texture;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 3, 2,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_rt;

    CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, rt), D3D_OK);
    vp.X = 0;
    vp.Y = 0;
    vp.Width = 3;
    vp.Height = 2;
    vp.MinZ = 0.0f;
    vp.MaxZ = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetViewport(device, &vp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_ZENABLE, FALSE),
            D3D_OK);
    point_size = 0x3f800000u;
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_POINTSIZE,
            point_size), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MINFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MAGFILTER,
            D3DTEXF_POINT), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0, D3DSAMP_MIPFILTER,
            D3DTEXF_NONE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0,
            (IDirect3DBaseTexture9 *)texture), D3D_OK);
    hr = IDirect3DDevice9_CreatePixelShader(device, p8_cube_sample_ps_2_0,
            &ps);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_texture_bind;
    CHECK_HR(IDirect3DDevice9_SetPixelShader(device, ps), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW | D3DFVF_TEX1
            | D3DFVF_TEXCOORDSIZE3(0)), D3D_OK);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xff000000u, 0.0f, 0), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST,
            ARRAY_SIZE(points), points, sizeof(points[0])), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_GetRenderTargetData(device, rt, readback),
            D3D_OK);

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(readback, &locked, NULL,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        const DWORD *row0 = locked.pBits;
        const DWORD *row1 = (const DWORD *)((const BYTE *)locked.pBits
                + locked.Pitch);
        CHECK_TRUE(row0[0] == expected[0]);
        CHECK_TRUE(row0[1] == expected[1]);
        CHECK_TRUE(row0[2] == expected[2]);
        CHECK_TRUE(row1[0] == expected[3]);
        CHECK_TRUE(row1[1] == expected[4]);
        CHECK_TRUE(row1[2] == expected[5]);
        CHECK_HR(IDirect3DSurface9_UnlockRect(readback), D3D_OK);
    }

    CHECK_HR(IDirect3DDevice9_SetPixelShader(device, NULL), D3D_OK);
    if (ps) IDirect3DPixelShader9_Release(ps);
done_texture_bind:
    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0, NULL), D3D_OK);
    IDirect3DSurface9_Release(readback);
done_rt:
    IDirect3DSurface9_Release(rt);
done_texture:
    IDirect3DCubeTexture9_Release(texture);
done:
    return;
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: palette_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Pins dxmt9's palettized runtime path beyond API round-trip state:
 * D3DFMT_P8 and D3DFMT_A8P8 textures must lock as index texels, expand
 * through the current texture palette into an A8R8G8B8 backing, and
 * sample those expanded color/alpha values through both fixed-function
 * texture-stage state and a ps_2_0 texld path.
 */
void test_visual_p8_texture_sampler_policy(const struct d3d9_api *api)
{
    static const BYTE p8_texels[] = {1, 2, 3, 4};
    static const BYTE a8p8_texels[] = {1, 0x80, 2, 0x40, 3, 0x20, 4, 0x10};
    static const BYTE p8_lod_texels[] = {5};
    static const BYTE a8p8_lod_texels[] = {5, 0x60};
    static const BYTE p8_cube_texels[] = {1, 2, 3, 4, 5, 6};
    static const BYTE p8_volume_texels[] = {1, 2, 5, 6};
    static const BYTE a8p8_cube_texels[] =
    {
        1, 0xf0, 2, 0xd0, 3, 0xb0,
        4, 0x90, 5, 0x70, 6, 0x50,
    };
    static const BYTE a8p8_volume_texels[] =
    {
        1, 0xe0, 2, 0xc0, 5, 0xa0, 6, 0x80,
    };
    static const DWORD p8_expected[] =
    {
        0xff112233u, 0xff445566u,
        0xff778899u, 0xffaabbccu,
    };
    static const DWORD a8p8_expected[] =
    {
        0x80112233u, 0x40445566u,
        0x20778899u, 0x10aabbccu,
    };
    static const DWORD p8_lod_expected[] =
    {
        0xffddee11u, 0xffddee11u,
        0xffddee11u, 0xffddee11u,
    };
    static const DWORD a8p8_lod_expected[] =
    {
        0x60ddee11u, 0x60ddee11u,
        0x60ddee11u, 0x60ddee11u,
    };
    static const DWORD p8_updated_expected[] =
    {
        0xff224466u, 0xff6688aau,
        0xff99bbddu, 0xffccdd22u,
    };
    static const DWORD a8p8_updated_expected[] =
    {
        0x80224466u, 0x406688aau,
        0x2099bbddu, 0x10ccdd22u,
    };
    static const DWORD p8_cube_expected[] =
    {
        0xff112233u, 0xff445566u, 0xff778899u,
        0xffaabbccu, 0xffddee11u, 0xff3366ccu,
    };
    static const DWORD a8p8_cube_expected[] =
    {
        0xf0112233u, 0xd0445566u, 0xb0778899u,
        0x90aabbccu, 0x70ddee11u, 0x503366ccu,
    };
    static const DWORD p8_volume_expected[] =
    {
        0xff112233u, 0xff445566u, 0xffddee11u, 0xff3366ccu,
    };
    static const DWORD a8p8_volume_expected[] =
    {
        0xe0112233u, 0xc0445566u, 0xa0ddee11u, 0x803366ccu,
    };
    IDirect3DDevice9 *device = NULL;
    PALETTEENTRY updated_palette[256];
    PALETTEENTRY palette[256];
    D3DCAPS9 caps;
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

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
            D3DFMT_P8);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_P8 textures are not supported");
        goto done_d3d9;
    }
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_TEXTURE,
            D3DFMT_A8P8);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_A8P8 textures are not supported");
        goto done_d3d9;
    }
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_CUBETEXTURE,
            D3DFMT_P8);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_P8 cube textures are not supported");
        goto done_d3d9;
    }
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_CUBETEXTURE,
            D3DFMT_A8P8);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_A8P8 cube textures are not supported");
        goto done_d3d9;
    }
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_VOLUMETEXTURE,
            D3DFMT_P8);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_P8 volume textures are not supported");
        goto done_d3d9;
    }
    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, 0, D3DRTYPE_VOLUMETEXTURE,
            D3DFMT_A8P8);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_A8P8 volume textures are not supported");
        goto done_d3d9;
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

    memset(palette, 0, sizeof(palette));
    for (i = 0; i < ARRAY_SIZE(palette); ++i)
        palette[i].peFlags = 0xff;
    palette[1].peRed = 0x11; palette[1].peGreen = 0x22;
    palette[1].peBlue = 0x33; palette[1].peFlags = 0xff;
    palette[2].peRed = 0x44; palette[2].peGreen = 0x55;
    palette[2].peBlue = 0x66; palette[2].peFlags = 0xff;
    palette[3].peRed = 0x77; palette[3].peGreen = 0x88;
    palette[3].peBlue = 0x99; palette[3].peFlags = 0xff;
    palette[4].peRed = 0xaa; palette[4].peGreen = 0xbb;
    palette[4].peBlue = 0xcc; palette[4].peFlags = 0xff;
    palette[5].peRed = 0xdd; palette[5].peGreen = 0xee;
    palette[5].peBlue = 0x11; palette[5].peFlags = 0xff;
    palette[6].peRed = 0x33; palette[6].peGreen = 0x66;
    palette[6].peBlue = 0xcc; palette[6].peFlags = 0xff;
    memcpy(updated_palette, palette, sizeof(palette));
    updated_palette[1].peRed = 0x22; updated_palette[1].peGreen = 0x44;
    updated_palette[1].peBlue = 0x66; updated_palette[1].peFlags = 0xff;
    updated_palette[2].peRed = 0x66; updated_palette[2].peGreen = 0x88;
    updated_palette[2].peBlue = 0xaa; updated_palette[2].peFlags = 0xff;
    updated_palette[3].peRed = 0x99; updated_palette[3].peGreen = 0xbb;
    updated_palette[3].peBlue = 0xdd; updated_palette[3].peFlags = 0xff;
    updated_palette[4].peRed = 0xcc; updated_palette[4].peGreen = 0xdd;
    updated_palette[4].peBlue = 0x22; updated_palette[4].peFlags = 0xff;
    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, palette),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0),
            D3D_OK);

    check_visual_palettized_texture_sampler(device, D3DFMT_P8,
            p8_texels, 1, p8_expected, FALSE, NULL, NULL, NULL, NULL, 0,
            FALSE);
    check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
            a8p8_texels, 2, a8p8_expected, FALSE, NULL, NULL, NULL, NULL,
            0, FALSE);
    check_visual_palettized_texture_sampler(device, D3DFMT_P8,
            p8_texels, 1, p8_expected, FALSE, p8_lod_texels,
            p8_lod_expected, NULL, NULL, 0, FALSE);
    check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
            a8p8_texels, 2, a8p8_expected, FALSE, a8p8_lod_texels,
            a8p8_lod_expected, NULL, NULL, 0, FALSE);
    if (caps.PixelShaderVersion >= D3DPS_VERSION(2, 0))
    {
        check_visual_palettized_texture_sampler(device, D3DFMT_P8,
                p8_texels, 1, p8_expected, TRUE, NULL, NULL, NULL, NULL, 0,
                FALSE);
        check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
                a8p8_texels, 2, a8p8_expected, TRUE, NULL, NULL, NULL,
                NULL, 0, FALSE);
        check_visual_palettized_texture_sampler(device, D3DFMT_P8,
                p8_texels, 1, p8_expected, TRUE, p8_lod_texels,
                p8_lod_expected, NULL, NULL, 0, FALSE);
        check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
                a8p8_texels, 2, a8p8_expected, TRUE, a8p8_lod_texels,
                a8p8_lod_expected, NULL, NULL, 0, FALSE);
        check_visual_palettized_cube_texture_sampler(device, D3DFMT_P8,
                p8_cube_texels, 1, p8_cube_expected);
        check_visual_palettized_cube_texture_sampler(device, D3DFMT_A8P8,
                a8p8_cube_texels, 2, a8p8_cube_expected);
        check_visual_palettized_volume_texture_sampler(device, D3DFMT_P8,
                p8_volume_texels, 1, p8_volume_expected);
        check_visual_palettized_volume_texture_sampler(device, D3DFMT_A8P8,
                a8p8_volume_texels, 2, a8p8_volume_expected);
    }
    else
    {
        skip_current_test("ps_2_0 is not supported");
    }
    check_visual_palettized_texture_sampler(device, D3DFMT_P8,
            p8_texels, 1, p8_expected, FALSE, NULL, NULL,
            updated_palette, p8_updated_expected, 0, FALSE);
    check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
            a8p8_texels, 2, a8p8_expected, FALSE, NULL, NULL,
            updated_palette, a8p8_updated_expected, 0, FALSE);
    check_visual_palettized_texture_sampler(device, D3DFMT_P8,
            p8_texels, 1, p8_expected, FALSE, NULL, NULL,
            updated_palette, p8_updated_expected, 1, FALSE);
    check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
            a8p8_texels, 2, a8p8_expected, FALSE, NULL, NULL,
            updated_palette, a8p8_updated_expected, 1, FALSE);
    check_visual_palettized_texture_sampler(device, D3DFMT_P8,
            p8_texels, 1, p8_expected, FALSE, NULL, NULL,
            updated_palette, p8_updated_expected, 1, TRUE);
    check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
            a8p8_texels, 2, a8p8_expected, FALSE, NULL, NULL,
            updated_palette, a8p8_updated_expected, 1, TRUE);
    check_visual_palettized_update_texture_sampler(device, D3DFMT_P8,
            p8_texels, 1, p8_expected, updated_palette,
            p8_updated_expected, FALSE);
    check_visual_palettized_update_texture_sampler(device, D3DFMT_A8P8,
            a8p8_texels, 2, a8p8_expected, updated_palette,
            a8p8_updated_expected, FALSE);
    if (caps.PixelShaderVersion >= D3DPS_VERSION(2, 0))
    {
        check_visual_palettized_texture_sampler(device, D3DFMT_P8,
                p8_texels, 1, p8_expected, TRUE, NULL, NULL,
                updated_palette, p8_updated_expected, 0, FALSE);
        check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
                a8p8_texels, 2, a8p8_expected, TRUE, NULL, NULL,
                updated_palette, a8p8_updated_expected, 0, FALSE);
        check_visual_palettized_texture_sampler(device, D3DFMT_P8,
                p8_texels, 1, p8_expected, TRUE, NULL, NULL,
                updated_palette, p8_updated_expected, 1, FALSE);
        check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
                a8p8_texels, 2, a8p8_expected, TRUE, NULL, NULL,
                updated_palette, a8p8_updated_expected, 1, FALSE);
        check_visual_palettized_texture_sampler(device, D3DFMT_P8,
                p8_texels, 1, p8_expected, TRUE, NULL, NULL,
                updated_palette, p8_updated_expected, 1, TRUE);
        check_visual_palettized_texture_sampler(device, D3DFMT_A8P8,
                a8p8_texels, 2, a8p8_expected, TRUE, NULL, NULL,
                updated_palette, a8p8_updated_expected, 1, TRUE);
        check_visual_palettized_update_texture_sampler(device, D3DFMT_P8,
                p8_texels, 1, p8_expected, updated_palette,
                p8_updated_expected, TRUE);
        check_visual_palettized_update_texture_sampler(device, D3DFMT_A8P8,
                a8p8_texels, 2, a8p8_expected, updated_palette,
                a8p8_updated_expected, TRUE);
    }
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: shadow_test
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_visual_shadow_depth_compare_caps_policy(const struct d3d9_api *api)
{
    static const D3DFORMAT shadow_fourcc[] =
    {
        D3DFMT_D16, D3DFMT_D24S8, D3DFMT_D24X8,
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

    for (i = 0; i < ARRAY_SIZE(shadow_fourcc); ++i)
    {
        /* Probe whether the depth format can be sampled as a shadow
         * texture (D3DUSAGE_DEPTHSTENCIL + texture rtype). */
        hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_DEPTHSTENCIL,
                D3DRTYPE_TEXTURE, shadow_fourcc[i]);
        CHECK_TRUE(hr == D3D_OK || hr == D3DERR_NOTAVAILABLE);
    }

    IDirect3D9_Release(d3d9);
}
