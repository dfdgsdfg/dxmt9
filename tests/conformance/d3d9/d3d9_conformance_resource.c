/*
 * Resource-shaped conformance cases split from d3d9_conformance.c.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/device.c (test_private_data, test_shared_handle)
 * - dlls/d3d9/tests/d3d9ex.c (test_user_memory, test_format_unknown)
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Covers SetPrivateData byte and IUnknown round-trips on Surface, Texture,
 * CubeTexture, VolumeTexture, Vertex/IndexBuffer, plus shared-handle policy
 * and CreateRenderTarget/CreateTexture/etc. unknown-format failures.
 */

#include "d3d9_conformance_fixtures.h"

struct dual_unknown_vtbl
{
    void *query_interface;
    ULONG (WINAPI *add_ref)(IUnknown *iface);
    ULONG (WINAPI *release)(IUnknown *iface);
};

struct dual_unknown
{
    IUnknown iface;
    LONG refs;
};

static struct dual_unknown private_data_unknown;
static struct dual_unknown_vtbl private_data_vtbl;
static struct dual_unknown_vtbl private_data_wrong_vtbl;
static LONG private_data_add_refs;
static LONG private_data_releases;
static LONG private_data_wrong_add_refs;
static LONG private_data_wrong_releases;

static HRESULT WINAPI private_data_qi(IUnknown *iface, REFIID riid, void **out)
{
    (void)iface;
    (void)riid;
    if (out)
        *out = NULL;
    return E_NOINTERFACE;
}

static ULONG WINAPI private_data_add_ref(IUnknown *iface)
{
    (void)iface;
    InterlockedIncrement(&private_data_add_refs);
    return InterlockedIncrement(&private_data_unknown.refs);
}

static ULONG WINAPI private_data_release(IUnknown *iface)
{
    (void)iface;
    InterlockedIncrement(&private_data_releases);
    return InterlockedDecrement(&private_data_unknown.refs);
}

static ULONG WINAPI private_data_wrong_add_ref(IUnknown *iface)
{
    (void)iface;
    InterlockedIncrement(&private_data_wrong_add_refs);
    return 2;
}

static ULONG WINAPI private_data_wrong_release(IUnknown *iface)
{
    (void)iface;
    InterlockedIncrement(&private_data_wrong_releases);
    return 1;
}

static void reset_private_data_unknown(void)
{
    private_data_wrong_vtbl.query_interface = private_data_qi;
    private_data_wrong_vtbl.add_ref = private_data_wrong_add_ref;
    private_data_wrong_vtbl.release = private_data_wrong_release;

    /*
     * Correct D3DSPD_IUNKNOWN handling calls AddRef/Release on
     * private_data_unknown. A pointer-to-pointer implementation instead reads
     * the object's first pointer and calls through that address. This vtbl is
     * shaped so either path stays callable; the counters identify the path.
     */
    private_data_vtbl.query_interface = &private_data_wrong_vtbl;
    private_data_vtbl.add_ref = private_data_add_ref;
    private_data_vtbl.release = private_data_release;

    private_data_unknown.iface.lpVtbl = (IUnknownVtbl *)&private_data_vtbl;
    private_data_unknown.refs = 1;
    private_data_add_refs = 0;
    private_data_releases = 0;
    private_data_wrong_add_refs = 0;
    private_data_wrong_releases = 0;
}

/* Wine device.c: D3DSPD_IUNKNOWN stores and releases an owned IUnknown ref. */
void test_private_data_iunknown_ownership_smoke(
        const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    IDirect3DDevice9 *device = NULL;
    IDirect3DSurface9 *surface = NULL;
    IUnknown *ptr = NULL;
    HWND window;
    DWORD size;
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

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 4, 4,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateOffscreenPlainSurface failed with %s",
                hr_buffer);
        goto done_device;
    }

    reset_private_data_unknown();

    hr = IDirect3DSurface9_SetPrivateData(surface, &private_data_guid,
            &private_data_unknown.iface, 0, D3DSPD_IUNKNOWN);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    if (SUCCEEDED(hr))
        IDirect3DSurface9_FreePrivateData(surface, &private_data_guid);

    hr = IDirect3DSurface9_SetPrivateData(surface, &private_data_guid,
            &private_data_unknown.iface, sizeof(IUnknown *) * 2,
            D3DSPD_IUNKNOWN);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    if (SUCCEEDED(hr))
        IDirect3DSurface9_FreePrivateData(surface, &private_data_guid);

    reset_private_data_unknown();

    hr = IDirect3DSurface9_SetPrivateData(surface, &private_data_guid,
            &private_data_unknown.iface, sizeof(IUnknown *), D3DSPD_IUNKNOWN);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(private_data_add_refs == 1);
    CHECK_TRUE(private_data_wrong_add_refs == 0);
    CHECK_TRUE(private_data_releases == 0);

    size = sizeof(ptr);
    hr = IDirect3DSurface9_GetPrivateData(surface, &private_data_guid, &ptr,
            &size);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(size == sizeof(ptr));
    CHECK_TRUE(ptr == &private_data_unknown.iface);
    CHECK_TRUE(private_data_add_refs == 2);
    CHECK_TRUE(private_data_wrong_add_refs == 0);

    if (ptr)
    {
        IUnknown_Release(ptr);
        ptr = NULL;
    }
    CHECK_TRUE(private_data_releases == 1);

    hr = IDirect3DSurface9_FreePrivateData(surface, &private_data_guid);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(private_data_releases == 2);
    CHECK_TRUE(private_data_wrong_releases == 0);
    CHECK_TRUE(private_data_unknown.refs == 1);

    IDirect3DSurface9_Release(surface);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

