#!/usr/bin/env python3
#
# headercheck_discover.py -- discover headers for headerscheck at configure time
#
# Scans the source tree for .h files, filters out skipped headers,
# and outputs one line per header: "prelude:relative_path"
#
# Arguments: source_root skip_file mode
#   mode is "c" or "c++"
#
# Copyright (c) 2026, PostgreSQL Global Development Group

import os
import sys


def load_skip_list(skip_file, mode):
    """Load the skip list, returning a set of paths to skip."""
    skipped = set()
    with open(skip_file) as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith('#'):
                continue
            if line.startswith('c_only:'):
                if mode == 'c':
                    skipped.add(line[len('c_only:'):].strip())
            elif line.startswith('cxx_only:'):
                if mode == 'c++':
                    skipped.add(line[len('cxx_only:'):].strip())
            else:
                skipped.add(line)
    return skipped


def get_prelude(header_path):
    """Determine the prelude (#include) needed for a header.

    Uses the same logic as the headerscheck bash script.
    """
    if header_path in ('src/include/postgres.h',
                       'src/include/postgres_fe.h',
                       'src/include/c.h'):
        return 'bare'

    if header_path in ('src/interfaces/libpq/libpq-fe.h',
                       'src/interfaces/libpq/libpq-events.h'):
        return 'bare'

    if header_path == 'src/interfaces/ecpg/ecpglib/ecpglib_extern.h':
        return 'postgres_fe'

    if header_path.startswith('src/interfaces/ecpg/ecpglib/'):
        return 'bare'

    if header_path.startswith('src/interfaces/'):
        return 'postgres_fe'

    if header_path.startswith('src/bin/'):
        return 'postgres_fe'

    if header_path.startswith('src/fe_utils/'):
        return 'postgres_fe'

    if header_path.startswith('src/port/'):
        return 'bare'

    if header_path.startswith('src/common/'):
        return 'c'

    return 'postgres'


def main():
    source_root = sys.argv[1]
    skip_file = sys.argv[2]
    mode = sys.argv[3]

    skipped = load_skip_list(skip_file, mode)

    headers = []
    for search_dir in ['src', 'contrib']:
        search_path = os.path.join(source_root, search_dir)
        if not os.path.isdir(search_path):
            continue
        for dirpath, dirnames, filenames in os.walk(search_path):
            dirnames.sort()
            for filename in sorted(filenames):
                if not filename.endswith('.h'):
                    continue
                full_path = os.path.join(dirpath, filename)
                rel_path = os.path.relpath(full_path, source_root)
                if rel_path in skipped:
                    continue
                prelude = get_prelude(rel_path)
                headers.append('{}:{}'.format(prelude, rel_path))

    for entry in headers:
        print(entry)


if __name__ == '__main__':
    main()
