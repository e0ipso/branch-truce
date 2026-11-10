import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { cpSync, mkdirSync, mkdtempSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// packaging/scripts/tsv.sh reads every table under packaging/ by its header line.

const TSV = join(dirname(fileURLToPath(import.meta.url)), 'tsv.sh');

function table(t: { after: (fn: () => void) => void }, content: string) {
  const directory = mkdtempSync(join(tmpdir(), 'branch-truce-tsv-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const path = join(directory, 'sample.tsv');
  writeFileSync(path, content);
  return (fn: string, ...args: string[]) => spawnSync('/bin/bash',
    ['-c', 'set -euo pipefail; source "$0"; "$@"', TSV, fn, path, ...args], { encoding: 'utf8' });
}

const SAMPLE = '# A comment, then a blank line.\n\nkey\tfirst\tsecond\n# Rows follow.\nalpha\ta1\ta2\n\nbeta\tb1\tb2\n';

test('keys skip comments, blank lines and the header', (t) => {
  const run = table(t, SAMPLE);
  assert.equal(run('tsv_keys').stdout, 'alpha\nbeta\n');
});

test('a field is found by its column name, not its position', (t) => {
  const run = table(t, SAMPLE);
  assert.equal(run('tsv_field', 'beta', 'second').stdout, 'b2\n');
  const reordered = table(t, 'key\tsecond\tfirst\nbeta\tb2\tb1\n');
  assert.equal(reordered('tsv_field', 'beta', 'second').stdout, 'b2\n');
  assert.equal(run('tsv_column', 'first').stdout, 'alpha\ta1\nbeta\tb1\n');
});

test('an unknown row, column or table is status 2 with one error line', (t) => {
  const run = table(t, SAMPLE);
  const cases: [string[], RegExp][] = [
    [['tsv_field', 'gamma', 'first'], /^error: sample\.tsv has no row 'gamma'; use one of: alpha beta\n$/],
    [['tsv_field', 'alpha', 'third'], /^error: sample\.tsv has no column 'third'\n$/],
    [['tsv_column', 'third'], /^error: sample\.tsv has no column 'third'\n$/],
  ];
  for (const [args, message] of cases) {
    const result = run(args[0]!, ...args.slice(1));
    assert.equal(result.status, 2, args.join(' '));
    assert.equal(result.stdout, '');
    assert.match(result.stderr, message);
  }
  const missing = spawnSync('/bin/bash', ['-c', 'source "$0"; tsv_keys /nonexistent.tsv', TSV], { encoding: 'utf8' });
  assert.equal(missing.status, 2);
  assert.match(missing.stderr, /^error: cannot read the table '\/nonexistent\.tsv'\n$/);
});


test('empty, header-only, duplicate and truncated tables fail without partial output', t => {
  for (const content of ['', 'key\tfirst\n', 'key\tfirst\na\t1\na\t2\n', 'key\tfirst\na\t1\nb\n', 'key\tfirst\na\t\n', 'key\tkey\na\t1\n']) {
    const run = table(t, content);
    for (const [fn, args] of [['tsv_keys', []], ['tsv_column', ['first']], ['tsv_field', ['a', 'first']]] as const) {
      const result = run(fn, ...args);
      assert.equal(result.status, 2);
      assert.equal(result.stdout, '');
      assert.match(result.stderr, /malformed or empty/);
    }
  }
});

test('table failures stop matrix, container selection and baseline before effects', t => {
  const directory = mkdtempSync(join(tmpdir(), 'branch-truce-table-consumers-'));
  t.after(() => rmSync(directory, { recursive: true, force: true }));
  const packaging = join(directory, 'packaging');
  mkdirSync(packaging);
  cpSync(dirname(TSV), join(packaging, 'scripts'), { recursive: true });
  cpSync(join(dirname(TSV), '../ci'), join(packaging, 'ci'), { recursive: true });
  for (const content of ['', 'id\ttarget\n']) {
    writeFileSync(join(packaging, 'formats.tsv'), content);
    for (const args of [['scripts/formats.sh', 'matrix'], ['scripts/container-check.sh', '--version', '1.2.3', '--output', join(directory, 'out')]]) {
      const result = spawnSync('bash', [join(packaging, args[0]!), ...args.slice(1)], { encoding: 'utf8' });
      assert.equal(result.status, 2, result.stderr);
      assert.equal(result.stdout, '');
      assert.match(result.stderr, /table/);
    }
  }
  cpSync(join(dirname(TSV), '../formats.tsv'), join(packaging, 'formats.tsv'));
  for (const content of ['', 'component\tminimum\n']) {
    writeFileSync(join(packaging, 'minimum-versions.tsv'), content);
    const result = spawnSync('bash', [join(packaging, 'ci/baseline.sh'), '--format', 'kubuntu-deb'], { encoding: 'utf8' });
    assert.equal(result.status, 2, result.stderr);
    assert.equal(result.stdout, '');
    assert.match(result.stderr, /table/);
  }
});
