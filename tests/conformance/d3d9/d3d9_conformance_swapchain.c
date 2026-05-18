/*
 * Swap-chain / display-mode shaped conformance cases split from
 * d3d9_conformance.c.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/device.c:test_swapchain_parameters
 * - dlls/d3d9/tests/d3d9ex.c:display mode Ex validation
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 */

#include "d3d9_conformance_fixtures.h"

struct present_validation_case
{
    BOOL windowed;
    UINT backbuffer_count;
    D3DSWAPEFFECT swap_effect;
    UINT presentation_interval;
    HRESULT hr;
};

static void check_create_device_present_case(IDirect3D9 *d3d9, HWND window,
        const D3DDISPLAYMODE *mode, const struct present_validation_case *test)
{
    D3DPRESENT_PARAMETERS pp;
    IDirect3DDevice9 *device;
    IDirect3DSwapChain9 *swapchain = NULL;
    D3DPRESENT_PARAMETERS actual;
    HRESULT hr;

    pp = test->windowed ? default_present_parameters(window)
            : default_fullscreen_present_parameters(window, mode);
    pp.SwapEffect = test->swap_effect;
    pp.BackBufferCount = test->backbuffer_count;
    pp.PresentationInterval = test->presentation_interval;

    device = (IDirect3DDevice9 *)0xdeadbeef;
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    CHECK_HR(hr, test->hr);

    if (FAILED(hr))
    {
        CHECK_TRUE(device == NULL);
        return;
    }

    CHECK_TRUE(device != NULL && device != (IDirect3DDevice9 *)0xdeadbeef);
    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_SUCCEEDED(hr);
    if (SUCCEEDED(hr))
    {
        UINT expected_count = test->backbuffer_count ? test->backbuffer_count : 1;

        hr = IDirect3DSwapChain9_GetPresentParameters(swapchain, &actual);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(actual.SwapEffect == test->swap_effect);
            CHECK_TRUE(actual.BackBufferCount == expected_count);
            CHECK_TRUE(actual.Windowed == test->windowed);
            CHECK_TRUE(actual.PresentationInterval == test->presentation_interval);
        }
        IDirect3DSwapChain9_Release(swapchain);
    }
    IDirect3DDevice9_Release(device);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_swapchain_parameters()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_present_parameter_validation(const struct d3d9_api *api)
{
    static const struct present_validation_case tests[] =
    {
        {TRUE,  1, 0,                            D3DPRESENT_INTERVAL_IMMEDIATE, D3DERR_INVALIDCALL},
        {TRUE,  2, D3DSWAPEFFECT_COPY,           D3DPRESENT_INTERVAL_IMMEDIATE, D3DERR_INVALIDCALL},
        {TRUE,  4, D3DSWAPEFFECT_DISCARD,        D3DPRESENT_INTERVAL_IMMEDIATE, D3DERR_INVALIDCALL},
        {TRUE,  1, D3DSWAPEFFECT_FLIPEX,         D3DPRESENT_INTERVAL_IMMEDIATE, D3DERR_INVALIDCALL},
        {TRUE,  1, D3DSWAPEFFECT_DISCARD,        5,                             D3DERR_INVALIDCALL},
        {TRUE,  0, D3DSWAPEFFECT_COPY,           D3DPRESENT_INTERVAL_IMMEDIATE, D3D_OK},
        {TRUE,  3, D3DSWAPEFFECT_DISCARD,        D3DPRESENT_INTERVAL_IMMEDIATE, D3D_OK},
    };
    IDirect3D9 *d3d9;
    D3DDISPLAYMODE mode;
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

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &mode);
    CHECK_SUCCEEDED(hr);
    if (FAILED(hr))
        goto done_window;

    for (i = 0; i < ARRAY_SIZE(tests); ++i)
        check_create_device_present_case(d3d9, window, &mode, &tests[i]);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_window_position()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_fullscreen_window_position_restore(const struct d3d9_api *api)
{
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    D3DDISPLAYMODE mode;
    MONITORINFO monitor_info;
    RECT window_rect;
    HMONITOR monitor;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    BOOL ret;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    monitor = IDirect3D9_GetAdapterMonitor(d3d9, D3DADAPTER_DEFAULT);
    CHECK_TRUE(monitor != NULL);
    if (!monitor)
        goto done_d3d9;

    memset(&monitor_info, 0, sizeof(monitor_info));
    monitor_info.cbSize = sizeof(monitor_info);
    ret = GetMonitorInfoA(monitor, &monitor_info);
    CHECK_TRUE(ret);
    if (!ret)
        goto done_d3d9;

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &mode);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_d3d9;

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    pp = default_fullscreen_present_parameters(window, &mode);
    pp.BackBufferWidth = monitor_info.rcMonitor.right
            - monitor_info.rcMonitor.left;
    pp.BackBufferHeight = monitor_info.rcMonitor.bottom
            - monitor_info.rcMonitor.top;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
    {
        skip_current_test("fullscreen CreateDevice failed");
        goto done_window;
    }

    pump_window_messages();
    ret = GetWindowRect(window, &window_rect);
    CHECK_TRUE(ret);
    if (ret)
        CHECK_TRUE(EqualRect(&window_rect, &monitor_info.rcMonitor));

    ret = SetWindowPos(window, NULL, monitor_info.rcMonitor.left + 11,
            monitor_info.rcMonitor.top + 13, 0, 0,
            SWP_NOZORDER | SWP_NOSIZE | SWP_NOACTIVATE);
    CHECK_TRUE(ret);

    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        pump_window_messages();
        ret = GetWindowRect(window, &window_rect);
        CHECK_TRUE(ret);
        if (ret)
            CHECK_TRUE(EqualRect(&window_rect, &monitor_info.rcMonitor));
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_swapchain_parameters()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_present_parameter_normalization(const struct d3d9_api *api)
{
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS actual;
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
    pp.BackBufferCount = 0;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_window;

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&actual, 0xcc, sizeof(actual));
        hr = IDirect3DSwapChain9_GetPresentParameters(swapchain, &actual);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(actual.hDeviceWindow == window);
            CHECK_TRUE(actual.Windowed == TRUE);
            CHECK_TRUE(actual.SwapEffect == D3DSWAPEFFECT_COPY);
            CHECK_TRUE(actual.BackBufferCount == 1);
            CHECK_TRUE(actual.PresentationInterval ==
                    D3DPRESENT_INTERVAL_IMMEDIATE);
        }
        IDirect3DSwapChain9_Release(swapchain);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/* Wine d3d9ex.c: display mode Ex validates Size and honors filter shape. */
