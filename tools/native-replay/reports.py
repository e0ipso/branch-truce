"""Atomic report files for the native replay tools.

A report is written under a temporary name beside its destination and renamed over it, so a
reader, an interrupted run or a failed write never sees a truncated report. Importing the module
has no side effects.
"""
import json
import os
from pathlib import Path


def write_text(path, text):
    """Replace PATH with TEXT in one rename. On failure PATH keeps its previous content."""
    path = Path(path)
    partial = path.with_name(f'.{path.name}.partial')
    try:
        partial.write_text(text)
        os.replace(partial, path)
    except BaseException:
        partial.unlink(missing_ok=True)
        raise


def write_json(path, value):
    """Replace PATH with VALUE as indented JSON and a final newline, atomically."""
    write_text(path, json.dumps(value, indent=2) + '\n')
