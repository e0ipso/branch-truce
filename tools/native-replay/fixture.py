"""Exact approved sources and a disposable, deterministic three-file Git merge."""
from pathlib import Path
import hashlib
import json
import os
import struct
import subprocess
import zlib

REFERENCE = Path(__file__).resolve().parent / 'reference'
PATHS = ['src/net/retry.cpp', 'docs/release-notes.md', 'assets/logo.png']


def sha256(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def reference_bytes(name):
    row = next(r for r in json.loads((REFERENCE / 'provenance.json').read_text())['files']
               if r['file'] == name)
    data = (REFERENCE / name).read_bytes()
    if len(data) != row['bytes'] or hashlib.sha256(data).hexdigest() != row['sha256']:
        raise ValueError(f'Pinned reference changed: {name}')
    return data


def png(colour):
    def chunk(kind, data):
        return (struct.pack('>I', len(data)) + kind + data
                + struct.pack('>I', zlib.crc32(kind + data) & 0xffffffff))
    header = b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 64, 64, 8, 2, 0, 0, 0))
    pixels = chunk(b'IDAT', zlib.compress((b'\x00' + bytes(colour) * 64) * 64))
    ending = chunk(b'IEND', b'')
    padding = chunk(b'tEXt', b'fixture\x00' + b'.' * (18637 - len(header + pixels + ending) - 20))
    return header + pixels + padding + ending


def git(repo, *args, check=True):
    # Ignore invoking-user Git settings, templates, hooks and repository overrides.
    env = {k: v for k, v in os.environ.items() if not k.startswith('GIT_')}
    env.update(GIT_CONFIG_GLOBAL='/dev/null', GIT_CONFIG_NOSYSTEM='1',
               GIT_ATTR_NOSYSTEM='1', GIT_TERMINAL_PROMPT='0', LC_ALL='C.UTF-8',
               GIT_AUTHOR_NAME='Branch Truce Fixture', GIT_AUTHOR_EMAIL='fixture@example.invalid',
               GIT_COMMITTER_NAME='Branch Truce Fixture', GIT_COMMITTER_EMAIL='fixture@example.invalid',
               GIT_AUTHOR_DATE='2000-01-01T00:00:00+0000', GIT_COMMITTER_DATE='2000-01-01T00:00:00+0000')
    return subprocess.run(['git', '-c', 'core.hooksPath=/dev/null', '-c', 'core.attributesFile=/dev/null', '-C', str(repo), *args],
                          env=env, capture_output=True, check=check)


def make_fixture(root):
    root = Path(root)
    root.mkdir(exist_ok=False)
    repo = root / 'retry-service'
    repo.mkdir()
    left, right = (reference_bytes(f'expected-{side}.cpp') for side in ('left', 'right'))
    base = left.replace(b'maxAttempts = 5', b'maxAttempts = 3').replace(b'attemptTimeout = 2s', b'attemptTimeout = 1s')
    base = base.replace(b'send(request, attemptTimeout)', b'send(request)').replace(b'        logAttempt(request, attempt, result.error());\n', b'')
    git(repo, 'init', '--template=', '--object-format=sha1', '-b', 'main')
    git(repo, 'config', 'commit.gpgsign', 'false')
    git(repo, 'config', 'merge.conflictStyle', 'merge')
    notes = {'base': 'Retry policy is unchanged.', 'main': 'Add retry timeouts.',
             'feature/backoff': 'Add backoff between retries.'}
    messages = {'base': 'chore: add retry fixture', 'main': 'feat: add retry timeouts',
                'feature/backoff': 'feat: add retry backoff'}
    for branch, code, colour in [('base', base, (100, 100, 100)),
                                  ('feature/backoff', right, (61, 174, 233)),
                                  ('main', left, (39, 174, 96))]:
        if branch == 'feature/backoff':
            git(repo, 'checkout', '-b', branch)
        if branch == 'main':
            git(repo, 'checkout', 'main')
        for name in PATHS:
            (repo / name).parent.mkdir(parents=True, exist_ok=True)
        (repo / PATHS[0]).write_bytes(code)
        (repo / PATHS[1]).write_bytes(('# Release notes\n\n' + notes[branch] + '\n').encode())
        (repo / PATHS[2]).write_bytes(png(colour))
        git(repo, 'add', '--', *PATHS)
        git(repo, 'commit', '-m', messages[branch])
    merged = git(repo, 'merge', '--no-edit', 'feature/backoff', check=False)
    if merged.returncode != 1 or sorted(unmerged(repo)) != sorted(PATHS):
        raise RuntimeError(f'Expected exactly three conflicts: {merged.stdout!r} {merged.stderr!r}')
    for side in ('left', 'right', 'result'):
        (root / f'expected-{side}.cpp').write_bytes(reference_bytes(f'expected-{side}.cpp'))
    manifest = dict(main=git(repo, 'rev-parse', 'main').stdout.decode().strip(),
                    incoming=git(repo, 'rev-parse', 'feature/backoff').stdout.decode().strip(),
                    base=git(repo, 'merge-base', 'main', 'feature/backoff').stdout.decode().strip(),
                    left_sha256=sha256(root / 'expected-left.cpp'),
                    right_sha256=sha256(root / 'expected-right.cpp'),
                    result_sha256=sha256(root / 'expected-result.cpp'),
                    image_bytes=18637, image_sha256=sha256(repo / PATHS[2]))
    (root / 'fixture.json').write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def unmerged(repo):
    return git(repo, 'diff', '--name-only', '--diff-filter=U').stdout.decode().splitlines()