void test_display_mode_ex_size_filter_smoke(const struct d3d9_api *api)
{
    IDirect3D9Ex *d3d9ex;
    IDirect3D9 *d3d9 = NULL;
    D3DDISPLAYMODE mode;
    D3DDISPLAYMODEEX mode_ex;
    D3DDISPLAYMODEFILTER filter;
    D3DDISPLAYROTATION rotation = (D3DDISPLAYROTATION)0xdeadbeef;
    UINT adapter_count;
    UINT mode_count;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    adapter_count = IDirect3D9Ex_GetAdapterCount(d3d9ex);
    if (!adapter_count)
    {
        skip_current_test("no adapters available");
        goto done;
    }

    memset(&mode_ex, 0, sizeof(mode_ex));
    hr = IDirect3D9Ex_GetAdapterDisplayModeEx(d3d9ex, D3DADAPTER_DEFAULT,
            &mode_ex, &rotation);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    memset(&mode_ex, 0, sizeof(mode_ex));
    mode_ex.Size = sizeof(mode_ex);
    hr = IDirect3D9Ex_GetAdapterDisplayModeEx(d3d9ex, adapter_count + 1,
            &mode_ex, &rotation);
    CHECK_HR(hr, D3DERR_INVALIDCALL);

    memset(&mode_ex, 0, sizeof(mode_ex));
    mode_ex.Size = sizeof(mode_ex);
    hr = IDirect3D9Ex_GetAdapterDisplayModeEx(d3d9ex, D3DADAPTER_DEFAULT,
            &mode_ex, &rotation);
    CHECK_SUCCEEDED(hr);
    if (FAILED(hr))
        goto done;

    hr = IDirect3D9Ex_QueryInterface(d3d9ex, &IID_IDirect3D9,
            (void **)&d3d9);
    CHECK_HR(hr, S_OK);
    if (FAILED(hr))
        goto done;

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &mode);
    CHECK_SUCCEEDED(hr);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(mode_ex.Size == sizeof(mode_ex));
        CHECK_TRUE(mode_ex.Width == mode.Width);
        CHECK_TRUE(mode_ex.Height == mode.Height);
        CHECK_TRUE(mode_ex.RefreshRate == mode.RefreshRate);
        CHECK_TRUE(mode_ex.Format == mode.Format);
        CHECK_TRUE(mode_ex.ScanLineOrdering != 0);
    }

    memset(&filter, 0, sizeof(filter));
    filter.Size = sizeof(filter);
    filter.Format = mode_ex.Format;
    mode_count = IDirect3D9Ex_GetAdapterModeCountEx(d3d9ex,
            D3DADAPTER_DEFAULT, &filter);
    CHECK_TRUE(mode_count > 0);

    if (mode_count > 0)
    {
        memset(&mode_ex, 0, sizeof(mode_ex));
        mode_ex.Size = sizeof(mode_ex);
        hr = IDirect3D9Ex_EnumAdapterModesEx(d3d9ex, D3DADAPTER_DEFAULT,
                &filter, 0, &mode_ex);
        CHECK_SUCCEEDED(hr);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(mode_ex.Size == sizeof(mode_ex));
            CHECK_TRUE(mode_ex.Format == filter.Format);
        }
    }

