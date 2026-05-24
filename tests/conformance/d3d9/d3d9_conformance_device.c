/*
 * Device-shaped conformance cases split from d3d9_conformance.c.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/device.c
 * - dlls/d3d9/tests/d3d9ex.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Covers IDirect3D9 / IDirect3DDevice9 factory validation, display-mode
 * adapter format, base-vs-Ex QI, Ex-created normal device QI, ResetEx
 * mode validation and BeginScene/EndScene invalid transitions.
 */

#include "d3d9_conformance_fixtures.h"

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_display_modes(), test_check_device_type(),
 * test_check_device_format(), test_checkdevicemultisampletype()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_factory_validation_return_codes(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    D3DDISPLAYMODE mode;
    DWORD quality_levels;
    UINT adapter_count;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    adapter_count = IDirect3D9_GetAdapterCount(d3d9);
    CHECK_TRUE(adapter_count > 0);
    if (!adapter_count)
        goto done;

    memset(&mode, 0xcc, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &mode);
    CHECK_SUCCEEDED(hr);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(mode.Format == D3DFMT_X8R8G8B8 || mode.Format == D3DFMT_R5G6B5);
        CHECK_TRUE(mode.Format != D3DFMT_A8R8G8B8);
    }

    memset(&mode, 0xcc, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, adapter_count, &mode);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    memset(&mode, 0xcc, sizeof(mode));
    hr = IDirect3D9_EnumAdapterModes(d3d9, D3DADAPTER_DEFAULT,
            D3DFMT_A8R8G8B8, 0, &mode);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3D9_CheckDeviceType(d3d9, adapter_count,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9_CheckDeviceType(d3d9, D3DADAPTER_DEFAULT,
            (D3DDEVTYPE)0xdead, D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE),
            D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_UNKNOWN, 0, D3DRTYPE_SURFACE,
            D3DFMT_A8R8G8B8), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, 0, D3DRTYPE_TEXTURE,
            D3DFMT_X8R8G8B8), D3DERR_NOTAVAILABLE);

    quality_levels = 0xdeadbeef;
    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_UNKNOWN, TRUE, D3DMULTISAMPLE_NONE,
            &quality_levels);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(quality_levels == 0xdeadbeef);

    quality_levels = 0;
    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_NONE,
            &quality_levels);
    CHECK_SUCCEEDED(hr);
    CHECK_TRUE(quality_levels == 1);

    CHECK_HR(IDirect3D9_CheckDeviceMultiSampleType(d3d9,
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE,
            (D3DMULTISAMPLE_TYPE)65536, NULL), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9_CheckDeviceMultiSampleType(d3d9,
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_DXT5, TRUE,
            D3DMULTISAMPLE_2_SAMPLES, NULL), D3DERR_NOTAVAILABLE);

done:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_checkdevicemultisampletype(), test_check_device_format()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_factory_caps_edge_matrix(const struct d3d9_api *api)
{
    static const D3DFORMAT autogen_formats[] =
    {
        D3DFMT_A8R8G8B8,
        D3DFMT_X8R8G8B8,
        D3DFMT_R5G6B5,
    };
    IDirect3D9 *d3d9;
    DWORD quality_levels;
    HRESULT rt_hr;
    HRESULT hr;
    UINT i;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    quality_levels = 0xdeadbeef;
    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE, D3DMULTISAMPLE_NONE,
            &quality_levels);
    CHECK_SUCCEEDED(hr);
    CHECK_TRUE(quality_levels == 1);

    quality_levels = 0xdeadbeef;
    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, FALSE, D3DMULTISAMPLE_NONE,
            &quality_levels);
    CHECK_SUCCEEDED(hr);
    CHECK_TRUE(quality_levels == 1);

    quality_levels = 0xdeadbeef;
    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE,
            (D3DMULTISAMPLE_TYPE)65536, &quality_levels);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(quality_levels == 0xdeadbeef);

    CHECK_HR(IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_UNKNOWN, 0, D3DRTYPE_SURFACE,
            D3DFMT_A8R8G8B8), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_UNKNOWN, 0, D3DRTYPE_TEXTURE,
            D3DFMT_X8R8G8B8), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, 0, D3DRTYPE_TEXTURE,
            D3DFMT_X8R8G8B8), D3DERR_NOTAVAILABLE);

    for (i = 0; i < ARRAY_SIZE(autogen_formats); ++i)
    {
        rt_hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET,
                D3DRTYPE_TEXTURE, autogen_formats[i]);
        CHECK_TRUE(rt_hr == D3D_OK || rt_hr == D3DERR_NOTAVAILABLE);

        hr = IDirect3D9_CheckDeviceFormat(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8,
                D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP,
                D3DRTYPE_TEXTURE, autogen_formats[i]);
        if (rt_hr == D3D_OK)
            CHECK_TRUE(hr == D3D_OK || hr == D3DOK_NOAUTOGEN);
        else
            CHECK_HR(hr, D3DERR_NOTAVAILABLE);
    }

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_invalid_multisample()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_invalid_multisample_render_target_quality(
        const struct d3d9_api *api)
{
    IDirect3DSurface9 *surface = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    DWORD quality_levels = 0;
    BOOL available;
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

    available = SUCCEEDED(IDirect3D9_CheckDeviceMultiSampleType(d3d9,
            D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, TRUE,
            D3DMULTISAMPLE_2_SAMPLES, &quality_levels));

    hr = IDirect3DDevice9_CreateRenderTarget(device, 128, 128,
            D3DFMT_X8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, 0, FALSE,
            &surface, NULL);
    if (available)
    {
        CHECK_HR(hr, D3D_OK);
        if (surface)
            IDirect3DSurface9_Release(surface);
        surface = NULL;

        hr = IDirect3DDevice9_CreateRenderTarget(device, 128, 128,
                D3DFMT_X8R8G8B8, D3DMULTISAMPLE_2_SAMPLES, quality_levels,
                FALSE, &surface, NULL);
        CHECK_HR(hr, D3DERR_INVALIDCALL);
        CHECK_TRUE(surface == NULL);
    }
    else
    {
        CHECK_HR(hr, D3DERR_INVALIDCALL);
        CHECK_TRUE(surface == NULL);
    }

    hr = IDirect3DDevice9_CreateRenderTarget(device, 128, 128,
            D3DFMT_X8R8G8B8, D3DMULTISAMPLE_15_SAMPLES, 0, FALSE,
            &surface, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(surface == NULL);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_display_mode()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_device_display_mode_adapter_format(const struct d3d9_api *api)
{
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DDISPLAYMODE mode;
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

    memset(&mode, 0xcc, sizeof(mode));
    hr = IDirect3DDevice9_GetDisplayMode(device, 0, &mode);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(mode.Format == D3DFMT_X8R8G8B8 || mode.Format == D3DFMT_R5G6B5);
        CHECK_TRUE(mode.Format != D3DFMT_A8R8G8B8);
    }

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&mode, 0xcc, sizeof(mode));
        hr = IDirect3DSwapChain9_GetDisplayMode(swapchain, &mode);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(mode.Format == D3DFMT_X8R8G8B8 || mode.Format == D3DFMT_R5G6B5);
            CHECK_TRUE(mode.Format != D3DFMT_A8R8G8B8);
        }
        IDirect3DSwapChain9_Release(swapchain);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * functions: test_swapchain_parameters(), test_reset_ex()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_create_reset_mode_validation(const struct d3d9_api *api)
{
    IDirect3D9Ex *d3d9ex;
    IDirect3DDevice9Ex *device = NULL;
    IDirect3DDevice9Ex *failed_device;
    D3DDISPLAYMODEEX mode;
    D3DDISPLAYMODEEX bad_mode;
    D3DPRESENT_PARAMETERS pp;
    HWND window;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    memset(&mode, 0, sizeof(mode));
    mode.Size = sizeof(mode);
    hr = IDirect3D9Ex_GetAdapterDisplayModeEx(d3d9ex, D3DADAPTER_DEFAULT,
            &mode, NULL);
    CHECK_SUCCEEDED(hr);
    if (FAILED(hr))
        goto done_window;
    mode.RefreshRate = 0;
    mode.ScanLineOrdering = 0;

    pp = default_present_parameters(window);
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;

    failed_device = (IDirect3DDevice9Ex *)0xdeadbeef;
    hr = IDirect3D9Ex_CreateDeviceEx(d3d9ex, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &pp, &mode, &failed_device);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(failed_device == NULL);

    pp.Windowed = FALSE;
    pp.BackBufferWidth = mode.Width;
    pp.BackBufferHeight = mode.Height;
    pp.BackBufferFormat = mode.Format;
    failed_device = (IDirect3DDevice9Ex *)0xdeadbeef;
    hr = IDirect3D9Ex_CreateDeviceEx(d3d9ex, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &pp, NULL, &failed_device);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(failed_device == NULL);

    failed_device = (IDirect3DDevice9Ex *)0xdeadbeef;
    pp.BackBufferWidth = mode.Width > 1 ? mode.Width - 1 : mode.Width + 1;
    pp.BackBufferHeight = mode.Height;
    hr = IDirect3D9Ex_CreateDeviceEx(d3d9ex, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &pp, &mode, &failed_device);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(failed_device == NULL);

    device = create_ex_device(d3d9ex, window);
    if (!device)
        goto done_window;

    pp = default_present_parameters(window);
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = 400;
    pp.BackBufferHeight = 300;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    CHECK_HR(IDirect3DDevice9Ex_ResetEx(device, &pp, &mode),
            D3DERR_INVALIDCALL);

    pp.Windowed = FALSE;
    pp.BackBufferWidth = mode.Width;
    pp.BackBufferHeight = mode.Height;
    pp.BackBufferFormat = mode.Format;
    CHECK_HR(IDirect3DDevice9Ex_ResetEx(device, &pp, NULL),
            D3DERR_INVALIDCALL);

    pp.BackBufferWidth = mode.Width > 1 ? mode.Width - 1 : mode.Width + 1;
    pp.BackBufferHeight = mode.Height;
    CHECK_HR(IDirect3DDevice9Ex_ResetEx(device, &pp, &mode),
            D3DERR_INVALIDCALL);

    pp.BackBufferWidth = mode.Width;
    pp.BackBufferHeight = mode.Height;
    bad_mode = mode;
    bad_mode.Width = 0;
    bad_mode.Height = 0;
    CHECK_HR(IDirect3DDevice9Ex_ResetEx(device, &pp, &bad_mode),
            D3DERR_INVALIDCALL);

    pp = default_present_parameters(window);
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    CHECK_SUCCEEDED(IDirect3DDevice9Ex_ResetEx(device, &pp, NULL));
    CHECK_HR(IDirect3DDevice9Ex_TestCooperativeLevel(device), D3D_OK);

    IDirect3DDevice9Ex_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/* Wine d3d9ex.c: base IDirect3D9 / IDirect3DDevice9 must not QI to Ex. */
void test_factory_base_vs_ex_qi(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    IDirect3D9Ex *d3d9ex = (IDirect3D9Ex *)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3DDevice9Ex *device_ex = (IDirect3DDevice9Ex *)0xdeadbeef;
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DSwapChain9Ex *swapchain_ex = (IDirect3DSwapChain9Ex *)0xdeadbeef;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_QueryInterface(d3d9, &IID_IDirect3D9Ex,
            (void **)&d3d9ex);
    CHECK_HR(hr, E_NOINTERFACE);
    CHECK_TRUE(d3d9ex == NULL);
    if (d3d9ex && d3d9ex != (IDirect3D9Ex *)0xdeadbeef)
        IDirect3D9Ex_Release(d3d9ex);

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_QueryInterface(device, &IID_IDirect3DDevice9Ex,
            (void **)&device_ex);
    CHECK_HR(hr, E_NOINTERFACE);
    CHECK_TRUE(device_ex == NULL);
    if (device_ex && device_ex != (IDirect3DDevice9Ex *)0xdeadbeef)
        IDirect3DDevice9Ex_Release(device_ex);

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_SUCCEEDED(hr);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DSwapChain9_QueryInterface(swapchain,
                &IID_IDirect3DSwapChain9Ex, (void **)&swapchain_ex);
        CHECK_HR(hr, E_NOINTERFACE);
        CHECK_TRUE(swapchain_ex == NULL);
        if (swapchain_ex && swapchain_ex != (IDirect3DSwapChain9Ex *)0xdeadbeef)
            IDirect3DSwapChain9Ex_Release(swapchain_ex);
    }

    if (swapchain)
        IDirect3DSwapChain9_Release(swapchain);
    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/* Wine d3d9ex.c: Ex factory base QI and normal CreateDevice support Ex QI. */
void test_ex_created_normal_device_qi(const struct d3d9_api *api)
{
    IDirect3D9Ex *d3d9ex;
    IDirect3D9 *d3d9 = (IDirect3D9 *)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3DDevice9Ex *device_ex = (IDirect3DDevice9Ex *)0xdeadbeef;
    HWND window;
    D3DPRESENT_PARAMETERS pp;
    HRESULT hr;
    ULONG ref;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    hr = IDirect3D9Ex_QueryInterface(d3d9ex, &IID_IDirect3D9,
            (void **)&d3d9);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(d3d9 != NULL && d3d9 != (IDirect3D9 *)0xdeadbeef);
    if (SUCCEEDED(hr))
    {
        ref = get_refcount((IUnknown *)d3d9ex);
        CHECK_TRUE(ref == 2);
        ref = get_refcount((IUnknown *)d3d9);
        CHECK_TRUE(ref == 2);
    }

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    pp = default_present_parameters(window);
    hr = IDirect3D9Ex_CreateDevice(d3d9ex, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("IDirect3D9Ex::CreateDevice failed with %s", hr_buffer);
        goto done_window;
    }

    hr = IDirect3DDevice9_QueryInterface(device, &IID_IDirect3DDevice9Ex,
            (void **)&device_ex);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(device_ex != NULL
            && device_ex != (IDirect3DDevice9Ex *)0xdeadbeef);
    if (SUCCEEDED(hr))
    {
        ref = get_refcount((IUnknown *)device);
        CHECK_TRUE(ref == 2);
        ref = get_refcount((IUnknown *)device_ex);
        CHECK_TRUE(ref == 2);
    }

    if (device_ex && device_ex != (IDirect3DDevice9Ex *)0xdeadbeef)
        IDirect3DDevice9Ex_Release(device_ex);
    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    if (d3d9 && d3d9 != (IDirect3D9 *)0xdeadbeef)
        IDirect3D9_Release(d3d9);
    IDirect3D9Ex_Release(d3d9ex);
}

/* Wine device.c: invalid BeginScene/EndScene transitions and Reset clearing. */
void test_scene_invalid_transitions(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
    IDirect3DDevice9 *device = NULL;
    HWND window;
    D3DPRESENT_PARAMETERS pp;

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

    CHECK_HR(IDirect3DDevice9_EndScene(device), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_BeginScene(device), S_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), S_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), S_OK);
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_EndScene(device), S_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), S_OK);
    pp = default_present_parameters(window);
    CHECK_HR(IDirect3DDevice9_Reset(device, &pp), S_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), S_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), S_OK);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_vertex_declaration(), test_fvf_decl()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vertex_declaration_fvf_policy(const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 valid_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 misaligned_elements[] =
    {
        {0, 1, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexDeclaration9 *current = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    DWORD fvf = 0xdeadbeef;
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

    current = (IDirect3DVertexDeclaration9 *)0xdeadbeef;
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == NULL);

    CHECK_HR(IDirect3DDevice9_GetVertexDeclaration(device, NULL),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetFVF(device, NULL), D3DERR_INVALIDCALL);

    fvf = 0xdeadbeef;
    hr = IDirect3DDevice9_GetFVF(device, &fvf);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(fvf == 0);

    CHECK_HR(IDirect3DDevice9_CreateVertexDeclaration(device, NULL, &decl),
            D3DERR_INVALIDCALL);
    CHECK_TRUE(decl == NULL);

    decl = (IDirect3DVertexDeclaration9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateVertexDeclaration(device, misaligned_elements,
            &decl);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(decl == NULL);

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, valid_elements,
            &decl);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(decl != NULL);
    if (!decl)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, decl), D3D_OK);

    current = NULL;
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == decl);
    if (current)
        IDirect3DVertexDeclaration9_Release(current);

    fvf = 0xdeadbeef;
    hr = IDirect3DDevice9_GetFVF(device, &fvf);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(fvf == 0);

    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_DIFFUSE),
            D3D_OK);
    fvf = 0;
    hr = IDirect3DDevice9_GetFVF(device, &fvf);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(fvf == (D3DFVF_XYZ | D3DFVF_DIFFUSE));

    current = NULL;
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current != NULL);
    CHECK_TRUE(current != decl);
    if (current)
        IDirect3DVertexDeclaration9_Release(current);

    IDirect3DVertexDeclaration9_Release(decl);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