static void check_private_data_bytes_on_resource(IDirect3DResource9 *resource)
{
    static const unsigned char expected[] = {0x11, 0x22, 0x33, 0x44};
    unsigned char actual[sizeof(expected)] = {0};
    DWORD size;
    HRESULT hr;

    hr = IDirect3DResource9_SetPrivateData(resource, &private_data_guid,
            expected, sizeof(expected), 0);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        return;

    size = 0;
    hr = IDirect3DResource9_GetPrivateData(resource, &private_data_guid,
            NULL, &size);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(size == sizeof(expected));

    size = sizeof(actual);
    hr = IDirect3DResource9_GetPrivateData(resource, &private_data_guid,
            actual, &size);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(size == sizeof(expected));
    CHECK_TRUE(memcmp(actual, expected, sizeof(expected)) == 0);

    memset(actual, 0xcc, sizeof(actual));
    size = 1;
    hr = IDirect3DResource9_GetPrivateData(resource, &private_data_guid,
            actual, &size);
    CHECK_HR(hr, D3DERR_MOREDATA);
    CHECK_TRUE(size == sizeof(expected));
    CHECK_TRUE(actual[0] == 0xcc);

    hr = IDirect3DResource9_FreePrivateData(resource, &private_data_guid);
    CHECK_HR(hr, D3D_OK);

    size = 0xdeadbabe;
    hr = IDirect3DResource9_GetPrivateData(resource, &private_data_guid,
            actual, &size);
    CHECK_HR(hr, D3DERR_NOTFOUND);
    CHECK_TRUE(size == 0xdeadbabe);
}

static void check_private_data_bytes_on_volume(IDirect3DVolume9 *volume)
{
    static const unsigned char expected[] = {0x55, 0x66, 0x77, 0x88};
    unsigned char actual[sizeof(expected)] = {0};
    DWORD size;
    HRESULT hr;

    hr = IDirect3DVolume9_SetPrivateData(volume, &private_data_guid,
            expected, sizeof(expected), 0);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        return;

    size = sizeof(actual);
    hr = IDirect3DVolume9_GetPrivateData(volume, &private_data_guid,
            actual, &size);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(size == sizeof(expected));
    CHECK_TRUE(memcmp(actual, expected, sizeof(expected)) == 0);

    hr = IDirect3DVolume9_FreePrivateData(volume, &private_data_guid);
    CHECK_HR(hr, D3D_OK);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_private_data()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_private_data_resource_wrappers(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DVolumeTexture9 *volume_texture = NULL;
    IDirect3DCubeTexture9 *cube_texture = NULL;
    IDirect3DIndexBuffer9 *index_buffer = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DSurface9 *surface2 = NULL;
    IDirect3DSurface9 *rt = NULL;
    IDirect3DSurface9 *ds = NULL;
    IDirect3DVolume9 *volume = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    HWND window;
    DWORD size;
    BYTE data[4];
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

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 4, 4,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_private_data_bytes_on_resource((IDirect3DResource9 *)surface);
        IDirect3DSurface9_Release(surface);
        surface = NULL;
    }

    hr = IDirect3DDevice9_CreateRenderTarget(device, 4, 4,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_private_data_bytes_on_resource((IDirect3DResource9 *)rt);
        IDirect3DSurface9_Release(rt);
        rt = NULL;
    }

    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 4, 4,
            D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &ds, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_private_data_bytes_on_resource((IDirect3DResource9 *)ds);
        IDirect3DSurface9_Release(ds);
        ds = NULL;
    }

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 2, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_private_data_bytes_on_resource((IDirect3DResource9 *)texture);

        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 1, &surface2);
        CHECK_HR(hr, D3D_OK);
        if (surface && surface2)
        {
            static const BYTE texture_data[4] = {1, 2, 3, 4};

            hr = IDirect3DTexture9_SetPrivateData(texture, &private_data_guid,
                    texture_data, sizeof(texture_data), 0);
            CHECK_HR(hr, D3D_OK);
            size = sizeof(data);
            hr = IDirect3DSurface9_GetPrivateData(surface, &private_data_guid,
                    data, &size);
            CHECK_HR(hr, D3DERR_NOTFOUND);

            hr = IDirect3DSurface9_SetPrivateData(surface, &private_data_guid,
                    texture_data, sizeof(texture_data), 0);
            CHECK_HR(hr, D3D_OK);
            size = sizeof(data);
            hr = IDirect3DSurface9_GetPrivateData(surface2, &private_data_guid,
                    data, &size);
            CHECK_HR(hr, D3DERR_NOTFOUND);
            IDirect3DSurface9_FreePrivateData(surface, &private_data_guid);
            IDirect3DTexture9_FreePrivateData(texture, &private_data_guid);
        }
        if (surface2)
            IDirect3DSurface9_Release(surface2);
        if (surface)
            IDirect3DSurface9_Release(surface);
        surface = NULL;
        surface2 = NULL;
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    if (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP)
    {
        hr = IDirect3DDevice9_CreateCubeTexture(device, 4, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &cube_texture, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            check_private_data_bytes_on_resource(
                    (IDirect3DResource9 *)cube_texture);
            hr = IDirect3DCubeTexture9_GetCubeMapSurface(cube_texture,
                    D3DCUBEMAP_FACE_POSITIVE_X, 0, &surface);
            CHECK_HR(hr, D3D_OK);
            if (surface)
            {
                check_private_data_bytes_on_resource(
                        (IDirect3DResource9 *)surface);
                IDirect3DSurface9_Release(surface);
                surface = NULL;
            }
            IDirect3DCubeTexture9_Release(cube_texture);
            cube_texture = NULL;
        }
    }

    if (caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP)
    {
        hr = IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &volume_texture, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            check_private_data_bytes_on_resource(
                    (IDirect3DResource9 *)volume_texture);
            hr = IDirect3DVolumeTexture9_GetVolumeLevel(volume_texture, 0,
                    &volume);
            CHECK_HR(hr, D3D_OK);
            if (volume)
            {
                check_private_data_bytes_on_volume(volume);
                IDirect3DVolume9_Release(volume);
                volume = NULL;
            }
            IDirect3DVolumeTexture9_Release(volume_texture);
            volume_texture = NULL;
        }
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 16, 0, 0,
            D3DPOOL_DEFAULT, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_private_data_bytes_on_resource(
                (IDirect3DResource9 *)vertex_buffer);
        IDirect3DVertexBuffer9_Release(vertex_buffer);
        vertex_buffer = NULL;
    }

    hr = IDirect3DDevice9_CreateIndexBuffer(device, 16, 0, D3DFMT_INDEX16,
            D3DPOOL_DEFAULT, &index_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_private_data_bytes_on_resource(
                (IDirect3DResource9 *)index_buffer);
        IDirect3DIndexBuffer9_Release(index_buffer);
        index_buffer = NULL;
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_private_data()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_private_data_replace_and_size_policy(const struct d3d9_api *api)
{
    static const BYTE first_data[] = {0x10, 0x20};
    static const BYTE second_data[] = {0x30, 0x40, 0x50, 0x60};
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    BYTE actual[sizeof(second_data)];
    DWORD size;
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

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 4, 4,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DSurface9_SetPrivateData(surface, &private_data_guid,
            first_data, sizeof(first_data), 0);
    CHECK_HR(hr, D3D_OK);

    size = sizeof(actual);
    memset(actual, 0xcc, sizeof(actual));
    hr = IDirect3DSurface9_GetPrivateData(surface, &private_data_guid,
            actual, &size);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(size == sizeof(first_data));
    CHECK_TRUE(memcmp(actual, first_data, sizeof(first_data)) == 0);

    hr = IDirect3DSurface9_SetPrivateData(surface, &private_data_guid,
            second_data, sizeof(second_data), 0);
    CHECK_HR(hr, D3D_OK);

    size = sizeof(first_data);
    memset(actual, 0xcc, sizeof(actual));
    hr = IDirect3DSurface9_GetPrivateData(surface, &private_data_guid,
            actual, &size);
    CHECK_HR(hr, D3DERR_MOREDATA);
    CHECK_TRUE(size == sizeof(second_data));
    CHECK_TRUE(actual[0] == 0xcc);

    size = sizeof(actual);
    memset(actual, 0, sizeof(actual));
    hr = IDirect3DSurface9_GetPrivateData(surface, &private_data_guid,
            actual, &size);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(size == sizeof(second_data));
    CHECK_TRUE(memcmp(actual, second_data, sizeof(second_data)) == 0);

    CHECK_HR(IDirect3DSurface9_FreePrivateData(surface, &private_data_guid),
            D3D_OK);
    CHECK_HR(IDirect3DSurface9_FreePrivateData(surface, &private_data_guid),
            D3DERR_NOTFOUND);

    IDirect3DSurface9_Release(surface);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

static void check_surface_lock_policy(IDirect3DSurface9 *surface)
{
    D3DLOCKED_RECT locked;
    RECT rect;
    HRESULT hr;

    CHECK_HR(IDirect3DSurface9_LockRect(surface, NULL, NULL, 0),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3DERR_INVALIDCALL);

    rect.left = 3;
    rect.top = 0;
    rect.right = 1;
    rect.bottom = 2;
    CHECK_HR(IDirect3DSurface9_LockRect(surface, &locked, &rect, 0),
            D3DERR_INVALIDCALL);

    rect.left = -1;
    rect.top = 0;
    rect.right = 2;
    rect.bottom = 2;
    CHECK_HR(IDirect3DSurface9_LockRect(surface, &locked, &rect, 0),
            D3DERR_INVALIDCALL);

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(surface, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.Pitch >= 16);
        CHECK_HR(IDirect3DSurface9_LockRect(surface, &locked, NULL, 0),
                D3DERR_INVALIDCALL);
        CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3D_OK);
        CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3DERR_INVALIDCALL);
    }

    rect.left = 1;
    rect.top = 1;
    rect.right = 3;
    rect.bottom = 3;
    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(surface, &locked, &rect, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.Pitch >= 16);
        memset(locked.pBits, 0x5a, 2 * 4);
        CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3D_OK);
    }
}

