dxmt9_render_tape_tool = executable(
  'dxmt9-render-tape',
  'dxmt9_render_tape.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  install: false,
)

dxmt9_render_tape_provider_tool = executable(
  'dxmt9-render-tape-provider',
  'dxmt9_render_tape_provider.cpp',
  include_directories: [
    dxmt9_inc,
    include_directories('../src/dxmt9'),
    include_directories('../src/d3d9'),
  ],
  dependencies: [dxmt9_dep, dxmt9_frontend_dep],
  install: false,
)

if host_machine.system() == 'darwin'
  dxmt9_optimizer_probe = executable(
    'dxmt9-framegraph-optimizer-probe',
    'dxmt9_framegraph_optimizer_probe.cpp',
    'reference_passcoalesce.cpp',
    'passcoalesce_refinement.cpp',
    include_directories: dxmt9_inc,
    dependencies: [dxmt9_dep, dxmt9_frontend_dep],
    install: false,
  )
  if get_option('build_tests')
    test('dxmt9-framegraph-optimizer-fixture', dxmt9_optimizer_probe,
         args: ['--self-test'])
  endif
  dxmt9_thread_cpu_tool = executable(
    'dxmt9-thread-cpu',
    'dxmt9_thread_cpu.cpp',
    install: false,
  )
endif