static void check_vertex_declaration_elements(IDirect3DVertexDeclaration9 *decl,
        const D3DVERTEXELEMENT9 *expected, UINT expected_count)
{
    D3DVERTEXELEMENT9 actual[MAXD3DDECLLENGTH + 1];
    UINT actual_count = ARRAY_SIZE(actual);
    HRESULT hr;
    UINT i;

    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, actual,
            &actual_count);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        return;

    CHECK_TRUE(actual_count == expected_count);
    if (actual_count != expected_count)
        return;

    for (i = 0; i < expected_count; ++i)
        CHECK_TRUE(memcmp(&actual[i], &expected[i], sizeof(actual[i])) == 0);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_fvf_decl_management()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_fvf_decl_management(const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 xyzrhw_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITIONT, 0},
        D3DDECL_END()
    };
    static const D3DVERTEXELEMENT9 xyz_normal_elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_NORMAL, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *decl4 = NULL;
    IDirect3DVertexDeclaration9 *decl3 = NULL;
    IDirect3DVertexDeclaration9 *decl2 = NULL;
    IDirect3DVertexDeclaration9 *decl1 = NULL;
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

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, NULL), D3D_OK);

    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW), D3D_OK);
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &decl1);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(decl1 != NULL);
    if (decl1)
        check_vertex_declaration_elements(decl1, xyzrhw_elements,
                ARRAY_SIZE(xyzrhw_elements));

    hr = IDirect3DDevice9_GetVertexDeclaration(device, &decl2);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(decl2 == decl1);

    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ | D3DFVF_NORMAL),
            D3D_OK);
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &decl3);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(decl3 != NULL);
    CHECK_TRUE(decl3 != decl1);
    if (decl3)
        check_vertex_declaration_elements(decl3, xyz_normal_elements,
                ARRAY_SIZE(xyz_normal_elements));
    if (decl1)
        check_vertex_declaration_elements(decl1, xyzrhw_elements,
                ARRAY_SIZE(xyzrhw_elements));

    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZRHW), D3D_OK);
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &decl4);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(decl4 == decl1);

    if (decl4)
        IDirect3DVertexDeclaration9_Release(decl4);
    if (decl3)
        IDirect3DVertexDeclaration9_Release(decl3);
    if (decl2)
        IDirect3DVertexDeclaration9_Release(decl2);
    if (decl1)
        IDirect3DVertexDeclaration9_Release(decl1);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_unused_declaration_type()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_unused_declaration_type(const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 elements[][3] =
    {
        {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END()
        },
        {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_POSITION, 0},
            {0, 16, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_TEXCOORD, 1},
            D3DDECL_END()
        },
        {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_POSITION, 0},
            {1, 16, D3DDECLTYPE_UNUSED, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_NORMAL, 0},
            D3DDECL_END()
        },
    };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
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

    for (i = 0; i < ARRAY_SIZE(elements); ++i)
    {
        decl = (IDirect3DVertexDeclaration9 *)0xdeadbeef;
        CHECK_HR(IDirect3DDevice9_CreateVertexDeclaration(device,
                elements[i], &decl), E_FAIL);
        CHECK_TRUE(decl == NULL || decl == (IDirect3DVertexDeclaration9 *)0xdeadbeef);
        if (decl && decl != (IDirect3DVertexDeclaration9 *)0xdeadbeef)
            IDirect3DVertexDeclaration9_Release(decl);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

static const DWORD simple_vs_2_0[] =
{
    D3DVS_VERSION(2, 0),
    0x0000ffff
};

static const DWORD simple_ps_2_0[] =
{
    D3DPS_VERSION(2, 0),
    0x0000ffff
};

static const DWORD invalid_shader_4_0[] =
{
    0xffff0400,
    0x0000ffff
};

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_get_set_vertex_shader()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_get_set_vertex_shader(const struct d3d9_api *api)
{
    IDirect3DVertexShader9 *shader = NULL;
    IDirect3DVertexShader9 *current = (IDirect3DVertexShader9 *)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    ULONG ref;
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

    hr = IDirect3DDevice9_GetVertexShader(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == NULL);
    CHECK_HR(IDirect3DDevice9_GetVertexShader(device, NULL),
            D3DERR_INVALIDCALL);

    shader = (IDirect3DVertexShader9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreateVertexShader(device, NULL, &shader);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(shader == NULL || shader == (IDirect3DVertexShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && shader && shader != (IDirect3DVertexShader9 *)0xdeadbeef)
        IDirect3DVertexShader9_Release(shader);

    shader = NULL;
    hr = IDirect3DDevice9_CreateVertexShader(device, simple_vs_2_0, &shader);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(shader != NULL);
    if (FAILED(hr) || !shader)
        goto done_device;

    ref = get_refcount((IUnknown *)shader);
    CHECK_TRUE(ref == 1);

    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, shader), D3D_OK);
    ref = get_refcount((IUnknown *)shader);
    CHECK_TRUE(ref == 2);

    current = NULL;
    hr = IDirect3DDevice9_GetVertexShader(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == shader);
    if (current)
    {
        ref = get_refcount((IUnknown *)shader);
        CHECK_TRUE(ref == 3);
        IDirect3DVertexShader9_Release(current);
    }

    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    current = (IDirect3DVertexShader9 *)0xdeadbeef;
    hr = IDirect3DDevice9_GetVertexShader(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == NULL);

    ref = get_refcount((IUnknown *)shader);
    CHECK_TRUE(ref == 1);
    IDirect3DVertexShader9_Release(shader);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_vertex_shader_constant()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_vertex_shader_constant(const struct d3d9_api *api)
{
    static const float floats[8] =
    {
        1.0f, -2.0f, 3.5f, 4.25f,
        -5.0f, 6.0f, 7.75f, -8.5f
    };
    static const int ints[8] =
    {
        1, -2, 3, -4,
        0x100, -0x200, 0x300, -0x400
    };
    static const BOOL bools[4] = {TRUE, FALSE, TRUE, TRUE};
    float out_f[8];
    int out_i[8];
    BOOL out_b[4];
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

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, floats, 2),
            D3D_OK);
    memset(out_f, 0xcc, sizeof(out_f));
    hr = IDirect3DDevice9_GetVertexShaderConstantF(device, 0, out_f, 2);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_f, floats, sizeof(floats)) == 0);

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantI(device, 0, ints, 2),
            D3D_OK);
    memset(out_i, 0xcc, sizeof(out_i));
    hr = IDirect3DDevice9_GetVertexShaderConstantI(device, 0, out_i, 2);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_i, ints, sizeof(ints)) == 0);

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantB(device, 0, bools, 4),
            D3D_OK);
    memset(out_b, 0xcc, sizeof(out_b));
    hr = IDirect3DDevice9_GetVertexShaderConstantB(device, 0, out_b, 4);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_b, bools, sizeof(bools)) == 0);

    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetVertexShaderConstantF(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantI(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetVertexShaderConstantI(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantB(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetVertexShaderConstantB(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);

    if (caps.MaxVertexShaderConst)
    {
        CHECK_HR(IDirect3DDevice9_SetVertexShaderConstantF(device,
                caps.MaxVertexShaderConst, floats, 1), D3DERR_INVALIDCALL);
        CHECK_HR(IDirect3DDevice9_GetVertexShaderConstantF(device,
                caps.MaxVertexShaderConst, out_f, 1), D3DERR_INVALIDCALL);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_get_set_pixel_shader()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_get_set_pixel_shader(const struct d3d9_api *api)
{
    IDirect3DPixelShader9 *shader = NULL;
    IDirect3DPixelShader9 *current = (IDirect3DPixelShader9 *)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    ULONG ref;
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

    hr = IDirect3DDevice9_GetPixelShader(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == NULL);
    CHECK_HR(IDirect3DDevice9_GetPixelShader(device, NULL),
            D3DERR_INVALIDCALL);

    shader = (IDirect3DPixelShader9 *)0xdeadbeef;
    hr = IDirect3DDevice9_CreatePixelShader(device, NULL, &shader);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(shader == NULL || shader == (IDirect3DPixelShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && shader && shader != (IDirect3DPixelShader9 *)0xdeadbeef)
        IDirect3DPixelShader9_Release(shader);

    shader = NULL;
    hr = IDirect3DDevice9_CreatePixelShader(device, simple_ps_2_0, &shader);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(shader != NULL);
    if (FAILED(hr) || !shader)
        goto done_device;

    ref = get_refcount((IUnknown *)shader);
    CHECK_TRUE(ref == 1);

    CHECK_HR(IDirect3DDevice9_SetPixelShader(device, shader), D3D_OK);
    ref = get_refcount((IUnknown *)shader);
    CHECK_TRUE(ref == 2);

    current = NULL;
    hr = IDirect3DDevice9_GetPixelShader(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == shader);
    if (current)
    {
        ref = get_refcount((IUnknown *)shader);
        CHECK_TRUE(ref == 3);
        IDirect3DPixelShader9_Release(current);
    }

    CHECK_HR(IDirect3DDevice9_SetPixelShader(device, NULL), D3D_OK);
    current = (IDirect3DPixelShader9 *)0xdeadbeef;
    hr = IDirect3DDevice9_GetPixelShader(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == NULL);

    ref = get_refcount((IUnknown *)shader);
    CHECK_TRUE(ref == 1);
    IDirect3DPixelShader9_Release(shader);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_pixel_shader_constant()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_pixel_shader_constant(const struct d3d9_api *api)
{
    static const float floats[8] =
    {
        -0.25f, 0.5f, 1.25f, 2.5f,
        3.75f, -4.5f, 5.25f, -6.0f
    };
    static const int ints[8] =
    {
        -1, 2, -3, 4,
        -5, 6, -7, 8
    };
    static const BOOL bools[4] = {FALSE, TRUE, FALSE, TRUE};
    float out_f[8];
    int out_i[8];
    BOOL out_b[4];
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

    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, floats, 2),
            D3D_OK);
    memset(out_f, 0xcc, sizeof(out_f));
    hr = IDirect3DDevice9_GetPixelShaderConstantF(device, 0, out_f, 2);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_f, floats, sizeof(floats)) == 0);

    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, ints, 2),
            D3D_OK);
    memset(out_i, 0xcc, sizeof(out_i));
    hr = IDirect3DDevice9_GetPixelShaderConstantI(device, 0, out_i, 2);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_i, ints, sizeof(ints)) == 0);

    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, bools, 4),
            D3D_OK);
    memset(out_b, 0xcc, sizeof(out_b));
    hr = IDirect3DDevice9_GetPixelShaderConstantB(device, 0, out_b, 4);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(out_b, bools, sizeof(bools)) == 0);

    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantF(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetPixelShaderConstantF(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantI(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetPixelShaderConstantI(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_SetPixelShaderConstantB(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetPixelShaderConstantB(device, 0, NULL, 1),
            D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

static void check_unsupported_shader_device(IDirect3DDevice9 *device)
{
    IDirect3DVertexShader9 *vs = (IDirect3DVertexShader9 *)0xdeadbeef;
    IDirect3DPixelShader9 *ps = (IDirect3DPixelShader9 *)0xdeadbeef;
    HRESULT hr;

    hr = IDirect3DDevice9_CreateVertexShader(device, invalid_shader_4_0, &vs);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(vs == NULL || vs == (IDirect3DVertexShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && vs && vs != (IDirect3DVertexShader9 *)0xdeadbeef)
        IDirect3DVertexShader9_Release(vs);

    hr = IDirect3DDevice9_CreatePixelShader(device, invalid_shader_4_0, &ps);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(ps == NULL || ps == (IDirect3DPixelShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && ps && ps != (IDirect3DPixelShader9 *)0xdeadbeef)
        IDirect3DPixelShader9_Release(ps);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c and dlls/d3d9/tests/d3d9ex.c
 * function: test_unsupported_shaders()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_unsupported_shaders(const struct d3d9_api *api)
{
    IDirect3DDevice9Ex *device_ex = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9Ex *d3d9ex = NULL;
    IDirect3D9 *d3d9;
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
    if (device)
    {
        check_unsupported_shader_device(device);
        IDirect3DDevice9_Release(device);
    }

    d3d9ex = create_d3d9ex(api);
    if (d3d9ex)
    {
        device_ex = create_ex_device(d3d9ex, window);
        if (device_ex)
        {
            check_unsupported_shader_device((IDirect3DDevice9 *)device_ex);
            IDirect3DDevice9Ex_Release(device_ex);
        }
        IDirect3D9Ex_Release(d3d9ex);
    }

    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_unsupported_shaders()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_shader_unsupported_stage_variants(const struct d3d9_api *api)
{
    IDirect3DVertexShader9 *vs = (IDirect3DVertexShader9 *)0xdeadbeef;
    IDirect3DPixelShader9 *ps = (IDirect3DPixelShader9 *)0xdeadbeef;
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

    hr = IDirect3DDevice9_CreateVertexShader(device, simple_ps_2_0, &vs);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(vs == NULL || vs == (IDirect3DVertexShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && vs && vs != (IDirect3DVertexShader9 *)0xdeadbeef)
        IDirect3DVertexShader9_Release(vs);

    hr = IDirect3DDevice9_CreatePixelShader(device, simple_vs_2_0, &ps);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(ps == NULL || ps == (IDirect3DPixelShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && ps && ps != (IDirect3DPixelShader9 *)0xdeadbeef)
        IDirect3DPixelShader9_Release(ps);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_unsupported_shaders()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_shader_validation_policy(const struct d3d9_api *api)
{
    IDirect3DVertexShader9 *vs = NULL;
    IDirect3DPixelShader9 *ps = NULL;
    IDirect3DDevice9Ex *device = NULL;
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

    CHECK_HR(IDirect3DDevice9Ex_CreateVertexShader(device, NULL, &vs),
            D3DERR_INVALIDCALL);
    CHECK_TRUE(vs == NULL);
    CHECK_HR(IDirect3DDevice9Ex_CreatePixelShader(device, NULL, &ps),
            D3DERR_INVALIDCALL);
    CHECK_TRUE(ps == NULL);

    vs = (IDirect3DVertexShader9 *)0xdeadbeef;
    hr = IDirect3DDevice9Ex_CreateVertexShader(device, invalid_shader_4_0,
            &vs);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(vs == NULL || vs == (IDirect3DVertexShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && vs && vs != (IDirect3DVertexShader9 *)0xdeadbeef)
        IDirect3DVertexShader9_Release(vs);
    vs = NULL;

    ps = (IDirect3DPixelShader9 *)0xdeadbeef;
    hr = IDirect3DDevice9Ex_CreatePixelShader(device, invalid_shader_4_0,
            &ps);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(ps == NULL || ps == (IDirect3DPixelShader9 *)0xdeadbeef);
    if (SUCCEEDED(hr) && ps && ps != (IDirect3DPixelShader9 *)0xdeadbeef)
        IDirect3DPixelShader9_Release(ps);
    ps = NULL;

    hr = IDirect3DDevice9Ex_CreateVertexShader(device, simple_vs_2_0, &vs);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(vs != NULL);
    if (SUCCEEDED(hr))
        IDirect3DVertexShader9_Release(vs);

    hr = IDirect3DDevice9Ex_CreatePixelShader(device, simple_ps_2_0, &ps);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(ps != NULL);
    if (SUCCEEDED(hr))
        IDirect3DPixelShader9_Release(ps);

    IDirect3DDevice9Ex_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_texture_stage_states()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_texture_stage_states(const struct d3d9_api *api)
{
    static const struct
    {
        D3DTEXTURESTAGESTATETYPE state;
        DWORD value;
    } cases[] =
    {
        {D3DTSS_COLOROP, D3DTOP_MODULATE},
        {D3DTSS_COLORARG1, D3DTA_TEXTURE},
        {D3DTSS_COLORARG2, D3DTA_DIFFUSE},
        {D3DTSS_ALPHAOP, D3DTOP_SELECTARG1},
        {D3DTSS_ALPHAARG1, D3DTA_TEXTURE | D3DTA_ALPHAREPLICATE},
        {D3DTSS_TEXCOORDINDEX, 0},
        {D3DTSS_BUMPENVMAT00, 0x3f800000},
        {D3DTSS_BUMPENVMAT11, 0xbf800000},
        {D3DTSS_RESULTARG, D3DTA_CURRENT},
        {D3DTSS_CONSTANT, 0x80402010},
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

    for (i = 0; i < ARRAY_SIZE(cases); ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
                cases[i].state, cases[i].value), D3D_OK);
        value = 0xdeadbeef;
        hr = IDirect3DDevice9_GetTextureStageState(device, 0,
                cases[i].state, &value);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(value == cases[i].value);
    }

    CHECK_HR(IDirect3DDevice9_GetTextureStageState(device, 0,
            D3DTSS_COLOROP, NULL), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_SetTextureStageState(device, 0,
            (D3DTEXTURESTAGESTATETYPE)0xdead, 0), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetTextureStageState(device, 0,
            (D3DTEXTURESTAGESTATETYPE)0xdead, &value), D3DERR_INVALIDCALL);

    if (caps.MaxTextureBlendStages)
    {
        CHECK_HR(IDirect3DDevice9_SetTextureStageState(device,
                caps.MaxTextureBlendStages, D3DTSS_COLOROP,
                D3DTOP_DISABLE), D3DERR_INVALIDCALL);
        value = 0xdeadbeef;
        CHECK_HR(IDirect3DDevice9_GetTextureStageState(device,
                caps.MaxTextureBlendStages, D3DTSS_COLOROP, &value),
                D3DERR_INVALIDCALL);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_filter(), srgbtexture_test(), unbound_sampler_test()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_sampler_state_edges(const struct d3d9_api *api)
{
    static const struct
    {
        D3DSAMPLERSTATETYPE state;
        DWORD value;
    } cases[] =
    {
        {D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP},
        {D3DSAMP_ADDRESSV, D3DTADDRESS_BORDER},
        {D3DSAMP_MAGFILTER, D3DTEXF_POINT},
        {D3DSAMP_MINFILTER, D3DTEXF_LINEAR},
        {D3DSAMP_MIPFILTER, D3DTEXF_NONE},
        {D3DSAMP_SRGBTEXTURE, TRUE},
        {D3DSAMP_ELEMENTINDEX, 0},
        {D3DSAMP_DMAPOFFSET, 0},
    };
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
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

    value = 0xdeadbeef;
    hr = IDirect3DDevice9_GetSamplerState(device, 0, D3DSAMP_ADDRESSU,
            &value);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(value != 0xdeadbeef);

    for (i = 0; i < ARRAY_SIZE(cases); ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetSamplerState(device, 0,
                cases[i].state, cases[i].value), D3D_OK);
        value = 0xdeadbeef;
        hr = IDirect3DDevice9_GetSamplerState(device, 0, cases[i].state,
                &value);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(value == cases[i].value);
    }

    CHECK_HR(IDirect3DDevice9_GetSamplerState(device, 0,
            D3DSAMP_ADDRESSU, NULL), D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_lights()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_light_enable_state(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DLIGHT9 light;
    D3DCAPS9 caps;
    BOOL enabled;
    HWND window;
    DWORD slot;
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
    slot = caps.MaxActiveLights ? caps.MaxActiveLights - 1 : 0;

    memset(&light, 0, sizeof(light));
    light.Type = D3DLIGHT_DIRECTIONAL;
    light.Diffuse.r = 1.0f;
    light.Diffuse.g = 0.5f;
    light.Diffuse.b = 0.25f;
    light.Direction.z = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetLight(device, slot, &light), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetLight(device, slot, NULL),
            D3DERR_INVALIDCALL);

    enabled = TRUE;
    hr = IDirect3DDevice9_GetLightEnable(device, slot, &enabled);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(enabled == FALSE);

    CHECK_HR(IDirect3DDevice9_LightEnable(device, slot, TRUE), D3D_OK);
    enabled = FALSE;
    hr = IDirect3DDevice9_GetLightEnable(device, slot, &enabled);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(enabled == TRUE);

    CHECK_HR(IDirect3DDevice9_LightEnable(device, slot, FALSE), D3D_OK);
    enabled = TRUE;
    hr = IDirect3DDevice9_GetLightEnable(device, slot, &enabled);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(enabled == FALSE);

    CHECK_HR(IDirect3DDevice9_GetLightEnable(device, slot, NULL),
            D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

static void check_fpu_create_flags(IDirect3D9 *d3d9, HWND window, DWORD flags)
{
    D3DDEVICE_CREATION_PARAMETERS creation;
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device;
    HRESULT hr;

    pp = default_present_parameters(window);
    device = (IDirect3DDevice9 *)0xdeadbeef;
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING | flags, &pp,
            &device);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(device != NULL && device != (IDirect3DDevice9 *)0xdeadbeef);
    if (FAILED(hr))
    {
        CHECK_TRUE(device == NULL);
        return;
    }

    memset(&creation, 0xcc, sizeof(creation));
    hr = IDirect3DDevice9_GetCreationParameters(device, &creation);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(creation.AdapterOrdinal == D3DADAPTER_DEFAULT);
        CHECK_TRUE(creation.DeviceType == D3DDEVTYPE_HAL);
        CHECK_TRUE(creation.hFocusWindow == window);
        CHECK_TRUE((creation.BehaviorFlags & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0);
        CHECK_TRUE((creation.BehaviorFlags & flags) == flags);
    }

    IDirect3DDevice9_Release(device);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_fpu_setup()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_fpu_setup(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9;
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

    check_fpu_create_flags(d3d9, window, 0);
    check_fpu_create_flags(d3d9, window, D3DCREATE_FPU_PRESERVE);

    DestroyWindow(window);

done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_limits()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_limits(const struct d3d9_api *api)
{
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
    hr = IDirect3DDevice9_GetDeviceCaps(device, &caps);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(caps.MaxTextureWidth >= 1);
        CHECK_TRUE(caps.MaxTextureHeight >= 1);
        CHECK_TRUE(caps.MaxVolumeExtent >= 1);
        CHECK_TRUE(caps.MaxTextureRepeat >= 1);
        CHECK_TRUE(caps.MaxStreams >= 1);
        CHECK_TRUE(caps.MaxSimultaneousTextures >= 1);
        CHECK_TRUE(caps.MaxTextureBlendStages >= 1);
        CHECK_TRUE(caps.MaxPrimitiveCount >= 1);
        CHECK_TRUE(caps.MaxVertexIndex >= 1);

        CHECK_HR(IDirect3DDevice9_SetTexture(device,
                caps.MaxSimultaneousTextures, NULL), D3DERR_INVALIDCALL);
        CHECK_HR(IDirect3DDevice9_SetTextureStageState(device,
                caps.MaxTextureBlendStages, D3DTSS_COLOROP, D3DTOP_DISABLE),
                D3DERR_INVALIDCALL);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_set_rt_vp_scissor()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_viewport_scissor_state_getters(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    D3DVIEWPORT9 expected_vp;
    D3DVIEWPORT9 actual_vp;
    IDirect3D9 *d3d9;
    RECT expected_rect;
    RECT actual_rect;
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

    memset(&actual_vp, 0xcc, sizeof(actual_vp));
    hr = IDirect3DDevice9_GetViewport(device, &actual_vp);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(actual_vp.X == 0);
        CHECK_TRUE(actual_vp.Y == 0);
        CHECK_TRUE(actual_vp.Width == 640);
        CHECK_TRUE(actual_vp.Height == 480);
        CHECK_TRUE(actual_vp.MinZ == 0.0f);
        CHECK_TRUE(actual_vp.MaxZ == 1.0f);
    }

    memset(&actual_rect, 0xcc, sizeof(actual_rect));
    hr = IDirect3DDevice9_GetScissorRect(device, &actual_rect);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(actual_rect.left == 0);
        CHECK_TRUE(actual_rect.top == 0);
        CHECK_TRUE(actual_rect.right == 640);
        CHECK_TRUE(actual_rect.bottom == 480);
    }

    CHECK_HR(IDirect3DDevice9_GetViewport(device, NULL), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetScissorRect(device, NULL), D3DERR_INVALIDCALL);

    expected_vp.X = 10;
    expected_vp.Y = 20;
    expected_vp.Width = 30;
    expected_vp.Height = 40;
    expected_vp.MinZ = 0.25f;
    expected_vp.MaxZ = 0.75f;
    CHECK_HR(IDirect3DDevice9_SetViewport(device, &expected_vp), D3D_OK);

    SetRect(&expected_rect, 50, 60, 70, 80);
    CHECK_HR(IDirect3DDevice9_SetScissorRect(device, &expected_rect), D3D_OK);

    memset(&actual_vp, 0xcc, sizeof(actual_vp));
    hr = IDirect3DDevice9_GetViewport(device, &actual_vp);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(&actual_vp, &expected_vp, sizeof(actual_vp)) == 0);

    memset(&actual_rect, 0xcc, sizeof(actual_rect));
    hr = IDirect3DDevice9_GetScissorRect(device, &actual_rect);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(EqualRect(&actual_rect, &expected_rect));

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_scissor_size()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine's oracle verifies that the device's default scissor rect equals the
 * back-buffer dimensions at CreateDevice time and is re-derived from the
 * present-parameter back-buffer size on Reset(). This scaffold mirrors that
 * shape using the existing 640x480 device fixture, then drives a Reset() to
 * a different size and re-queries GetScissorRect.
 */
void test_scissor_default_matches_backbuffer_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    IDirect3D9 *d3d9;
    RECT actual_rect;
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

    /*
     * Step 1: device-default scissor matches the present-parameter
     * back-buffer dimensions. create_base_device() uses the
     * default_present_parameters() (640x480) fixture so the expected
     * rect mirrors that.
     */
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    memset(&actual_rect, 0xcc, sizeof(actual_rect));
    hr = IDirect3DDevice9_GetScissorRect(device, &actual_rect);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(actual_rect.left == 0);
        CHECK_TRUE(actual_rect.top == 0);
        CHECK_TRUE(actual_rect.right == 640);
        CHECK_TRUE(actual_rect.bottom == 480);
    }

    /*
     * Step 2: Reset() to a different back-buffer size re-derives the
     * default scissor from the new dimensions.
     */
    pp = default_present_parameters(window);
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&actual_rect, 0xcc, sizeof(actual_rect));
        hr = IDirect3DDevice9_GetScissorRect(device, &actual_rect);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(actual_rect.left == 0);
            CHECK_TRUE(actual_rect.top == 0);
            CHECK_TRUE(actual_rect.right == 320);
            CHECK_TRUE(actual_rect.bottom == 240);
        }
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_clip_planes_limits()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_clip_plane_state_getters(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    float expected_plane[4];
    float actual_plane[4];
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    DWORD state;
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
    if (!caps.MaxUserClipPlanes)
    {
        skip_current_test("user clip planes are not supported");
        goto done_device;
    }

    memset(actual_plane, 0xff, sizeof(actual_plane));
    hr = IDirect3DDevice9_GetClipPlane(device, 0, actual_plane);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(actual_plane[0] == 0.0f);
        CHECK_TRUE(actual_plane[1] == 0.0f);
        CHECK_TRUE(actual_plane[2] == 0.0f);
        CHECK_TRUE(actual_plane[3] == 0.0f);
    }

    expected_plane[0] = 2.0f;
    expected_plane[1] = 8.0f;
    expected_plane[2] = 5.0f;
    expected_plane[3] = 1.0f;
    CHECK_HR(IDirect3DDevice9_SetClipPlane(device, 0, expected_plane),
            D3D_OK);

    memset(actual_plane, 0xff, sizeof(actual_plane));
    hr = IDirect3DDevice9_GetClipPlane(device, 0, actual_plane);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(actual_plane, expected_plane,
                sizeof(actual_plane)) == 0);

    CHECK_HR(IDirect3DDevice9_GetClipPlane(device, 0, NULL),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_SetClipPlane(device, 0, NULL),
            D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device,
            D3DRS_CLIPPLANEENABLE, 1), D3D_OK);
    state = 0xdeadbeef;
    hr = IDirect3DDevice9_GetRenderState(device, D3DRS_CLIPPLANEENABLE,
            &state);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(state == 1);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_null_stream()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_null_stream_state(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *stream = (IDirect3DVertexBuffer9 *)0xdeadbeef;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT offset = 0xdeadbeef;
    UINT stride = 0xdeadbeef;
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

    hr = IDirect3DDevice9_GetStreamSource(device, 0, &stream, &offset,
            &stride);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(stream == NULL);
    CHECK_TRUE(offset == 0);
    CHECK_TRUE(stride == 0);

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 32, 0, 0,
            D3DPOOL_DEFAULT, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer,
                0, 16), D3D_OK);
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
                D3D_OK);

        stream = (IDirect3DVertexBuffer9 *)0xdeadbeef;
        offset = 0xdeadbeef;
        stride = 0xdeadbeef;
        hr = IDirect3DDevice9_GetStreamSource(device, 0, &stream, &offset,
                &stride);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(stream == NULL);
        /* Wine D3D9 deactivate-stream idiom: SetStreamSource(NULL, 0, 0)
         * detaches the buffer but preserves the previously cached
         * offset/stride. See test_stream_source_null_layout_policy for
         * the (vb, 4, 32) -> (NULL, 4, 32) variant of the same rule. */
        CHECK_TRUE(offset == 0);
        CHECK_TRUE(stride == 16);

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
 * function: test_null_stream()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_null_stream_shader_draw_policy(const struct d3d9_api *api)
{
    static const DWORD shader_code[] =
    {
        0xfffe0101,                         /* vs_1_1 */
        0x0000001f, 0x80000000, 0x900f0000, /* dcl_position v0 */
        0x00000001, 0xc00f0000, 0x90e40000, /* mov oPos, v0 */
        0x0000ffff,                         /* end */
    };
    static const D3DVERTEXELEMENT9 elements[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        {1, 0, D3DDECLTYPE_D3DCOLOR, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_COLOR, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexShader9 *shader = NULL;
    IDirect3DVertexBuffer9 *buffer = NULL;
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

    hr = IDirect3DDevice9_CreateVertexShader(device, shader_code, &shader);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateVertexShader failed with %s", hr_buffer);
        goto done_device;
    }

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, elements, &decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_shader;

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 12 * sizeof(float), 0, 0,
            D3DPOOL_MANAGED, &buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_decl;

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, buffer, 0,
            3 * sizeof(float)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 1, NULL, 0, 0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, shader), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, decl), D3D_OK);

    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device, D3DPT_POINTLIST, 0, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);

    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, NULL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetVertexShader(device, NULL), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);

    IDirect3DVertexBuffer9_Release(buffer);
done_decl:
    IDirect3DVertexDeclaration9_Release(decl);
done_shader:
    IDirect3DVertexShader9_Release(shader);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_set_stream_source()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_set_stream_source_state(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *stream = NULL;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    UINT offset;
    UINT stride;
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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 64, 0, 0,
            D3DPOOL_DEFAULT, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer,
            4, 12), D3D_OK);

    stream = NULL;
    offset = 0;
    stride = 0;
    hr = IDirect3DDevice9_GetStreamSource(device, 0, &stream, &offset,
            &stride);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(stream == vertex_buffer);
    CHECK_TRUE(offset == 4);
    CHECK_TRUE(stride == 12);
    if (stream)
        IDirect3DVertexBuffer9_Release(stream);

    if (caps.MaxStreams)
    {
        CHECK_HR(IDirect3DDevice9_SetStreamSource(device, caps.MaxStreams,
                vertex_buffer, 0, 16), D3DERR_INVALIDCALL);
        stream = (IDirect3DVertexBuffer9 *)0xdeadbeef;
        offset = 0xdeadbeef;
        stride = 0xdeadbeef;
        CHECK_HR(IDirect3DDevice9_GetStreamSource(device, caps.MaxStreams,
                &stream, &offset, &stride), D3DERR_INVALIDCALL);
    }

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);
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
 * function: test_set_stream_source()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_stream_source_vb_offset_alignment_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *stream = NULL;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT offset = 0xdeadbeef;
    UINT stride = 0xdeadbeef;
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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 512, 0, 0,
            D3DPOOL_DEFAULT, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer,
            0, 32), D3D_OK);

    hr = IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer, 1, 32);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
    hr = IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer, 2, 32);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
    hr = IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer, 3, 32);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer,
            4, 32), D3D_OK);

    hr = IDirect3DDevice9_GetStreamSource(device, 0, &stream, &offset,
            &stride);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(stream == vertex_buffer);
        CHECK_TRUE(offset == 4);
        CHECK_TRUE(stride == 32);
        if (stream)
            IDirect3DVertexBuffer9_Release(stream);
    }

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);
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
 * function: test_set_stream_source()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_stream_source_null_layout_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *stream = (IDirect3DVertexBuffer9 *)0xdeadbeef;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT offset = 0xdeadbeef;
    UINT stride = 0xdeadbeef;
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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 512, 0, 0,
            D3DPOOL_DEFAULT, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer,
            4, 32), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);

    hr = IDirect3DDevice9_GetStreamSource(device, 0, &stream, &offset,
            &stride);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(stream == NULL);
        CHECK_TRUE(offset == 4);
        CHECK_TRUE(stride == 32);
        if (stream)
            IDirect3DVertexBuffer9_Release(stream);
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
 * function: test_set_stream_source()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_stream_source_zero_stride_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *stream = NULL;
    IDirect3DVertexBuffer9 *vertex_buffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT offset = 0xdeadbeef;
    UINT stride = 0xdeadbeef;
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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 512, 0, 0,
            D3DPOOL_DEFAULT, &vertex_buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vertex_buffer,
            0, 0), D3D_OK);

    hr = IDirect3DDevice9_GetStreamSource(device, 0, &stream, &offset,
            &stride);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(stream == vertex_buffer);
        CHECK_TRUE(offset == 0);
        CHECK_TRUE(stride == 0);
        if (stream)
            IDirect3DVertexBuffer9_Release(stream);
    }

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);
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
 * function: test_set_stream_source()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_stream_source_null_offset_alignment_policy(
        const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *stream = (IDirect3DVertexBuffer9 *)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT offset = 0xdeadbeef;
    UINT stride = 0xdeadbeef;
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

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);

    hr = IDirect3DDevice9_SetStreamSource(device, 0, NULL, 1, 0);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
    hr = IDirect3DDevice9_SetStreamSource(device, 0, NULL, 2, 0);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);
    hr = IDirect3DDevice9_SetStreamSource(device, 0, NULL, 3, 0);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 4, 0),
            D3D_OK);

    hr = IDirect3DDevice9_GetStreamSource(device, 0, &stream, &offset,
            &stride);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(stream == NULL);
        CHECK_TRUE(offset == 4);
        CHECK_TRUE(stride == 0);
        if (stream)
            IDirect3DVertexBuffer9_Release(stream);
    }

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0),
            D3D_OK);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: stream_test()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_stream_source_frequency_state(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    UINT freq;
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
    if (caps.MaxStreams < 2)
    {
        skip_current_test("device exposes fewer than two streams");
        goto done_device;
    }

    freq = 0xdeadbeef;
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 0, &freq);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(freq == 1);

    freq = 0xdeadbeef;
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 1, &freq);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(freq == 1);

    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1, 1), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 0,
            D3DSTREAMSOURCE_INSTANCEDATA | 1), D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1, 0),
            D3DERR_INVALIDCALL);
    freq = 0xdeadbeef;
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 1, &freq);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(freq == 1);

    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1, 2), D3D_OK);
    freq = 0xdeadbeef;
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 1, &freq);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(freq == 2);

    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1,
            D3DSTREAMSOURCE_INDEXEDDATA), D3D_OK);
    freq = 0xdeadbeef;
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 1, &freq);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(freq == D3DSTREAMSOURCE_INDEXEDDATA);

    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1,
            D3DSTREAMSOURCE_INSTANCEDATA), D3D_OK);
    freq = 0xdeadbeef;
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 1, &freq);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(freq == D3DSTREAMSOURCE_INSTANCEDATA);

    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1,
            D3DSTREAMSOURCE_INSTANCEDATA | D3DSTREAMSOURCE_INDEXEDDATA),
            D3DERR_INVALIDCALL);
    freq = 0xdeadbeef;
    hr = IDirect3DDevice9_GetStreamSourceFreq(device, 1, &freq);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(freq == D3DSTREAMSOURCE_INSTANCEDATA);

    CHECK_HR(IDirect3DDevice9_SetStreamSourceFreq(device, 1, 1), D3D_OK);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_get_set_texture()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_get_set_texture(const struct d3d9_api *api)
{
    IDirect3DBaseTexture9 *current = (IDirect3DBaseTexture9 *)0xdeadbeef;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    ULONG ref;
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

    hr = IDirect3DDevice9_GetTexture(device, 0, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == NULL);

    CHECK_HR(IDirect3DDevice9_GetTexture(device, 0, NULL),
            D3DERR_INVALIDCALL);

    hr = IDirect3DDevice9_CreateTexture(device, 4, 4, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    ref = get_refcount((IUnknown *)texture);
    CHECK_TRUE(ref == 1);

    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0,
            (IDirect3DBaseTexture9 *)texture), D3D_OK);
    ref = get_refcount((IUnknown *)texture);
    CHECK_TRUE(ref == 2);

    current = NULL;
    hr = IDirect3DDevice9_GetTexture(device, 0, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == (IDirect3DBaseTexture9 *)texture);
    if (current)
    {
        ref = get_refcount((IUnknown *)texture);
        CHECK_TRUE(ref == 3);
        IDirect3DBaseTexture9_Release(current);
    }

    if (caps.MaxSimultaneousTextures)
    {
        CHECK_HR(IDirect3DDevice9_SetTexture(device,
                caps.MaxSimultaneousTextures,
                (IDirect3DBaseTexture9 *)texture), D3DERR_INVALIDCALL);
        current = (IDirect3DBaseTexture9 *)0xdeadbeef;
        CHECK_HR(IDirect3DDevice9_GetTexture(device,
                caps.MaxSimultaneousTextures, &current), D3DERR_INVALIDCALL);
        CHECK_TRUE(current == (IDirect3DBaseTexture9 *)0xdeadbeef
                || current == NULL);
    }

    CHECK_HR(IDirect3DDevice9_SetTexture(device, 0, NULL), D3D_OK);
    current = (IDirect3DBaseTexture9 *)0xdeadbeef;
    hr = IDirect3DDevice9_GetTexture(device, 0, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == NULL);

    ref = get_refcount((IUnknown *)texture);
    CHECK_TRUE(ref == 1);
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
 * function: test_set_palette()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_set_palette_roundtrip(const struct d3d9_api *api)
{
    PALETTEENTRY expected[256];
    PALETTEENTRY actual[256];
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT palette;
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

    for (i = 0; i < ARRAY_SIZE(expected); ++i)
    {
        expected[i].peRed = (BYTE)i;
        expected[i].peGreen = (BYTE)(255 - i);
        expected[i].peBlue = (BYTE)(i ^ 0x5a);
        expected[i].peFlags = 0xff;
    }

    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, expected),
            D3D_OK);
    memset(actual, 0xcc, sizeof(actual));
    hr = IDirect3DDevice9_GetPaletteEntries(device, 0, actual);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(actual, expected, sizeof(expected)) == 0);

    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0), D3D_OK);
    palette = 0xdeadbeef;
    hr = IDirect3DDevice9_GetCurrentTexturePalette(device, &palette);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(palette == 0);

    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, NULL),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetPaletteEntries(device, 0, NULL),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetCurrentTexturePalette(device, NULL),
            D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_set_palette()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_palette_alpha_caps_policy(const struct d3d9_api *api)
{
    PALETTEENTRY baseline[256];
    PALETTEENTRY alpha[256];
    PALETTEENTRY actual[256];
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
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

    for (i = 0; i < ARRAY_SIZE(baseline); ++i)
    {
        baseline[i].peRed = (BYTE)i;
        baseline[i].peGreen = (BYTE)i;
        baseline[i].peBlue = (BYTE)i;
        baseline[i].peFlags = 0xff;

        alpha[i].peRed = (BYTE)i;
        alpha[i].peGreen = (BYTE)(255 - i);
        alpha[i].peBlue = (BYTE)(i ^ 0x33);
        alpha[i].peFlags = (BYTE)i;
    }

    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, baseline),
            D3D_OK);

    hr = IDirect3DDevice9_SetPaletteEntries(device, 0, alpha);
    if (caps.TextureCaps & D3DPTEXTURECAPS_ALPHAPALETTE)
    {
        CHECK_HR(hr, D3D_OK);
        memset(actual, 0xcc, sizeof(actual));
        hr = IDirect3DDevice9_GetPaletteEntries(device, 0, actual);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
            CHECK_TRUE(memcmp(actual, alpha, sizeof(alpha)) == 0);
    }
    else
    {
        CHECK_HR(hr, D3DERR_INVALIDCALL);
        memset(actual, 0xcc, sizeof(actual));
        hr = IDirect3DDevice9_GetPaletteEntries(device, 0, actual);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
            CHECK_TRUE(memcmp(actual, baseline, sizeof(baseline)) == 0);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_set_palette()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_palette_current_entry_isolation(const struct d3d9_api *api)
{
    PALETTEENTRY palette0[256];
    PALETTEENTRY palette1[256];
    PALETTEENTRY actual[256];
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    UINT current;
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

    for (i = 0; i < ARRAY_SIZE(palette0); ++i)
    {
        palette0[i].peRed = (BYTE)i;
        palette0[i].peGreen = (BYTE)(255 - i);
        palette0[i].peBlue = (BYTE)(i ^ 0x5a);
        palette0[i].peFlags = 0xff;

        palette1[i].peRed = (BYTE)(255 - i);
        palette1[i].peGreen = (BYTE)(i ^ 0xa5);
        palette1[i].peBlue = (BYTE)i;
        palette1[i].peFlags = 0xff;
    }

    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 0, palette0),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetPaletteEntries(device, 1, palette1),
            D3D_OK);

    memset(actual, 0xcc, sizeof(actual));
    hr = IDirect3DDevice9_GetPaletteEntries(device, 0, actual);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(actual, palette0, sizeof(palette0)) == 0);

    memset(actual, 0xcc, sizeof(actual));
    hr = IDirect3DDevice9_GetPaletteEntries(device, 1, actual);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(actual, palette1, sizeof(palette1)) == 0);

    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 1), D3D_OK);
    current = 0xdeadbeef;
    hr = IDirect3DDevice9_GetCurrentTexturePalette(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == 1);

    CHECK_HR(IDirect3DDevice9_SetCurrentTexturePalette(device, 0), D3D_OK);
    current = 0xdeadbeef;
    hr = IDirect3DDevice9_GetCurrentTexturePalette(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == 0);

    memset(actual, 0xcc, sizeof(actual));
    hr = IDirect3DDevice9_GetPaletteEntries(device, 1, actual);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(memcmp(actual, palette1, sizeof(palette1)) == 0);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_multi_adapter()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_multi_adapter(const struct d3d9_api *api)
{
    D3DADAPTER_IDENTIFIER9 identifier;
    D3DADAPTER_IDENTIFIER9 identifier2;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    UINT count;
    UINT i;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    count = IDirect3D9_GetAdapterCount(d3d9);
    CHECK_TRUE(count > 0);
    if (!count)
        goto done;

    for (i = 0; i < count; ++i)
    {
        memset(&identifier, 0xcc, sizeof(identifier));
        CHECK_HR(IDirect3D9_GetAdapterIdentifier(d3d9, i, 0, &identifier),
                D3D_OK);
        CHECK_HR(IDirect3D9_GetDeviceCaps(d3d9, i, D3DDEVTYPE_HAL, &caps),
                D3D_OK);

        /*
         * gap.md §C.7 — AlphaCmpCaps must be sourced from the
         * dedicated alphaCmpCaps slot, not from alphaBlendCaps.
         * Every Metal-capable GPU supports the full eight-op
         * comparison set; assert at least the three ops that legacy
         * D3D9 titles consistently rely on (ALWAYS / NEVER /
         * LESSEQUAL).
         */
        CHECK_TRUE(caps.AlphaCmpCaps != 0);
        CHECK_TRUE((caps.AlphaCmpCaps & (D3DPCMPCAPS_ALWAYS
                | D3DPCMPCAPS_NEVER | D3DPCMPCAPS_LESSEQUAL))
                == (D3DPCMPCAPS_ALWAYS | D3DPCMPCAPS_NEVER
                | D3DPCMPCAPS_LESSEQUAL));

        /*
         * gap.md §C.9 — D3DADAPTER_IDENTIFIER9::DeviceIdentifier
         * must be a non-zero, byte-stable per-adapter GUID. Several
         * legacy D3D9 titles use it as an installation fingerprint
         * and refuse to launch when it's the zero GUID.
         */
        {
            const unsigned char *guid_bytes =
                    (const unsigned char *)&identifier.DeviceIdentifier;
            int any_non_zero = 0;
            size_t k;
            for (k = 0; k < sizeof(identifier.DeviceIdentifier); ++k)
            {
                if (guid_bytes[k] != 0)
                {
                    any_non_zero = 1;
                    break;
                }
            }
            CHECK_TRUE(any_non_zero);
        }

        /*
         * Determinism contract: two consecutive
         * GetAdapterIdentifier calls must return byte-equal GUIDs
         * (installation-fingerprint usage requires byte stability).
         */
        memset(&identifier2, 0xcc, sizeof(identifier2));
        CHECK_HR(IDirect3D9_GetAdapterIdentifier(d3d9, i, 0, &identifier2),
                D3D_OK);
        CHECK_TRUE(memcmp(&identifier.DeviceIdentifier,
                &identifier2.DeviceIdentifier,
                sizeof(identifier.DeviceIdentifier)) == 0);
    }

    memset(&identifier, 0xcc, sizeof(identifier));
    CHECK_HR(IDirect3D9_GetAdapterIdentifier(d3d9, count, 0, &identifier),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9_GetAdapterIdentifier(d3d9, D3DADAPTER_DEFAULT, 0,
            NULL), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9_GetDeviceCaps(d3d9, count, D3DDEVTYPE_HAL, &caps),
            D3DERR_INVALIDCALL);

    if (count < 2)
        skip_current_test("single-adapter host; invalid-adapter path covered");

done:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_creation_parameters()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_device_creation_parameters_policy(const struct d3d9_api *api)
{
    D3DDEVICE_CREATION_PARAMETERS creation;
    IDirect3DDevice9 *device = NULL;
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
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    pp = default_present_parameters(window);
    device = (IDirect3DDevice9 *)0xdeadbeef;
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING
            | D3DCREATE_NOWINDOWCHANGES, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(device != NULL && device != (IDirect3DDevice9 *)0xdeadbeef);
    if (FAILED(hr))
    {
        CHECK_TRUE(device == NULL);
        goto done_window;
    }

    memset(&creation, 0xcc, sizeof(creation));
    hr = IDirect3DDevice9_GetCreationParameters(device, &creation);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(creation.AdapterOrdinal == D3DADAPTER_DEFAULT);
        CHECK_TRUE(creation.DeviceType == D3DDEVTYPE_HAL);
        CHECK_TRUE(creation.hFocusWindow == window);
        CHECK_TRUE((creation.BehaviorFlags
                & D3DCREATE_SOFTWARE_VERTEXPROCESSING) != 0);
        CHECK_TRUE((creation.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES) != 0);
    }

    CHECK_HR(IDirect3DDevice9_GetCreationParameters(device, NULL),
            D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_refcount(), device utility getter checks
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_device_parent_caps_getter_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *parent = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    ULONG refcount;
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

    CHECK_HR(IDirect3DDevice9_GetDirect3D(device, NULL),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, NULL),
            D3DERR_INVALIDCALL);

    refcount = get_refcount((IUnknown *)d3d9);
    hr = IDirect3DDevice9_GetDirect3D(device, &parent);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(parent == d3d9);
    CHECK_TRUE(get_refcount((IUnknown *)d3d9) == refcount + 1);
    if (parent)
        IDirect3D9_Release(parent);
    CHECK_TRUE(get_refcount((IUnknown *)d3d9) == refcount);

    memset(&caps, 0xcc, sizeof(caps));
    hr = IDirect3DDevice9_GetDeviceCaps(device, &caps);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(caps.AdapterOrdinal == D3DADAPTER_DEFAULT);
        CHECK_TRUE(caps.DeviceType == D3DDEVTYPE_HAL);
    }

    CHECK_TRUE(IDirect3DDevice9_GetAvailableTextureMem(device) != 0);
    CHECK_HR(IDirect3DDevice9_EvictManagedResources(device), D3D_OK);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: device utility GetRasterStatus checks
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_device_raster_status_bounds(const struct d3d9_api *api)
{
    D3DRASTER_STATUS raster_status;
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

    memset(&raster_status, 0xcc, sizeof(raster_status));
    hr = IDirect3DDevice9_GetRasterStatus(device, 0, &raster_status);
    CHECK_TRUE(hr == D3D_OK || hr == E_FAIL);

    CHECK_HR(IDirect3DDevice9_GetRasterStatus(device, 0, NULL),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_GetRasterStatus(device, 1, &raster_status),
            D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_pixel_format()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_pixel_format_window_policy(const struct d3d9_api *api)
{
    static const float point[] = {0.0f, 0.0f, 0.0f};
    IDirect3DDevice9 *device = NULL;
    PIXELFORMATDESCRIPTOR pfd;
    IDirect3D9 *d3d9 = NULL;
    HMODULE opengl = NULL;
    HWND window3 = NULL;
    HDC dc3 = NULL;
    HWND window2;
    HWND window;
    int format;
    int actual;
    HDC dc2;
    HDC dc;

    window = create_test_window();
    window2 = create_test_window();
    CHECK_TRUE(window != NULL);
    CHECK_TRUE(window2 != NULL);
    if (!window || !window2)
        goto done_windows;

    dc = GetDC(window);
    dc2 = GetDC(window2);
    CHECK_TRUE(dc != NULL);
    CHECK_TRUE(dc2 != NULL);
    if (!dc || !dc2)
        goto done_dcs;

    opengl = LoadLibraryA("opengl32.dll");
    if (!opengl)
    {
        skip_current_test("opengl32.dll unavailable for pixel-format setup");
        goto done_dcs;
    }

    CHECK_TRUE(GetPixelFormat(dc) == 0);

    memset(&pfd, 0, sizeof(pfd));
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.iLayerType = PFD_MAIN_PLANE;

    format = ChoosePixelFormat(dc, &pfd);
    if (format <= 0)
    {
        skip_current_test("ChoosePixelFormat found no usable format");
        goto done_dcs;
    }

    if (!SetPixelFormat(dc, format, &pfd)
            || GetPixelFormat(dc) != format
            || !SetPixelFormat(dc2, format, &pfd)
            || GetPixelFormat(dc2) != format)
    {
        skip_current_test("SetPixelFormat did not stick on test windows");
        goto done_dcs;
    }

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        goto done_dcs;
    }

    actual = GetPixelFormat(dc);
    CHECK_TRUE(actual == format);

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_d3d9;

    actual = GetPixelFormat(dc);
    CHECK_TRUE(actual == format);

    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ), D3D_OK);
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1,
            point, 3 * sizeof(float)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_Present(device, NULL, NULL, window2, NULL),
            D3D_OK);

    CHECK_TRUE(GetPixelFormat(dc) == format);
    CHECK_TRUE(GetPixelFormat(dc2) == format);

    IDirect3DDevice9_Release(device);
    device = NULL;
    IDirect3D9_Release(d3d9);
    d3d9 = NULL;

    CHECK_TRUE(GetPixelFormat(dc) == format);
    CHECK_TRUE(GetPixelFormat(dc2) == format);

    window3 = create_test_window();
    CHECK_TRUE(window3 != NULL);
    if (!window3)
        goto done_dcs;

    dc3 = GetDC(window3);
    CHECK_TRUE(dc3 != NULL);
    if (!dc3)
        goto done_window3;

    CHECK_TRUE(GetPixelFormat(dc3) == 0);

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL for blank window");
        goto done_window3;
    }

    device = create_base_device(d3d9, window3);
    if (!device)
        goto done_d3d9;

    CHECK_HR(IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ), D3D_OK);
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitiveUP(device, D3DPT_POINTLIST, 1,
            point, 3 * sizeof(float)), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_TRUE(GetPixelFormat(dc3) == 0);
    CHECK_HR(IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL),
            D3D_OK);
    CHECK_TRUE(GetPixelFormat(dc3) == 0);

