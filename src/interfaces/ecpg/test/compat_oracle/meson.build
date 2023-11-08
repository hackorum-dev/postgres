# Copyright (c) 2022-2026, PostgreSQL Global Development Group

pgc_files = [
  'char_array',
]

exe_preproc_kw = {}

# ecpg leaks memory like no tomorrow; silence AddressSanitizer
if get_option('b_sanitize').contains('address')
  exe_preproc_kw += {
    'env': {
      'ASAN_OPTIONS': 'detect_leaks=0'
    }
  }
endif

foreach pgc_file : pgc_files
  exe_input = custom_target('@0@.c'.format(pgc_file),
    input: '@0@.pgc'.format(pgc_file),
    output: '@BASENAME@.c',
    command: ecpg_preproc_test_command_start +
      ['-C', 'ORACLE',] +
      ecpg_preproc_test_command_end,
    install: false,
    build_by_default: false,
    kwargs: exe_preproc_kw,
  )

  ecpg_test_dependencies += executable(pgc_file,
    exe_input,
    kwargs: ecpg_test_exec_kw,
  )
endforeach
