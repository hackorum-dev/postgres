# Copyright (c) 2022-2026, PostgreSQL Global Development Group

int16_sources = files(
    'int16.c',
    'int16_numeric.c',
    'int16_funcs.c',
)

int16 = shared_module('int16',
    int16_sources,
    kwargs: pg_mod_args,
)

install_data(
    'int16--1.0.sql',
    kwargs: install_data_args,
)

install_data(
    'int16.control',
    kwargs: install_data_args,
)

# Regression tests
int16_tests = [
    'init',
    'int16',
    'int16_overflow',
    'int16_numeric',
    'int16_funcs',
]

pg_test_config = {
    'testname': 'int16',
    'tests': int16_tests,
}

configure_file(
    input: 'meson.build.in',
    output: 'meson.build',
    copy: true,
    configuration: pg_test_config,
)

tests += {
    'name': 'int16',
    'sd': meson.current_source_dir(),
    'bd': meson.current_build_dir(),
    'regress': {
        'sql': files('sql', 'int16.sql'),
        'expected': files('expected', 'int16.out'),
        'schedule': files('parallel_schedule'),
    },
}
