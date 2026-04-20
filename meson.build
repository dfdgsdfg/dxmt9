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

if host_machine.system() != 'darwin'
  error('dxmt9 is darwin-only. The device_c client bridge and Windows PE cross-compile paths have been removed — the runtime is now built as a darwin-native library only.')
endif

add_languages('objc', 'objcpp', native: false)
subdir('include')
subdir('src')
if get_option('build_tests')
  subdir('tests')
endif
