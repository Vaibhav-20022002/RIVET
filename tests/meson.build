gtest_dep = dependency('gtest', main : false, fallback : ['gtest', 'gtest_dep'])
gmock_dep = dependency('gmock', fallback : ['gtest', 'gmock_dep'])

test_inc = include_directories('.', '..', '../include')
test_main_src = 'support/TestMain.cpp'

unit_srcs = [
  test_main_src,
  'unit/memory/test_memory.cpp',
  'unit/proto/rtp/test_rtp.cpp',
  'unit/proto/rtsp/test_rtsp.cpp',
  'unit/media/test_media.cpp',
  'unit/media/test_hevc.cpp',
  'unit/core/test_core.cpp',
  'unit/config/test_config.cpp'
]

unit_tests_exe = executable('unit_tests',
  unit_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

conformance_srcs = [
  test_main_src,
  'conformance/test_conformance.cpp'
]

conformance_tests_exe = executable('conformance_tests',
  conformance_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

component_srcs = [
  test_main_src,
  'component/test_component.cpp'
]

component_tests_exe = executable('component_tests',
  component_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

multiprocess_srcs = [
  test_main_src,
  'component/test_multiprocess.cpp'
]

multiprocess_tests_exe = executable('multiprocess_tests',
  multiprocess_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

fuzz_smoke_srcs = [
  test_main_src,
  'fuzz/fuzz_rtsp_framer.cpp',
  'fuzz/fuzz_sdp_parse.cpp',
  'fuzz/fuzz_hevc_classify.cpp',
  'fuzz/fuzz_sps_ptl.cpp',
  'fuzz/test_fuzz_smoke.cpp'
]

fuzz_smoke_tests_exe = executable('fuzz_smoke_tests',
  fuzz_smoke_srcs,
  include_directories : [inc, root_inc, test_inc],
  dependencies : [rivet_dep, gtest_dep, gmock_dep]
)

fork_tsan_env = ['TSAN_OPTIONS=die_after_fork=0:halt_on_error=1']

test('unit_tests', unit_tests_exe, timeout : 60)
test('conformance_tests', conformance_tests_exe, timeout : 60)
test('component_tests', component_tests_exe, env : fork_tsan_env, timeout : 90)
test('multiprocess_tests', multiprocess_tests_exe, env : fork_tsan_env, timeout : 90)
test('fuzz_smoke_tests', fuzz_smoke_tests_exe, timeout : 60)

all_test_exes = [
  unit_tests_exe,
  conformance_tests_exe,
  component_tests_exe,
  multiprocess_tests_exe,
  fuzz_smoke_tests_exe
]

if get_option('fuzz')
  fuzz_targets = [
    ['fuzz_rtsp_framer', 'fuzz/fuzz_rtsp_framer.cpp'],
    ['fuzz_sdp_parse', 'fuzz/fuzz_sdp_parse.cpp'],
    ['fuzz_hevc_classify', 'fuzz/fuzz_hevc_classify.cpp'],
    ['fuzz_sps_ptl', 'fuzz/fuzz_sps_ptl.cpp']
  ]
  foreach ft : fuzz_targets
    executable(ft[0],
      ft[1],
      cpp_args : ['-DRIVET_LIBFUZZER_MAIN=1', '-fsanitize=fuzzer'],
      link_args : ['-fsanitize=fuzzer'],
      include_directories : [inc, root_inc, test_inc],
      dependencies : [rivet_dep]
    )
  endforeach
endif

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
    env : fork_tsan_env,
    timeout : 90
  )
endif
