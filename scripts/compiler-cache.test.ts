import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

test('compiler caches survive compatible builds and remain outside installed-runtime containers', () => {
  const result = spawnSync('bash', [fileURLToPath(new URL('../tests/packaging/CompilerCacheMounts.sh', import.meta.url))], {
    encoding: 'utf8',
  });
  assert.equal(result.status, 0, result.stdout + result.stderr);
});