done:
    if (d3d9)
        IDirect3D9_Release(d3d9);
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * functions: test_get_adapter_luid(), test_get_adapter_displaymode_ex()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_adapter_luid_display_mode(const struct d3d9_api *api)
{
    D3DDISPLAYROTATION rotation = (D3DDISPLAYROTATION)0xdeadbeef;
    IDirect3D9Ex *d3d9ex;
    IDirect3D9 *d3d9 = NULL;
    D3DDISPLAYMODEEX mode_ex;
    D3DDISPLAYMODE mode;
    UINT adapter_count;
    LUID luid_a;
    LUID luid_b;
    HRESULT hr;

    d3d9ex = create_d3d9ex(api);
    if (!d3d9ex)
        return;

    adapter_count = IDirect3D9Ex_GetAdapterCount(d3d9ex);
    if (!adapter_count)
    {
        skip_current_test("no adapters available");
        goto done;
    }

    memset(&luid_a, 0xcc, sizeof(luid_a));
    CHECK_HR(IDirect3D9Ex_GetAdapterLUID(d3d9ex, D3DADAPTER_DEFAULT,
            &luid_a), D3D_OK);
    memset(&luid_b, 0, sizeof(luid_b));
    CHECK_HR(IDirect3D9Ex_GetAdapterLUID(d3d9ex, D3DADAPTER_DEFAULT,
            &luid_b), D3D_OK);
    CHECK_TRUE(luid_a.LowPart == luid_b.LowPart);
    CHECK_TRUE(luid_a.HighPart == luid_b.HighPart);
    CHECK_HR(IDirect3D9Ex_GetAdapterLUID(d3d9ex, adapter_count, &luid_b),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3D9Ex_GetAdapterLUID(d3d9ex, D3DADAPTER_DEFAULT, NULL),
            D3DERR_INVALIDCALL);

    memset(&mode_ex, 0, sizeof(mode_ex));
    mode_ex.Size = sizeof(mode_ex);
    hr = IDirect3D9Ex_GetAdapterDisplayModeEx(d3d9ex, D3DADAPTER_DEFAULT,
            &mode_ex, &rotation);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done;

    hr = IDirect3D9Ex_QueryInterface(d3d9ex, &IID_IDirect3D9,
            (void **)&d3d9);
    CHECK_HR(hr, S_OK);
    if (FAILED(hr))
        goto done;

    memset(&mode, 0, sizeof(mode));
    hr = IDirect3D9_GetAdapterDisplayMode(d3d9, D3DADAPTER_DEFAULT, &mode);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(mode_ex.Width == mode.Width);
        CHECK_TRUE(mode_ex.Height == mode.Height);
        CHECK_TRUE(mode_ex.RefreshRate == mode.RefreshRate);
        CHECK_TRUE(mode_ex.Format == mode.Format);
    }
    CHECK_TRUE(rotation != (D3DDISPLAYROTATION)0xdeadbeef);

    memset(&mode_ex, 0, sizeof(mode_ex));
    CHECK_HR(IDirect3D9Ex_GetAdapterDisplayModeEx(d3d9ex,
            D3DADAPTER_DEFAULT, &mode_ex, &rotation), D3DERR_INVALIDCALL);
    mode_ex.Size = sizeof(mode_ex);
    CHECK_HR(IDirect3D9Ex_GetAdapterDisplayModeEx(d3d9ex, adapter_count,
            &mode_ex, &rotation), D3DERR_INVALIDCALL);

