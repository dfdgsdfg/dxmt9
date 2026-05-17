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
