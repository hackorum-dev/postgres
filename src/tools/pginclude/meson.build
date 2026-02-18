# Copyright (c) 2026, PostgreSQL Global Development Group

# Native headerscheck: compile every source-tree header standalone to
# verify it doesn't have missing includes or other portability issues.
#
# At configure time, headercheck_discover.py scans the source tree and
# emits one "prelude:path" line per header.  At build time,
# headercheck_generate.py produces a tiny .c/.cpp file for each header
# that #includes the appropriate prelude (postgres.h, postgres_fe.h,
# c.h, or nothing) followed by the header under test.  These files are
# compiled in a static_library as part of the default build.
#
# Controlled by -Dheaderscheck=true|false (default: true).
#
# The skip list (headerscheck_skip) is shared with the shell script
# (src/tools/pginclude/headerscheck) used by the make build.

if not get_option('headerscheck')
  subdir_done()
endif

headercheck_discover = files('headercheck_discover.py')
headercheck_skip_file = files('headerscheck_skip')

# --- C mode ---

c_header_entries = run_command(python, headercheck_discover,
  meson.project_source_root(), headercheck_skip_file, 'c',
  check: true).stdout().strip().split('\n')

c_gen_names = []
foreach entry : c_header_entries
  parts = entry.split(':')
  path = parts[1]
  c_gen_names += 'hdrchk_' + path.underscorify() + '.c'
endforeach

headercheck_c_gen = custom_target('headercheck_c_gen',
  output: c_gen_names,
  command: [python, files('headercheck_generate.py'),
            '@OUTDIR@', 'false'] + c_header_entries,
  depends: [generated_backend_headers_stamp],
)

# Include paths matching what the shell script uses.  Adding all extra
# paths (perl, python, ecpg, etc.) is harmless for headers that don't
# need them, and avoids needing per-file compiler flags.
headercheck_args = [
  '-I' + meson.project_source_root(),
  '-I' + meson.project_build_root(),
  '-I' + meson.project_source_root() / 'src' / 'include',
  '-I' + meson.project_build_root() / 'src' / 'include',
  '-I' + meson.project_source_root() / 'src' / 'interfaces' / 'libpq',
  '-I' + meson.project_build_root() / 'src' / 'interfaces' / 'libpq',
  '-I' + meson.project_build_root() / 'src' / 'interfaces' / 'ecpg' / 'include',
  '-I' + meson.project_source_root() / 'src' / 'interfaces' / 'ecpg' / 'include',
  '-I' + meson.project_build_root() / 'src' / 'backend' / 'parser',
  '-I' + meson.project_build_root() / 'src' / 'backend' / 'utils' / 'adt',
]
if perl_dep.found()
  headercheck_args += perl_ccflags
endif

headercheck_deps = []
if python3_dep.found()
  headercheck_deps += python3_dep
endif
if icu.found()
  headercheck_deps += icu
endif
if llvm.found()
  headercheck_deps += llvm
endif

headercheck_c_lib = static_library('headercheck_c',
  headercheck_c_gen,
  c_args: headercheck_args,
  dependencies: headercheck_deps,
  implicit_include_directories: false,
  install: false,
)

# --- C++ mode ---

if have_cxx
  cxx_header_entries = run_command(python, headercheck_discover,
    meson.project_source_root(), headercheck_skip_file, 'c++',
    check: true).stdout().strip().split('\n')

  cxx_gen_names = []
  foreach entry : cxx_header_entries
    parts = entry.split(':')
    path = parts[1]
    cxx_gen_names += 'hdrchk_' + path.underscorify() + '.cpp'
  endforeach

  headercheck_cxx_gen = custom_target('headercheck_cxx_gen',
    output: cxx_gen_names,
    command: [python, files('headercheck_generate.py'),
              '@OUTDIR@', 'true'] + cxx_header_entries,
    depends: [generated_backend_headers_stamp],
  )

  headercheck_cxx_lib = static_library('headercheck_cxx',
    headercheck_cxx_gen,
    cpp_args: headercheck_args,
    dependencies: headercheck_deps,
    implicit_include_directories: false,
    install: false,
  )
endif