done:
    if (d3d9)
        IDirect3D9_Release(d3d9);
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_swapchain_get_displaymode_ex()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_swapchain_display_mode(const struct d3d9_api *api)
{
    IDirect3DSwapChain9Ex *swapchain_ex = NULL;
    D3DDISPLAYROTATION rotation;
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DDevice9Ex *device = NULL;
    D3DDISPLAYMODEEX mode_ex;
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

    memset(&mode_ex, 0, sizeof(mode_ex));
    CHECK_HR(IDirect3DDevice9Ex_GetDisplayModeEx(device, 0, &mode_ex,
            &rotation), D3DERR_INVALIDCALL);

    memset(&mode_ex, 0, sizeof(mode_ex));
    mode_ex.Size = sizeof(mode_ex);
    rotation = (D3DDISPLAYROTATION)0xdeadbeef;
    hr = IDirect3DDevice9Ex_GetDisplayModeEx(device, 0, &mode_ex,
            &rotation);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(mode_ex.Width > 0);
        CHECK_TRUE(mode_ex.Height > 0);
        CHECK_TRUE(mode_ex.Format != D3DFMT_UNKNOWN);
        CHECK_TRUE(rotation != (D3DDISPLAYROTATION)0xdeadbeef);
    }

    CHECK_HR(IDirect3DDevice9Ex_GetDisplayModeEx(device, 1, &mode_ex,
            &rotation), D3DERR_INVALIDCALL);

    hr = IDirect3DDevice9Ex_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DSwapChain9_QueryInterface(swapchain,
            &IID_IDirect3DSwapChain9Ex, (void **)&swapchain_ex);
    CHECK_HR(hr, S_OK);
    if (FAILED(hr))
        goto done_device;

    memset(&mode_ex, 0, sizeof(mode_ex));
    CHECK_HR(IDirect3DSwapChain9Ex_GetDisplayModeEx(swapchain_ex, &mode_ex,
            &rotation), D3DERR_INVALIDCALL);

    memset(&mode_ex, 0, sizeof(mode_ex));
    mode_ex.Size = sizeof(mode_ex);
    rotation = (D3DDISPLAYROTATION)0xdeadbeef;
    hr = IDirect3DSwapChain9Ex_GetDisplayModeEx(swapchain_ex, &mode_ex,
            &rotation);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        CHECK_TRUE(mode_ex.Width > 0);
        CHECK_TRUE(mode_ex.Height > 0);
        CHECK_TRUE(mode_ex.Format != D3DFMT_UNKNOWN);
        CHECK_TRUE(rotation != (D3DDISPLAYROTATION)0xdeadbeef);
    }