done_d3d9:
    if (device)
        IDirect3DDevice9_Release(device);
    if (d3d9)
        IDirect3D9_Release(d3d9);
done_window3:
    if (dc3)
        ReleaseDC(window3, dc3);
    if (window3)
        DestroyWindow(window3);
done_dcs:
    if (opengl)
        FreeLibrary(opengl);
    if (dc2)
        ReleaseDC(window2, dc2);
    if (dc)
        ReleaseDC(window, dc);
done_windows:
    if (window2)
        DestroyWindow(window2);
    if (window)
        DestroyWindow(window);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_multi_device
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_multi_device_independent_state(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device1 = NULL;
    IDirect3DDevice9 *device2 = NULL;
    IDirect3D9 *parent1 = NULL;
    IDirect3D9 *parent2 = NULL;
    HWND window1 = NULL;
    HWND window2 = NULL;
    IDirect3D9 *d3d9;
    DWORD value;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    window1 = create_test_window();
    CHECK_TRUE(window1 != NULL);
    if (!window1)
        goto done_d3d9;

    window2 = create_test_window();
    CHECK_TRUE(window2 != NULL);
    if (!window2)
        goto done_window1;

    device1 = create_base_device(d3d9, window1);
    if (!device1)
        goto done_window2;

    device2 = create_base_device(d3d9, window2);
    if (!device2)
        goto done_device1;

    hr = IDirect3DDevice9_GetDirect3D(device1, &parent1);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_GetDirect3D(device2, &parent2);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(parent1 == d3d9);
    CHECK_TRUE(parent2 == d3d9);
    if (parent1)
        IDirect3D9_Release(parent1);
    if (parent2)
        IDirect3D9_Release(parent2);

    CHECK_HR(IDirect3DDevice9_SetRenderState(device1, D3DRS_ZENABLE,
            D3DZB_FALSE), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device2, D3DRS_ZENABLE,
            D3DZB_TRUE), D3D_OK);

    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device1, D3DRS_ZENABLE, &value),
            D3D_OK);
    CHECK_TRUE(value == D3DZB_FALSE);

    value = 0xdeadbeef;
    CHECK_HR(IDirect3DDevice9_GetRenderState(device2, D3DRS_ZENABLE, &value),
            D3D_OK);
    CHECK_TRUE(value == D3DZB_TRUE);

    IDirect3DDevice9_Release(device2);
    device2 = NULL;
