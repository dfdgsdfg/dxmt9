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
