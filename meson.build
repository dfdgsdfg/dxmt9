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

if host_machine.system() == 'darwin'
  add_languages('objc', 'objcpp', native: false)
endif

subdir('include')
subdir('src')

if get_option('build_tests')
  subdir('tests')
endif
