import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// The saved-environment contract of packaging/scripts/compiler-cache.sh. With no cache
// directory configured, cache_begin only saves and clears its variables, so no ccache is needed.

const CACHE = join(dirname(fileURLToPath(import.meta.url)), 'compiler-cache.sh');

function shell(code: string) {
  const result = spawnSync('/bin/bash', ['-c', `set -euo pipefail; source "$0"; ${code}`, CACHE], {
    env: { PATH: '/usr/bin:/bin', CCACHE_DIR: '/original' }, encoding: 'utf8',
  });
  assert.equal(result.status, 0, result.stderr);
  return result.stdout;
}

test('cache_end restores the variables as they were before the first cache_begin', () => {
  assert.equal(shell('cache_begin one /tmp 2>/dev/null; export CCACHE_DIR=/changed; cache_begin two /tmp 2>/dev/null; cache_end; echo "$CCACHE_DIR"'),
    '/original\n');
});

test('sourcing the file again keeps an active build\'s saved values', () => {
  assert.equal(shell('cache_begin one /tmp 2>/dev/null; export CCACHE_DIR=/changed; source "$0"; cache_end; echo "$CCACHE_DIR"'),
    '/original\n');
});

test('cache_end without cache_begin leaves the environment alone', () => {
  assert.equal(shell('cache_end; echo "$CCACHE_DIR"'), '/original\n');
});