static void check_texture_lock_policy(IDirect3DTexture9 *texture)
{
    D3DLOCKED_RECT locked;
    RECT rect;
    HRESULT hr;

    CHECK_HR(IDirect3DTexture9_LockRect(texture, 3, &locked, NULL, 0),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 3), D3DERR_INVALIDCALL);

    rect.left = 0;
    rect.top = 0;
    rect.right = 1;
    rect.bottom = 1;
    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(texture, 1, &locked, &rect, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.Pitch >= 8);
        memset(locked.pBits, 0xa5, 4);
        CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 1), D3D_OK);
    }

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL,
            D3DLOCK_DISCARD);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
}

static void check_volume_lock_policy(IDirect3DVolumeTexture9 *texture)
{
    D3DLOCKED_BOX locked;
    D3DBOX box;
    HRESULT hr;

    CHECK_HR(IDirect3DVolumeTexture9_LockBox(texture, 0, NULL, NULL, 0),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DVolumeTexture9_LockBox(texture, 2, &locked, NULL, 0),
            D3DERR_INVALIDCALL);

    box.Left = 3;
    box.Top = 0;
    box.Right = 1;
    box.Bottom = 2;
    box.Front = 0;
    box.Back = 1;
    CHECK_HR(IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, &box, 0),
            D3DERR_INVALIDCALL);

    box.Left = 1;
    box.Top = 1;
    box.Right = 3;
    box.Bottom = 3;
    box.Front = 1;
    box.Back = 3;
    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, &box, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.RowPitch >= 16);
        CHECK_TRUE(locked.SlicePitch >= locked.RowPitch * 4);
        CHECK_HR(IDirect3DVolumeTexture9_LockBox(texture, 0, &locked,
                NULL, 0), D3DERR_INVALIDCALL);
        memset(locked.pBits, 0x3c, 2 * 4);
        CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0), D3D_OK);
        CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0),
                D3DERR_INVALIDCALL);
    }
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_lockrect_invalid(), test_lockrect_offset(),
 * test_mipmap_lock(), test_volume_locking()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_resource_lock_error_policy(const struct d3d9_api *api)
{
    IDirect3DVolumeTexture9 *volume_texture = NULL;
    IDirect3DTexture9 *writeonly_texture = NULL;
    IDirect3DTexture9 *texture = NULL;
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

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 4, 4,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_surface_lock_policy(surface);
        IDirect3DSurface9_Release(surface);
        surface = NULL;
    }

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 3, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_texture_lock_policy(texture);
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 1,
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, D3DFMT_A8R8G8B8,
            D3DPOOL_DEFAULT, &writeonly_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(writeonly_texture, 0, &locked,
                NULL, D3DLOCK_DISCARD);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(locked.pBits != NULL);
            memset(locked.pBits, 0x7e, 4 * 4 * 4);
            CHECK_HR(IDirect3DTexture9_UnlockRect(writeonly_texture, 0),
                    D3D_OK);
        }
        IDirect3DTexture9_Release(writeonly_texture);
        writeonly_texture = NULL;
    }

    hr = IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &volume_texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        check_volume_lock_policy(volume_texture);
        IDirect3DVolumeTexture9_Release(volume_texture);
        volume_texture = NULL;
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_vb_lock_flags()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vb_lock_flags(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *dynamic_vb = NULL;
    IDirect3DVertexBuffer9 *system_vb = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    void *data = NULL;
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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 64, 0, 0,
            D3DPOOL_SYSTEMMEM, &system_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DVertexBuffer9_Lock(system_vb, 0, 16, NULL, 0),
                D3DERR_INVALIDCALL);
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(system_vb),
                D3DERR_INVALIDCALL);

        data = NULL;
        hr = IDirect3DVertexBuffer9_Lock(system_vb, 4, 16, &data,
                D3DLOCK_READONLY);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(data != NULL);
            CHECK_HR(IDirect3DVertexBuffer9_Lock(system_vb, 0, 16, &data, 0),
                    D3DERR_INVALIDCALL);
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(system_vb), D3D_OK);
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(system_vb),
                    D3DERR_INVALIDCALL);
        }

        data = NULL;
        hr = IDirect3DVertexBuffer9_Lock(system_vb, 64, 1, &data, 0);
        CHECK_HR(hr, D3DERR_INVALIDCALL);

        IDirect3DVertexBuffer9_Release(system_vb);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 64,
            D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT,
            &dynamic_vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        data = NULL;
        hr = IDirect3DVertexBuffer9_Lock(dynamic_vb, 0, 0, &data,
                D3DLOCK_DISCARD);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(data != NULL);
            memset(data, 0x5a, 64);
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(dynamic_vb), D3D_OK);
        }

        data = NULL;
        hr = IDirect3DVertexBuffer9_Lock(dynamic_vb, 16, 16, &data,
                D3DLOCK_NOOVERWRITE);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(data != NULL);
            memset(data, 0xa5, 16);
            CHECK_HR(IDirect3DVertexBuffer9_Unlock(dynamic_vb), D3D_OK);
        }

        IDirect3DVertexBuffer9_Release(dynamic_vb);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_vertex_buffer_alignment()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vertex_buffer_alignment(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    BYTE *base_data = NULL;
    BYTE *data = NULL;
    BYTE *offset_data = NULL;
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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 64, 0, 0,
            D3DPOOL_SYSTEMMEM, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DVertexBuffer9_Lock(vertex_buffer, 0, 0, (void **)&data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(data != NULL);
        CHECK_TRUE(((ULONG_PTR)data & 15) == 0);
        base_data = data;
        memset(data, 0x1d, 64);
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(vertex_buffer), D3D_OK);
    }

    hr = IDirect3DVertexBuffer9_Lock(vertex_buffer, 4, 16,
            (void **)&offset_data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        if (base_data)
            CHECK_TRUE(offset_data == base_data + 4);
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(vertex_buffer), D3D_OK);
    }

    IDirect3DVertexBuffer9_Release(vertex_buffer);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_surface_alignment()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_surface_alignment(const struct d3d9_api *api)
{
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_RECT locked;
    IDirect3D9 *d3d9;
    RECT rect;
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

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 8, 4,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(surface, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(((ULONG_PTR)locked.pBits & 3) == 0);
        CHECK_TRUE(locked.Pitch >= 8 * 4);
        CHECK_TRUE((locked.Pitch & 3) == 0);
        CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3D_OK);
    }

    rect.left = 1;
    rect.top = 1;
    rect.right = 3;
    rect.bottom = 3;
    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(surface, &locked, &rect, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(((ULONG_PTR)locked.pBits & 3) == 0);
        CHECK_TRUE(locked.Pitch >= 8 * 4);
        CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3D_OK);
    }

    IDirect3DSurface9_Release(surface);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_surface_dimensions()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_surface_dimensions(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DSURFACE_DESC desc;
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

    surface = (IDirect3DSurface9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 0, 4,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(surface == NULL);

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 7, 5,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&desc, 0xcc, sizeof(desc));
        hr = IDirect3DSurface9_GetDesc(surface, &desc);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(desc.Type == D3DRTYPE_SURFACE);
            CHECK_TRUE(desc.Width == 7);
            CHECK_TRUE(desc.Height == 5);
            CHECK_TRUE(desc.Format == D3DFMT_A8R8G8B8);
            CHECK_TRUE(desc.Pool == D3DPOOL_SCRATCH);
        }
        IDirect3DSurface9_Release(surface);
        surface = NULL;
    }

    hr = IDirect3DDevice9_CreateTexture(device, 4, 2, 3, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(IDirect3DTexture9_GetLevelCount(texture) == 3);

        memset(&desc, 0xcc, sizeof(desc));
        hr = IDirect3DTexture9_GetLevelDesc(texture, 0, &desc);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(desc.Width == 4);
        CHECK_TRUE(desc.Height == 2);

        memset(&desc, 0xcc, sizeof(desc));
        hr = IDirect3DTexture9_GetLevelDesc(texture, 1, &desc);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(desc.Width == 2);
        CHECK_TRUE(desc.Height == 1);

        memset(&desc, 0xcc, sizeof(desc));
        hr = IDirect3DTexture9_GetLevelDesc(texture, 2, &desc);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(desc.Width == 1);
        CHECK_TRUE(desc.Height == 1);

        CHECK_HR(IDirect3DTexture9_GetLevelDesc(texture, 3, &desc),
                D3DERR_INVALIDCALL);
        CHECK_HR(IDirect3DTexture9_GetLevelDesc(texture, 0, NULL),
                D3DERR_INVALIDCALL);

        IDirect3DTexture9_Release(texture);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_surface_format_null()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_surface_format_null_policy(const struct d3d9_api *api)
{
    static const D3DFORMAT null_format = (D3DFORMAT)MAKEFOURCC('N', 'U', 'L', 'L');
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DSurface9 *previous_rt = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_RECT locked;
    D3DSURFACE_DESC desc;
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
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
            D3DRTYPE_SURFACE, null_format);
    if (hr != D3D_OK)
    {
        skip_current_test("D3DFMT_NULL render-target surfaces are unavailable");
        goto done_d3d9;
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    CHECK_HR(IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
            D3DRTYPE_TEXTURE, null_format), D3D_OK);
    CHECK_SUCCEEDED(IDirect3D9_CheckDepthStencilMatch(d3d9,
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
            null_format, D3DFMT_D24S8));

    hr = IDirect3DDevice9_CreateRenderTarget(device, 128, 128, null_format,
            D3DMULTISAMPLE_NONE, 0, TRUE, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DDevice9_GetRenderTarget(device, 0, &previous_rt);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, surface),
                D3D_OK);
        CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
                0, 1.0f, 0), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetRenderTarget(device, 0, previous_rt),
                D3D_OK);
        IDirect3DSurface9_Release(previous_rt);
    }

    memset(&desc, 0xcc, sizeof(desc));
    hr = IDirect3DSurface9_GetDesc(surface, &desc);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(desc.Width == 128);
        CHECK_TRUE(desc.Height == 128);
        CHECK_TRUE(desc.Format == null_format);
        CHECK_TRUE(desc.Usage == D3DUSAGE_RENDERTARGET);
    }

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DSurface9_LockRect(surface, &locked, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.Pitch != 0);
        CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3D_OK);
    }
    IDirect3DSurface9_Release(surface);

    hr = IDirect3DDevice9_CreateTexture(device, 128, 128, 0,
            D3DUSAGE_RENDERTARGET, null_format, D3DPOOL_DEFAULT, &texture,
            NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        IDirect3DTexture9_Release(texture);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_resource_type()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_resource_type(const struct d3d9_api *api)
{
    IDirect3DVolumeTexture9 *volume_texture = NULL;
    IDirect3DCubeTexture9 *cube_texture = NULL;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DIndexBuffer9 *index_buffer = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DTexture9 *texture = NULL;
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

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 4, 4,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(IDirect3DSurface9_GetType(surface) == D3DRTYPE_SURFACE);
        IDirect3DSurface9_Release(surface);
    }

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(IDirect3DTexture9_GetType(texture) == D3DRTYPE_TEXTURE);
        IDirect3DTexture9_Release(texture);
    }

    if (caps.TextureCaps & D3DPTEXTURECAPS_CUBEMAP)
    {
        hr = IDirect3DDevice9_CreateCubeTexture(device, 4, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube_texture, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(IDirect3DCubeTexture9_GetType(cube_texture)
                    == D3DRTYPE_CUBETEXTURE);
            IDirect3DCubeTexture9_Release(cube_texture);
        }
    }

    if (caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP)
    {
        hr = IDirect3DDevice9_CreateVolumeTexture(device, 4, 4, 4, 1, 0,
                D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &volume_texture, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(IDirect3DVolumeTexture9_GetType(volume_texture)
                    == D3DRTYPE_VOLUMETEXTURE);
            IDirect3DVolumeTexture9_Release(volume_texture);
        }
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 16, 0, 0,
            D3DPOOL_SYSTEMMEM, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(IDirect3DVertexBuffer9_GetType(vertex_buffer)
                == D3DRTYPE_VERTEXBUFFER);
        IDirect3DVertexBuffer9_Release(vertex_buffer);
    }

    hr = IDirect3DDevice9_CreateIndexBuffer(device, 16, 0, D3DFMT_INDEX16,
            D3DPOOL_SYSTEMMEM, &index_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(IDirect3DIndexBuffer9_GetType(index_buffer)
                == D3DRTYPE_INDEXBUFFER);
        IDirect3DIndexBuffer9_Release(index_buffer);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_volume_resource()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_volume_resource_container_desc(const struct d3d9_api *api)
{
    IDirect3DVolumeTexture9 *container = NULL;
    IDirect3DVolumeTexture9 *texture = NULL;
    IDirect3DResource9 *resource = NULL;
    IDirect3DVolume9 *volume = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DVOLUME_DESC desc;
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
    if (!(caps.TextureCaps & D3DPTEXTURECAPS_VOLUMEMAP))
    {
        skip_current_test("volume textures are not supported");
        goto done_device;
    }

    hr = IDirect3DDevice9_CreateVolumeTexture(device, 8, 4, 2, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DVolumeTexture9_GetVolumeLevel(texture, 0, &volume);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(volume != NULL);
    if (FAILED(hr) || !volume)
        goto done_texture;

    memset(&desc, 0xcc, sizeof(desc));
    hr = IDirect3DVolume9_GetDesc(volume, &desc);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(desc.Type == D3DRTYPE_VOLUME);
        CHECK_TRUE(desc.Width == 8);
        CHECK_TRUE(desc.Height == 4);
        CHECK_TRUE(desc.Depth == 2);
        CHECK_TRUE(desc.Format == D3DFMT_A8R8G8B8);
        CHECK_TRUE(desc.Pool == D3DPOOL_DEFAULT);
    }

    hr = IDirect3DVolume9_GetContainer(volume, &IID_IDirect3DVolumeTexture9,
            (void **)&container);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(container == texture);
    if (container)
        IDirect3DVolumeTexture9_Release(container);

    resource = (IDirect3DResource9 *)0xdeadbeef;
    hr = IDirect3DVolume9_QueryInterface(volume, &IID_IDirect3DResource9,
            (void **)&resource);
    CHECK_HR(hr, E_NOINTERFACE);
    CHECK_TRUE(resource == NULL);
    if (resource && resource != (IDirect3DResource9 *)0xdeadbeef)
        IDirect3DResource9_Release(resource);

    IDirect3DVolume9_Release(volume);

done_texture:
    IDirect3DVolumeTexture9_Release(texture);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_volume_blocks()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_volume_block_lock_layout(const struct d3d9_api *api)
{
    IDirect3DVolumeTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_BOX locked;
    IDirect3D9 *d3d9;
    D3DBOX box;
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

    hr = IDirect3DDevice9_CreateVolumeTexture(device, 8, 8, 2, 1, 0,
            D3DFMT_DXT1, D3DPOOL_SCRATCH, &texture, NULL);
    if (hr == D3DERR_INVALIDCALL || hr == D3DERR_NOTAVAILABLE)
    {
        skip_current_test("DXT1 scratch volume textures are unavailable");
        goto done_device;
    }
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, NULL,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.RowPitch >= 16);
        CHECK_TRUE((locked.RowPitch % 8) == 0);
        CHECK_TRUE(locked.SlicePitch >= locked.RowPitch * 2);
        CHECK_TRUE((locked.SlicePitch % locked.RowPitch) == 0);
        CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0), D3D_OK);
    }

    box.Left = 4;
    box.Top = 4;
    box.Front = 1;
    box.Right = 8;
    box.Bottom = 8;
    box.Back = 2;
    memset(&locked, 0xcc, sizeof(locked));
    hr = IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, &box,
            D3DLOCK_READONLY);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(locked.pBits != NULL);
        CHECK_TRUE(locked.RowPitch >= 16);
        CHECK_TRUE(locked.SlicePitch >= locked.RowPitch * 2);
        CHECK_HR(IDirect3DVolumeTexture9_UnlockBox(texture, 0), D3D_OK);
    }

    box.Left = 1;
    box.Top = 0;
    box.Front = 0;
    box.Right = 5;
    box.Bottom = 4;
    box.Back = 1;
    CHECK_HR(IDirect3DVolumeTexture9_LockBox(texture, 0, &locked, &box,
            D3DLOCK_READONLY), D3DERR_INVALIDCALL);

    IDirect3DVolumeTexture9_Release(texture);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_surface_format_null(), test_volume_blocks()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vendor_format_public_api_policy(const struct d3d9_api *api)
{
    static const D3DFORMAT null_format = (D3DFORMAT)MAKEFOURCC('N','U','L','L');
    static const D3DFORMAT ati1_format = (D3DFORMAT)MAKEFOURCC('A','T','I','1');
    static const D3DFORMAT ati2_format = (D3DFORMAT)MAKEFOURCC('A','T','I','2');
    static const D3DFORMAT intz_format = (D3DFORMAT)MAKEFOURCC('I','N','T','Z');
    static const D3DFORMAT fetch4_format = (D3DFORMAT)MAKEFOURCC('G','E','T','4');
    static const D3DFORMAT resz_format = (D3DFORMAT)MAKEFOURCC('R','E','S','Z');
    IDirect3DSurface9 *surface = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DFORMAT formats[] =
    {
        ati1_format,
        ati2_format,
        intz_format,
        fetch4_format,
        resz_format,
    };
    HWND window;
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

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
            D3DRTYPE_SURFACE, null_format);
    if (hr != D3D_OK)
    {
        skip_current_test("NULL render-target format is unavailable");
        goto done_device;
    }

    CHECK_HR(IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
            D3DRTYPE_TEXTURE, null_format), D3D_OK);

    hr = IDirect3DDevice9_CreateRenderTarget(device, 16, 16, null_format,
            D3DMULTISAMPLE_NONE, 0, TRUE, &surface, NULL);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(surface != NULL);
    if (surface)
    {
        IDirect3DSurface9_Release(surface);
        surface = NULL;
    }

    hr = IDirect3DDevice9_CreateTexture(device, 16, 16, 1,
            D3DUSAGE_RENDERTARGET, null_format, D3DPOOL_DEFAULT, &texture,
            NULL);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(texture != NULL);
    if (texture)
        IDirect3DTexture9_Release(texture);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_resource_priority()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_resource_priority_roundtrip(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    DWORD previous;
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

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(IDirect3DTexture9_GetPriority(texture) == 0);
        previous = IDirect3DTexture9_SetPriority(texture, 7);
        CHECK_TRUE(previous == 0);
        CHECK_TRUE(IDirect3DTexture9_GetPriority(texture) == 7);
        previous = IDirect3DTexture9_SetPriority(texture, 3);
        CHECK_TRUE(previous == 7);
        CHECK_TRUE(IDirect3DTexture9_GetPriority(texture) == 3);
        IDirect3DTexture9_Release(texture);
    }

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 16, 0, 0,
            D3DPOOL_MANAGED, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(IDirect3DVertexBuffer9_GetPriority(vertex_buffer) == 0);
        previous = IDirect3DVertexBuffer9_SetPriority(vertex_buffer, 11);
        CHECK_TRUE(previous == 0);
        CHECK_TRUE(IDirect3DVertexBuffer9_GetPriority(vertex_buffer) == 11);
        IDirect3DVertexBuffer9_Release(vertex_buffer);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_render_target_device_mismatch()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_render_target_device_mismatch(const struct d3d9_api *api)
{
    IDirect3DSurface9 *foreign_depth = NULL;
    IDirect3DSurface9 *foreign_rt = NULL;
    IDirect3DDevice9 *device_a = NULL;
    IDirect3DDevice9 *device_b = NULL;
    IDirect3D9 *d3d9;
    HWND window_a;
    HWND window_b;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    window_a = create_test_window();
    CHECK_TRUE(window_a != NULL);
    if (!window_a)
        goto done_d3d9;

    window_b = create_test_window();
    CHECK_TRUE(window_b != NULL);
    if (!window_b)
        goto done_window_a;

    device_a = create_base_device(d3d9, window_a);
    if (!device_a)
        goto done_window_b;

    device_b = create_base_device(d3d9, window_b);
    if (!device_b)
        goto done_device_a;

    hr = IDirect3DDevice9_CreateRenderTarget(device_b, 16, 16,
            D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &foreign_rt,
            NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_SetRenderTarget(device_a, 0, foreign_rt),
                D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(foreign_rt);
    }

    hr = IDirect3DDevice9_CreateDepthStencilSurface(device_b, 16, 16,
            D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, &foreign_depth,
            NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_SetDepthStencilSurface(device_a,
                foreign_depth), D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(foreign_depth);
    }

    IDirect3DDevice9_Release(device_b);

done_device_a:
    IDirect3DDevice9_Release(device_a);
done_window_b:
    DestroyWindow(window_b);
done_window_a:
    DestroyWindow(window_a);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_create_depth_stencil_surface_ex()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_create_depth_stencil_surface_ex(const struct d3d9_api *api)
{
    IDirect3DDevice9Ex *device = NULL;
    IDirect3DSurface9 *surface;
    D3DSURFACE_DESC desc;
    IDirect3D9Ex *d3d9ex;
    HWND window;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_ex_device(d3d9ex, window);
    if (!device)
        goto done_window;

    CHECK_HR(IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx(device, 8, 8,
            D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, FALSE, NULL, NULL, 0),
            D3DERR_INVALIDCALL);

    surface = (IDirect3DSurface9 *)0xdeadbeef;
    hr = IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx(device, 8, 8,
            D3DFMT_UNKNOWN, D3DMULTISAMPLE_NONE, 0, FALSE, &surface, NULL, 0);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(surface == NULL);

    surface = NULL;
    hr = IDirect3DDevice9Ex_CreateDepthStencilSurfaceEx(device, 8, 8,
            D3DFMT_D16, D3DMULTISAMPLE_NONE, 0, TRUE, &surface, NULL, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&desc, 0xcc, sizeof(desc));
        hr = IDirect3DSurface9_GetDesc(surface, &desc);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(desc.Type == D3DRTYPE_SURFACE);
            CHECK_TRUE(desc.Usage == D3DUSAGE_DEPTHSTENCIL);
            CHECK_TRUE(desc.Pool == D3DPOOL_DEFAULT);
            CHECK_TRUE(desc.Format == D3DFMT_D16);
            CHECK_TRUE(desc.Width == 8);
            CHECK_TRUE(desc.Height == 8);
            CHECK_TRUE(desc.MultiSampleType == D3DMULTISAMPLE_NONE);
        }
        IDirect3DSurface9_Release(surface);
    }

    IDirect3DDevice9Ex_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_shared_handle()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_shared_handle_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DIndexBuffer9 *index_buffer = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HANDLE handle = NULL;
    void *allocation;
    void *mem;
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

    allocation = HeapAlloc(GetProcessHeap(), 0, 128 * 128 * 4);
    CHECK_TRUE(allocation != NULL);
    if (!allocation)
        goto done_device;

    hr = IDirect3DDevice9_CreateTexture(device, 128, 128, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &texture, &handle);
    CHECK_HR(hr, E_NOTIMPL);
    CHECK_TRUE(texture == NULL);

    mem = allocation;
    hr = IDirect3DDevice9_CreateTexture(device, 128, 128, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &texture, &mem);
    CHECK_HR(hr, E_NOTIMPL);
    CHECK_TRUE(texture == NULL);

    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 128, 128,
            D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &surface, &handle);
    CHECK_HR(hr, E_NOTIMPL);
    CHECK_TRUE(surface == NULL);

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 16, 0, 0,
            D3DPOOL_DEFAULT, &vertex_buffer, &handle);
    CHECK_HR(hr, E_NOTIMPL);
    CHECK_TRUE(vertex_buffer == NULL);

    hr = IDirect3DDevice9_CreateIndexBuffer(device, 16, 0, D3DFMT_INDEX16,
            D3DPOOL_DEFAULT, &index_buffer, &handle);
    CHECK_HR(hr, E_NOTIMPL);
    CHECK_TRUE(index_buffer == NULL);

    HeapFree(GetProcessHeap(), 0, allocation);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_user_memory()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_shared_handle_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DIndexBuffer9 *index_buffer = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9Ex *device = NULL;
    IDirect3D9Ex *d3d9ex;
    void *allocation;
    void *mem;
    HWND window;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_ex_device(d3d9ex, window);
    if (!device)
        goto done_window;

    allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 128 * 128 * 4);
    CHECK_TRUE(allocation != NULL);
    if (!allocation)
        goto done_device;

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateTexture(device, 128, 128, 0, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &texture, &mem);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(texture == NULL);

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateTexture(device, 128, 128, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &texture, &mem);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(texture == NULL);

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateIndexBuffer(device, 16, 0, D3DFMT_INDEX32,
            D3DPOOL_SYSTEMMEM, &index_buffer, &mem);
    CHECK_HR(hr, D3DERR_NOTAVAILABLE);
    CHECK_TRUE(index_buffer == NULL);

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateVertexBuffer(device, 16, 0, 0,
            D3DPOOL_SYSTEMMEM, &vertex_buffer, &mem);
    CHECK_HR(hr, D3DERR_NOTAVAILABLE);
    CHECK_TRUE(vertex_buffer == NULL);

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, 128, 128,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, &mem);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(surface != NULL);
    if (surface)
    {
        IDirect3DSurface9_Release(surface);
        surface = NULL;
    }

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, 128, 128,
            D3DFMT_A8R8G8B8, D3DPOOL_SCRATCH, &surface, &mem);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(surface == NULL);

    HeapFree(GetProcessHeap(), 0, allocation);

