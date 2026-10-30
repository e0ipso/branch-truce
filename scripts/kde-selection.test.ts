import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join, resolve } from 'node:path';
import { createRequire } from 'node:module';
import { runInNewContext } from 'node:vm';

const require = createRequire(import.meta.url);
const workflow = require('js-yaml').load(readFileSync(resolve('.github/workflows/ci.yml'), 'utf8'));
const selection = resolve('packaging/ci/next-release.js');
const expressionForJS = (expression: string) => expression.replace(/\bneeds\.([\w-]+)/g, 'needs["$1"]');
const formats = ['kubuntu-deb', 'fedora-rpm', 'arch-pkg'];

function repository(t: { after: (fn: () => void) => void }) {
  const cwd = mkdtempSync(join(tmpdir(), 'branch-truce-selection-'));
  t.after(() => rmSync(cwd, { recursive: true, force: true }));
  const git = (...args: string[]) => execFileSync('git', args, { cwd, encoding: 'utf8' }).trim();
  git('init', '--quiet');
  git('-c', 'user.name=Test', '-c', 'user.email=test@example.com', '-c', 'commit.gpgsign=false', 'commit', '--quiet', '--allow-empty', '-m', 'chore: fixture');
  writeFileSync(join(cwd, 'package.json'), JSON.stringify({ version: '1.2.3' }));
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
  assert.notEqual(run('schedule').status, 0);
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
        const expression = (job.if || 'true').replace(/^\$\{\{\s*|\s*\}\}$/g, '');
        const allowed = runInNewContext(expressionForJS(expression), { github: { event_name: event, ref: 'refs/heads/main' }, needs,
          cancelled: () => false, always: () => true, success: () => deps.every(dep => needs[dep]?.result === 'success') });
        const explicitStatus = /always\(|cancelled\(|success\(|failure\(/.test(expression);
        const active = Boolean(allowed) && (explicitStatus || deps.every(dep => needs[dep]?.result === 'success'));
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
  assert.deepEqual(jobs.kde.strategy.matrix.include.map((leg: any) => leg.format), formats);
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
    const info = JSON.parse(execFileSync(resolve('packaging/scripts/package-info.sh'), ['--format', format, '--version', version], { encoding: 'utf8' }));
    writeFileSync(join(output, info.filename), `${format} validated bytes`);
    const dir = join(box.cwd, format);
    const result = spawnSync('bash', ['-euo', 'pipefail', '-c', step.run], { cwd: process.cwd(), encoding: 'utf8', env: {
      PATH: process.env.PATH, OUTPUT: output, DIR: dir, FORMAT: format, VERSION: version,
      REVISION: execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(), RUN_ID: 'fixture-run',
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
