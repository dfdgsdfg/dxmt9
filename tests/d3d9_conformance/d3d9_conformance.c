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

struct test_case
{
    const char *name;
    void (*func)(const struct d3d9_api *api);
};

static const struct test_case tests[] =
{
    {"factory_base_vs_ex_qi", test_factory_base_vs_ex_qi},
    {"ex_created_normal_device_qi", test_ex_created_normal_device_qi},
    {"display_mode_ex_size_filter_smoke",
            test_display_mode_ex_size_filter_smoke},
    {"scene_invalid_transitions", test_scene_invalid_transitions},
    {"stateblock_invalid_type_recording_invalid_calls",
            test_stateblock_invalid_type_recording_invalid_calls},
    {"private_data_iunknown_ownership_smoke",
            test_private_data_iunknown_ownership_smoke},
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
