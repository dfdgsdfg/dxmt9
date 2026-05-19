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
