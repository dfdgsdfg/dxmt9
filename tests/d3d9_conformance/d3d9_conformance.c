/*
 * Focused D3D9 PE conformance harness derived from Wine:
 *   dlls/d3d9/tests/d3d9ex.c
 *   dlls/d3d9/tests/device.c
 *   dlls/d3d9/tests/stateblock.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * This keeps the slice small and self-contained: factory/base-vs-Ex QI,
 * Ex-created normal device QI, display-mode Ex size/filter smoke,
 * BeginScene/EndScene invalid transitions, stateblock invalid type and
 * recording invalid calls, and D3DSPD_IUNKNOWN ownership smoke.
 */

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT sdk_version);
typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT sdk_version,
        IDirect3D9Ex **d3d9ex);

struct d3d9_api
{
    HMODULE module;
    PFN_Direct3DCreate9 create9;
    PFN_Direct3DCreate9Ex create9ex;
};

static const GUID private_data_guid =
{
    0x9f1f9f4d, 0x4b01, 0x4a28,
    {0x93, 0x5e, 0x76, 0xc5, 0x69, 0x2f, 0x3d, 0x91}
};

static const char *current_test;
static unsigned int checks_run;
static unsigned int checks_failed;
static unsigned int tests_skipped;

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

static void print_hr(char *buffer, size_t size, HRESULT hr)
{
    snprintf(buffer, size, "0x%08lx", (unsigned long)(DWORD)hr);
}

