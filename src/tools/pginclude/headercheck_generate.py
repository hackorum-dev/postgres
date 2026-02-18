#!/usr/bin/env python3
#
# headercheck_generate.py -- generate source files for headerscheck at build time
#
# For each header entry, generates a .c (or .cpp) file that includes
# the header with the appropriate prelude.
#
# Arguments: output_dir is_cpp entry [entry ...]
#   is_cpp is "true" or "false"
#   Each entry is "prelude:relative_path" as produced by headercheck_discover.py
#
# Copyright (c) 2026, PostgreSQL Global Development Group

import os
import sys


def underscorify(s):
    """Replace non-alphanumeric characters with underscores (like meson's underscorify)."""
    return ''.join(c if c.isalnum() else '_' for c in s)


def main():
    output_dir = sys.argv[1]
    is_cpp = sys.argv[2] == 'true'
    entries = sys.argv[3:]

    ext = 'cpp' if is_cpp else 'c'

    for entry in entries:
        prelude, rel_path = entry.split(':', 1)
        out_name = 'hdrchk_' + underscorify(rel_path) + '.' + ext
        out_path = os.path.join(output_dir, out_name)

        lines = []
        if is_cpp:
            lines.append('extern "C" {')
        if prelude == 'postgres':
            lines.append('#include "postgres.h"')
        elif prelude == 'postgres_fe':
            lines.append('#include "postgres_fe.h"')
        elif prelude == 'c':
            lines.append('#include "c.h"')
        # bare: no prelude
        lines.append('#include "{}"'.format(rel_path))
        if is_cpp:
            lines.append('}')
        lines.append('')

        with open(out_path, 'w') as f:
            f.write('\n'.join(lines))


if __name__ == '__main__':
    main()
