# Copyright (c) 2022-2026, PostgreSQL Global Development Group

ecpg_inc = include_directories('.')

ecpg_conf_keys = [
  'SIZEOF_LONG',
  'SIZEOF_LONG_LONG',
]

ecpg_conf_data = configuration_data()

ecpg_conf_data.set('ENABLE_THREAD_SAFETY', 1)

foreach key : ecpg_conf_keys
  if cdata.has(key)
    ecpg_conf_data.set(key, cdata.get(key))
  endif
endforeach

ecpg_config_h = configure_file(
  output: 'ecpg_config.h',
  configuration: ecpg_conf_data,
  install_dir: dir_include,
)
configure_files += ecpg_config_h

generated_sources_ac += {'src/interfaces/ecpg/include': ['stamp-h']}

_h = []
foreach l : fs.read('public_headers.list').strip().split('\n')
  l = l.strip()
  if l != '' and not l.startswith('#')
    _h += l
  endif
endforeach
install_headers(_h)

_h = []
foreach l : fs.read('informix_headers.list').strip().split('\n')
  l = l.strip()
  if l != '' and not l.startswith('#')
    _h += l
  endif
endforeach
install_headers(_h, install_dir: dir_include_pkg / 'informix' / 'esql')
