if host_machine.system() == 'windows'
  subdir_done()
endif

dxmt9_winemetal_unix_runtime_subdir = host_machine.cpu_family() == 'x86_64' ? 'x86_64-unix' : host_machine.cpu_family() + '-unix'
dxmt9_winemetal_unix_common_link_args = []
dxmt9_winemetal_unix_override_options = {}
dxmt9_winemetal_unix_build_rpath = '@loader_path/:@loader_path/../../'
dxmt9_winemetal_unix_needs_install_name_fixup = false

wine_build_path = get_option('wine_build_path')
wine_install_path = get_option('wine_install_path')

if host_machine.system() == 'darwin'
  cpp = meson.get_compiler('cpp')
  dxmt9_winemetal_unix_common_link_args += cpp.get_supported_link_arguments([
    '-Wl,-no_warn_duplicate_libraries',
  ])

  c_args = get_option('c_args')
  if c_args.length() >= 2
    foreach i : range(c_args.length() - 1)
      if c_args[i] == '-arch'
        arch_name = c_args[i + 1]
        if arch_name == 'x86_64'
          dxmt9_winemetal_unix_runtime_subdir = 'x86_64-unix'
        elif arch_name == 'arm64'
          dxmt9_winemetal_unix_runtime_subdir = 'aarch64-unix'
        endif
      endif
    endforeach
  endif

  if wine_build_path != ''
    if not wine_build_path.startswith('/')
      wine_build_path = join_paths(meson.project_source_root(), wine_build_path)
    endif
    wine_winemac_dir = join_paths(wine_build_path, 'dlls/winemac.drv')
    wine_ntdll_dir = join_paths(wine_build_path, 'dlls/ntdll')
    dxmt9_winemetal_unix_common_link_args += [
      join_paths(wine_winemac_dir, 'winemac.so'),
      join_paths(wine_ntdll_dir, 'ntdll.so'),
    ]
    dxmt9_winemetal_unix_build_rpath = dxmt9_winemetal_unix_build_rpath + ':' + wine_winemac_dir + ':' + wine_ntdll_dir
    dxmt9_winemetal_unix_override_options = { 'b_asneeded' : false }
    dxmt9_winemetal_unix_needs_install_name_fixup = true
  elif wine_install_path != ''
    if not wine_install_path.startswith('/')
      wine_install_path = join_paths(meson.project_source_root(), wine_install_path)
    endif
    wine_unix_runtime_dir = join_paths(wine_install_path, 'lib/wine', dxmt9_winemetal_unix_runtime_subdir)
    dxmt9_winemetal_unix_common_link_args += [
      join_paths(wine_unix_runtime_dir, 'winemac.so'),
      join_paths(wine_unix_runtime_dir, 'ntdll.so'),
    ]
    dxmt9_winemetal_unix_build_rpath = dxmt9_winemetal_unix_build_rpath + ':' + wine_unix_runtime_dir
    dxmt9_winemetal_unix_override_options = { 'b_asneeded' : false }
    dxmt9_winemetal_unix_needs_install_name_fixup = true
  endif
endif

dxmt9_winemetal_unix_install_dir = get_option('libdir') / 'wine' / dxmt9_winemetal_unix_runtime_subdir
dxmt9_winemetal_unix_framework_dep = dependency('appleframeworks', modules: [
  'AppKit',
  'Foundation',
  'Metal',
  'QuartzCore',
])

dxmt9_winemetal_unix_module = shared_library(
  'winemetal',
  files(
    'winemetal_unix.cpp',
    'winemetal_private_api.mm',
    # Shader service unix-call handlers (slots 0..3 of the dispatch
    # table). Not part of any reusable lib — restored from 96df4b5^
    # specifically for the PE↔ELF bridge.
    '../../dxmt9/dxmt9_provider_shader_handlers.cpp',
  ) + [
    # Generated device_c bridge server thunks (one per dxmt9c_* function
    # in include/dxmt9/device_c.h). Slots from
    # DXMT9_WINEMETAL_BRIDGE_OP_BASE upward in __wine_unix_call_funcs[]
    # delegate to these. The thunks decode the Args_* struct emitted by
    # the PE-side client and call the native dxmt9c_* implementation.
    bridge_codegen[2],  # dxmt9_wine_unix_dispatch.generated.cpp
  ],
  name_prefix: '',
  name_suffix: 'so',
  include_directories: [
    dxmt9_inc,
    bridge_gen_inc,
    dxmt9_winemetal_internal_inc,
    dxmt9_runtime_internal_inc,
    include_directories('../..'),
  ],
  # link_whole the full native runtime + frontend so the generated
  # dxmt9c_* server thunks resolve their callees end-to-end:
  #   dxmt9c_factory_create  (d3d9/device_c_bridge_factory.cpp)
  #     → dxmt9p_factory_create  (d3d9/device_c_factory.cpp)
  #       → core::Factory(...) using dxmt9::Device + CommandQueue
  #         + Presenter from dxmt9_runtime_lib.
  # Mirrors libdxmt9_native.dylib's link_whole set so winemetal.so is a
  # self-contained .so replica of the native runtime, loadable as a
  # Wine unix module.
  link_whole: [
    dxmt9_frontend_bridge_lib,
    dxmt9_frontend_core_lib,
    dxmt9_frontend_provider_lib,
    dxmt9_runtime_lib,
    dxmt9_shader_service_core_lib,
    dxmt9_provider_service_abi_lib,
  ],
  dependencies: [
    dxmt9_util_dep,
    dxmt9_winemetal_unix_framework_dep,
  ],
  cpp_args: ['-DWINE_UNIX_LIB=1'],
  link_args: ['-Wl,-install_name,@rpath/winemetal.so'] + dxmt9_winemetal_unix_common_link_args,
  build_rpath: dxmt9_winemetal_unix_build_rpath,
  install_rpath: '@loader_path/',
  override_options: dxmt9_winemetal_unix_override_options,
  install: true,
  install_dir: dxmt9_winemetal_unix_install_dir,
)

if host_machine.system() == 'darwin' and dxmt9_winemetal_unix_needs_install_name_fixup
  custom_target(
    'winemetal_unix_install_name_fixup',
    input: dxmt9_winemetal_unix_module,
    output: 'winemetal_unix_install_name_fixup.stamp',
    command: [
      'sh', '-c',
      'if otool -L "$1" | grep -Fq "winemac.so "; then install_name_tool -change winemac.so @rpath/winemac.so "$1"; fi; if otool -L "$1" | grep -Fq "ntdll.so "; then install_name_tool -change ntdll.so @rpath/ntdll.so "$1"; fi; touch "$2"',
      'sh', '@INPUT@', '@OUTPUT@',
    ],
    build_by_default: true,
  )
endif
