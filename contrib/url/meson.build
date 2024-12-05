
if not icu.found()
  subdir_done()
endif

url_sources = files(
  'url.c',
  'lexbor/core/array.c',
	'lexbor/core/array_obj.c',
	'lexbor/core/bst.c',
	'lexbor/core/conv.c',
	'lexbor/core/diyfp.c',
	'lexbor/core/dobject.c',
	'lexbor/core/dtoa.c',
	'lexbor/core/mem.c',
	'lexbor/core/memory.c',
	'lexbor/core/mraw.c',
	'lexbor/core/plog.c',
	'lexbor/core/str.c',
	'lexbor/core/strtod.c',
	'lexbor/url/url.c',
)

if host_system == 'windows'
  ltree_sources += rc_lib_gen.process(win32ver_rc, extra_args: [
    '--NAME', 'url',
    '--FILEDESC', 'url - Uniform Resource Locator data type',])
endif

url = shared_module('url',
  url_sources,
  include_directories: include_directories('.'),
  kwargs: contrib_mod_args,
)
contrib_targets += url

install_data(
  'url.control',
  'url--1.0.sql',
  kwargs: contrib_data_args,
)

tests += {
  'name': 'url',
  'sd': meson.current_source_dir(),
  'bd': meson.current_build_dir(),
  'regress': {
    'sql': [
      'url'
    ],
  },
}
