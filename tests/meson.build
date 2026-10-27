gtest_dep = dependency('gtest', main : true, fallback : ['gtest', 'gtest_main_dep'])
gmock_dep = dependency('gmock', fallback : ['gtest', 'gmock_dep'])

test_inc = include_directories('.', '..', '../include')

unit_srcs = [
  'unit/memory/test_memory.cpp',
  'unit/proto/rtp/test_rtp.cpp',
  'unit/proto/rtsp/test_rtsp.cpp',
  'unit/media/test_media.cpp',
  'unit/core/test_core.cpp',
  'unit/config/test_config.cpp'
]

unit_tests_exe = executable('unit_tests',
  unit_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

conformance_srcs = [
  'conformance/test_conformance.cpp'
]

conformance_tests_exe = executable('conformance_tests',
  conformance_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

component_srcs = [
  'component/test_component.cpp'
]

component_tests_exe = executable('component_tests',
  component_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

test('unit_tests', unit_tests_exe)
test('conformance_tests', conformance_tests_exe)
test('component_tests', component_tests_exe)

all_test_exes = [unit_tests_exe, conformance_tests_exe, component_tests_exe]

if get_option('test_on_build')
  py = import('python').find_installation('python3')
  custom_target('run-test-gate',
    output : 'tests.stamp',
    command : [py, files('tools/run_gate.py'),
               '--stamp', '@OUTPUT@',
               '--suites', 'unit,conformance,component',
               all_test_exes],
    depends : all_test_exes,
    build_by_default : true
  )
endif

if get_option('e2e').allowed()
  py = import('python').find_installation('python3')
  test('ffmpeg_e2e',
    py,
    args : [files('e2e/test_ffmpeg_e2e.py'), rivet_exe],
    timeout : 30
  )
endif
