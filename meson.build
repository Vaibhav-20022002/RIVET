project('rivet', 'cpp',
  version : '0.2.0',
  default_options : [
    'cpp_std=c++20',
    'buildtype=release',
    'b_ndebug=if-release',
    'warning_level=3',
    'werror=true'
  ]
)

inc = include_directories('include')
root_inc = include_directories('.')

is_linux = host_machine.system() == 'linux'

deps = [
  dependency('threads')
]

core_srcs = [
  'src/cli.cpp',
  'src/worker_main.cpp'
]

if is_linux and get_option('io_uring').allowed()
  liburing_dep = dependency('liburing', required: get_option('io_uring').enabled())
  if liburing_dep.found()
    deps += liburing_dep
    add_project_arguments('-DRIVET_HAS_IO_URING=1', language: 'cpp')
    core_srcs += 'include/rivet/net/io_uring/IoOperation.cpp'
  endif
endif

if get_option('trace_probes')
  add_project_arguments('-DRIVET_ENABLE_PROBES=1', language: 'cpp')
endif

if get_option('coverage_gate')
  add_project_arguments('-fprofile-instr-generate', '-fcoverage-mapping', language: 'cpp')
  add_project_link_arguments('-fprofile-instr-generate', language: 'cpp')
endif

if get_option('tracy')
  add_project_arguments('-DRIVET_ENABLE_TRACY=1', '-DTRACY_ENABLE=1', language: 'cpp')
  tracy_dep = dependency('tracy', required: false)
  if tracy_dep.found()
    deps += tracy_dep
  else
    tracy_prefix = get_option('tracy_dir')
    if tracy_prefix == ''
      error('Tracy enabled (-Dtracy=true) but pkg-config tracy not found and -Dtracy_dir was not provided')
    endif
    deps += declare_dependency(
      include_directories: include_directories(tracy_prefix / 'include' / 'tracy'),
      link_args: ['-L' + (tracy_prefix / 'lib'), '-lTracyClient']
    )
  endif
endif

rivet_core_lib = static_library('rivet_core',
  core_srcs,
  include_directories : inc,
  dependencies : deps
)

rivet_dep = declare_dependency(
  link_with : rivet_core_lib,
  include_directories : inc,
  dependencies : deps
)

rivet_exe = executable('rivet',
  'src/main.cpp',
  dependencies : rivet_dep,
  install : true
)

if get_option('tests').allowed()
  subdir('tests')
endif

if get_option('benchmarks')
  subdir('bench')
endif