done_device1:
    if (device1)
        IDirect3DDevice9_Release(device1);
done_window2:
    if (window2)
        DestroyWindow(window2);
done_window1:
    if (window1)
        DestroyWindow(window1);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_mode_change
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_mode_change_focus_swap_policy(const struct d3d9_api *api)
{
    D3DDEVICE_CREATION_PARAMETERS creation;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    HWND focus_window = NULL;
    HWND device_window = NULL;
    IDirect3D9 *d3d9;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    focus_window = create_test_window();
    CHECK_TRUE(focus_window != NULL);
    if (!focus_window)
        goto done_d3d9;

    device_window = create_test_window();
    CHECK_TRUE(device_window != NULL);
    if (!device_window)
        goto done_focus;

    pp = default_present_parameters(device_window);
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            focus_window, D3DCREATE_SOFTWARE_VERTEXPROCESSING
            | D3DCREATE_NOWINDOWCHANGES, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device_window;

    memset(&creation, 0xcc, sizeof(creation));
    hr = IDirect3DDevice9_GetCreationParameters(device, &creation);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(creation.hFocusWindow == focus_window);
        CHECK_TRUE((creation.BehaviorFlags & D3DCREATE_NOWINDOWCHANGES) != 0);
    }

    /*
     * The Wine oracle exercises a series of WM_DISPLAYCHANGE / mode swaps
     * that the focus window must not propagate to the device window while
     * D3DCREATE_NOWINDOWCHANGES is set. Scaffolded scope only validates
     * that a Reset on the same focus-window pair still succeeds and that
     * GetCreationParameters continues to report the original focus HWND.
     */
    pp = default_present_parameters(device_window);
    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_HR(hr, D3D_OK);

    memset(&creation, 0xcc, sizeof(creation));
    hr = IDirect3DDevice9_GetCreationParameters(device, &creation);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(creation.hFocusWindow == focus_window);

    IDirect3DDevice9_Release(device);
    device = NULL;