done_device:
    if (swapchain_ex)
        IDirect3DSwapChain9Ex_Release(swapchain_ex);
    if (swapchain)
        IDirect3DSwapChain9_Release(swapchain);
    IDirect3DDevice9Ex_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * function: test_frame_latency()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_ex_frame_latency_state(const struct d3d9_api *api)
{
    IDirect3DDevice9Ex *device = NULL;
    IDirect3D9Ex *d3d9ex;
    UINT latency;
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

    latency = 0xdeadbeef;
    hr = IDirect3DDevice9Ex_GetMaximumFrameLatency(device, &latency);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
        CHECK_TRUE(latency == 3);

    CHECK_HR(IDirect3DDevice9Ex_GetMaximumFrameLatency(device, NULL),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9Ex_SetMaximumFrameLatency(device, 0),
            D3DERR_INVALIDCALL);
    CHECK_HR(IDirect3DDevice9Ex_SetMaximumFrameLatency(device, 31),
            D3DERR_INVALIDCALL);

    CHECK_HR(IDirect3DDevice9Ex_SetMaximumFrameLatency(device, 1), D3D_OK);
    latency = 0;
    hr = IDirect3DDevice9Ex_GetMaximumFrameLatency(device, &latency);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(latency == 1);

    CHECK_HR(IDirect3DDevice9Ex_SetMaximumFrameLatency(device, 3), D3D_OK);
    latency = 0;
    hr = IDirect3DDevice9Ex_GetMaximumFrameLatency(device, &latency);
    CHECK_HR(hr, D3D_OK);
    CHECK_TRUE(latency == 3);

    IDirect3DDevice9Ex_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9Ex_Release(d3d9ex);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_lockable_backbuffer()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_lockable_backbuffer_lock_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
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

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    pp = default_present_parameters(window);
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_window;

    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
            &backbuffer);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        hr = IDirect3DSurface9_LockRect(backbuffer, &locked, NULL,
                D3DLOCK_READONLY);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(locked.pBits != NULL);
            CHECK_TRUE(locked.Pitch != 0);
            CHECK_HR(IDirect3DSurface9_UnlockRect(backbuffer), D3D_OK);
        }
        IDirect3DSurface9_Release(backbuffer);
        backbuffer = NULL;
    }

    IDirect3DDevice9_Release(device);
    device = NULL;

    pp = default_present_parameters(window);
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_window;

    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0, D3DBACKBUFFER_TYPE_MONO,
            &backbuffer);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        CHECK_HR(IDirect3DSurface9_LockRect(backbuffer, &locked, NULL,
                D3DLOCK_READONLY), D3DERR_INVALIDCALL);
        IDirect3DSurface9_Release(backbuffer);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_lockable_backbuffer()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_nonlockable_backbuffer_getdc_policy(const struct d3d9_api *api)
{
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DLOCKED_RECT locked;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;
    HDC dc;

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

    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0,
            D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        CHECK_HR(IDirect3DSurface9_LockRect(backbuffer, &locked, NULL,
                D3DLOCK_DISCARD), D3DERR_INVALIDCALL);

        dc = (HDC)(ULONG_PTR)0xdeadbeef;
        CHECK_HR(IDirect3DSurface9_GetDC(backbuffer, &dc),
                D3DERR_INVALIDCALL);
        CHECK_TRUE(dc == (HDC)(ULONG_PTR)0xdeadbeef);

        IDirect3DSurface9_Release(backbuffer);
    }

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_lockable_backbuffer()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_reset_lockable_backbuffer_policy(const struct d3d9_api *api)
{
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DSurface9 *backbuffer = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS actual;
    D3DPRESENT_PARAMETERS pp;
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

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    device = create_base_device(d3d9, window);
    if (!device)
        goto done_window;

    pp = default_present_parameters(window);
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.Flags = D3DPRESENTFLAG_LOCKABLE_BACKBUFFER;

    hr = IDirect3DDevice9_Reset(device, &pp);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_device;

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&actual, 0xcc, sizeof(actual));
        hr = IDirect3DSwapChain9_GetPresentParameters(swapchain, &actual);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE((actual.Flags & D3DPRESENTFLAG_LOCKABLE_BACKBUFFER) != 0);
            CHECK_TRUE(actual.SwapEffect == D3DSWAPEFFECT_DISCARD);
        }
        IDirect3DSwapChain9_Release(swapchain);
    }

    hr = IDirect3DDevice9_GetBackBuffer(device, 0, 0,
            D3DBACKBUFFER_TYPE_MONO, &backbuffer);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&locked, 0xcc, sizeof(locked));
        hr = IDirect3DSurface9_LockRect(backbuffer, &locked, NULL,
                D3DLOCK_DISCARD);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
        {
            CHECK_TRUE(locked.pBits != NULL);
            CHECK_TRUE(locked.Pitch != 0);
            CHECK_HR(IDirect3DSurface9_UnlockRect(backbuffer), D3D_OK);
        }
        IDirect3DSurface9_Release(backbuffer);
    }

