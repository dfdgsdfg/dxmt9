project(
  'dxmt9',
  ['cpp', 'c'],
  version: '0.1.0',
  meson_version: '>=1.10.0',
  default_options: [
    'warning_level=2',
    'cpp_std=c++20',
    'c_std=c17',
    'b_ndebug=if-release',
    'buildtype=debugoptimized',
  ],
)

dxmt9_inc = include_directories('include')
bridge_gen_inc = include_directories('.')

python3 = find_program('python3', required: true)
bridge_codegen = custom_target(
  'dxmt9_wine_bridge_codegen',
  output: [
    'dxmt9_bridge_ops.generated.h',
    'dxmt9_device_c_bridge_exports.generated.cpp',
    'dxmt9_wine_unix_dispatch.generated.cpp',
  ],
  command: [
    python3,
    files('scripts/gen_wine_bridge.py'),
    '--ops-header', '@OUTPUT0@',
    '--client-cpp', '@OUTPUT1@',
    '--server-cpp', '@OUTPUT2@',
  ],
  depend_files: files(
    'include/dxmt9/device_c.h',
    'include/dxmt9/winemetal.h',
    'scripts/gen_wine_bridge.py',
  ),
)

if host_machine.system() == 'darwin'
  add_languages('objc', 'objcpp', native: false)
  subdir('include')
  subdir('src')
  if get_option('build_tests')
    subdir('tests')
  endif
elif host_machine.system() == 'windows'
  subdir('src/util')
  subdir('src/d3d9')
  subdir('src/win32')
endif
