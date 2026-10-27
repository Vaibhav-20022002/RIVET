project('rivet', 'cpp', 
  version : '0.1.0',
  default_options : [
    'cpp_std=c++20',
    'warning_level=3',
    'werror=true' # Treat warnings as errors for strict code quality
  ]
)

# Add include directory
inc = include_directories('include')
root_inc = include_directories('.')

is_linux = host_machine.system() == 'linux'

deps = [
  dependency('threads')
]

core_srcs = [
  'src/cli.cpp'
]

if is_linux 
  # On linux (Docker), we require liburing for zero-copy sockets
  deps += dependency('liburing', required: true)

  # Only compile our io_uring code on linux
  core_srcs += 'include/rivet/net/io_uring/IoOperation.cpp'
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
  tracy_inc = include_directories('/opt/homebrew/opt/tracy/include/tracy')
  deps += declare_dependency(
    include_directories: tracy_inc,
    link_args: ['-L/opt/homebrew/opt/tracy/lib', '-lTracyClient']
  )
endif

# Build core library
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

# Build the RIVET executable
rivet_exe = executable('rivet',
  'src/main.cpp',
  dependencies : rivet_dep,
  install: true
)

if get_option('tests').allowed()
  subdir('tests')
endif
