/*
 * Shared harness for the d3d9 PE conformance slice.
 *
 * Counters (current_test, checks_run, checks_failed, tests_skipped) are
 * defined exactly once in d3d9_conformance.c and declared extern here so
 * each domain bucket TU references the same instances.
 *
 * All other helpers are static inline so multiple TUs can include this
 * header without ODR violations in C.
 */

#ifndef DXMT9_TESTS_D3D9_CONFORMANCE_FIXTURES_H
#define DXMT9_TESTS_D3D9_CONFORMANCE_FIXTURES_H

#define WIN32_LEAN_AND_MEAN
#define COBJMACROS
#include <windows.h>
#include <initguid.h>
#include <d3d9.h>

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef ARRAY_SIZE
#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#endif

typedef IDirect3D9 *(WINAPI *PFN_Direct3DCreate9)(UINT sdk_version);
typedef HRESULT (WINAPI *PFN_Direct3DCreate9Ex)(UINT sdk_version,
        IDirect3D9Ex **d3d9ex);

struct d3d9_api
{
    HMODULE module;
    PFN_Direct3DCreate9 create9;
    PFN_Direct3DCreate9Ex create9ex;
};

struct test_case
{
    const char *name;
    void (*func)(const struct d3d9_api *api);
};

/*
 * Single counter set shared across every bucket TU. Definitions live in
 * d3d9_conformance.c next to main(); per-bucket files only reference them.
 */
extern const char *current_test;
extern unsigned int checks_run;
extern unsigned int checks_failed;
extern unsigned int tests_skipped;

/*
 * Stable GUID used by SetPrivateData round-trip tests. Defined as static
 * const so each TU that includes this header has its own copy; the value
 * is identical and only used as an opaque key passed back to D3D9.
 */
static const GUID private_data_guid =
{
    0x9f1f9f4d, 0x4b01, 0x4a28,
    {0x93, 0x5e, 0x76, 0xc5, 0x69, 0x2f, 0x3d, 0x91}
};

static inline void print_hr(char *buffer, size_t size, HRESULT hr)
{
    snprintf(buffer, size, "0x%08lx", (unsigned long)(DWORD)hr);
}

static inline void report_failure(const char *file, int line, const char *fmt, ...)
{
    va_list args;

    ++checks_failed;
    printf("FAIL %s:%d [%s] ", file, line, current_test);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static inline void check_true_at(const char *file, int line, bool condition,
        const char *expr)
{
    ++checks_run;
    if (!condition)
        report_failure(file, line, "expected true: %s", expr);
}

static inline void check_hr_at(const char *file, int line, HRESULT actual,
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

static inline void check_succeeded_at(const char *file, int line, HRESULT actual,
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

static inline void skip_current_test(const char *fmt, ...)
{
    va_list args;

    ++tests_skipped;
    printf("SKIP [%s] ", current_test);
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
    printf("\n");
}

static inline void strip_filename(char *path)
{
    char *slash = strrchr(path, '\\');
    char *alt_slash = strrchr(path, '/');

    if (!slash || alt_slash > slash)
        slash = alt_slash;
    if (slash)
        *slash = '\0';
}

static inline HMODULE load_d3d9_module(void)
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

static inline bool load_d3d9_api(struct d3d9_api *api)
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

static inline HWND create_test_window(void)
{
    RECT rect = {0, 0, 640, 480};

    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW | WS_VISIBLE, FALSE);
    return CreateWindowA("static", "dxmt9-d3d9-conformance",
            WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0,
            rect.right - rect.left, rect.bottom - rect.top,
            NULL, NULL, NULL, NULL);
}

static inline void pump_window_messages(void)
{
    MSG msg;

    while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE))
        DispatchMessageA(&msg);
}

static inline D3DPRESENT_PARAMETERS default_present_parameters(HWND window)
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

