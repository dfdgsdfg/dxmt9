project('dxmt9-private-perfetto', 'cpp',
  version: '49.0', default_options: ['cpp_std=c++20', 'warning_level=0'])

# The parent consumes this dependency only in its private exporter target.
# This project is never configured by SDK-off or PE builds.
perfetto_sdk = static_library('dxmt9_perfetto_sdk', 'sdk/perfetto.cc',
  include_directories: include_directories('sdk'),
  gnu_symbol_visibility: 'hidden',
  install: false)
perfetto_private_dep = declare_dependency(
  link_with: perfetto_sdk, include_directories: include_directories('sdk'))
