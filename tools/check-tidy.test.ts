import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

test('tidy validates the complete invocation before running tools or creating a manifest', t => {
  const root = mkdtempSync(join(tmpdir(), 'branch-truce-tidy-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const log = join(root, 'calls');
  mkdirSync(join(root, 'bin'));
  writeFileSync(log, '');
  writeFileSync(join(root, 'compile_commands.json'), '[]');
  for (const tool of ['clang-tidy', 'python3', 'mktemp', 'xargs']) {
    writeFileSync(join(root, 'bin', tool), '#!/bin/sh\necho called >> "$CALLS"\nexit 99\n', { mode: 0o755 });
  }
  for (const args of [[], [root, '1', 'extra'], ['--help', 'extra'], [root, '0'], [root, '-1'], [root, '1x'], [''], ['--help'], ['-h']]) {
    const result = spawnSync('/bin/bash', ['tools/check-tidy.sh', ...args], {
      encoding: 'utf8', env: { ...process.env, PATH: `${root}/bin:${process.env.PATH}`, CALLS: log }, timeout: 5000,
    });
    const help = args.length === 1 && ['-h', '--help'].includes(args[0]!);
    assert.equal(result.status, help ? 0 : 2, result.stderr);
    assert.match(help ? result.stdout : result.stderr, /^(Usage: |Jobs must be a positive integer)/);
    assert.equal(help ? result.stderr : result.stdout, '');
    assert.equal(readFileSync(log, 'utf8'), '', 'work started before validation');
  }
});
