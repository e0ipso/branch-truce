import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { copyFileSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// tools/check-shell-syntax.sh checks the scripts under the tree it sits in, so each test copies it
// into a throwaway tree with the scripts the case needs.

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');

function check(t: { after: (fn: () => void) => void }, scripts: Record<string, string>) {
  const root = mkdtempSync(join(tmpdir(), 'branch-truce-shell-syntax-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  mkdirSync(join(root, 'tools'));
  copyFileSync(join(ROOT, 'tools/check-shell-syntax.sh'), join(root, 'tools/check-shell-syntax.sh'));
  for (const [path, content] of Object.entries(scripts)) {
    mkdirSync(dirname(join(root, path)), { recursive: true });
    writeFileSync(join(root, path), content);
  }
  return spawnSync('/bin/bash', [join(root, 'tools/check-shell-syntax.sh')], {
    env: { PATH: '/usr/bin:/bin', LC_ALL: 'C' }, encoding: 'utf8',
  });
}

test('every script is checked and reported, and a syntax error fails the run with status 1', (t) => {
  const result = check(t, {
    'tests/a.sh': 'if then\n',
    'tests/b.sh': 'echo fine\n',
    'packaging/c.sh': 'fi\n',
  });
  assert.equal(result.status, 1, result.stderr);
  for (const script of ['tests/a.sh', 'tests/b.sh', 'packaging/c.sh', 'tools/check-shell-syntax.sh']) {
    assert.match(result.stdout, new RegExp(`^Syntax checked: ${script}$`, 'm'));
  }
  assert.match(result.stderr, /^tests\/a\.sh: syntax error$/m);
  assert.match(result.stderr, /^packaging\/c\.sh: syntax error$/m);
  assert.doesNotMatch(result.stderr, /b\.sh: syntax error/);
});

test('a bare negated command fails the run, a guarded one does not', (t) => {
  const result = check(t, {
    'tests/bare.sh': 'true\n  ! grep -q x /dev/null\n',
    'tests/guarded.sh': '! grep -q x /dev/null || exit 1\n',
  });
  assert.equal(result.status, 1, result.stderr);
  assert.match(result.stderr, /^tests\/bare\.sh:2: a bare negated command never fails the script$/m);
  assert.doesNotMatch(result.stderr, /guarded\.sh/);
});

test('clean scripts pass with status 0 and a count', (t) => {
  const result = check(t, { 'tests/b.sh': 'echo fine\n' });
  assert.equal(result.status, 0, result.stderr);
  assert.match(result.stdout, /^2 project shell scripts passed syntax checks$/m);
});

test('an operand is a usage error with status 2', () => {
  const result = spawnSync('/bin/bash', [join(ROOT, 'tools/check-shell-syntax.sh'), 'extra'], { encoding: 'utf8' });
  assert.equal(result.status, 2);
  assert.match(result.stderr, /^Usage: /);
});
