dxmt9_winemetal_internal_inc = include_directories('.', '..', '../dxmt9')
fs = import('fs')

if host_machine.system() == 'windows'
  # PE-side winemetal_dxmt9.dll — DllMain bootstrap + __wine_unix_call thunk shims
  # for the bridge ops generated from device_c.h. The actual Metal calls
  # land in the darwin-built winemetal_dxmt9.so (Mach-O, separate target).
  cc = meson.get_compiler('c')
  cpp = meson.get_compiler('cpp')
  wine_builtin_dll = get_option('wine_builtin_dll')
  wine_build_path = get_option('wine_build_path')
  wine_install_path = get_option('wine_install_path')
  dxguid_dep = cpp.find_library('dxguid', required: true)

  winemetal_runtime_subdir = 'x86_64-windows'
  if host_machine.cpu_family() == 'x86'
    winemetal_runtime_subdir = 'i386-windows'
  elif host_machine.cpu_family() != 'x86_64'
    winemetal_runtime_subdir = host_machine.cpu_family() + '-windows'
  endif

  winemetal_install_dir = get_option('prefix') / 'wine' / winemetal_runtime_subdir

  winemetal_bridge_deps = []
  winemetal_bridge_link_depends = []
  winemetal_bridge_c_args = []
  winemetal_bridge_cpp_args = []
  winemetal_bridge_link_args = []

  if get_option('dxmt9_bridge_perf_counters')
    winemetal_bridge_cpp_args += ['-DDXMT9_BRIDGE_PERF_COUNTERS=1']
  else
    winemetal_bridge_cpp_args += ['-DDXMT9_BRIDGE_PERF_COUNTERS=0']
  endif

  if wine_builtin_dll
    if wine_build_path == '' and wine_install_path == ''
      error('wine_builtin_dll=true requires -Dwine_build_path or -Dwine_install_path')
    endif

    if wine_build_path != '' and not wine_build_path.startswith('/')
      wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
    endif
    if wine_install_path != '' and not wine_install_path.startswith('/')
      wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
    endif

    if wine_build_path != ''
      winecrt0_dirs = [
        join_paths(wine_build_path, 'libs/winecrt0', winemetal_runtime_subdir),
        join_paths(wine_build_path, 'dlls/winecrt0', winemetal_runtime_subdir),
      ]
      lib_winecrt0 = cc.find_library('winecrt0', dirs: winecrt0_dirs, required: true)
      lib_ntdll = cc.find_library('ntdll', dirs: [
        join_paths(wine_build_path, 'dlls/ntdll', winemetal_runtime_subdir),
      ], static: true, required: true)
      lib_dbghelp = cc.find_library('dbghelp', dirs: [
        join_paths(wine_build_path, 'dlls/dbghelp', winemetal_runtime_subdir),
      ], required: true)
      winebuild = join_paths(wine_build_path, 'tools/winebuild/winebuild')
    else
      winecrt0_dirs = [
        join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
      ]
      lib_winecrt0 = cc.find_library('winecrt0', dirs: winecrt0_dirs, required: true)
      lib_ntdll = cc.find_library('ntdll', dirs: [
        join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
      ], static: true, required: true)
      lib_dbghelp = cc.find_library('dbghelp', dirs: [
        join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
      ], required: true)
      winebuild = join_paths(wine_install_path, 'bin/winebuild')
    endif

    dxmt9_winebuild = winebuild
    winemetal_bridge_deps += [lib_ntdll, lib_dbghelp]
    winemetal_bridge_c_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']
    winemetal_bridge_cpp_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']

    winecrt0_path = ''
    foreach dir : winecrt0_dirs
      candidate = join_paths(dir, 'libwinecrt0.a')
      if winecrt0_path == '' and fs.exists(candidate)
        winecrt0_path = candidate
      endif
    endforeach
    if winecrt0_path == ''
      error('could not locate libwinecrt0.a in winecrt0 search dirs')
    endif

    llvm_ar = find_program('ar', 'llvm-ar', required: true, native: false)
    winecrt0_no_tls = custom_target(
      'winecrt0_no_tls',
      input: winecrt0_path,
      output: 'libwinecrt0_no_tls.a',
      command: [
        'sh', '-c',
        'cp "$1" "$2" && "$3" d "$2" "$4"',
        'sh', '@INPUT@', '@OUTPUT@', llvm_ar,
        'libs/winecrt0' / winemetal_runtime_subdir / 'tls.o',
      ],
    )
    # Wine's winecrt0 and llvm-mingw's C++ runtime both provide PE TLS
    # anchors. Keep winecrt0's builtin entry points, but let MinGW own the
    # TLS directory so libmingw32/tlssup.o is not a duplicate definition.
    winemetal_bridge_link_args += [
      join_paths(meson.current_build_dir(), 'libwinecrt0_no_tls.a'),
    ]
    winemetal_bridge_link_depends += [winecrt0_no_tls]
    winemetal_bridge_link_args += cpp.get_supported_link_arguments([
      '-Wl,--allow-multiple-definition',
    ])
  endif

  winemetal_dll = shared_library(
    'winemetal_dxmt9',
    files('main.c', 'winemetal_thunks.cpp', 'winemetal_bridge.cpp',
          'winemetal_abi_check.cpp') + [
      bridge_codegen[0],  # dxmt9_bridge_ops.generated.h — order-only dep so
                          # winemetal_abi_check.cpp (which #includes the
                          # generated kBridgeAbiHash) waits for codegen.
    ],
    name_prefix: '',
    name_suffix: 'dll',
    vs_module_defs: files('winemetal.def')[0],
    include_directories: [dxmt9_inc, bridge_gen_inc, dxmt9_winemetal_internal_inc],
    dependencies: winemetal_bridge_deps + [
      dxguid_dep,
      dxmt9_util_dep,
    ],
    c_args: winemetal_bridge_c_args,
    cpp_args: winemetal_bridge_cpp_args,
    link_args: winemetal_bridge_link_args,
    link_depends: winemetal_bridge_link_depends,
    install: true,
    install_dir: winemetal_install_dir,
  )

  dxmt9_winemetal_import_dep = declare_dependency(
    link_with: winemetal_dll,
    include_directories: [dxmt9_inc, bridge_gen_inc, dxmt9_winemetal_internal_inc],
    dependencies: [dxmt9_util_dep],
  )

  dxmt9_winemetal_dep = declare_dependency(
    link_with: winemetal_dll,
    include_directories: [dxmt9_inc, bridge_gen_inc, dxmt9_winemetal_internal_inc],
    dependencies: [dxmt9_util_dep],
  )

  if wine_builtin_dll
    custom_target(
      'winemetal_builtin_postprocess',
      input: winemetal_dll,
      output: 'winemetal_dxmt9.dll.postproc',
      command: [
        'sh', '-c', '"$1" --builtin "$2" && touch "$3"',
        'sh', winebuild, '@INPUT@', '@OUTPUT@',
      ],
      depends: winemetal_dll,
      build_by_default: true,
    )
  endif

  subdir_done()
endif

dxmt9_provider_service_abi_lib = static_library(
  'dxmt9_provider_service_abi',
  dxmt9_provider_service_abi_srcs,
  include_directories: [
    dxmt9_inc,
    dxmt9_winemetal_internal_inc,
    dxmt9_runtime_internal_inc,
    include_directories('..'),
  ],
  dependencies: [
    dxmt9_util_dep,
  ],
  install: false,
)

dxmt9_provider_service_abi_dep = declare_dependency(
  link_with: dxmt9_provider_service_abi_lib,
  include_directories: [
    dxmt9_inc,
    dxmt9_winemetal_internal_inc,
  ],
  dependencies: [dxmt9_util_dep],
)

subdir('unix')

dxmt9_winemetal_dep = declare_dependency(
  link_with: dxmt9_winemetal_unix_module,
  include_directories: [dxmt9_inc, dxmt9_winemetal_internal_inc],
  dependencies: [
    dxmt9_util_dep,
  ],
)