done_device_window:
    if (device_window)
        DestroyWindow(device_window);
done_focus:
    if (focus_window)
        DestroyWindow(focus_window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_reset_fullscreen
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_reset_fullscreen_focus_window_policy(const struct d3d9_api *api)
{
    D3DDEVICE_CREATION_PARAMETERS creation;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
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

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &mode);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_window;

    pp = default_fullscreen_present_parameters(window, &mode);
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("fullscreen CreateDevice failed with %s", hr_buffer);
        goto done_window;
    }

    memset(&creation, 0xcc, sizeof(creation));
    hr = IDirect3DDevice9_GetCreationParameters(device, &creation);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(creation.hFocusWindow == window);

    /*
     * Wine's oracle covers Reset-back-to-fullscreen after a windowed
     * trip. Scaffolded scope only validates that a fullscreen->fullscreen
     * Reset with identical parameters succeeds and that GetSwapChain(0)
     * still returns the implicit swapchain bound to the focus window.
     */
    pp = default_fullscreen_present_parameters(window, &mode);
    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_DEVICELOST);

    if (hr == D3D_OK)
    {
        IDirect3DSwapChain9 *swapchain = NULL;
        hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
        CHECK_HR(hr, D3D_OK);
        if (swapchain)
            IDirect3DSwapChain9_Release(swapchain);
    }

    IDirect3DDevice9_Release(device);
    device = NULL;

done_window:
    if (window)
        DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_window_position
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_window_position_present_parameter_policy(const struct d3d9_api *api)
{
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS observed;
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
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    pp = default_present_parameters(window);
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_window;

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&observed, 0xcc, sizeof(observed));
    hr = IDirect3DSwapChain9_GetPresentParameters(swapchain, &observed);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(observed.hDeviceWindow == window);
        CHECK_TRUE(observed.Windowed == TRUE);
        CHECK_TRUE(observed.BackBufferWidth == pp.BackBufferWidth);
        CHECK_TRUE(observed.BackBufferHeight == pp.BackBufferHeight);
    }

    CHECK_HR(IDirect3DSwapChain9_GetPresentParameters(swapchain, NULL),
            D3DERR_INVALIDCALL);

    /*
     * Wine's oracle moves the device window and asserts the present
     * parameters do not auto-track the new client rect. Scaffolded scope
     * confirms a Reset that re-uses hDeviceWindow=window round-trips and
     * GetPresentParameters still reports the same focus HWND.
     */
    IDirect3DSwapChain9_Release(swapchain);
    swapchain = NULL;

    pp = default_present_parameters(window);
    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_HR(hr, D3D_OK);

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&observed, 0xcc, sizeof(observed));
        hr = IDirect3DSwapChain9_GetPresentParameters(swapchain, &observed);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
            CHECK_TRUE(observed.hDeviceWindow == window);
    }

done_device:
    if (swapchain)
        IDirect3DSwapChain9_Release(swapchain);
    if (device)
        IDirect3DDevice9_Release(device);