done_device:
    IDirect3DDevice9Ex_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_user_memory_getdc()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_user_memory_getdc_dib_identity(const struct d3d9_api *api)
{
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9Ex *device = NULL;
    DIBSECTION dib;
    HBITMAP bitmap;
    IDirect3D9Ex *d3d9ex;
    unsigned int *data;
    void *mem;
    HWND window;
    HDC dc;
    HRESULT hr;
    int size;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_ex_device(d3d9ex, window);
    if (!device)
        goto done_window;

    data = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY,
            16 * 16 * sizeof(*data));
    CHECK_TRUE(data != NULL);
    if (!data)
        goto done_device;

    mem = data;
    hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, 16, 16,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, &mem);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_allocation;

    dc = NULL;
    hr = IDirect3DSurface9_GetDC(surface, &dc);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        bitmap = GetCurrentObject(dc, OBJ_BITMAP);
        CHECK_TRUE(bitmap != NULL);
        if (bitmap)
        {
            memset(&dib, 0, sizeof(dib));
            size = GetObjectA(bitmap, sizeof(dib), &dib);
            CHECK_TRUE(size == sizeof(dib));
            if (size == sizeof(dib))
                CHECK_TRUE(dib.dsBm.bmBits == data);
        }

        CHECK_TRUE(BitBlt(dc, 0, 0, 16, 8, NULL, 0, 0, WHITENESS));
        CHECK_TRUE(BitBlt(dc, 0, 8, 16, 8, NULL, 0, 0, BLACKNESS));
        CHECK_HR(IDirect3DSurface9_ReleaseDC(surface, dc), D3D_OK);
        CHECK_TRUE(data[0] == 0xffffffffu);
        CHECK_TRUE(data[8 * 16] == 0x00000000u);
    }

    IDirect3DSurface9_Release(surface);

