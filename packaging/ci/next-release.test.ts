import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { chmodSync, copyFileSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';
import { runInNewContext } from 'node:vm';
import { packageable, select, UsageError } from './next-release.js';

const require = createRequire(import.meta.url);
const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..', '..');
const workflow = require('js-yaml').load(readFileSync(join(ROOT, '.github/workflows/ci.yml'), 'utf8'));
const selection = join(ROOT, 'packaging/ci/next-release.js');
const accepts = (version: string) => packageable(version, ROOT);
const expressionForJS = (expression: string) => expression.replace(/\bneeds\.([\w-]+)/g, 'needs["$1"]');
const formats = ['kubuntu-deb', 'fedora-rpm', 'arch-pkg'];
// The kde matrix the select job generates from packaging/formats.tsv.
const matrix = (): { include: { format: string; target: string; native_fidelity: string }[] } => {
  const step = workflow.jobs.select.steps.find((item: { id?: string }) => item.id === 'formats');
  const output = mkdtempSync(join(tmpdir(), 'branch-truce-matrix-'));
  try {
    execFileSync('bash', ['-euo', 'pipefail', '-c', step.run], { cwd: ROOT, env: { PATH: process.env.PATH, GITHUB_OUTPUT: join(output, 'out') } });
    const line = readFileSync(join(output, 'out'), 'utf8').trim();
    assert.match(line, /^matrix=/);
    return JSON.parse(line.slice('matrix='.length));
  } finally {
    rmSync(output, { recursive: true, force: true });
  }
};

function repository(t: { after: (fn: () => void) => void }) {
  const cwd = mkdtempSync(join(tmpdir(), 'branch-truce-selection-'));
  t.after(() => rmSync(cwd, { recursive: true, force: true }));
  const git = (...args: string[]) => execFileSync('git', args, { cwd, encoding: 'utf8' }).trim();
  git('init', '--quiet');
  git('-c', 'user.name=Test', '-c', 'user.email=test@example.com', '-c', 'commit.gpgsign=false', 'commit', '--quiet', '--allow-empty', '-m', 'chore: fixture');
  writeFileSync(join(cwd, 'package.json'), JSON.stringify({ version: '1.2.3' }));
  // The selection reads everything from the checkout it runs in.
  mkdirSync(join(cwd, 'packaging/scripts'), { recursive: true });
  copyFileSync(join(ROOT, 'packaging/scripts/native-version.sh'), join(cwd, 'packaging/scripts/native-version.sh'));
  return { cwd, revision: git('rev-parse', 'HEAD') };
}

test('PR and main selection bind candidate, publication intent and source revision without a PR analyzer', t => {
  const box = repository(t);
  const run = (event: string, env = {}) => spawnSync(process.execPath, [selection], {
    cwd: box.cwd, env: { PATH: process.env.PATH, HOME: box.cwd, BRANCH_TRUCE_CI_EVENT: event, ...env }, encoding: 'utf8',
  });
  // No remote, release config, credentials or helpers: a privileged dry run would fail here.
  const pr = run('pull_request');
  assert.equal(pr.status, 0, pr.stderr);
  assert.deepEqual(JSON.parse(pr.stdout), { version: '1.2.3', revision: box.revision, publish: false });
  for (const candidate of ['', '1.3.0']) {
    const result = run('push', { BRANCH_TRUCE_ANALYZED_VERSION: candidate, BRANCH_TRUCE_ANALYZED_REVISION: box.revision });
    assert.equal(result.status, 0, result.stderr);
    assert.deepEqual(JSON.parse(result.stdout), { version: candidate || '1.2.3', revision: box.revision, publish: Boolean(candidate) });
  }
  for (const env of [{}, { BRANCH_TRUCE_ANALYZED_REVISION: '0'.repeat(40) }]) {
    const result = run('push', env);
    assert.notEqual(result.status, 0, 'failed or stale analysis became no-release');
    assert.equal(result.stdout, '');
  }
  const schedule = run('schedule');
  assert.equal(schedule.status, 2, 'an unsupported event is a configuration error');
  assert.equal(schedule.stdout, '');
  assert.match(schedule.stderr, /^error: Unsupported validation event schedule\n$/, 'one line, no stack trace');
  const extra = spawnSync(process.execPath, [selection, '--analyze', 'extra'], { cwd: box.cwd, encoding: 'utf8' });
  assert.equal(extra.status, 2, 'extra arguments are a usage error');
  assert.match(extra.stderr, /^error: usage: /);
});

test('selection binds the analysed candidate and accepts only packageable versions', () => {
  const revision = 'a'.repeat(40);
  assert.deepEqual(select({ event: 'pull_request', revision, packageVersion: '1.2.3-rc.1' }, accepts),
    { version: '1.2.3-rc.1', revision, publish: false });
  assert.deepEqual(select({ event: 'push', revision, analyzedRevision: revision, analyzedVersion: '', packageVersion: '1.2.3' }, accepts),
    { version: '1.2.3', revision, publish: false });
  assert.deepEqual(select({ event: 'push', revision, analyzedRevision: revision, analyzedVersion: '1.3.0', packageVersion: '1.2.3' }, accepts),
    { version: '1.3.0', revision, publish: true });
  assert.throws(() => select({ event: 'push', revision, analyzedRevision: 'b'.repeat(40), analyzedVersion: '1.3.0', packageVersion: '1.2.3' }, accepts),
    /belongs to another revision/);
  assert.throws(() => select({ event: 'schedule', revision, packageVersion: '1.2.3' }, accepts), UsageError);
  for (const version of ['1.2.3-rc-1', '1.2.3-rc.0', '1.2', 'v1.2.3', '--help', '-h']) {
    assert.equal(packageable(version, ROOT), false, version);
    assert.throws(() => select({ event: 'pull_request', revision, packageVersion: version }, accepts), /Unsupported native package version/);
  }
  for (const version of ['0.0.0', '1.2.3', '1.2.3-rc', '1.2.3-rc.10']) assert.equal(packageable(version, ROOT), true, version);
  // The grammar check is the caller's: select runs no process of its own.
  const asked: string[] = [];
  assert.deepEqual(select({ event: 'pull_request', revision, packageVersion: 'any' }, version => asked.push(version) > 0),
    { version: 'any', revision, publish: false });
  assert.deepEqual(asked, ['any']);
});

test('event dependency graphs retain all validation legs, fail closed on analysis errors and serialize publishing', () => {
  const jobs = workflow.jobs;
  // Resolve the actual job predicates against representative dependency results.
  for (const event of ['pull_request', 'push']) {
    for (const publish of [false, true]) {
      const needs: Record<string, { result: string; outputs: Record<string, string> }> = {};
      const scheduled: string[] = [];
      for (const [id, job] of Object.entries(jobs) as [string, any][]) {
        const deps: string[] = typeof job.needs === 'string' ? [job.needs] : job.needs || [];
        // GitHub's implicit success() also examines skipped ancestors, even
        // when an immediate dependency explicitly allowed that ancestor.
        const dependencySucceeded = (id: string): boolean => {
          const parent = jobs[id];
          const ancestors: string[] = typeof parent.needs === 'string' ? [parent.needs] : parent.needs || [];
          return needs[id]?.result === 'success' && ancestors.every(dependencySucceeded);
        };
        const expression = (job.if || 'true').replace(/^\$\{\{\s*|\s*\}\}$/g, '');
        const allowed = runInNewContext(expressionForJS(expression), { github: { event_name: event, ref: 'refs/heads/main' }, needs,
          cancelled: () => false, always: () => true, success: () => deps.every(dependencySucceeded) });
        const explicitStatus = /always\(|cancelled\(|success\(|failure\(/.test(expression);
        const active = Boolean(allowed) && (explicitStatus || deps.every(dependencySucceeded));
        needs[id] = { result: active ? 'success' : 'skipped', outputs: { publish: String(publish) } };
        if (active) scheduled.push(id);
        if (event === 'pull_request' && active) assert.notEqual(job.permissions?.contents, 'write', `${id} grants PR write access`);
      }
      for (const id of ['test', 'select', 'kde']) assert.ok(scheduled.includes(id), `${event} lost ${id}`);
      assert.equal(scheduled.includes('release'), event === 'push' && publish);
    }
  }
  const failedNeeds = { 'analyze-release': { result: 'failure', outputs: {} } };
  assert.equal(Boolean(runInNewContext(expressionForJS(jobs.select.if), { github: { event_name: 'push' }, needs: failedNeeds, cancelled: () => false })), false);
  assert.equal(jobs.kde.strategy.matrix, '${{ fromJSON(needs.select.outputs.matrix) }}');
  assert.equal(jobs.select.outputs.matrix, '${{ steps.formats.outputs.matrix }}');
  assert.deepEqual(matrix().include.map(leg => leg.format), formats);
  assert.equal(jobs['kde-release'], undefined);
  assert.deepEqual(jobs.release.needs, ['test', 'select', 'kde']);
  assert.equal(jobs.release.concurrency, 'release');
  const group = workflow.concurrency.group;
  assert.match(group, /github.event_name/);
  assert.match(group, /github.event.pull_request.number/);
  assert.match(group, /github.run_id/);
  assert.match(workflow.concurrency['cancel-in-progress'], /pull_request/);
  const core = jobs.test.steps.find((step: any) => step.name === 'Core and sanitizer CTest').run;
  assert.match(core, /ctest --test-dir build\/core/);
  assert.match(core, /ctest --test-dir build\/sanitize/);
});

test('matrix recording copies only the exact native candidate and binds manifest to this run', t => {
  const box = repository(t);
  const step = workflow.jobs.kde.steps.find((item: any) => item.name === 'Record the package');
  const output = join(box.cwd, 'raw');
  mkdirSync(join(output, 'previous'), { recursive: true });
  writeFileSync(join(output, 'unrelated.log'), 'must stay out');
  for (const format of formats) {
    const version = '1.3.0-rc.1';
    const info = JSON.parse(execFileSync(join(ROOT, 'packaging/scripts/package-info.sh'), ['--format', format, '--version', version], { encoding: 'utf8' }));
    writeFileSync(join(output, info.filename), `${format} validated bytes`);
    const dir = join(box.cwd, format);
    const result = spawnSync('bash', ['-euo', 'pipefail', '-c', step.run], { cwd: ROOT, encoding: 'utf8', env: {
      PATH: process.env.PATH, OUTPUT: output, DIR: dir, FORMAT: format, VERSION: version,
      REVISION: execFileSync('git', ['rev-parse', 'HEAD'], { cwd: ROOT, encoding: 'utf8' }).trim(), RUN_ID: 'fixture-run',
    } });
    assert.equal(result.status, 0, result.stderr);
    assert.deepEqual(readdirSync(dir).sort(), [info.filename, `kde-${format}.json`].sort());
    const manifest = JSON.parse(readFileSync(join(dir, `kde-${format}.json`), 'utf8'));
    assert.equal(manifest.package, info.filename);
    assert.equal(manifest.nativeVersion, info.nativeVersion);
    assert.equal(manifest.architecture, info.architecture);
    assert.equal(manifest.version, version);
    assert.equal(manifest.runId, 'fixture-run');
    assert.equal(manifest.sha256, createHash('sha256').update(readFileSync(join(dir, info.filename))).digest('hex'));
  }
});

test('workflow compilation cache reaches the build and reuses compatible revisions with isolated fresh saves', t => {
  const box = repository(t);
  const steps = workflow.jobs.kde.steps;
  const identify = steps.find((step: any) => step.id === 'images');
  const restore = steps.find((step: any) => step.id === 'restore-compiler');
  const save = steps.find((step: any) => step.name === 'Save compiler cache');
  const build = steps.find((step: any) => step.name === 'Test, package and check');
  assert.ok(restore && save, 'workflow never restores or saves compiled results');
  const expand = (value: string, context: object) => value.replace(/\$\{\{\s*(.*?)\s*\}\}/g,
    (_, expression) => String(runInNewContext(expressionForJS(expression), context)));
  for (const format of formats) {
    const envFile = join(box.cwd, `${format}.env`);
    const outputFile = join(box.cwd, `${format}.output`);
    execFileSync('bash', ['-euo', 'pipefail', '-c', identify.run], { env: {
      PATH: process.env.PATH, FORMAT: format, GITHUB_ENV: envFile, GITHUB_OUTPUT: outputFile,
    } });
    const outputs = Object.fromEntries(readFileSync(outputFile, 'utf8').trim().split('\n').map(line => {
      const index = line.indexOf('=');
      return [line.slice(0, index), line.slice(index + 1)];
    }));
    const keys: string[] = [];
    let prefix = '';
    for (const [sha, runId, attempt] of [['a'.repeat(40), '101', '1'], ['b'.repeat(40), '102', '1'], ['b'.repeat(40), '102', '2']]) {
      const context = { runner: { temp: box.cwd }, matrix: { format }, steps: { images: { outputs } },
        github: { sha, run_id: runId, run_attempt: attempt, event_name: 'pull_request', ref: 'refs/pull/7/merge' } };
      const currentPrefix = expand(restore.with['restore-keys'], context).trim();
      assert.match(currentPrefix, new RegExp(`^compiler-v1-${format}-${outputs.arch}-${outputs.id}-$`));
      assert.equal(prefix || currentPrefix, currentPrefix, 'source revision prevents compatible restore');
      prefix = currentPrefix;
      const key = expand(save.with.key, context);
      assert.equal(expand(restore.with.key, context), key);
      assert.equal(key, `${prefix}${sha}-${runId}-${attempt}`);
      keys.push(key);
      const cachePath = expand(build.env.BRANCH_TRUCE_COMPILER_CACHE, context);
      assert.equal(expand(restore.with.path, context), cachePath);
      assert.equal(expand(save.with.path, context), cachePath);
      assert.ok(cachePath.startsWith(`${box.cwd}/`));
      assert.notEqual(cachePath, join(box.cwd, 'kde-images'));
      // Invoke the real workflow shell boundary. Native compiler/mount behavior
      // is exercised separately by CompilerCacheMounts and the real-image runs.
      const adapter = join(box.cwd, 'packaging/scripts');
      mkdirSync(adapter, { recursive: true });
      writeFileSync(join(adapter, 'container-check.sh'), '#!/bin/bash\nset -euo pipefail\n' +
        'test "$BRANCH_TRUCE_COMPILER_CACHE" = "$EXPECTED_CACHE"\n' +
        'test "$*" = "--version $VERSION --format $FORMAT --output $OUTPUT"\n' +
        'mkdir -p "$BRANCH_TRUCE_COMPILER_CACHE"\nprintf compiled > "$BRANCH_TRUCE_COMPILER_CACHE/fixture"\n', { mode: 0o755 });
      execFileSync('bash', ['-euo', 'pipefail', '-c', build.run], { cwd: box.cwd, env: {
        PATH: process.env.PATH, BRANCH_TRUCE_COMPILER_CACHE: cachePath, EXPECTED_CACHE: cachePath,
        FORMAT: format, VERSION: '1.2.3', OUTPUT: join(box.cwd, 'raw'), PREVIOUS: '',
      } });
      assert.equal(readFileSync(join(cachePath, 'fixture'), 'utf8'), 'compiled');
    }
    assert.equal(new Set(keys).size, 3, 'immutable cache key prevents accumulation');
    for (const outcome of ['success', 'failure', 'cancelled']) {
      const enabled = runInNewContext(expressionForJS(save.if.replace(/^\$\{\{\s*|\s*\}\}$/g, '')), {
        success: () => outcome === 'success', steps: { build: { outcome } },
      });
      assert.equal(Boolean(enabled), outcome === 'success', 'failed build can save cache');
    }
  }
  assert.equal(workflow.jobs.kde.permissions.contents, 'read');
  assert.equal(restore.with.ref, undefined);
  assert.equal(save.with.ref, undefined, 'PR cache must keep GitHub merge-ref scope');
});

test('full fidelity uses one baseline while every platform still reaches native validation', t => {
  const box = repository(t);
  const job = workflow.jobs.kde;
  const legs = matrix().include;
  assert.deepEqual(legs.map(entry => entry.format), formats);
  const build = job.steps.find((step: any) => step.name === 'Test, package and check');
  assert.equal(build.if, undefined, 'platform validation must remain unconditional');
  const configured = String(job.env.BRANCH_TRUCE_NATIVE_FIDELITY);
  assert.match(configured, /^\$\{\{.*\}\}$/);
  const expression = configured.replace(/^\$\{\{\s*|\s*\}\}$/g, '');
  const trace = join(box.cwd, 'native-validation-routes');
  const directory = join(box.cwd, 'packaging/scripts');
  mkdirSync(directory, { recursive: true });
  writeFileSync(join(directory, 'container-check.sh'), '#!/bin/bash\nset -euo pipefail\n' +
    'printf "%s %s\\n" "$FORMAT" "${BRANCH_TRUCE_NATIVE_FIDELITY:?}" >> "$TRACE"\n', { mode: 0o755 });
  for (const leg of legs) {
    const format = leg.format;
    const mode = String(runInNewContext(expressionForJS(expression), { matrix: leg }));
    assert.equal(mode, format === 'kubuntu-deb' ? '1' : '0');
    execFileSync('bash', ['-euo', 'pipefail', '-c', build.run], { cwd: box.cwd, env: {
      PATH: process.env.PATH, FORMAT: format, VERSION: '1.2.3', OUTPUT: join(box.cwd, format),
      PREVIOUS: '', TRACE: trace, BRANCH_TRUCE_NATIVE_FIDELITY: mode,
    } });
  }
  assert.deepEqual(readFileSync(trace, 'utf8').trim().split('\n'),
    ['kubuntu-deb 1', 'fedora-rpm 0', 'arch-pkg 0']);
});


test('validator launch and infrastructure failures remain errors', t => {
  const { cwd } = repository(t);
  const validator = join(cwd, 'packaging/scripts/native-version.sh');
  for (const body of ['#!/bin/sh\nexit 1\n', '#!/bin/sh\nkill -TERM $$\n']) {
    writeFileSync(validator, body);
    assert.throws(() => packageable('1.2.3', cwd));
  }
  chmodSync(validator, 0o644);
  assert.throws(() => packageable('1.2.3', cwd), { code: 'EACCES' });
  rmSync(validator);
  assert.throws(() => packageable('1.2.3', cwd), { code: 'ENOENT' });
});
