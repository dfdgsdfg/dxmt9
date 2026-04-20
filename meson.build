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
device_c_unix_schema = custom_target(
  'dxmt9_device_c_unix_schema_codegen',
  input: files('include/dxmt9/device_c.h'),
  output: 'dxmt9_device_c_unix_schema.generated.h',
  command: [
    python3,
    files('scripts/extract_device_c_schema.py'),
    '@INPUT@',
    '@OUTPUT@',
  ],
)

bridge_codegen = custom_target(
  'dxmt9_wine_bridge_codegen',
  input: device_c_unix_schema,
  output: [
    'dxmt9_bridge_ops.generated.h',
    'dxmt9_device_c_bridge_exports.generated.cpp',
    'dxmt9_wine_unix_dispatch.generated.cpp',
    'dxmt9_wine_unix_entries.generated.h',
  ],
  command: [
    python3,
    files('scripts/gen_wine_bridge.py'),
    '--schema-header', files('src/winemetal/winemetal_unix_schema.h'),
    '--schema-header', '@INPUT@',
    '--ops-header', '@OUTPUT0@',
    '--client-cpp', '@OUTPUT1@',
    '--server-cpp', '@OUTPUT2@',
    '--server-entries', '@OUTPUT3@',
  ],
  depend_files: files(
    'src/winemetal/winemetal_unix_schema.h',
    'scripts/gen_wine_bridge.py',
    'scripts/extract_device_c_schema.py',
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
  subdir('src/winemetal')
  subdir('src/d3d9')
  subdir('src/win32')
endif