done_allocation:
    HeapFree(GetProcessHeap(), 0, data);
done_device:
    IDirect3DDevice9Ex_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_user_memory()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_user_memory_lock_identity(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9Ex *device = NULL;
    D3DLOCKED_RECT locked;
    IDirect3D9Ex *d3d9ex;
    void *allocation;
    void *mem;
    HWND window;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_ex_device(d3d9ex, window);
    if (!device)
        goto done_window;

    allocation = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, 128 * 128 * 4);
    CHECK_TRUE(allocation != NULL);
    if (!allocation)
        goto done_device;

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateTexture(device, 128, 128, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &texture, &mem);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        hr = IDirect3DTexture9_LockRect(texture, 0, &locked, NULL, 0);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(locked.Pitch == 128 * 4);
            CHECK_TRUE(locked.pBits == allocation);
            CHECK_HR(IDirect3DTexture9_UnlockRect(texture, 0), D3D_OK);
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    mem = allocation;
    hr = IDirect3DDevice9Ex_CreateOffscreenPlainSurface(device, 128, 128,
            D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &surface, &mem);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        hr = IDirect3DSurface9_LockRect(surface, &locked, NULL, 0);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(locked.Pitch == 128 * 4);
            CHECK_TRUE(locked.pBits == allocation);
            CHECK_HR(IDirect3DSurface9_UnlockRect(surface), D3D_OK);
        }
        IDirect3DSurface9_Release(surface);
        surface = NULL;
    }

    HeapFree(GetProcessHeap(), 0, allocation);