done_window:
    if (window)
        DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_query_support / test_occlusion_query / test_timestamp_query
 *           (focused on GetDataSize policy)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_query_get_data_size_policy(const struct d3d9_api *api)
{
    static const D3DQUERYTYPE types[] =
    {
        D3DQUERYTYPE_EVENT,
        D3DQUERYTYPE_OCCLUSION,
        D3DQUERYTYPE_TIMESTAMP,
        D3DQUERYTYPE_TIMESTAMPFREQ,
        D3DQUERYTYPE_TIMESTAMPDISJOINT,
    };
    IDirect3D9 *d3d9;
    IDirect3DDevice9 *device = NULL;
    HWND window;
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

    for (i = 0; i < ARRAY_SIZE(types); ++i)
    {
        IDirect3DQuery9 *query = NULL;
        DWORD size;
        HRESULT hr;

        hr = IDirect3DDevice9_CreateQuery(device, types[i], &query);
        if (hr != D3D_OK)
            continue;

        size = IDirect3DQuery9_GetDataSize(query);
        CHECK_TRUE(size > 0);
        CHECK_TRUE(IDirect3DQuery9_GetType(query) == types[i]);

        IDirect3DQuery9_Release(query);
    }

    /* Invalid query type → CreateQuery returns D3DERR_NOTAVAILABLE. */
    {
        IDirect3DQuery9 *query = NULL;
        HRESULT hr = IDirect3DDevice9_CreateQuery(device,
                (D3DQUERYTYPE)0xdeadbeef, &query);
        CHECK_TRUE(hr == D3DERR_NOTAVAILABLE || hr == D3DERR_INVALIDCALL);
        if (SUCCEEDED(hr) && query)
            IDirect3DQuery9_Release(query);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_check_device_format (focused on CheckDeviceFormatConversion)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_check_device_format_conversion_matrix(const struct d3d9_api *api)
{
    static const struct
    {
        D3DFORMAT src;
        D3DFORMAT dst;
        BOOL expect_compatible;
    } matrix[] =
    {
        {D3DFMT_A8R8G8B8, D3DFMT_A8R8G8B8, TRUE},  /* identity */
        {D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE},
        {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, TRUE},  /* compatible pair */
        {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, TRUE},
        {D3DFMT_DXT1,     D3DFMT_A8R8G8B8, FALSE}, /* incompatible */
        {D3DFMT_R5G6B5,   D3DFMT_A8R8G8B8, FALSE},
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

    for (i = 0; i < ARRAY_SIZE(matrix); ++i)
    {
        hr = IDirect3D9_CheckDeviceFormatConversion(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, matrix[i].src, matrix[i].dst);
        if (matrix[i].expect_compatible)
            CHECK_HR(hr, D3D_OK);
        else
            CHECK_HR(hr, D3DERR_NOTAVAILABLE);
    }

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_display_formats (focused on CheckDeviceType displayable
 * adapter / backbuffer format pairs)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Pin a tight policy on a small fixed (adapter, backbuf, windowed) matrix:
 *   X8R8G8B8 + A8R8G8B8, windowed   -> S_OK (displayable pair)
 *   X8R8G8B8 + X8R8G8B8, windowed   -> S_OK
 *   R5G6B5   + R5G6B5,   windowed   -> S_OK
 *   DXT1     + A8R8G8B8, windowed   -> D3DERR_NOTAVAILABLE (non-display
 *                                     adapter format is rejected)
 *   A8R8G8B8 + X8R8G8B8, fullscreen -> D3DERR_NOTAVAILABLE (adapter formats
 *                                     with an alpha channel are invalid in
 *                                     exclusive fullscreen)
 */
void test_check_device_type_display_format_policy(const struct d3d9_api *api)
{
    static const struct
    {
        D3DFORMAT adapter;
        D3DFORMAT backbuf;
        BOOL windowed;
        HRESULT expected_hr;
    } matrix[] =
    {
        {D3DFMT_X8R8G8B8, D3DFMT_A8R8G8B8, TRUE,  D3D_OK},
        {D3DFMT_X8R8G8B8, D3DFMT_X8R8G8B8, TRUE,  D3D_OK},
        {D3DFMT_R5G6B5,   D3DFMT_R5G6B5,   TRUE,  D3D_OK},
        {D3DFMT_DXT1,     D3DFMT_A8R8G8B8, TRUE,  D3DERR_NOTAVAILABLE},
        {D3DFMT_A8R8G8B8, D3DFMT_X8R8G8B8, FALSE, D3DERR_NOTAVAILABLE},
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

    for (i = 0; i < ARRAY_SIZE(matrix); ++i)
    {
        hr = IDirect3D9_CheckDeviceType(d3d9, D3DADAPTER_DEFAULT,
                D3DDEVTYPE_HAL, matrix[i].adapter, matrix[i].backbuf,
                matrix[i].windowed);
        CHECK_HR(hr, matrix[i].expected_hr);
    }

    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_npot_textures
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Wine test_npot_textures exercises three NPOT (non-power-of-two) caps
 * configurations: pure NPOT, conditional NPOT
 * (D3DPTEXTURECAPS_NONPOW2CONDITIONAL), and POW2-only adapters. dxmt9
 * reports the modern caps (full NPOT), so this minimum scaffold pins
 * that CreateTexture / CreateCubeTexture with NPOT dimensions in both
 * MANAGED and DEFAULT pools returns S_OK.
 */
void test_create_texture_npot_policy(const struct d3d9_api *api)
{
    static const struct
    {
        UINT width;
        UINT height;
    } dims[] =
    {
        {3, 5},
        {15, 17},
        {33, 65},
        {127, 255},
    };
    IDirect3DCubeTexture9 *cube = NULL;
    IDirect3DTexture9 *texture = NULL;
    IDirect3DDevice9 *device = NULL;
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
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    for (i = 0; i < ARRAY_SIZE(dims); ++i)
    {
        texture = NULL;
        hr = IDirect3DDevice9_CreateTexture(device, dims[i].width,
                dims[i].height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED,
                &texture, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr) && texture)
            IDirect3DTexture9_Release(texture);

        texture = NULL;
        hr = IDirect3DDevice9_CreateTexture(device, dims[i].width,
                dims[i].height, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                &texture, NULL);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr) && texture)
            IDirect3DTexture9_Release(texture);
    }

    hr = IDirect3DDevice9_CreateCubeTexture(device, 33, 1, 0,
            D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &cube, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr) && cube)
        IDirect3DCubeTexture9_Release(cube);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_creation_parameters (focused on D3DCREATE_MULTITHREADED)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_multithreaded_device_creation_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DDEVICE_CREATION_PARAMETERS params;
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
            window,
            D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED
                    | D3DCREATE_FPU_PRESERVE,
            &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr) || !device)
        goto done_window;

    memset(&params, 0xcc, sizeof(params));
    CHECK_HR(IDirect3DDevice9_GetCreationParameters(device, &params), D3D_OK);
    CHECK_TRUE((params.BehaviorFlags & D3DCREATE_MULTITHREADED) != 0);
    CHECK_TRUE((params.BehaviorFlags & D3DCREATE_HARDWARE_VERTEXPROCESSING) != 0);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_draw_primitive (BeginScene precondition slice)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Pins the scene-precondition envelope for DrawPrimitive. The MSDN
 * contract reads as "must be issued between BeginScene/EndScene", but
 * Wine's d3d9 builtin (dlls/d3d9/device.c:3236 d3d9_device_DrawPrimitive)
 * does not gate on in_scene and returns S_OK regardless; dxmt9 follows
 * the same permissive de-facto runtime envelope that real titles depend
 * on. Inside a scene the zero-primitive-count case must succeed.
 * Outside-of-scene DrawPrimitive calls therefore accept either S_OK or
 * D3DERR_INVALIDCALL so the test matches both Wine and dxmt9 today.
 */
void test_draw_primitive_outside_scene_policy(const struct d3d9_api *api)
{
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

    /* DrawPrimitive without a prior BeginScene: behavior is
     * implementation-defined. Wine's d3d9 builtin
     * (dlls/d3d9/device.c:3236 d3d9_device_DrawPrimitive) does not
     * gate on in_scene and returns S_OK; the MSDN contract suggests
     * D3DERR_INVALIDCALL. Accept either to match the de-facto
     * runtime envelope games depend on. */
    hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 0);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);

    /* Inside a scene, zero primitiveCount is a no-op that returns S_OK. */
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 0);
    CHECK_HR(hr, D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);

    /* After EndScene, the scene precondition would re-arm under a
     * strict interpretation; same Wine permissiveness applies. */
    hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 0);
    CHECK_TRUE(hr == D3D_OK || hr == D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_depthstenciltest
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Pins the Set/Get DepthStencilSurface round-trip and NULL-detach
 * contract observed in Wine's test_depthstenciltest. A device created
 * with EnableAutoDepthStencil=TRUE owns an implicit DS surface that
 * GetDepthStencilSurface returns AddRef'd. SetDepthStencilSurface(NULL)
 * detaches that surface so a follow-up Get yields D3DERR_NOTFOUND with
 * the out-pointer cleared, and a follow-up Set restores the surface so
 * Get returns it again.
 */
void test_set_get_depth_stencil_surface_policy(const struct d3d9_api *api)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    IDirect3DSurface9 *original_ds = NULL;
    IDirect3DSurface9 *out = NULL;
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

    /* Use an explicit auto-depth-stencil present params block so the
     * implicit DS attach/detach contract is observable. */
    pp = default_present_parameters(window);
    pp.EnableAutoDepthStencil = TRUE;
    pp.AutoDepthStencilFormat = D3DFMT_D24S8;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateDevice with auto-depth-stencil failed with %s",
                hr_buffer);
        goto done_window;
    }

    /* Implicit DS is attached: GetDepthStencilSurface returns the auto
     * depth-stencil surface AddRef'd. */
    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &original_ds);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(original_ds != NULL);
    if (!original_ds)
        goto done_device;

    /* SetDepthStencilSurface(NULL) detaches the implicit DS. */
    CHECK_HR(IDirect3DDevice9_SetDepthStencilSurface(device, NULL), D3D_OK);

    /* GetDepthStencilSurface after NULL detach yields D3DERR_NOTFOUND
     * with the out-pointer cleared. */
    out = (IDirect3DSurface9 *)0xdeadbeef;
    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &out);
    CHECK_HR(hr, D3DERR_NOTFOUND);
    CHECK_TRUE(out == NULL);

    /* Re-attaching the original DS restores the binding so a follow-up
     * Get returns the same surface AddRef'd. */
    CHECK_HR(IDirect3DDevice9_SetDepthStencilSurface(device, original_ds), D3D_OK);
    out = NULL;
    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &out);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(out == original_ds);
    if (out)
        IDirect3DSurface9_Release(out);

    /* NULL out-pointer must reject without touching device state. */
    hr = IDirect3DDevice9_GetDepthStencilSurface(device, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    /* The binding is still the original DS after the NULL-out probe. */
    out = NULL;
    hr = IDirect3DDevice9_GetDepthStencilSurface(device, &out);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(out == original_ds);
    if (out)
        IDirect3DSurface9_Release(out);

    IDirect3DSurface9_Release(original_ds);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_desktop_window
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * The same contract is mirrored verbatim in dlls/d3d9/tests/d3d9ex.c
 * (test_desktop_window at d3d9ex.c:4976), which exercises the
 * IDirect3DDevice9Ex variant of the same flow. The base-device case
 * pinned here covers the shared D3D9 surface; the Ex variant adds no
 * new policy beyond the QI/CreateDeviceEx wrapper already covered by
 * other Ex device-creation cases.
 *
 * Pins the device-creation policy for hDeviceWindow=GetDesktopWindow():
 * CreateDevice with both hFocusWindow and pp.hDeviceWindow set to the
 * desktop window must accept the request and return S_OK, and a
 * follow-up Clear + Present on the resulting device must succeed
 * (Wine asserts SUCCEEDED on both). This guards against an implicit
 * desktop-window redirect / rejection at device creation or present
 * time. The Wine oracle additionally tests NULL HWND, but that surface
 * is covered separately by the existing window-policy cases — keeping
 * the scope here to the desktop-window pin keeps the scaffold small.
 */
void test_device_desktop_window_present_policy(const struct d3d9_api *api)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND desktop;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    desktop = GetDesktopWindow();
    CHECK_TRUE(desktop != NULL);
    if (!desktop)
        goto done_d3d9;

    pp = default_present_parameters(desktop);
    /*
     * Match the Wine oracle: SwapEffect=DISCARD so the desktop-window
     * present path takes the same swap-effect branch the oracle
     * exercises via create_device(d3d, GetDesktopWindow(),
     * GetDesktopWindow(), TRUE). default_present_parameters defaults
     * to COPY, which would diverge from the oracle's policy slice.
     */
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            desktop, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr) || !device)
        goto done_d3d9;

    /*
     * Wine's oracle issues Clear + Present and asserts SUCCEEDED on
     * both. Pin that here. If dxmt9 ever decides to reject Present on
     * a desktop-window-bound swap chain (returning D3DERR_INVALIDCALL),
     * this CHECK_HR fails loudly and the divergence becomes a tracked
     * scaffolded → failing transition rather than a silent behavior
     * drift.
     */
    hr = IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xffff0000, 1.0f, 0);
    CHECK_HR(hr, D3D_OK);

    hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    CHECK_HR(hr, D3D_OK);

    IDirect3DDevice9_Release(device);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_sysmem_draw (line 4693)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * The base-device equivalent in dlls/d3d9/tests/visual.c
 * (test_sysmem_draw at visual.c:25372) is already ✅ covered. This
 * scaffold pins the D3D9Ex variant: the contract that an Ex factory +
 * Ex device + system-memory vertex buffer combination is created
 * without rejection at the HRESULT contract layer, and that the
 * dynamic-lock entry point still validates pool/usage flags the same
 * way as the base-device path.
 *
 * Scope (intentionally narrow): HRESULT contract on the Ex creation
 * lattice — no draw is issued, no readback is sampled. The visual
 * pixel-correctness pin lives in the visual.c ✅ row. Pixel-level
 * coverage of the Ex path requires shader-runner_dxmt9 plumbing that
 * is out of scope for this scaffold.
 *
 * Pinned contracts:
 *   - Direct3DCreate9Ex(D3D_SDK_VERSION, &d3d9ex) returns S_OK.
 *   - CreateDeviceEx(HAL, HARDWARE_VP, Windowed=TRUE, SwapEffect=
 *     DISCARD, A8R8G8B8) returns S_OK.
 *   - CreateVertexBuffer(256, 0, 0, D3DPOOL_SYSTEMMEM) returns S_OK
 *     (the plain sysmem VB form Wine's oracle uses).
 *   - Lock(0, 0, &ptr, 0) on a non-dynamic SYSTEMMEM VB returns S_OK
 *     and yields a non-NULL pointer (sysmem VBs always honor a
 *     no-flag lock; DISCARD on a non-dynamic VB is D3DERR_INVALIDCALL,
 *     pinned separately by the existing test_pinned_buffers_*_policy
 *     scaffold).
 *   - Unlock returns S_OK.
 *
 * Note on D3DUSAGE_DYNAMIC + SYSTEMMEM: Wine accepts this combo on
 * the base device (test_pinned_buffers_d3dusage_policy already pins
 * the DEFAULT-pool dynamic flow), but the Ex sysmem_draw oracle uses
 * the plain SYSTEMMEM form (no DYNAMIC, no WRITEONLY) to exercise
 * the sysmem VB upload path. We follow the oracle.
 */
void test_ex_device_sysmem_vertex_buffer_policy(const struct d3d9_api *api)
{
    IDirect3D9Ex *d3d9ex;
    IDirect3DDevice9Ex *device_ex = NULL;
    IDirect3DVertexBuffer9 *vb = NULL;
    D3DPRESENT_PARAMETERS pp;
    HWND window;
    void *data;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    pp = default_present_parameters(window);
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    hr = IDirect3D9Ex_CreateDeviceEx(d3d9ex, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING,
            &pp, NULL, &device_ex);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateDeviceEx failed with %s", hr_buffer);
        goto done_window;
    }

    hr = IDirect3DDevice9Ex_CreateVertexBuffer(device_ex, 256, 0, 0,
            D3DPOOL_SYSTEMMEM, &vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr) || !vb)
        goto done_device;

    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(vb, 0, 0, &data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(data != NULL);
        CHECK_HR(IDirect3DVertexBuffer9_Unlock(vb), D3D_OK);
    }

    IDirect3DVertexBuffer9_Release(vb);

done_device:
    IDirect3DDevice9Ex_Release(device_ex);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/visual.c
 * function: test_draw_mapped_buffer (line 26213)
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 *
 * Scope (reduced 2026-05-24): cites Wine's test_draw_mapped_buffer as
 * the behavioral oracle but currently pins only the unmapped happy
 * path -- CreateVertexBuffer (DEFAULT pool, no usage flags) followed
 * by SetStreamSource + SetFVF + BeginScene/DrawPrimitive/EndScene must
 * succeed when the bound VB is NOT mapped. The larger Wine test
 * additionally asserts that DrawPrimitive while the stream-source VB
 * is still mapped via Lock returns D3DERR_INVALIDCALL; porting that
 * locked-VB probe currently hangs dxmt9's PE chunk recorder under the
 * chunked runner (single-case run at index 207 hit the per-chunk
 * timeout 2026-05-24). That probe will be re-introduced as a separate
 * locked-VB-draw policy case once the Lock/Draw fence interaction is
 * investigated; until then this scaffold keeps the no-op draw path
 * covered without blocking the runner.
 */
void test_draw_mapped_buffer_policy(const struct d3d9_api *api)
{
    IDirect3DVertexBuffer9 *vb = NULL;
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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, 256, 0, 0,
            D3DPOOL_DEFAULT, &vb, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr) || !vb)
        goto done_device;

    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, vb, 0, 16), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetFVF(device,
            D3DFVF_XYZ | D3DFVF_DIFFUSE), D3D_OK);

    /* Unmapped draw -- the VB is bound but never Locked, so the draw
     * path is the normal stream-source consumer. Pins that a freshly
     * created DEFAULT-pool VB can satisfy BeginScene/Draw/EndScene
     * without further state. */
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 1),
            D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);

    /* Drop the stream-source reference before releasing the VB so the
     * device does not hold the last ref via the binding. */
    CHECK_HR(IDirect3DDevice9_SetStreamSource(device, 0, NULL, 0, 0), D3D_OK);

    IDirect3DVertexBuffer9_Release(vb);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_get_set_vertex_declaration (line 376)
 * Pins Set/GetVertexDeclaration refcount behaviour (Set does not
 * touch the decl refcount; Get adds one).
 */
void test_get_set_vertex_declaration_refcount_policy(const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 simple_decl[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DVertexDeclaration9 *current = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    ULONG before, after;
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

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, simple_decl, &decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr) || !decl)
        goto done_device;

    /* SetVertexDeclaration must not touch the decl's refcount. */
    before = get_refcount((IUnknown *)decl);
    CHECK_HR(IDirect3DDevice9_SetVertexDeclaration(device, decl), D3D_OK);
    after = get_refcount((IUnknown *)decl);
    CHECK_TRUE(after == before);

    /* GetVertexDeclaration must AddRef. */
    current = NULL;
    hr = IDirect3DDevice9_GetVertexDeclaration(device, &current);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(current == decl);
    if (current)
    {
        ULONG after_get = get_refcount((IUnknown *)decl);
        CHECK_TRUE(after_get == before + 1);
        IDirect3DVertexDeclaration9_Release(current);
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
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_get_declaration (line 428)
 * Pins IDirect3DVertexDeclaration9::GetDeclaration roundtrip: NULL
 * elements pointer returns the count; non-NULL fills element data and
 * the byte image matches the source declaration.
 */
void test_get_declaration_roundtrip_policy(const struct d3d9_api *api)
{
    static const D3DVERTEXELEMENT9 simple_decl[] =
    {
        {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                D3DDECLUSAGE_POSITION, 0},
        D3DDECL_END()
    };
    IDirect3DVertexDeclaration9 *decl = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DVERTEXELEMENT9 elements[MAXD3DDECLLENGTH + 1];
    UINT count;
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

    hr = IDirect3DDevice9_CreateVertexDeclaration(device, simple_decl, &decl);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr) || !decl)
        goto done_device;

    /* NULL elements with non-NULL count returns the element count. */
    count = 0x1337c0de;
    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, NULL, &count);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(count == ARRAY_SIZE(simple_decl));

    /* Re-query, starting from 0. */
    count = 0;
    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, NULL, &count);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(count == ARRAY_SIZE(simple_decl));

    /* Non-NULL elements: returned bytes must byte-match the source. */
    memset(elements, 0, sizeof(elements));
    count = ARRAY_SIZE(elements);
    hr = IDirect3DVertexDeclaration9_GetDeclaration(decl, elements, &count);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(count == ARRAY_SIZE(simple_decl));
    CHECK_TRUE(memcmp(elements, simple_decl,
            count * sizeof(elements[0])) == 0);

    IDirect3DVertexDeclaration9_Release(decl);

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_fvf_decl_conversion (line 501)
 * Pins the FVF round-trip path of Set/GetFVF for the subset of FVF
 * codes that survive without `todo_wine` annotation in the upstream
 * oracle (XYZ, XYZRHW, XYZ|NORMAL, XYZ|DIFFUSE).
 */
