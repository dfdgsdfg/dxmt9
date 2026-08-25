if host_machine.system() != 'windows'
  subdir_done()
endif

c = meson.get_compiler('c')
cpp = meson.get_compiler('cpp')
fs = import('fs')

d3d9_conformance = executable(
  'dxmt9-d3d9-conformance',
  [
    'd3d9_conformance.c',
    'd3d9_conformance_device.c',
    'd3d9_conformance_resource.c',
    'd3d9_conformance_swapchain.c',
    'd3d9_conformance_query_stateblock.c',
    'd3d9_conformance_visual_formats.c',
    'd3d9_conformance_visual_depth.c',
    'd3d9_conformance_visual_render.c',
    'd3d9_conformance_visual_misc.c',
    'd3d9_conformance_visual_raster.c',
    'd3d9_conformance_visual_planned.c',
    'd3d9_conformance_vendor_policy.c',
    'd3d9_conformance_sr_blocked_policy.c',
  ],
  dependencies: [
    c.find_library('gdi32', required: true),
    c.find_library('user32', required: true),
  ],
  install: false,
)

d3d9_device_lifetime = executable(
  'dxmt9-d3d9-device-lifetime',
  'd3d9_device_lifetime.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

d3d9_exports = executable(
  'd3d9_exports_x64',
  'd3d9_exports.cpp',
  install: false,
)

d3d9_auxiliary = executable(
  'd3d9_auxiliary_x64',
  'd3d9_auxiliary.cpp',
  install: false,
)

d3d9_queries = executable(
  'd3d9_queries_x64',
  'd3d9_queries.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

d3d9_resources = executable(
  'd3d9_resources_x64',
  'd3d9_resources.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('gdi32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

d3d9_device_misc = executable(
  'd3d9_device_misc_x64',
  'd3d9_device_misc.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

d3d9_stateblock = executable(
  'd3d9_stateblock_x64',
  'd3d9_stateblock_matrix.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

d3d9_reset_lost = executable(
  'd3d9_reset_lost_x64',
  'd3d9_reset_lost.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

d3d9_window_cursor = executable(
  'd3d9_window_cursor_x64',
  'd3d9_window_cursor.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

d3d9_conformance_runtime_targets = []

mingw_runtime_triplet = ''
if host_machine.cpu_family() == 'x86_64'
  mingw_runtime_triplet = 'x86_64-w64-mingw32'
elif host_machine.cpu_family() == 'x86'
  mingw_runtime_triplet = 'i686-w64-mingw32'
endif

if mingw_runtime_triplet != ''
  mingw_runtime_root = run_command(
    'sh',
    '-c',
    'printf %s "${DXMT9_MINGW_RUNTIME_ROOT:-$HOME/llvm-mingw}"',
    check: true,
  ).stdout().strip()
  mingw_runtime_bin = join_paths(mingw_runtime_root, mingw_runtime_triplet, 'bin')

  foreach runtime_dep : [
    ['libcxx', 'libc++.dll'],
    ['libunwind', 'libunwind.dll'],
  ]
    runtime_dep_path = join_paths(mingw_runtime_bin, runtime_dep[1])
    if fs.exists(runtime_dep_path)
      d3d9_conformance_runtime_targets += custom_target(
        'd3d9_conformance_runtime_' + runtime_dep[0],
        input: runtime_dep_path,
        output: runtime_dep[1],
        command: [
          python3,
          '-c',
          'import shutil, sys; shutil.copy2(sys.argv[1], sys.argv[2])',
          '@INPUT@',
          '@OUTPUT@',
        ],
        build_by_default: true,
      )
    endif
  endforeach
endif

if is_variable('d3d9_dll')
  d3d9_conformance_runtime_targets += custom_target(
    'd3d9_conformance_runtime_d3d9',
    input: d3d9_dll,
    output: 'd3d9.dll',
    command: [
      python3,
      '-c',
      'import shutil, sys; shutil.copy2(sys.argv[1], sys.argv[2])',
      '@INPUT@',
      '@OUTPUT@',
    ],
    build_by_default: true,
  )
endif

if is_variable('winemetal_dll')
  d3d9_conformance_runtime_targets += custom_target(
    'd3d9_conformance_runtime_winemetal',
    input: winemetal_dll,
    output: 'winemetal_dxmt9.dll',
    command: [
      python3,
      '-c',
      'import shutil, sys; shutil.copy2(sys.argv[1], sys.argv[2])',
      '@INPUT@',
      '@OUTPUT@',
    ],
    build_by_default: true,
  )
endif

alias_target(
  'dxmt9-d3d9-conformance-runtime',
  [
    d3d9_conformance,
    d3d9_device_lifetime,
    d3d9_exports,
    d3d9_auxiliary,
    d3d9_queries,
    d3d9_resources,
    d3d9_device_misc,
    d3d9_stateblock,
    d3d9_reset_lost,
    d3d9_window_cursor,
  ]
    + d3d9_conformance_runtime_targets,
)
