/*
 * StateBlock-shaped conformance cases split from d3d9_conformance.c.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/stateblock.c (creation/record/apply paths)
 * - dlls/d3d9/tests/device.c (invalid-call recording transitions)
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 */

#include "d3d9_conformance_fixtures.h"

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