done_device:
    IDirect3DDevice9_Release(device);
done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * function: test_swapchain_multisample_reset()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
void test_swapchain_multisample_reset(const struct d3d9_api *api)
{
    IDirect3DSwapChain9 *swapchain = NULL;
    IDirect3DDevice9 *device = NULL;
    D3DPRESENT_PARAMETERS pp;
    DWORD quality_levels = 0;
    IDirect3D9 *d3d9;
    HWND window;
    HRESULT hr;

    d3d9 = api->create9(D3D_SDK_VERSION);
    if (!d3d9)
    {
        skip_current_test("Direct3DCreate9 returned NULL");
        return;
    }

    hr = IDirect3D9_CheckDeviceMultiSampleType(d3d9, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, D3DFMT_A8R8G8B8, TRUE,
            D3DMULTISAMPLE_2_SAMPLES, &quality_levels);
    if (hr == D3DERR_NOTAVAILABLE || !quality_levels)
    {
        skip_current_test("2x multisample A8R8G8B8 swapchains are unavailable");
        goto done_d3d9;
    }
    CHECK_HR(hr, D3D_OK);

    window = create_test_window();
    CHECK_TRUE(window != NULL);
    if (!window)
        goto done_d3d9;

    pp = default_present_parameters(window);
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    CHECK_HR(hr, D3D_OK);
    if (FAILED(hr))
        goto done_window;

    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xffffffff, 1.0f, 0), D3D_OK);

    hr = IDirect3DDevice9_GetSwapChain(device, 0, &swapchain);
    CHECK_HR(hr, D3D_OK);
    if (SUCCEEDED(hr))
    {
        memset(&pp, 0xcc, sizeof(pp));
        hr = IDirect3DSwapChain9_GetPresentParameters(swapchain, &pp);
        CHECK_HR(hr, D3D_OK);
        if (SUCCEEDED(hr))
            CHECK_TRUE(pp.MultiSampleType == D3DMULTISAMPLE_NONE);
        IDirect3DSwapChain9_Release(swapchain);
    }

    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.MultiSampleType = D3DMULTISAMPLE_2_SAMPLES;
    pp.MultiSampleQuality = quality_levels - 1;
    CHECK_HR(IDirect3DDevice9_Reset(device, &pp), D3D_OK);
    CHECK_HR(IDirect3DDevice9_Clear(device, 0, NULL, D3DCLEAR_TARGET,
            0xffffffff, 1.0f, 0), D3D_OK);

    IDirect3DDevice9_Release(device);

done_window:
    DestroyWindow(window);
done_d3d9:
    IDirect3D9_Release(d3d9);
}
