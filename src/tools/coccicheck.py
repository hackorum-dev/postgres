#!/usr/bin/env python3

"""Run Coccinelle on a set of files and directories.

This is a re-written version of the Linux ``coccicheck`` script.

Coccicheck can run in two different modes (the original have four
different modes):

- *patch*: patch files using the cocci file.

- *report*: report will report any improvements that this script can
  make, but not show any patch.

- *context*: show the context where the patch can be applied.

The program will take a single cocci file and call spatch(1) with a
set of paths that can be either files or directories.

When starting, the cocci file will be parsed and any lines containing
"Options:" or "Requires:" will be treated specially.

- Lines containing "Options:" will have a list of options to add to
  the call of the spatch(1) program. These options will be added last.

- Lines containing "Requires:" can contain a version of spatch(1) that
  is required for this cocci file. If the version requirements are not
  satisfied, the file will not be used.

When calling spatch(1), it will set the virtual rules "patch",
"report", or "context" and the cocci file can use these to act
differently depending on the mode.

The following environment variables can be set:

SPATCH: Path to spatch program. This will be used if no path is
  passed using the option --spatch.

SPFLAGS: Extra flags to use when calling spatch. These will be added
  last.

MODE: Mode to use. It will be used if no --mode is passed to
  coccicheck.py.

"""

import argparse
import os
import sys
import subprocess
import re

from pathlib import PurePath, Path
from packaging import version

VERSION_CRE = re.compile(
    r'spatch version (\S+) compiled with OCaml version (\S+)'
)


def parse_metadata(cocci_file):
    """Parse metadata in Cocci file."""
    metadata = {}
    with open(cocci_file) as fh:
        for line in fh:
            mre = re.match(r'(Options|Requires):(.*)', line, re.IGNORECASE)
            if mre:
                metadata[mre.group(1).lower()] = mre.group(2)
    return metadata


def get_config(args):
    """Compute configuration information."""
    # Figure out spatch version. We just need to read the first line
    config = {}
    cmd = [args.spatch, '--version']
    with subprocess.Popen(cmd, stdout=subprocess.PIPE, text=True) as proc:
        for line in proc.stdout:
            mre = VERSION_CRE.match(line)
            if mre:
                config['spatch_version'] = mre.group(1)
                break
    return config


def run_spatch(cocci_file, args, config, env):
    """Run coccinelle on the provided file."""
    if args.verbose > 1:
        print("processing cocci file", cocci_file)
    spatch_version = config['spatch_version']
    metadata = parse_metadata(cocci_file)

    # Check that we have a valid version
    if 'required' in metadata:
        required_version = version.parse(metadata['required'])
        if required_version < spatch_version:
            print(
                f'Skipping SmPL patch {cocci_file}: '
                f'requires {required_version} (had {spatch_version})'
            )
            return

    command = [
        args.spatch,
        "-D",  args.mode,
        "--cocci-file", cocci_file,
        "--very-quiet",
    ]

    if 'options' in metadata:
        command.append(metadata['options'])
    if args.mode == 'report':
        command.append('--no-show-diff')
    if args.patchdir:
        command.extend(['--patch', args.patchdir])
    if args.jobs:
        command.extend(['--jobs', args.jobs])
    if args.spflags:
        command.append(args.spflags)

    for path in args.path:
        subprocess.run(command + [path], env=env, check=True)


def coccinelle(args, config, env):
    """Run coccinelle on all files matching the provided pattern."""
    root = '/' if PurePath(args.cocci).is_absolute() else '.'
    count = 0
    for cocci_file in Path(root).glob(args.cocci):
        count += 1
        run_spatch(cocci_file, args, config, env)
    return count


def main(argv):
    """Run coccicheck."""
    parser = argparse.ArgumentParser()
    parser.add_argument('--verbose', '-v', action='count', default=0)
    parser.add_argument('--spatch', type=PurePath, metavar='SPATCH',
                        default=os.environ.get('SPATCH'),
                        help=('Path to spatch binary. Defaults to '
                              'value of environment variable SPATCH.'))
    parser.add_argument('--spflags', type=PurePath,
                        metavar='SPFLAGS',
                        default=os.environ.get('SPFLAGS', None),
                        help=('Flags to pass to spatch call. Defaults '
                              'to value of enviroment variable SPFLAGS.'))
    parser.add_argument('--mode', choices=['patch', 'report', 'context'],
                        default=os.environ.get('MODE', 'report'),
                        help=('Mode to use for coccinelle. Defaults to '
                              'value of environment variable MODE.'))
    parser.add_argument('--jobs', default=os.environ.get('JOBS', None),
                        help=('Number of jobs to use for spatch. Defaults to '
                              'value of environment variable JOBS.'))
    parser.add_argument('--include', '-I', type=PurePath,
                        metavar='DIR',
                        help='Extra include directories.')
    parser.add_argument('--patchdir', type=PurePath, metavar='DIR',
                        help=('Path for which patch should be created '
                              'relative to.'))
    parser.add_argument('cocci', metavar='pattern',
                        help='Pattern for Cocci files to use.')
    parser.add_argument('path', nargs='+', type=PurePath,
                        help='Directory or source path to process.')

    args = parser.parse_args(argv)

    if args.verbose > 1:
        print("arguments:", args)

    if args.spatch is None:
        parser.error('spatch is part of the Coccinelle project and is '
                     'available at http://coccinelle.lip6.fr/')

    if coccinelle(args, get_config(args), os.environ) == 0:
        parser.error(f'no coccinelle files found matching {args.cocci}')


if __name__ == '__main__':
    try:
        main(sys.argv[1:])
    except KeyboardInterrupt:
        print("Execution aborted")
    except Exception as exc:
        print(exc)
