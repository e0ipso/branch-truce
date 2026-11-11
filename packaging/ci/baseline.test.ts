import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { resolve, join } from 'node:path';

test('baseline platform checks distinguish rolling images from fixed releases', () => {
  for (const [expected, distribution, version, status] of [
    ['arch', 'arch', '', 0], ['arch', 'arch', '20261025.0.616225', 0],
    ['arch', 'manjaro', '20261025.0.616225', 1], ['arch', '', '', 1],
    ['ubuntu 26.04', 'ubuntu', '26.04', 0], ['ubuntu 26.04', 'ubuntu', '24.04', 1],
    ['ubuntu 26.04', 'ubuntu', '26.10', 1], ['ubuntu 26.04', 'ubuntu', '', 1],
    ['ubuntu 26.04', 'arch', '26.04', 1], ['fedora 44', 'fedora', '44', 0],
    ['fedora 44', 'fedora', '43', 1], ['fedora 44', 'ubuntu', '44', 1],
  ] as const) {
    const result = spawnSync('/bin/bash', ['-c',
      'source "$1" --help >/dev/null; baseline_os_matches "$2" "$3" "$4"', '_',
      resolve('packaging/ci/baseline.sh'), expected, distribution, version], { encoding: 'utf8', timeout: 5000 });
    assert.equal(result.status, status, `${expected}: ${distribution} ${version}: ${result.stderr}`);
    assert.equal(result.stdout, '');
  }
});

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