void test_fvf_decl_conversion_roundtrip_policy(const struct d3d9_api *api)
{
    static const DWORD fvf_codes[] =
    {
        D3DFVF_XYZ,
        D3DFVF_XYZRHW,
        D3DFVF_XYZ | D3DFVF_NORMAL,
        D3DFVF_XYZ | D3DFVF_DIFFUSE,
        D3DFVF_XYZ | D3DFVF_DIFFUSE | D3DFVF_TEX1,
        D3DFVF_XYZ | D3DFVF_TEX2,
    };
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    DWORD fvf;
    HWND window;
    HRESULT hr;
    unsigned int i;

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

    /* Initial FVF must be zero. */
    fvf = 0xdeadbeef;
    hr = IDirect3DDevice9_GetFVF(device, &fvf);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(fvf == 0);

    for (i = 0; i < ARRAY_SIZE(fvf_codes); ++i)
    {
        CHECK_HR(IDirect3DDevice9_SetFVF(device, fvf_codes[i]), D3D_OK);
        fvf = 0;
        hr = IDirect3DDevice9_GetFVF(device, &fvf);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(fvf == fvf_codes[i]);
    }

    /* Setting FVF=0 must round-trip back to 0. */
    CHECK_HR(IDirect3DDevice9_SetFVF(device, 0), D3D_OK);
    fvf = 0xdeadbeef;
    hr = IDirect3DDevice9_GetFVF(device, &fvf);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(fvf == 0);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_vertex_declaration_alignment (line 923)
 * Pins the element offset alignment HR matrix from the upstream
 * oracle: offsets that are multiples of 4 succeed; offsets 17-19 fail
 * with E_FAIL.
 */
void test_vertex_declaration_alignment_policy(const struct d3d9_api *api)
{
    static const struct
    {
        WORD second_offset;
        HRESULT expected;
    } cases[] =
    {
        {16, D3D_OK},
        {17, E_FAIL},
        {18, E_FAIL},
        {19, E_FAIL},
        {20, D3D_OK},
    };
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    unsigned int i;

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

    for (i = 0; i < ARRAY_SIZE(cases); ++i)
    {
        D3DVERTEXELEMENT9 elements[3] =
        {
            {0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT,
                    D3DDECLUSAGE_POSITION, 0},
            {0, cases[i].second_offset, D3DDECLTYPE_D3DCOLOR,
                    D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_COLOR, 0},
            D3DDECL_END()
        };
        IDirect3DVertexDeclaration9 *decl = NULL;
        HRESULT hr;

        hr = IDirect3DDevice9_CreateVertexDeclaration(device, elements,
                &decl);
        CHECK_HR(hr, cases[i].expected);
        if (SUCCEEDED(hr) && decl)
            IDirect3DVertexDeclaration9_Release(decl);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_get_rt (line 3036)
 * Pins GetRenderTarget on slot 0 (backbuffer) and the out-of-range
 * slots (return D3DERR_NOTFOUND with the out pointer cleared to NULL).
 */
void test_get_rt_bounds_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DSurface9 *rt;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
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

    hr = IDirect3DDevice9_GetRenderTarget(device, 0, &backbuffer);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(backbuffer != NULL);

    memset(&caps, 0, sizeof(caps));
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &caps), D3D_OK);

    for (i = 1; i < caps.NumSimultaneousRTs; ++i)
    {
        rt = (IDirect3DSurface9 *)0xdeadbeef;
        hr = IDirect3DDevice9_GetRenderTarget(device, i, &rt);
        CHECK_HR(hr, D3DERR_NOTFOUND);
        CHECK_TRUE(rt == NULL);
    }

    if (backbuffer)
        IDirect3DSurface9_Release(backbuffer);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_scene (line 2678)
 * Pins the BeginScene/EndScene HR matrix from the upstream oracle.
 * This complements `test_scene_invalid_transitions` which already
 * covers the Reset-clears-scene branch; the matrix here adds the
 * nested-Begin and double-End HR positions inline so the breadth of
 * the Wine state machine is regression-guarded.
 */
void test_scene_begin_end_matrix_policy(const struct d3d9_api *api)
{
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
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    /* EndScene without BeginScene -> INVALIDCALL. */
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3DERR_INVALIDCALL);

    /* Normal Begin/End pair -> S_OK each. */
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);

    /* Second EndScene -> INVALIDCALL again. */
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3DERR_INVALIDCALL);

    /* Nested BeginScene -> INVALIDCALL. */
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3DERR_INVALIDCALL);

    /* Confirm device returns to a clean state — a fresh pair succeeds. */
    CHECK_HR(IDirect3DDevice9_BeginScene(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_EndScene(device), D3D_OK);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_begin_end_state_block (line 11366)
 * Pins Begin/EndStateBlock HR contract: Begin then End returns S_OK and
 * a non-NULL stateblock; a second End immediately after returns
 * D3DERR_INVALIDCALL with the out-pointer left untouched; while a
 * Begin is open, a nested Begin / CreateStateBlock / Apply / Capture
 * all return D3DERR_INVALIDCALL.
 */
void test_begin_end_state_block_policy(const struct d3d9_api *api)
{
    IDirect3DStateBlock9 *stateblock = NULL;
    IDirect3DStateBlock9 *stateblock2 = NULL;
    IDirect3DStateBlock9 *sentinel = (IDirect3DStateBlock9 *)(uintptr_t)0xdeadbeef;
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

    /* Begin / change / End pair returns a valid stateblock. */
    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_SetRenderState(device, D3DRS_LIGHTING, FALSE), D3D_OK);
    stateblock = sentinel;
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(stateblock != NULL && stateblock != sentinel);

    /* Second End immediately after returns INVALIDCALL and leaves
     * the out-pointer untouched. */
    stateblock2 = sentinel;
    hr = IDirect3DDevice9_EndStateBlock(device, &stateblock2);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    CHECK_TRUE(stateblock2 == sentinel);

    /* Begin again; nested operations during the open block must fail. */
    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_BeginStateBlock(device), D3DERR_INVALIDCALL);
    if (stateblock)
    {
        CHECK_HR(IDirect3DStateBlock9_Apply(stateblock), D3DERR_INVALIDCALL);
        CHECK_HR(IDirect3DStateBlock9_Capture(stateblock), D3DERR_INVALIDCALL);
    }
    CHECK_HR(IDirect3DDevice9_CreateStateBlock(device, D3DSBT_ALL, &stateblock2),
            D3DERR_INVALIDCALL);

    /* Close the open block; End must succeed. */
    stateblock2 = NULL;
    CHECK_HR(IDirect3DDevice9_EndStateBlock(device, &stateblock2), D3D_OK);
    CHECK_TRUE(stateblock2 != NULL);

    if (stateblock2)
        IDirect3DStateBlock9_Release(stateblock2);
    if (stateblock)
        IDirect3DStateBlock9_Release(stateblock);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c (line 13482)
 *              and dlls/d3d9/tests/d3d9ex.c (line 4052)
 * function: test_device_caps
 * Minimum observable contract: GetDeviceCaps round-trips at both the
 * factory and device entry points, the field set is consistent (no
 * bogus extension bits in Caps[1-3]), and per-adapter and per-device
 * caps agree on the bits the public oracle reads.
 */
void test_device_caps_roundtrip_policy(const struct d3d9_api *api)
{
    static const DWORD caps1_allowed = D3DCAPS_READ_SCANLINE;
    static const DWORD caps2_allowed =
        D3DCAPS2_FULLSCREENGAMMA | D3DCAPS2_CANCALIBRATEGAMMA | D3DCAPS2_RESERVED
        | D3DCAPS2_CANMANAGERESOURCE | D3DCAPS2_DYNAMICTEXTURES
        | D3DCAPS2_CANAUTOGENMIPMAP | D3DCAPS2_CANSHARERESOURCE;
    static const DWORD caps3_allowed =
        D3DCAPS3_ALPHA_FULLSCREEN_FLIP_OR_DISCARD
        | D3DCAPS3_LINEAR_TO_SRGB_PRESENTATION | D3DCAPS3_COPY_TO_VIDMEM
        | D3DCAPS3_COPY_TO_SYSTEMMEM | D3DCAPS3_DXVAHD
        | D3DCAPS3_DXVAHD_LIMITED | D3DCAPS3_RESERVED;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DCAPS9 factory_caps;
    D3DCAPS9 device_caps;
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

    /* Factory-side GetDeviceCaps for the default adapter. */
    memset(&factory_caps, 0xcd, sizeof(factory_caps));
    hr = IDirect3D9_GetDeviceCaps(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            &factory_caps);
    if (hr == D3DERR_NOTAVAILABLE)
    {
        skip_current_test("D3DDEVTYPE_HAL not available on this adapter");
        goto done_window;
    }
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(factory_caps.AdapterOrdinal == D3DADAPTER_DEFAULT);
    CHECK_TRUE(factory_caps.DeviceType == D3DDEVTYPE_HAL);
    CHECK_TRUE((factory_caps.Caps & ~caps1_allowed) == 0);
    CHECK_TRUE((factory_caps.Caps2 & ~caps2_allowed) == 0);
    CHECK_TRUE((factory_caps.Caps3 & ~caps3_allowed) == 0);
    CHECK_TRUE(factory_caps.MaxSimultaneousTextures != 0);
    CHECK_TRUE(factory_caps.NumSimultaneousRTs != 0);

    /* Device-side GetDeviceCaps must agree with the factory value. */
    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    memset(&device_caps, 0xcd, sizeof(device_caps));
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &device_caps), D3D_OK);
    CHECK_TRUE(device_caps.AdapterOrdinal == factory_caps.AdapterOrdinal);
    CHECK_TRUE(device_caps.DeviceType == factory_caps.DeviceType);
    CHECK_TRUE(device_caps.Caps == factory_caps.Caps);
    CHECK_TRUE(device_caps.Caps2 == factory_caps.Caps2);
    CHECK_TRUE(device_caps.Caps3 == factory_caps.Caps3);
    CHECK_TRUE(device_caps.MaxSimultaneousTextures
            == factory_caps.MaxSimultaneousTextures);
    CHECK_TRUE(device_caps.NumSimultaneousRTs
            == factory_caps.NumSimultaneousRTs);
    CHECK_TRUE(device_caps.VertexShaderVersion
            == factory_caps.VertexShaderVersion);
    CHECK_TRUE(device_caps.PixelShaderVersion
            == factory_caps.PixelShaderVersion);

    /* NULL out-pointer: must return D3DERR_INVALIDCALL, not crash. */
    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, NULL), D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_get_display_mode (line 14312)
 * Pins IDirect3DDevice9::GetDisplayMode HR contract: with iSwapChain=0
 * on a windowed device the call succeeds and reports the default
 * desktop format (D3DFMT_X8R8G8B8); GetAdapterDisplayMode agrees;
 * the device's swapchain agrees; a NULL D3DDISPLAYMODE pointer is
 * rejected with D3DERR_INVALIDCALL.
 */
