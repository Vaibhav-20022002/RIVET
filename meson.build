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

is_linux = host_machine.system() == 'linux'

deps = []
srcs = [
  'src/main.cpp'
]

if is_linux 
  # On linux (Docker), we require liburing for zero-copy sockets
  deps += dependency('liburing', required: true)

  # Only compile our io_uring code on linux
  srcs += 'include/rivet/net/io_uring/IoOperation.cpp'
endif

# Build the RIVET executable
executable('rivet',
  srcs,
  include_directories : inc,
  dependencies : deps,
  install: true
)