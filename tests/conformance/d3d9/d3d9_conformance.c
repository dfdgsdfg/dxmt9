/*
 * Focused D3D9 PE conformance harness driver.
 *
 * Wine behavioral oracle:
 * - dlls/d3d9/tests/d3d9ex.c
 * - dlls/d3d9/tests/device.c
 * - dlls/d3d9/tests/stateblock.c
 * Wine commit 6e073d28dee3af7f4c965daec94644e0f9f92727.
 *
 * Per-domain test bodies live in d3d9_conformance_<bucket>.c. This file
 * owns the unique definitions of the shared counters declared extern in
 * d3d9_conformance_fixtures.h, the test dispatch table, and main().
 */

#include "d3d9_conformance_fixtures.h"

const char *current_test;
unsigned int checks_run;
unsigned int checks_failed;
unsigned int tests_skipped;

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
    {"vertex_declaration_fvf_policy", test_vertex_declaration_fvf_policy},
    {"get_set_vertex_shader", test_get_set_vertex_shader},
    {"vertex_shader_constant", test_vertex_shader_constant},
    {"get_set_pixel_shader", test_get_set_pixel_shader},
    {"pixel_shader_constant", test_pixel_shader_constant},
    {"unsupported_shaders", test_unsupported_shaders},
    {"texture_stage_states", test_texture_stage_states},
    {"fpu_setup", test_fpu_setup},
    {"limits", test_limits},
    {"null_stream_state", test_null_stream_state},
    {"set_stream_source_state", test_set_stream_source_state},
    {"get_set_texture", test_get_set_texture},
    {"set_palette_roundtrip", test_set_palette_roundtrip},
    {"multi_adapter", test_multi_adapter},
    {"stateblock_invalid_type_recording_invalid_calls",
            test_stateblock_invalid_type_recording_invalid_calls},
    {"shader_constant_apply", test_shader_constant_apply},
    {"vdecl_apply", test_vdecl_apply},
    {"private_data_iunknown_ownership_smoke",
            test_private_data_iunknown_ownership_smoke},
    {"private_data_resource_wrappers",
            test_private_data_resource_wrappers},
    {"resource_lock_error_policy", test_resource_lock_error_policy},
    {"vb_lock_flags", test_vb_lock_flags},
    {"vertex_buffer_alignment", test_vertex_buffer_alignment},
    {"surface_alignment", test_surface_alignment},
    {"surface_dimensions", test_surface_dimensions},
    {"resource_type", test_resource_type},
    {"resource_priority_roundtrip", test_resource_priority_roundtrip},
    {"shared_handle_policy", test_shared_handle_policy},
    {"ex_shared_handle_policy", test_ex_shared_handle_policy},
    {"creation_failure_out_pointers", test_creation_failure_out_pointers},
    {"render_target_device_mismatch", test_render_target_device_mismatch},
    {"create_depth_stencil_surface_ex", test_create_depth_stencil_surface_ex},
    {"ex_adapter_luid_display_mode", test_ex_adapter_luid_display_mode},
    {"ex_swapchain_display_mode", test_ex_swapchain_display_mode},
    {"ex_frame_latency_state", test_ex_frame_latency_state},
    {"lockable_backbuffer_lock_policy", test_lockable_backbuffer_lock_policy},
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
