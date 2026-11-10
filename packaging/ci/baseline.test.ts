import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';

test('RPM baseline comparison accepts only successful exact comparison values', t => {
  const root = mkdtempSync(join(tmpdir(), 'branch-truce-baseline-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  mkdirSync(join(root, 'bin'));
  writeFileSync(join(root, 'bin/rpm'), '#!/bin/bash\nprintf "%s %s\\n" "$OLD" "$NEW" > "$CALLS"\nprintf "%s" "$ORDER"\nexit "$RESULT"\n', { mode: 0o755 });
  for (const [order, result, status] of [['0', 0, 0], ['1\n', 0, 0], ['-1', 0, 1], ['', 7, 1],
    ['1', 7, 1], ['', 0, 1], ['garbage', 0, 1], ['2', 0, 1], ['-2', 0, 1], [' 1', 0, 1], ['1\n-1', 0, 1]] as const) {
    const run = spawnSync('/bin/bash', ['-c', 'source "$1" --help >/dev/null; rpm_at_least 6.10 6.8', '_', resolve('packaging/ci/baseline.sh')], {
      encoding: 'utf8', timeout: 5000, env: { ...process.env, PATH: `${root}/bin:${process.env.PATH}`,
        CALLS: join(root, 'calls'), ORDER: order, RESULT: String(result) },
    });
    assert.equal(run.status, status, `${JSON.stringify({ order, result })}: ${run.stderr}`);
    assert.equal(run.stdout, '');
    assert.equal(readFileSync(join(root, 'calls'), 'utf8'), '6.10 6.8\n');
  }
});
