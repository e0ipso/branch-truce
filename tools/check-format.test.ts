import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

test('format CLI rejects every mixed invocation before running the formatter', t => {
  const root = mkdtempSync(join(tmpdir(), 'branch-truce-format-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const log = join(root, 'calls');
  mkdirSync(join(root, 'bin'));
  writeFileSync(log, '');
  writeFileSync(join(root, 'bin/clang-format'), '#!/bin/sh\necho called >> "$CALLS"\n', { mode: 0o755 });
  for (const args of [['--fix', '--help'], ['--help', '--fix'], ['--fix', 'extra'], ['--fix', '--fix'], ['extra'], [''], ['-h'], ['--help']]) {
    const result = spawnSync('/bin/bash', ['tools/check-format.sh', ...args], {
      encoding: 'utf8', env: { ...process.env, PATH: `${root}/bin:${process.env.PATH}`, CALLS: log },
    });
    assert.equal(result.status, args.length === 1 && ['-h', '--help'].includes(args[0]!) ? 0 : 2, result.stderr);
    assert.match(result.stdout + result.stderr, /^Usage: /);
    assert.equal(readFileSync(log, 'utf8'), '', 'formatter ran before argument validation');
  }
});
