"""Atomic report files for the native replay tools.

A report is written under a temporary name beside its destination and renamed over it, so a
reader, an interrupted run or a failed write never sees a truncated report. Importing the module
has no side effects.
"""
import json
import os
import secrets
from pathlib import Path


def write_text(path, text):
    """Replace PATH with TEXT in one rename. On failure PATH keeps its previous content."""
    path = Path(path)
    partial = path.with_name(f'.{path.name}.{secrets.token_hex(16)}.partial')
    with partial.open('x') as stream:
        try:
            try:
                os.fchmod(stream.fileno(), path.stat().st_mode & 0o777)
            except FileNotFoundError:
                pass
            stream.write(text)
            stream.close()
            os.replace(partial, path)
        finally:
            partial.unlink(missing_ok=True)


def write_json(path, value):
    """Replace PATH with VALUE as indented JSON and a final newline, atomically."""
    write_text(path, json.dumps(value, indent=2) + '\n')