static inline D3DPRESENT_PARAMETERS default_fullscreen_present_parameters(HWND window,
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

static inline IDirect3DDevice9 *create_base_device(IDirect3D9 *d3d9, HWND window)
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

static inline IDirect3D9Ex *create_d3d9ex(const struct d3d9_api *api)
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

static inline IDirect3DDevice9Ex *create_ex_device(IDirect3D9Ex *d3d9ex, HWND window)
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

static inline ULONG get_refcount(IUnknown *object)
{
    IUnknown_AddRef(object);
    return IUnknown_Release(object);
}

/* Per-domain test entries for main()'s dispatch table. */
void test_factory_validation_return_codes(const struct d3d9_api *api);
void test_factory_caps_edge_matrix(const struct d3d9_api *api);
void test_invalid_multisample_render_target_quality(const struct d3d9_api *api);
void test_device_display_mode_adapter_format(const struct d3d9_api *api);
void test_factory_base_vs_ex_qi(const struct d3d9_api *api);
void test_ex_created_normal_device_qi(const struct d3d9_api *api);
void test_ex_create_reset_mode_validation(const struct d3d9_api *api);
void test_scene_invalid_transitions(const struct d3d9_api *api);
void test_vertex_declaration_fvf_policy(const struct d3d9_api *api);
void test_fvf_decl_management(const struct d3d9_api *api);
void test_unused_declaration_type(const struct d3d9_api *api);
void test_get_set_vertex_shader(const struct d3d9_api *api);
void test_vertex_shader_constant(const struct d3d9_api *api);
void test_get_set_pixel_shader(const struct d3d9_api *api);
void test_pixel_shader_constant(const struct d3d9_api *api);
void test_unsupported_shaders(const struct d3d9_api *api);
void test_shader_unsupported_stage_variants(const struct d3d9_api *api);
void test_ex_shader_validation_policy(const struct d3d9_api *api);
void test_texture_stage_states(const struct d3d9_api *api);
void test_sampler_state_edges(const struct d3d9_api *api);
void test_light_enable_state(const struct d3d9_api *api);
void test_fpu_setup(const struct d3d9_api *api);
void test_limits(const struct d3d9_api *api);
void test_viewport_scissor_state_getters(const struct d3d9_api *api);
void test_scissor_default_matches_backbuffer_policy(const struct d3d9_api *api);
void test_clip_plane_state_getters(const struct d3d9_api *api);
void test_null_stream_state(const struct d3d9_api *api);
void test_null_stream_shader_draw_policy(const struct d3d9_api *api);
void test_set_stream_source_state(const struct d3d9_api *api);
void test_stream_source_frequency_state(const struct d3d9_api *api);
void test_stream_source_vb_offset_alignment_policy(const struct d3d9_api *api);
void test_stream_source_null_layout_policy(const struct d3d9_api *api);
void test_stream_source_zero_stride_policy(const struct d3d9_api *api);
void test_stream_source_null_offset_alignment_policy(const struct d3d9_api *api);
void test_get_set_texture(const struct d3d9_api *api);
void test_set_palette_roundtrip(const struct d3d9_api *api);
void test_palette_alpha_caps_policy(const struct d3d9_api *api);
void test_palette_current_entry_isolation(const struct d3d9_api *api);
void test_multi_adapter(const struct d3d9_api *api);
void test_device_creation_parameters_policy(const struct d3d9_api *api);
void test_device_parent_caps_getter_policy(const struct d3d9_api *api);
void test_device_raster_status_bounds(const struct d3d9_api *api);
void test_pixel_format_window_policy(const struct d3d9_api *api);
void test_multi_device_independent_state(const struct d3d9_api *api);
void test_mode_change_focus_swap_policy(const struct d3d9_api *api);
void test_reset_fullscreen_focus_window_policy(const struct d3d9_api *api);
void test_window_position_present_parameter_policy(const struct d3d9_api *api);

void test_display_mode_ex_size_filter_smoke(const struct d3d9_api *api);
void test_ex_adapter_luid_display_mode(const struct d3d9_api *api);
void test_ex_adapter_display_mode_null_rotation(const struct d3d9_api *api);
void test_ex_adapter_mode_enum_bounds(const struct d3d9_api *api);
void test_ex_swapchain_display_mode(const struct d3d9_api *api);
void test_ex_swapchain_display_mode_null_rotation(
        const struct d3d9_api *api);
void test_ex_frame_latency_state(const struct d3d9_api *api);
void test_present_parameter_validation(const struct d3d9_api *api);
void test_present_parameter_normalization(const struct d3d9_api *api);
void test_swapchain_backbuffer_getter_policy(const struct d3d9_api *api);
void test_additional_swapchain_backbuffer_bounds(const struct d3d9_api *api);
void test_device_get_swap_chain_bounds_policy(const struct d3d9_api *api);
void test_lockable_backbuffer_lock_policy(const struct d3d9_api *api);
void test_nonlockable_backbuffer_getdc_policy(const struct d3d9_api *api);
void test_reset_lockable_backbuffer_policy(const struct d3d9_api *api);
void test_swapchain_multisample_reset(const struct d3d9_api *api);
void test_fullscreen_window_position_restore(const struct d3d9_api *api);
void test_swapchain_get_display_mode_ex_policy(const struct d3d9_api *api);
void test_ex_get_adapter_luid_policy(const struct d3d9_api *api);
void test_ex_get_adapter_display_mode_ex_policy(const struct d3d9_api *api);
void test_backbuffer_resize_present_parameter_policy(const struct d3d9_api *api);

void test_stateblock_invalid_type_recording_invalid_calls(const struct d3d9_api *api);
void test_shader_constant_apply(const struct d3d9_api *api);
void test_shader_constant_stateblock_cross_stage(const struct d3d9_api *api);
void test_vdecl_apply(const struct d3d9_api *api);
void test_stateblock_transform_capture_apply(const struct d3d9_api *api);
void test_stateblock_multiply_transform_capture(const struct d3d9_api *api);

void test_private_data_iunknown_ownership_smoke(const struct d3d9_api *api);
void test_private_data_resource_wrappers(const struct d3d9_api *api);
void test_private_data_replace_and_size_policy(const struct d3d9_api *api);
void test_resource_lock_error_policy(const struct d3d9_api *api);
void test_surface_lockrect_subrect_offset_policy(const struct d3d9_api *api);
void test_compressed_surface_lockrect_block_offset(const struct d3d9_api *api);
void test_surface_reentrant_lock_preserves_output(const struct d3d9_api *api);
void test_texture_reentrant_lock_preserves_output(const struct d3d9_api *api);
void test_texture_level_surface_unlock_policy(const struct d3d9_api *api);
void test_surface_double_unlock_pool_policy(const struct d3d9_api *api);
void test_mipmap_surface_update_lock_policy(const struct d3d9_api *api);
void test_vb_lock_flags(const struct d3d9_api *api);
void test_writeonly_vertex_buffer_readback_policy(const struct d3d9_api *api);
void test_vertex_buffer_alignment(const struct d3d9_api *api);
void test_surface_alignment(const struct d3d9_api *api);
void test_surface_dimensions(const struct d3d9_api *api);
void test_texture_auto_mipmap_level_count(const struct d3d9_api *api);
void test_base_texture_metadata_iface_policy(const struct d3d9_api *api);
void test_texture_autogen_filter_level_policy(const struct d3d9_api *api);
void test_surface_format_null_policy(const struct d3d9_api *api);
void test_resource_type(const struct d3d9_api *api);
void test_texture_level_surface_desc_parity(const struct d3d9_api *api);
void test_cube_texture_face_desc_parity(const struct d3d9_api *api);
void test_resource_get_device_wrapper_policy(const struct d3d9_api *api);
void test_index_buffer_desc_binding_policy(const struct d3d9_api *api);
void test_vertex_buffer_desc_binding_policy(const struct d3d9_api *api);
void test_texture_surface_container_policy(const struct d3d9_api *api);
void test_cube_texture_level_surface_policy(const struct d3d9_api *api);
void test_volume_resource_container_desc(const struct d3d9_api *api);
void test_volume_container_interface_policy(const struct d3d9_api *api);
void test_volume_mipmap_level_desc_policy(const struct d3d9_api *api);
void test_volume_block_lock_layout(const struct d3d9_api *api);
void test_volume_lockbox_bounds_offset_policy(const struct d3d9_api *api);
void test_vendor_format_public_api_policy(const struct d3d9_api *api);
void test_intz_depth_sampleable_texture_policy(const struct d3d9_api *api);
void test_resource_priority_roundtrip(const struct d3d9_api *api);
void test_resource_priority_pool_policy(const struct d3d9_api *api);
void test_base_vidmem_accounting_policy(const struct d3d9_api *api);
void test_ex_vidmem_accounting_policy(const struct d3d9_api *api);
void test_texture_lod_policy(const struct d3d9_api *api);
void test_update_texture_pool_copy_2d(const struct d3d9_api *api);
void test_shared_handle_policy(const struct d3d9_api *api);
void test_ex_shared_handle_policy(const struct d3d9_api *api);
void test_ex_user_memory_lock_identity(const struct d3d9_api *api);
void test_ex_user_memory_getdc_dib_identity(const struct d3d9_api *api);
void test_ex_user_memory_getdc_format_policy(const struct d3d9_api *api);
void test_pinned_buffers_d3dusage_policy(const struct d3d9_api *api);
void test_volume_blocks_compressed_layout_policy(const struct d3d9_api *api);
void test_creation_failure_out_pointers(const struct d3d9_api *api);
void test_create_rt_ds_failure_policy(const struct d3d9_api *api);
void test_get_render_target_data_policy(const struct d3d9_api *api);
void test_render_target_device_mismatch(const struct d3d9_api *api);
void test_create_depth_stencil_surface_ex(const struct d3d9_api *api);

void test_visual_float_texture_format_policy(const struct d3d9_api *api);
void test_visual_g16r16_texture_format_policy(const struct d3d9_api *api);
void test_visual_volume_v16u16_format_policy(const struct d3d9_api *api);
void test_visual_srgb_texture_caps_policy(const struct d3d9_api *api);
void test_visual_srgb_write_caps_policy(const struct d3d9_api *api);
void test_visual_volume_srgb_caps_policy(const struct d3d9_api *api);
void test_visual_volume_dxtn_format_policy(const struct d3d9_api *api);
void test_visual_signed_formats_caps_policy(const struct d3d9_api *api);

/* Wine visual.c depth-stencil oracle scaffolds. */
void test_visual_z_range_render_state_policy(const struct d3d9_api *api);
void test_visual_ds_smaller_than_rt_policy(const struct d3d9_api *api);
void test_visual_depth_buffer_clear_policy(const struct d3d9_api *api);
void test_visual_depth_buffer_reset_policy(const struct d3d9_api *api);
void test_visual_depth_bounds_caps_policy(const struct d3d9_api *api);
void test_visual_zenable_render_state_policy(const struct d3d9_api *api);
void test_visual_zwriteenable_render_state_policy(const struct d3d9_api *api);
void test_visual_multisampled_depth_buffer_caps_policy(const struct d3d9_api *api);

/* Wine visual.c render-target / clear / surface oracle scaffolds. */
void test_visual_depth_clamp_render_state_policy(const struct d3d9_api *api);
void test_visual_clear_color_only_policy(const struct d3d9_api *api);
void test_visual_clear_smaller_rt_policy(const struct d3d9_api *api);
void test_visual_colorfill_format_policy(const struct d3d9_api *api);
void test_visual_offscreen_surface_creation_policy(const struct d3d9_api *api);
void test_visual_stencil_cull_caps_policy(const struct d3d9_api *api);
void test_visual_update_surface_policy(const struct d3d9_api *api);
void test_visual_swapchain_flip_present_policy(const struct d3d9_api *api);

/* Wine visual.c shading / lighting / surface misc oracle scaffolds. */
void test_visual_shademode_render_state_policy(const struct d3d9_api *api);
void test_visual_lighting_render_state_policy(const struct d3d9_api *api);
void test_visual_lighting_world_view_matrix_policy(const struct d3d9_api *api);
void test_visual_release_buffer_bound_policy(const struct d3d9_api *api);
void test_visual_evict_managed_resources_policy(const struct d3d9_api *api);
void test_visual_add_dirty_rect_policy(const struct d3d9_api *api);
void test_visual_multisample_get_front_buffer_data_policy(const struct d3d9_api *api);
void test_visual_multisample_rt_ds_mismatch_policy(const struct d3d9_api *api);

/* Wine visual.c additional planned-item scaffolds. */
void test_visual_buffer_no_dirty_update_policy(const struct d3d9_api *api);
void test_visual_yuv_color_caps_policy(const struct d3d9_api *api);
void test_visual_yuv_layout_lock_policy(const struct d3d9_api *api);
void test_visual_3dc_format_caps_policy(const struct d3d9_api *api);
void test_visual_position_index_decl_policy(const struct d3d9_api *api);
void test_visual_mvp_software_vp_policy(const struct d3d9_api *api);

/* Wine stateblock.c capture/apply matrix scaffolds. */
void test_state_management_all_capture_apply_matrix(const struct d3d9_api *api);
void test_state_management_pixel_capture_apply_slice(const struct d3d9_api *api);
void test_state_management_vertex_capture_apply_slice(const struct d3d9_api *api);

/* Wine device.c advanced query / format / multithreading scaffolds. */
void test_query_get_data_size_policy(const struct d3d9_api *api);
void test_check_device_format_conversion_matrix(const struct d3d9_api *api);
void test_check_device_type_display_format_policy(const struct d3d9_api *api);
void test_create_texture_npot_policy(const struct d3d9_api *api);
void test_multithreaded_device_creation_policy(const struct d3d9_api *api);
void test_draw_primitive_outside_scene_policy(const struct d3d9_api *api);
void test_set_get_depth_stencil_surface_policy(const struct d3d9_api *api);

void test_visual_shadow_depth_compare_caps_policy(const struct d3d9_api *api);

/* Wine visual.c raster / line-AA / blit policy scaffolds (G2). */
void test_visual_filling_convention_caps_policy(const struct d3d9_api *api);
void test_visual_line_antialiasing_blending_state_policy(const struct d3d9_api *api);
void test_visual_blit_format_conversion_policy(const struct d3d9_api *api);
void test_stretch_rect_null_and_degenerate_policy(const struct d3d9_api *api);


/* Wine visual.c planned-item scaffolds (G1). */
void test_visual_specular_lighting_render_state_policy(const struct d3d9_api *api);
void test_visual_max_index16_draw_policy(const struct d3d9_api *api);
void test_visual_null_format_caps_policy(const struct d3d9_api *api);
void test_visual_sample_mask_render_state_policy(const struct d3d9_api *api);
void test_visual_depth_stencil_init_policy(const struct d3d9_api *api);

/* Wine vendor-format / SM1 deferred-policy scaffolds (G3). */
void test_vendor_policy_texbem_unsupported(const struct d3d9_api *api);
void test_vendor_policy_texdepth_unsupported(const struct d3d9_api *api);
void test_vendor_policy_intz_caps(const struct d3d9_api *api);
void test_vendor_policy_fetch4_caps(const struct d3d9_api *api);
void test_vendor_policy_resz_caps(const struct d3d9_api *api);
void test_vendor_policy_mipmap_upload_policy(const struct d3d9_api *api);


/* Wine miptree layout PE-side scaffold (G4). */
void test_miptree_layout_lock_pitch_policy(const struct d3d9_api *api);

/* Wine SR-blocked narrow PE-policy scaffolds (G4). */
void test_visual_bumpenvmap_tss_policy(const struct d3d9_api *api);
void test_visual_pretransformed_vertex_declaration_policy(const struct d3d9_api *api);
void test_visual_vface_pixel_shader_create_policy(const struct d3d9_api *api);
void test_visual_fp_special_caps_policy(const struct d3d9_api *api);

#endif /* DXMT9_TESTS_D3D9_CONFORMANCE_FIXTURES_H */
