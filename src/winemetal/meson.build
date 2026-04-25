dxmt9_winemetal_internal_inc = include_directories('.', '..', '../dxmt9')

if host_machine.system() == 'windows'
  # PE-side winemetal.dll — DllMain bootstrap + __wine_unix_call thunk shims
  # for the bridge ops generated from device_c.h. The actual Metal calls
  # land in the darwin-built winemetal.so (Mach-O, separate target).
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
  winemetal_bridge_c_args = []
  winemetal_bridge_cpp_args = []
  winemetal_bridge_link_args = []

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
      lib_winecrt0 = cc.find_library('winecrt0', dirs: [
        join_paths(wine_build_path, 'libs/winecrt0', winemetal_runtime_subdir),
        join_paths(wine_build_path, 'dlls/winecrt0', winemetal_runtime_subdir),
      ], required: true)
      lib_ntdll = cc.find_library('ntdll', dirs: [
        join_paths(wine_build_path, 'dlls/ntdll', winemetal_runtime_subdir),
      ], static: true, required: true)
      lib_dbghelp = cc.find_library('dbghelp', dirs: [
        join_paths(wine_build_path, 'dlls/dbghelp', winemetal_runtime_subdir),
      ], required: true)
      winebuild = join_paths(wine_build_path, 'tools/winebuild/winebuild')
    else
      lib_winecrt0 = cc.find_library('winecrt0', dirs: [
        join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
      ], required: true)
      lib_ntdll = cc.find_library('ntdll', dirs: [
        join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
      ], static: true, required: true)
      lib_dbghelp = cc.find_library('dbghelp', dirs: [
        join_paths(wine_install_path, 'lib/wine', winemetal_runtime_subdir),
      ], required: true)
      winebuild = join_paths(wine_install_path, 'bin/winebuild')
    endif

    dxmt9_winebuild = winebuild
    winemetal_bridge_deps += [lib_winecrt0, lib_ntdll, lib_dbghelp]
    winemetal_bridge_c_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']
    winemetal_bridge_cpp_args += ['-DDXMT9_WINE_BUILTIN_DLL=1']
    # llvm-mingw's default C++ runtime can pull its own TLS anchors while
    # winecrt0 provides the same builtin-module anchors.
    winemetal_bridge_link_args += cpp.get_supported_link_arguments([
      '-Wl,--allow-multiple-definition',
    ])
  endif

  winemetal_dll = shared_library(
    'winemetal',
    files('main.c', 'winemetal_thunks.cpp', 'winemetal_bridge.cpp'),
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
      output: 'winemetal.dll.postproc',
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
