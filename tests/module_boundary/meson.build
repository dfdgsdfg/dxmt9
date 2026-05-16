if host_machine.system() != 'windows'
  subdir_done()
endif

cpp = meson.get_compiler('cpp')
fs = import('fs')

module_boundary_arch_suffix = host_machine.cpu_family() == 'x86' ? 'x86' : 'x64'

module_boundary_probe = executable(
  'dxmt9-module-boundary-probe_' + module_boundary_arch_suffix,
  'module_boundary_probe.cpp',
  dependencies: [
    cpp.find_library('user32', required: true),
    cpp.find_library('d3d9', required: true),
  ],
  install: false,
)

module_boundary_runtime_targets = [module_boundary_probe]

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
      module_boundary_runtime_targets += custom_target(
        'module_boundary_runtime_' + runtime_dep[0],
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
  module_boundary_runtime_targets += custom_target(
    'module_boundary_runtime_d3d9',
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
  module_boundary_runtime_targets += custom_target(
    'module_boundary_runtime_winemetal',
    input: winemetal_dll,
    output: 'winemetal.dll',
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

alias_target('dxmt9-module-boundary-runtime', module_boundary_runtime_targets)
