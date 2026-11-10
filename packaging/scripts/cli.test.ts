import { test } from 'node:test';
import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// packaging/scripts/cli.sh parses the options of every packaging script and tool. Each case
// sources it in a fresh Bash and prints what cli_parse left behind.

const CLI = join(dirname(fileURLToPath(import.meta.url)), 'cli.sh');

function parse(spec: string, args: string[], operands = 'operands') {
  return spawnSync('/bin/bash', ['-c', `set -euo pipefail; source "$0"
declare -A options=(); declare -a operands=()
status=0; cli_parse options ${operands} "$1" "\${@:2}" || status=$?
for key in $(printf '%s\\n' "\${!options[@]}" | sort); do printf '%s=%s\\n' "$key" "\${options[$key]}"; done
printf 'operand=%s\\n' "\${operands[@]}"
echo "status=$status"`, CLI, spec, ...args], { encoding: 'utf8' });
}

test('values, flags and operands land in their arrays', () => {
  const result = parse('format=! version= wayland', ['--format', 'kubuntu-deb', 'a.deb', '--wayland', '--version', '', '--', '--b']);
  assert.equal(result.stderr, '');
  assert.equal(result.stdout, 'format=kubuntu-deb\nversion=\nwayland=1\noperand=a.deb\noperand=--b\nstatus=0\n');
});

test('a later value replaces an earlier one', () => {
  assert.match(parse('format=', ['--format', 'a', '--format', 'b']).stdout, /^format=b$/m);
});

test('help ends the parse with status 1 and skips the required options', () => {
  for (const flag of ['-h', '--help']) {
    const result = parse('format=!', [flag, '--bogus']);
    assert.equal(result.stderr, '');
    assert.match(result.stdout, /^help=1$/m);
    assert.match(result.stdout, /status=1\n$/);
  }
});

test('usage errors exit 2 with one line naming the problem', () => {
  const cases: [string, string[], string, string?][] = [
    ['format=!', [], 'error: --format is required'],
    ['format=!', ['--format', ''], 'error: --format is required'],
    ['format=', ['--format'], 'error: --format needs a value'],
    ['format=', ['--other', 'x'], 'error: unknown option --other'],
    ['format=', ['-x'], 'error: unknown option -x'],
    ['format=', ['operand'], 'error: unexpected argument operand', '-'],
    ['format=', ['--', 'operand'], 'error: unexpected argument operand', '-'],
  ];
  for (const [spec, args, message, operands] of cases) {
    const result = parse(spec, args, operands);
    assert.equal(result.stderr, `${message}\n`, `${spec} ${args.join(' ')}`);
    assert.match(result.stdout, /status=2\n$/);
  }
});

test('cli_usage prints the usage on stdout for help and on stderr for errors', () => {
  const run = (status: number) => spawnSync('/bin/bash', ['-c',
    'source "$0"; usage() { echo "usage: tool"; }; cli_usage "$1" usage; echo "status=$?"', CLI, String(status)],
  { encoding: 'utf8' });
  assert.deepEqual([run(1).stdout, run(1).stderr], ['usage: tool\nstatus=0\n', '']);
  assert.deepEqual([run(2).stdout, run(2).stderr], ['status=2\n', 'usage: tool\n']);
});
