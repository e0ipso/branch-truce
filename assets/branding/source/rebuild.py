#!/usr/bin/env python3
"""Rebuild the exports and the guide, then refresh SHA256SUMS.txt.

Needs the same tools as build.py: Python 3, rsvg-convert, and ImageMagick 7.
Run from any directory. RSVG_CONVERT and MAGICK are passed on to build.py.
"""
import hashlib
from pathlib import Path
import subprocess
import sys

SOURCE = Path(__file__).resolve().parent
ROOT = SOURCE.parent
MANIFEST = ROOT / 'SHA256SUMS.txt'


def run(script):
    subprocess.run([sys.executable, '-B', str(SOURCE / script)], check=True)


def write_manifest():
    files = sorted(path.relative_to(ROOT).as_posix() for path in ROOT.rglob('*')
                   if path.is_file() and path != MANIFEST and '__pycache__' not in path.parts)
    MANIFEST.write_text(''.join(
        f'{hashlib.sha256((ROOT / name).read_bytes()).hexdigest()}  {name}\n' for name in files))
    print(f'Wrote {MANIFEST.name} for {len(files)} files.')


if __name__ == '__main__':
    run('build.py')
    run('make_guide.py')
    write_manifest()