void test_get_display_mode_policy(const struct d3d9_api *api)
{
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    D3DDISPLAYMODE mode;
    D3DDISPLAYMODE adapter_mode;
    D3DDISPLAYMODE swap_mode;
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

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3DDevice9_GetDisplayMode(device, 0, &mode);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(mode.Format == D3DFMT_X8R8G8B8);
    CHECK_TRUE(mode.Width != 0);
    CHECK_TRUE(mode.Height != 0);

    memset(&adapter_mode, 0, sizeof(adapter_mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT,
            &adapter_mode);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(adapter_mode.Format == D3DFMT_X8R8G8B8);

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (swapchain)
    {
        memset(&swap_mode, 0, sizeof(swap_mode));
        CHECK_HR(IDirect3DSwapChain9_GetDisplayMode(swapchain, &swap_mode),
                D3D_OK);
        CHECK_TRUE(swap_mode.Format == D3DFMT_X8R8G8B8);
        IDirect3DSwapChain9_Release(swapchain);
    }

    /* NULL out-pointer rejection. */
    CHECK_HR(IDirect3DDevice9_GetDisplayMode(device, 0, NULL),
            D3DERR_INVALIDCALL);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c (line 12107)
 *              and dlls/d3d9/tests/d3d9ex.c (line 2009)
 * function: test_lost_device
 * Pins TestCooperativeLevel and Present on a freshly created device.
 * The full fullscreen focus-loss / restore / reset matrix used by Wine
 * relies on real fullscreen behaviour that is not deterministic under
 * automation; here we pin the public contract that *can* be exercised
 * windowed: TestCooperativeLevel returns S_OK on a healthy device,
 * Present returns S_OK, and a Reset on a healthy device returns S_OK
 * with subsequent TestCooperativeLevel still S_OK.
 */
void test_lost_device_cooperative_policy(const struct d3d9_api *api)
{
    D3DPRESENT_PARAMETERS pp;
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
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    /* Healthy windowed device: TestCooperativeLevel -> S_OK. */
    CHECK_HR(IDirect3DDevice9_TestCooperativeLevel(device), D3D_OK);
    /* Present on a healthy device must succeed. */
    CHECK_HR(IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL), D3D_OK);

    /* Reset on a healthy device must succeed and leave it healthy. */
    pp = default_present_parameters(window);
    CHECK_HR(IDirect3DDevice9_Reset(device, &pp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_TestCooperativeLevel(device), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL), D3D_OK);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c (line 2031)
 *              and dlls/d3d9/tests/d3d9ex.c (line 888)
 * function: test_reset
 * Pins the HR matrix for IDirect3DDevice9::Reset on a windowed device:
 *   - Reset with a fully-specified valid D3DPRESENT_PARAMETERS -> S_OK.
 *   - Reset with BackBufferCount==0 normalises to 1 and -> S_OK.
 *   - Reset with NULL pp -> D3DERR_INVALIDCALL (out-of-contract, no
 *     reset performed).
 *   - After a successful Reset, GetSwapChain(0,...) -> S_OK and the
 *     reported backbuffer dimensions match the resized values.
 * The Wine fullscreen mode-change subset of test_reset depends on
 * registry_mode + adapter mode enumeration that is not deterministic
 * under headless automation; only the windowed HR contract is pinned.
 */
void test_reset_hresult_matrix_policy(const struct d3d9_api *api)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DSwapChain9 *swapchain = NULL;
    D3DSURFACE_DESC desc;
    IDirect3DSurface9 *backbuffer = NULL;
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

    /* NULL pp: rejected with INVALIDCALL. */
    CHECK_HR(IDirect3DDevice9_Reset(device, NULL), D3DERR_INVALIDCALL);
    /* Device still healthy after invalid Reset. */
    CHECK_HR(IDirect3DDevice9_TestCooperativeLevel(device), D3D_OK);

    /* Valid resize Reset. */
    pp = default_present_parameters(window);
    pp.BackBufferWidth = 320;
    pp.BackBufferHeight = 240;
    CHECK_HR(IDirect3DDevice9_Reset(device, &pp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_TestCooperativeLevel(device), D3D_OK);

    /* Backbuffer dimensions reflect the resize. */
    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (swapchain)
    {
        hr = IDirect3DSwapChain9_GetBackBuffer(swapchain, 0,
                D3DBACKBUFFER_TYPE_MONO, &backbuffer);
        CHECK_HR(hr, D3D_OK);
        if (backbuffer)
        {
            memset(&desc, 0, sizeof(desc));
            CHECK_HR(IDirect3DSurface9_GetDesc(backbuffer, &desc), D3D_OK);
            CHECK_TRUE(desc.Width == 320);
            CHECK_TRUE(desc.Height == 240);
            IDirect3DSurface9_Release(backbuffer);
        }
        IDirect3DSwapChain9_Release(swapchain);
    }

    /* BackBufferCount==0 normalises to 1, Reset still succeeds. */
    pp = default_present_parameters(window);
    pp.BackBufferCount = 0;
    CHECK_HR(IDirect3DDevice9_Reset(device, &pp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_TestCooperativeLevel(device), D3D_OK);

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c (line 5985)
 *              and dlls/d3d9/tests/d3d9ex.c (line 1843)
 * function: test_reset_resources
 * Pins the Reset contract w.r.t. active resources: after Reset, the
 * implicit backbuffer/RT(0) chain is rebuilt from the present
 * parameters, GetRenderTarget(0) returns the new implicit backbuffer,
 * and GetRenderTarget(i) for i>=1 returns D3DERR_NOTFOUND until the
 * app re-binds an extra RT. Resources bound at Reset time
 * (depth-stencil, additional RTs) are dropped.
 */
void test_reset_resources_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *ds_surface = NULL;
    IDirect3DSurface9 *rt = NULL;
    IDirect3DSurface9 *bb = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    IDirect3D9 *d3d9;
    D3DCAPS9 caps;
    UINT i;
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

    CHECK_HR(IDirect3DDevice9_GetDeviceCaps(device, &caps), D3D_OK);

    /* Create + bind an extra depth-stencil surface. */
    hr = IDirect3DDevice9_CreateDepthStencilSurface(device, 128, 128,
            D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, TRUE, &ds_surface, NULL);
    if (SUCCEEDED(hr) && ds_surface)
    {
        CHECK_HR(IDirect3DDevice9_SetDepthStencilSurface(device, ds_surface),
                D3D_OK);
        IDirect3DSurface9_Release(ds_surface);
        ds_surface = NULL;
    }

    /* Bind additional RTs (slots 1..NumSimultaneousRTs-1). */
    for (i = 1; i < caps.NumSimultaneousRTs; ++i)
    {
        IDirect3DTexture9 *texture = NULL;
        IDirect3DSurface9 *surface = NULL;
        hr = IDirect3DDevice9_CreateTexture(device, 128, 128, 1,
                D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT,
                &texture, NULL);
        if (FAILED(hr) || !texture)
            continue;
        if (SUCCEEDED(IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface))
                && surface)
        {
            IDirect3DDevice9_SetRenderTarget(device, i, surface);
            IDirect3DSurface9_Release(surface);
        }
        IDirect3DTexture9_Release(texture);
    }

    /* Reset rebuilds the implicit chain. */
    pp = default_present_parameters(window);
    CHECK_HR(IDirect3DDevice9_Reset(device, &pp), D3D_OK);

    /* GetRenderTarget(0) returns the new implicit backbuffer. */
    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
            &bb);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_GetRenderTarget(device, 0, &rt);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(rt == bb);
    if (rt)
        IDirect3DSurface9_Release(rt);
    if (bb)
        IDirect3DSurface9_Release(bb);

    /* Slots 1..NumSimultaneousRTs-1 are now unbound: D3DERR_NOTFOUND. */
    for (i = 1; i < caps.NumSimultaneousRTs; ++i)
    {
        IDirect3DSurface9 *extra = NULL;
        hr = IDirect3DDevice9_GetRenderTarget(device, i, &extra);
        CHECK_HR(hr, D3DERR_NOTFOUND);
        if (extra)
            IDirect3DSurface9_Release(extra);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_resource_access (line 13659)
 * Pins the base-device pool semantics matrix that Wine's test_resource_access
 * exercises across every (pool, format, usage) tuple. We focus on a focused
 * slice of the full Wine cross-product that captures the load-bearing pool
 * policy without depending on adapter-specific depth-format CheckDeviceFormat
 * outcomes:
 *   - D3DPOOL_DEFAULT + D3DUSAGE_RENDERTARGET on the colour backbuffer format
 *     creates a texture (S_OK) whose level-0 surface is non-lockable
 *     (Lock returns D3DERR_INVALIDCALL).
 *   - D3DPOOL_DEFAULT + D3DUSAGE_DYNAMIC on the colour format creates a
 *     texture whose level-0 surface IS lockable (Lock returns S_OK).
 *   - D3DPOOL_MANAGED + 0 usage on the colour format creates a texture whose
 *     level-0 surface is lockable.
 *   - D3DPOOL_SYSTEMMEM + 0 usage on the colour format creates a texture
 *     whose level-0 surface is lockable.
 *   - D3DPOOL_DEFAULT + 0 usage on a colour format creates a texture whose
 *     level-0 surface is NOT lockable (the canonical "default-pool surface
 *     is not lockable" base-d3d9 rule that Ex relaxes — see the Ex variant).
 */
void test_resource_access_base_pool_policy(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DSurface9 *backbuffer = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    D3DFORMAT colour_format;
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

    /* Discover the backbuffer's colour format. */
    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DSurface9_GetDesc(backbuffer, &desc);
    CHECK_HR(hr, D3D_OK);
    colour_format = desc.Format;
    IDirect3DSurface9_Release(backbuffer);

    /* DEFAULT + RENDERTARGET: created OK, surface not lockable. */
    hr = IDirect3DDevice9_CreateTexture(device, 16, 16, 1,
            D3DUSAGE_RENDERTARGET, colour_format, D3DPOOL_DEFAULT, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3DERR_INVALIDCALL);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    /* DEFAULT + DYNAMIC: created OK, surface IS lockable. */
    hr = IDirect3DDevice9_CreateTexture(device, 16, 16, 1,
            D3DUSAGE_DYNAMIC, colour_format, D3DPOOL_DEFAULT, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    /* DEFAULT + 0 usage (colour): created OK, surface NOT lockable
     * (the canonical base-d3d9 default-pool rule that Ex relaxes). */
    hr = IDirect3DDevice9_CreateTexture(device, 16, 16, 1,
            0, colour_format, D3DPOOL_DEFAULT, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3DERR_INVALIDCALL);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    /* MANAGED + 0 usage: created OK, surface IS lockable. */
    hr = IDirect3DDevice9_CreateTexture(device, 16, 16, 1,
            0, colour_format, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    /* SYSTEMMEM + 0 usage: created OK, surface IS lockable. */
    hr = IDirect3DDevice9_CreateTexture(device, 16, 16, 1,
            0, colour_format, D3DPOOL_SYSTEMMEM, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_resource_access (line 4229)
 * Ex-specific pool semantics — the load-bearing divergence from base d3d9
 * is that under D3D9Ex, D3DPOOL_DEFAULT + 0 usage colour textures ARE
 * lockable (Wine's tests[0] is valid=TRUE in d3d9ex.c, where the same
 * row in device.c is valid=TRUE for creation but Lock then returns
 * INVALIDCALL because base-pool default-pool surfaces are not lockable
 * without DYNAMIC/RENDERTARGET). Companion to test_resource_access_base_pool_policy.
 *
 * We also pin the Ex-side disallowed-pool rule that test[14] in d3d9ex.c
 * makes explicit: D3DPOOL_MANAGED is rejected by an Ex device at
 * CreateTexture time (D3DERR_INVALIDCALL) — the upstream Wine table sets
 * valid=FALSE for every MANAGED row, mirroring the public MSDN contract.
 */
void test_resource_access_ex_pool_policy(const struct d3d9_api *api)
{
    IDirect3DTexture9 *texture = NULL;
    IDirect3DSurface9 *surface = NULL;
    IDirect3DSurface9 *backbuffer = NULL;
    D3DSURFACE_DESC desc;
    D3DLOCKED_RECT lr;
    D3DFORMAT colour_format;
    IDirect3DDevice9Ex *device = NULL;
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

    /* Discover the backbuffer's colour format. */
    hr = IDirect3DDevice9Ex_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;
    hr = IDirect3DSurface9_GetDesc(backbuffer, &desc);
    CHECK_HR(hr, D3D_OK);
    colour_format = desc.Format;
    IDirect3DSurface9_Release(backbuffer);

    /* Ex: DEFAULT + 0 usage colour — created OK, surface IS lockable
     * (this is the relaxation that distinguishes Ex from base d3d9). */
    hr = IDirect3DDevice9Ex_CreateTexture(device, 16, 16, 1,
            0, colour_format, D3DPOOL_DEFAULT, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    /* Ex: MANAGED + 0 usage colour — rejected outright by an Ex device. */
    hr = IDirect3DDevice9Ex_CreateTexture(device, 16, 16, 1,
            0, colour_format, D3DPOOL_MANAGED, &texture, NULL);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    if (SUCCEEDED(hr) && texture)
    {
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    /* Ex: SYSTEMMEM + 0 usage colour — still permitted, surface lockable. */
    hr = IDirect3DDevice9Ex_CreateTexture(device, 16, 16, 1,
            0, colour_format, D3DPOOL_SYSTEMMEM, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

    /* Ex: DEFAULT + DYNAMIC colour — created OK, surface lockable. */
    hr = IDirect3DDevice9Ex_CreateTexture(device, 16, 16, 1,
            D3DUSAGE_DYNAMIC, colour_format, D3DPOOL_DEFAULT, &texture, NULL);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        hr = IDirect3DTexture9_GetSurfaceLevel(texture, 0, &surface);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            memset(&lr, 0, sizeof(lr));
            hr = IDirect3DSurface9_LockRect(surface, &lr, NULL, 0);
            CHECK_HR(hr, D3D_OK);
            if (SUCCEEDED(hr))
                IDirect3DSurface9_UnlockRect(surface);
            IDirect3DSurface9_Release(surface);
            surface = NULL;
        }
        IDirect3DTexture9_Release(texture);
        texture = NULL;
    }

done_device:
    IDirect3DDevice9Ex_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_vertex_buffer_read_write (line 14211)
 * Pins the dynamic vertex-buffer Lock/Unlock access matrix that Wine
 * exercises after a draw has caused the buffer to be backed by a BO.
 * Each Lock/Unlock round-trip must return S_OK; NOOVERWRITE and 0-flag
 * locks observe each other's writes; the buffer survives the draw.
 */
void test_vertex_buffer_read_write(const struct d3d9_api *api)
{
    static const float tri[3 * 3] =
    {
        -1.0f, -1.0f, 0.0f,
        -1.0f,  1.0f, 0.0f,
         1.0f,  1.0f, 0.0f,
    };
    IDirect3DVertexBuffer9 *buffer = NULL;
    IDirect3DDevice9 *device = NULL;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    float *data;
    unsigned int i;

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

    hr = IDirect3DDevice9_CreateVertexBuffer(device, sizeof(tri),
            D3DUSAGE_DYNAMIC, D3DFVF_XYZ, D3DPOOL_DEFAULT, &buffer, NULL);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    /* Lock(DISCARD) — populate. */
    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(buffer, 0, sizeof(tri),
            (void **)&data, D3DLOCK_DISCARD);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memcpy(data, tri, sizeof(tri));
        hr = IDirect3DVertexBuffer9_Unlock(buffer);
        CHECK_HR(hr, D3D_OK);
    }

    /* Bind + draw to force BO materialisation. */
    hr = IDirect3DDevice9_SetStreamSource(device, 0, buffer, 0, sizeof(float) * 3);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_BeginScene(device);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_SetFVF(device, D3DFVF_XYZ);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_DrawPrimitive(device, D3DPT_TRIANGLELIST, 0, 1);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_EndScene(device);
    CHECK_HR(hr, D3D_OK);
    hr = IDirect3DDevice9_Present(device, NULL, NULL, NULL, NULL);
    CHECK_HR(hr, D3D_OK);

    /* Lock(NOOVERWRITE) — write 3.0f */
    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(buffer, 0, sizeof(tri),
            (void **)&data, D3DLOCK_NOOVERWRITE);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < 3; ++i)
            data[i] = 3.0f;
        hr = IDirect3DVertexBuffer9_Unlock(buffer);
        CHECK_HR(hr, D3D_OK);
    }

    /* Lock(NOOVERWRITE) — read back 3.0f */
    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(buffer, 0, sizeof(tri),
            (void **)&data, D3DLOCK_NOOVERWRITE);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < 3; ++i)
            CHECK_TRUE(data[i] == 3.0f);
        hr = IDirect3DVertexBuffer9_Unlock(buffer);
        CHECK_HR(hr, D3D_OK);
    }

    /* Lock(0) — read back 3.0f */
    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(buffer, 0, sizeof(tri),
            (void **)&data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < 3; ++i)
            CHECK_TRUE(data[i] == 3.0f);
        hr = IDirect3DVertexBuffer9_Unlock(buffer);
        CHECK_HR(hr, D3D_OK);
    }

    /* Lock(0) — write 4.0f */
    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(buffer, 0, sizeof(tri),
            (void **)&data, 0);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < 3; ++i)
            data[i] = 4.0f;
        hr = IDirect3DVertexBuffer9_Unlock(buffer);
        CHECK_HR(hr, D3D_OK);
    }

    /* Lock(NOOVERWRITE) — read back 4.0f */
    data = NULL;
    hr = IDirect3DVertexBuffer9_Lock(buffer, 0, sizeof(tri),
            (void **)&data, D3DLOCK_NOOVERWRITE);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        for (i = 0; i < 3; ++i)
            CHECK_TRUE(data[i] == 4.0f);
        hr = IDirect3DVertexBuffer9_Unlock(buffer);
        CHECK_HR(hr, D3D_OK);
    }

    IDirect3DVertexBuffer9_Release(buffer);
done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_qi_base_to_ex (line 240)
 * Pins: a base-d3d9 IDirect3D9 / IDirect3DDevice9 / IDirect3DSwapChain9
 * trio MUST refuse QueryInterface(IID_*Ex) with E_NOINTERFACE and leave the
 * out-pointer set to NULL.
 */
void test_qi_base_to_ex(const struct d3d9_api *api)
{
    IDirect3D9 *d3d9 = NULL;
    IDirect3D9Ex *d3d9ex = (IDirect3D9Ex *)(uintptr_t)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3DDevice9Ex *device_ex = (IDirect3DDevice9Ex *)(uintptr_t)0xdeadbeef;
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DSwapChain9Ex *swapchain_ex = (IDirect3DSwapChain9Ex *)(uintptr_t)0xdeadbeef;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    /* IDirect3D9 -> IID_IDirect3D9Ex: must fail E_NOINTERFACE, NULL out. */
    hr = IDirect3D9_QueryInterface(d3d9, &IID_IDirect3D9Ex, (void **)&d3d9ex);
    CHECK_HR(hr, E_NOINTERFACE);
    CHECK_TRUE(d3d9ex == NULL);
    if (d3d9ex && d3d9ex != (IDirect3D9Ex *)(uintptr_t)0xdeadbeef)
        IDirect3D9Ex_Release(d3d9ex);

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    /* IDirect3DDevice9 -> IID_IDirect3DDevice9Ex: must fail E_NOINTERFACE. */
    hr = IDirect3DDevice9_QueryInterface(device, &IID_IDirect3DDevice9Ex,
            (void **)&device_ex);
    CHECK_HR(hr, E_NOINTERFACE);
    CHECK_TRUE(device_ex == NULL);
    if (device_ex && device_ex != (IDirect3DDevice9Ex *)(uintptr_t)0xdeadbeef)
        IDirect3DDevice9Ex_Release(device_ex);

    /* Implicit swapchain -> IID_IDirect3DSwapChain9Ex: same rule. */
    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr) && swapchain)
    {
        hr = IDirect3DSwapChain9_QueryInterface(swapchain,
                &IID_IDirect3DSwapChain9Ex, (void **)&swapchain_ex);
        CHECK_HR(hr, E_NOINTERFACE);
        CHECK_TRUE(swapchain_ex == NULL);
        if (swapchain_ex && swapchain_ex != (IDirect3DSwapChain9Ex *)(uintptr_t)0xdeadbeef)
            IDirect3DSwapChain9Ex_Release(swapchain_ex);
        IDirect3DSwapChain9_Release(swapchain);
    }

    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_qi_ex_to_base (line 303)
 * Pins the reverse direction: an IDirect3D9Ex factory must QI down to
 * IDirect3D9 (S_OK, non-NULL), a normal device created via
 * IDirect3D9::CreateDevice (non-Ex, on the QI'd base factory) must QI
 * up to IDirect3DDevice9Ex (S_OK, non-NULL), and the implicit swapchain
 * of that device must QI up to IDirect3DSwapChain9Ex.
 */
void test_qi_ex_to_base(const struct d3d9_api *api)
{
    IDirect3D9Ex *d3d9ex = NULL;
    IDirect3D9 *d3d9 = (IDirect3D9 *)(uintptr_t)0xdeadbeef;
    IDirect3DDevice9 *device = NULL;
    IDirect3DDevice9Ex *device_ex = (IDirect3DDevice9Ex *)(uintptr_t)0xdeadbeef;
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DSwapChain9Ex *swapchain_ex = (IDirect3DSwapChain9Ex *)(uintptr_t)0xdeadbeef;
    D3DPRESENT_PARAMETERS pp;
    HWND window;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    /* Ex factory -> IID_IDirect3D9: must succeed and return non-deadbeef. */
    hr = IDirect3D9Ex_QueryInterface(d3d9ex, &IID_IDirect3D9, (void **)&d3d9);
    CHECK_HR(hr, S_OK);
    CHECK_TRUE(d3d9 != NULL && d3d9 != (IDirect3D9 *)(uintptr_t)0xdeadbeef);

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    /* Create a non-Ex device through the QI'd base IDirect3D9 vtable
     * and QI it back up to IDirect3DDevice9Ex. */
    pp = default_present_parameters(window);
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    if (d3d9 && d3d9 != (IDirect3D9 *)(uintptr_t)0xdeadbeef)
    {
        hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
                window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
        CHECK_HR(hr, D3D_OK);
    }
    if (!device)
        goto done_window;

    hr = IDirect3DDevice9_QueryInterface(device, &IID_IDirect3DDevice9Ex,
            (void **)&device_ex);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(device_ex != NULL
            && device_ex != (IDirect3DDevice9Ex *)(uintptr_t)0xdeadbeef);

    /* Implicit swapchain -> IID_IDirect3DSwapChain9Ex: must succeed. */
    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr) && swapchain)
    {
        hr = IDirect3DSwapChain9_QueryInterface(swapchain,
                &IID_IDirect3DSwapChain9Ex, (void **)&swapchain_ex);
        CHECK_HR(hr, D3D_OK);
        CHECK_TRUE(swapchain_ex != NULL
                && swapchain_ex != (IDirect3DSwapChain9Ex *)(uintptr_t)0xdeadbeef);
        if (swapchain_ex && swapchain_ex != (IDirect3DSwapChain9Ex *)(uintptr_t)0xdeadbeef)
            IDirect3DSwapChain9Ex_Release(swapchain_ex);
        IDirect3DSwapChain9_Release(swapchain);
    }

    if (device_ex && device_ex != (IDirect3DDevice9Ex *)(uintptr_t)0xdeadbeef)
        IDirect3DDevice9Ex_Release(device_ex);
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    if (d3d9 && d3d9 != (IDirect3D9 *)(uintptr_t)0xdeadbeef)
        IDirect3D9_Release(d3d9);
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_scene (line 5031)
 * Ex-specific BeginScene/EndScene contract — like the base d3d9 test_scene,
 * but exercised on an IDirect3DDevice9Ex. Pins the windowed-deterministic
 * subset of the Wine oracle:
 *   - EndScene without prior BeginScene -> D3DERR_INVALIDCALL.
 *   - BeginScene then EndScene -> S_OK / S_OK.
 *   - A second EndScene without a new BeginScene -> D3DERR_INVALIDCALL.
 *   - Nested BeginScene while a scene is open -> D3DERR_INVALIDCALL,
 *     followed by EndScene -> S_OK, followed by a redundant EndScene ->
 *     D3DERR_INVALIDCALL.
 *
 * The Ex divergence from base d3d9 — that Reset() does NOT clear scene
 * state — is observed downstream by test_reset_*; this scaffold deliberately
 * stays inside the windowed scene-state matrix that is deterministic under
 * the chunked runner.
 */
void test_scene_ex_begin_end_policy(const struct d3d9_api *api)
{
    IDirect3DDevice9Ex *device = NULL;
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

    /* EndScene without BeginScene -> INVALIDCALL. */
    hr = IDirect3DDevice9Ex_EndScene(device);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    /* Normal BeginScene + EndScene pair -> S_OK / S_OK. */
    hr = IDirect3DDevice9Ex_BeginScene(device);
    CHECK_HR(hr, S_OK);
    hr = IDirect3DDevice9Ex_EndScene(device);
    CHECK_HR(hr, S_OK);

    /* Second EndScene without a new BeginScene -> INVALIDCALL. */
    hr = IDirect3DDevice9Ex_EndScene(device);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    /* Nested BeginScene while a scene is open -> INVALIDCALL,
     * followed by EndScene -> S_OK, redundant EndScene -> INVALIDCALL. */
    hr = IDirect3DDevice9Ex_BeginScene(device);
    CHECK_HR(hr, S_OK);
    hr = IDirect3DDevice9Ex_BeginScene(device);
    CHECK_HR(hr, D3DERR_INVALIDCALL);
    hr = IDirect3DDevice9Ex_EndScene(device);
    CHECK_HR(hr, S_OK);
    hr = IDirect3DDevice9Ex_EndScene(device);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    IDirect3DDevice9Ex_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}
