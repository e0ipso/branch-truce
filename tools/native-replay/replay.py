#!/usr/bin/env python3
"""Generate the approved fixture or replay its real native controls on private sway."""
import argparse
import json
from pathlib import Path
import signal
import sys

if __name__ == '__main__':
    sys.dont_write_bytecode = True

import capture_profile  # noqa: E402
import collection  # noqa: E402
from fixture import make_fixture  # noqa: E402
from processes import interrupt  # noqa: E402


def main():
    shape = capture_profile.run_shape(capture_profile.load())
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    fixture = sub.add_parser('fixture', help='Create a new deterministic merge without starting the app')
    fixture.add_argument('--output', required=True, type=Path)
    native = sub.add_parser('capture', help='Build the external driver and replay the real application twice')
    for name in ('source', 'build', 'output'):
        native.add_argument('--' + name, required=True, type=Path)
    native.add_argument('--height', type=int, choices=shape.heights, default=shape.heights[0],
                        help='logical output height from capture-profile.json (default %(default)s)')
    native.add_argument('--documentation', action='store_true', help='Also retain the fresh README preview pair')
    native.add_argument('--repeat', type=int, default=shape.repeat,
                        help='clean replays per theme, at least 2 (default %(default)s)')
    args = parser.parse_args()
    signal.signal(signal.SIGTERM, interrupt)
    if args.mode == 'fixture':
        print(json.dumps(make_fixture(args.output.resolve()), indent=2))
    else:
        report = collection.capture(source=args.source.resolve(), build=args.build.resolve(), output=args.output.resolve(),
                                    size=shape.size(args.height), repeat=args.repeat, documentation=args.documentation,
                                    schemes=shape.schemes, fonts=shape.kdeglobals_fonts, scheme_dir=Path('/usr/share/color-schemes'))
        print(f"{len(report['captures'])} native captures; {len(report['comparisons'])} comparisons, "
              'zero differing decoded pixels; cleanup confirmed')


if __name__ == '__main__':
    main()
