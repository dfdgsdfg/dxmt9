# src/win32/meson.build — builds the Win32 PE d3d9.dll
#
# Invoked when cross-compiling for Windows:
#   meson setup build-win32 --cross-file cross/aarch64-windows.ini
#   meson compile -C build-win32
#
# Prerequisites: llvm-mingw on PATH (provides aarch64-w64-mingw32-* tools).

if host_machine.system() != 'windows'
  subdir_done()
endif

# ── import stub for libdxmt9.dylib (installed as dxmt9.dll in the prefix) ───

_cpu = host_machine.cpu_family()
_dlltool_machine = _cpu == 'aarch64' ? 'arm64' : 'i386:x86-64'

dlltool = find_program(
  'aarch64-w64-mingw32-dlltool',
  'x86_64-w64-mingw32-dlltool',
  'llvm-dlltool',
  'dlltool',
  required: true,
)

dxmt9_import = custom_target('dxmt9_import_lib',
  input:   files('dxmt9_imports.def'),
  output:  'libdxmt9.dll.a',
  command: [dlltool,
            '--machine',     _dlltool_machine,
            '--input-def',   '@INPUT@',
            '--output-lib',  '@OUTPUT@'],
)

# ── d3d9.dll ─────────────────────────────────────────────────────────────────

d3d9_srcs = files(
  'entry.cpp',
  'factory.cpp',
  'device.cpp',
)

d3d9_dll = shared_library(
  'd3d9',
  d3d9_srcs,
  name_prefix:         '',
  name_suffix:         'dll',
  vs_module_defs:      files('d3d9.def')[0],
  include_directories: dxmt9_inc,
  link_args: [dxmt9_import[0].full_path()],
  link_depends: [dxmt9_import],
  install:      true,
  install_dir:  get_option('prefix') / 'windows',
)