done_device:
    IDirect3DDevice9Ex_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_format_unknown()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_creation_failure_out_pointers(const struct d3d9_api *api)
{
    IDirect3DVolumeTexture9 *volume_texture = (IDirect3DVolumeTexture9 *)0xdeadbeef;
    IDirect3DCubeTexture9 *cube_texture = (IDirect3DCubeTexture9 *)0xdeadbeef;
    IDirect3DTexture9 *texture = (IDirect3DTexture9 *)0xdeadbeef;
    IDirect3DSurface9 *surface = (IDirect3DSurface9 *)0xdeadbeef;
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

    hr = IDirect3DDevice9_CreateRenderTarget(device, 64, 64,
            D3DFMT_UNKNOWN, D3DMULTISAMPLE_NONE, 0, FALSE, &surface, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(surface == NULL);

    surface = (IDirect3DSurface9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 64, 64,
            D3DFMT_UNKNOWN, D3DMULTISAMPLE_NONE, 0, TRUE, &surface, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(surface == NULL);

    surface = (IDirect3DSurface9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateOffscreenPlainSurface(device, 64, 64,
            D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, &surface, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(surface == NULL);

    hr = IDirect3DDevice9_CreateTexture(device, 64, 64, 1, 0,
            D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, &texture, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(texture == NULL);

    hr = IDirect3DDevice9_CreateCubeTexture(device, 64, 1, 0,
            D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, &cube_texture, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(cube_texture == NULL);

    hr = IDirect3DDevice9_CreateVolumeTexture(device, 64, 64, 1, 1, 0,
            D3DFMT_UNKNOWN, D3DPOOL_DEFAULT, &volume_texture, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(volume_texture == NULL);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