static void report_failure(const char *file, int line, const char *fmt, ...)
{
    va_list args;

    ++checks_failed;
    printf("FAIL %s:%d [%s] ", file, line, current_test);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static void check_true_at(const char *file, int line, bool condition,
        const char *expr)
{
    ++checks_run;
    if (!condition)
        report_failure(file, line, "expected true: %s", expr);
}

static void check_hr_at(const char *file, int line, HRESULT actual,
        HRESULT expected, const char *expr)
{
    char actual_buffer[16];
    char expected_buffer[16];

    ++checks_run;
    if (actual == expected)
        return;

    print_hr(actual_buffer, sizeof(actual_buffer), actual);
    print_hr(expected_buffer, sizeof(expected_buffer), expected);
    report_failure(file, line, "%s returned %s, expected %s",
            expr, actual_buffer, expected_buffer);
}

static void check_succeeded_at(const char *file, int line, HRESULT actual,
        const char *expr)
{
    char actual_buffer[16];

    ++checks_run;
    if (SUCCEEDED(actual))
        return;

    print_hr(actual_buffer, sizeof(actual_buffer), actual);
    report_failure(file, line, "%s failed with %s", expr, actual_buffer);
}

#define CHECK_TRUE(expr) check_true_at(__FILE__, __LINE__, !!(expr), #expr)
#define CHECK_HR(expr, expected) \
    do { HRESULT hr__ = (expr); check_hr_at(__FILE__, __LINE__, hr__, (expected), #expr); } while (0)
#define CHECK_SUCCEEDED(expr) \
    do { HRESULT hr__ = (expr); check_succeeded_at(__FILE__, __LINE__, hr__, #expr); } while (0)

static void skip_current_test(const char *fmt, ...)
{
    va_list args;

    ++tests_skipped;
    printf("SKIP [%s] ", current_test);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static void strip_filename(char *path)
{
    char *slash = strrchr(path, '\\');
    char *alt_slash = strrchr(path, '/');

    if (!slash || alt_slash > slash)
        slash = alt_slash;
    if (slash)
        *slash = '\0';
}

static HMODULE load_d3d9_module(void)
{
    char exe_path[MAX_PATH];
    char candidate[MAX_PATH];
    HMODULE module;

    if (GetModuleFileNameA(NULL, exe_path, sizeof(exe_path)))
    {
        strip_filename(exe_path);

        snprintf(candidate, sizeof(candidate), "%s\\d3d9.dll", exe_path);
        module = LoadLibraryA(candidate);
        if (module)
            return module;

        snprintf(candidate, sizeof(candidate),
                "%s\\..\\..\\src\\win32\\d3d9.dll", exe_path);
        module = LoadLibraryA(candidate);
        if (module)
            return module;
    }

    return LoadLibraryA("d3d9.dll");
}

static bool load_d3d9_api(struct d3d9_api *api)
{
    memset(api, 0, sizeof(*api));

    api->module = load_d3d9_module();
    if (!api->module)
    {
        printf("SKIP failed to load d3d9.dll, GetLastError=%lu\n",
                GetLastError());
        return false;
    }

    api->create9 = (PFN_Direct3DCreate9)GetProcAddress(api->module,
            "Direct3DCreate9");
    api->create9ex = (PFN_Direct3DCreate9Ex)GetProcAddress(api->module,
            "Direct3DCreate9Ex");

    if (!api->create9)
    {
        printf("SKIP d3d9.dll does not export Direct3DCreate9\n");
        return false;
    }

    return true;
}

static HWND create_test_window(void)
{
    RECT rect = {0, 0, 640, 480};

    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW | WS_VISIBLE, FALSE);
    return CreateWindowA("static", "dxmt9-d3d9-conformance",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
            rect.right - rect.left, rect.bottom - rect.top,
            NULL, NULL, NULL, NULL);
}

static void pump_window_messages(void)
{
    MSG msg;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);
}

static D3DPRESENT_PARAMETERS default_present_parameters(HWND window)
{
    D3DPRESENT_PARAMETERS pp;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = TRUE;
    pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_COPY;
    pp.BackBufferWidth = 640;
    pp.BackBufferHeight = 480;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return pp;
}

static D3DPRESENT_PARAMETERS default_fullscreen_present_parameters(HWND window,
        const D3DDISPLAYMODE *mode)
{
    D3DPRESENT_PARAMETERS pp;

    memset(&pp, 0, sizeof(pp));
    pp.Windowed = FALSE;
    pp.hDeviceWindow = window;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferWidth = mode->Width;
    pp.BackBufferHeight = mode->Height;
    pp.BackBufferFormat = mode->Format;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_DEFAULT;
    return pp;
}

static IDirect3DDevice9 *create_base_device(IDirect3D9 *d3d9, HWND window)
{
    D3DPRESENT_PARAMETERS pp = default_present_parameters(window);
    IDirect3DDevice9 *device = NULL;
    HRESULT hr;

    hr = IDirect3D9_CreateDevice(d3d9, D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL,
            window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateDevice failed with %s", hr_buffer);
        return NULL;
    }

    return device;
}

static IDirect3D9Ex *create_d3d9ex(const struct d3d9_api *api)
{
    IDirect3D9Ex *d3d9ex = NULL;
    HRESULT hr;

    if (!api->create9ex)
    {
        skip_current_test("Direct3DCreate9Ex is not exported");
        return NULL;
    }

    hr = api->create9ex(D3D_SDK_VERSION, &d3d9ex);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("Direct3DCreate9Ex failed with %s", hr_buffer);
        return NULL;
    }

    return d3d9ex;
}

static ULONG get_refcount(IUnknown *object)
{
    IUnknown_AddRef(object);
    return IUnknown_Release(object);
}

/*
 * Wine provenance: dlls/d3d9/tests/device.c
 * functions: test_display_modes(), test_check_device_type(),
 * test_check_device_format(), test_checkdevicemultisampletype()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
static void test_factory_validation_return_codes(const struct d3d9_api *api)
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
static void test_device_display_mode_adapter_format(const struct d3d9_api *api)
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
static void test_present_parameter_validation(const struct d3d9_api *api)
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

static IDirect3DDevice9Ex *create_ex_device(IDirect3D9Ex *d3d9ex, HWND window)
{
    D3DPRESENT_PARAMETERS pp = default_present_parameters(window);
    IDirect3DDevice9Ex *device = NULL;
    HRESULT hr;

    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_A8R8G8B8;
    hr = IDirect3D9Ex_CreateDeviceEx(d3d9ex, D3DADAPTER_DEFAULT,
            D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING,
            &pp, NULL, &device);
    if (FAILED(hr))
    {
        char hr_buffer[16];
        print_hr(hr_buffer, sizeof(hr_buffer), hr);
        skip_current_test("CreateDeviceEx failed with %s", hr_buffer);
        return NULL;
    }

    return device;
}

/*
 * Wine provenance: dlls/d3d9/tests/d3d9ex.c
 * functions: test_swapchain_parameters(), test_reset_ex()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
static void test_ex_create_reset_mode_validation(const struct d3d9_api *api)
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
static void test_factory_base_vs_ex_qi(const struct d3d9_api *api)
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
static void test_ex_created_normal_device_qi(const struct d3d9_api *api)
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

/* Wine d3d9ex.c: display mode Ex validates Size and honors filter shape. */
static void test_display_mode_ex_size_filter_smoke(const struct d3d9_api *api)
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

/* Wine device.c: invalid BeginScene/EndScene transitions and Reset clearing. */
static void test_scene_invalid_transitions(const struct d3d9_api *api)
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
 * Wine stateblock.c covers stateblock creation/record/apply paths. Wine
 * device.c adds the invalid-call recording transitions kept in this slice.
 */
static void test_stateblock_invalid_type_recording_invalid_calls(
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
static void test_private_data_iunknown_ownership_smoke(
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
static void test_private_data_resource_wrappers(const struct d3d9_api *api)
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
 * function: test_shared_handle()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
static void test_shared_handle_policy(const struct d3d9_api *api)
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
static void test_ex_shared_handle_policy(const struct d3d9_api *api)
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
 * function: test_format_unknown()
 * commit: 6e073d28dee3af7f4c965daec94644e0f9f92727
 */
static void test_creation_failure_out_pointers(const struct d3d9_api *api)
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

struct test_case
{
    const char *name;
    void (*func)(const struct d3d9_api *api);
};

static const struct test_case tests[] =
{
    {"factory_validation_return_codes", test_factory_validation_return_codes},
    {"device_display_mode_adapter_format",
            test_device_display_mode_adapter_format},
    {"factory_base_vs_ex_qi", test_factory_base_vs_ex_qi},
    {"ex_created_normal_device_qi", test_ex_created_normal_device_qi},
    {"display_mode_ex_size_filter_smoke",
            test_display_mode_ex_size_filter_smoke},
    {"present_parameter_validation", test_present_parameter_validation},
    {"ex_create_reset_mode_validation",
            test_ex_create_reset_mode_validation},
    {"scene_invalid_transitions", test_scene_invalid_transitions},
    {"stateblock_invalid_type_recording_invalid_calls",
            test_stateblock_invalid_type_recording_invalid_calls},
    {"private_data_iunknown_ownership_smoke",
            test_private_data_iunknown_ownership_smoke},
    {"private_data_resource_wrappers",
            test_private_data_resource_wrappers},
    {"shared_handle_policy", test_shared_handle_policy},
    {"ex_shared_handle_policy", test_ex_shared_handle_policy},
    {"creation_failure_out_pointers", test_creation_failure_out_pointers},
};

int main(void)
{
    struct d3d9_api api;
    unsigned int i;

    if (!load_d3d9_api(&api))
        return 77;

    for (i = 0; i < sizeof(tests) / sizeof(tests[0]); ++i)
    {
        unsigned int failures_before = checks_failed;

        current_test = tests[i].name;
        printf("RUN  [%s]\n", current_test);
        tests[i].func(&api);
        pump_window_messages();
        printf("%s [%s]\n", checks_failed == failures_before ? "PASS" : "FAIL",
                current_test);
    }

    printf("SUMMARY checks=%u failures=%u skips=%u\n",
            checks_run, checks_failed, tests_skipped);

    if (api.module)
        FreeLibrary(api.module);

    return checks_failed ? 1 : 0;
}
