import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath, pathToFileURL } from 'node:url';

// The release gate (packaging/ci/semantic-release.js) and the dry run that picks the version the
// KDE packages are built for (packaging/ci/next-release.js) run only on a real release. These
// tests use disposable repositories and metadata adapters, without touching GitHub.

const HERE = dirname(fileURLToPath(import.meta.url));
const PACKAGING = join(HERE, '..', 'packaging');
const PLUGIN = join(PACKAGING, 'ci', 'semantic-release.js');
const NEXT_RELEASE = join(PACKAGING, 'ci', 'next-release.js');
const RELEASERC = join(HERE, '..', '.releaserc.json');

type Context = {
  cwd: string;
  env: Record<string, string | undefined>;
  nextRelease: { version: string };
  options?: { dryRun: boolean };
  logger: { log: (...args: unknown[]) => void };
};
type Plugin = {
  verifyRelease: (config: object, context: Context) => Promise<void>;
  prepare: (config: object, context: Context) => Promise<void>;
};
const plugin: Plugin = await import(pathToFileURL(PLUGIN).href);

const GIT_IDENTITY = ['-c', 'user.name=Test', '-c', 'user.email=test@example.com', '-c', 'commit.gpgsign=false'];

/** A disposable repository with the same package identity helpers as CI. */
function repository(t: { after: (fn: () => void) => void }) {
  const root = mkdtempSync(join(tmpdir(), 'branch-truce-kde-semantic-release-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const cwd = join(root, 'repo');
  mkdirSync(join(cwd, 'packaging', 'scripts'), { recursive: true });
  for (const name of ['native-version.sh', 'package-info.sh']) {
    const file = join(cwd, 'packaging', 'scripts', name);
    writeFileSync(file, readFileSync(join(PACKAGING, 'scripts', name)));
    chmodSync(file, 0o755);
  }
  writeFileSync(join(cwd, 'package.json'), JSON.stringify({ version: '1.0.0' }));
  const git = (...args: string[]) => execFileSync('git', [...GIT_IDENTITY, ...args], { cwd, encoding: 'utf8' }).trim();
  git('init', '--quiet', '--initial-branch', 'main');
  git('add', '.');
  git('commit', '--quiet', '-m', 'chore: fixture');
  return { root, cwd, git };
}

const sha256 = (content: string) => createHash('sha256').update(content).digest('hex');
const formats = ['kubuntu-deb', 'fedora-rpm', 'arch-pkg'] as const;
type Format = typeof formats[number];
type Identity = { filename: string; nativeVersion: string; architecture: string };

// Metadata adapters stand in for native tools, not for the gate. The separate
// real-artifact check runs the gate against genuine native package bytes.
function gate(t: { after: (fn: () => void) => void }, version = '1.2.3') {
  const repo = repository(t);
  const revision = repo.git('rev-parse', 'HEAD');
  const dist = join(repo.cwd, 'dist', 'kde');
  const bin = join(repo.root, 'bin');
  mkdirSync(dist, { recursive: true });
  mkdirSync(bin);
  const adapter = `#!/usr/bin/env node
const fs = require('node:fs');
const args = process.argv.slice(2);
if (process.argv[1].endsWith('dpkg-deb') && args[0] !== '--show') process.exit(64);
const info = JSON.parse(fs.readFileSync(args.find(a => fs.existsSync(a)), 'utf8'));
if (process.argv[1].endsWith('bsdtar')) console.log('pkgname = ' + info.name + '\\npkgver = ' + info.version + '\\narch = ' + info.architecture);
else console.log(info.name + '|' + (process.argv[1].endsWith('rpm') ? '0|' : '') + info.version + '|' + info.architecture);
`;
  for (const name of ['dpkg-deb', 'rpm', 'bsdtar']) {
    writeFileSync(join(bin, name), adapter);
    chmodSync(join(bin, name), 0o755);
  }
  const info = Object.fromEntries(formats.map(format => [format, JSON.parse(execFileSync(
    join(PACKAGING, 'scripts', 'package-info.sh'), ['--format', format, '--version', version], { encoding: 'utf8' },
  ))])) as Record<Format, Identity>;
  const put = (name: string, content: string) => writeFileSync(join(dist, name), content);
  const metadata = (format: Format, fields = {}) => put(info[format].filename, JSON.stringify({
    name: 'branch-truce', version: info[format].nativeVersion, architecture: info[format].architecture, ...fields,
  }));
  const manifest = (format: Format, fields: Record<string, unknown> = {}) => put(`kde-${format}.json`, JSON.stringify({
    format, version, nativeVersion: info[format].nativeVersion, architecture: info[format].architecture,
    revision, runId: 'fixture-run', package: info[format].filename,
    sha256: sha256(readFileSync(join(dist, info[format].filename), 'utf8')), ...fields,
  }));
  for (const format of formats) { metadata(format); manifest(format); }
  const context = (env: Record<string, string | undefined> = {}): Context => ({
    cwd: repo.cwd,
    env: { PATH: `${bin}:${process.env.PATH}`, BRANCH_TRUCE_KDE_RELEASE_VERSION: version,
      BRANCH_TRUCE_KDE_RELEASE_REVISION: revision, BRANCH_TRUCE_KDE_RELEASE_RUN_ID: 'fixture-run', ...env },
    nextRelease: { version }, logger: { log: () => {} },
  });
  return { ...repo, revision, dist, info, put, metadata, manifest, context };
}

test('validates exactly three stable and prerelease native artifacts before release mutations', async t => {
  for (const version of ['1.2.3', '1.3.0-rc.1']) {
    const box = gate(t, version);
    await plugin.verifyRelease({}, box.context());
    await plugin.prepare({}, box.context());
  }
});

test('refuses missing and extra packages or manifests for every native format', async t => {
  const box = gate(t);
  for (const format of formats) {
    for (const name of [box.info[format].filename, `kde-${format}.json`]) {
      const bytes = readFileSync(join(box.dist, name));
      rmSync(join(box.dist, name));
      await assert.rejects(plugin.verifyRelease({}, box.context()), /Expected .*found:/);
      writeFileSync(join(box.dist, name), bytes);
    }
  }
  for (const name of ['notes.txt', `branch-truce_${'1.2.2'}_amd64-ubuntu26.04.deb`, `branch-truce_${'1.2.3'}_amd64.deb`]) {
    box.put(name, 'unexpected');
    await assert.rejects(plugin.verifyRelease({}, box.context()), /Expected .*found:/);
    rmSync(join(box.dist, name));
  }
  rmSync(box.dist, { recursive: true });
  await assert.rejects(plugin.prepare({}, box.context()), /found: nothing/);
});

test('rejects every manifest identity mismatch, omitted field, invalid JSON and changed bytes before prepare', async t => {
  const box = gate(t);
  for (const format of formats) {
    for (const key of ['format', 'version', 'nativeVersion', 'architecture', 'revision', 'runId', 'package', 'sha256']) {
      for (const value of ['wrong', undefined]) {
        box.manifest(format, { [key]: value });
        await assert.rejects(plugin.verifyRelease({}, box.context()), new RegExp(`has ${key} `));
      }
      box.manifest(format);
    }
    box.put(`kde-${format}.json`, '{');
    await assert.rejects(plugin.verifyRelease({}, box.context()), /not valid JSON/);
    box.manifest(format);
    box.put(box.info[format].filename, 'tampered');
    await assert.rejects(plugin.verifyRelease({}, box.context()), /has sha256 /);
    box.metadata(format); box.manifest(format);
  }
});

test('rejects wrong native package name, version and architecture even with a matching digest', async t => {
  const box = gate(t);
  for (const format of formats) {
    for (const key of ['name', 'version', 'architecture']) {
      box.metadata(format, { [key]: 'wrong' }); box.manifest(format);
      await assert.rejects(plugin.verifyRelease({}, box.context()), /native metadata/);
    }
    box.metadata(format); box.manifest(format);
  }
});

test('stale version, revision and workflow run candidates fail before any prepare mutation', async t => {
  const box = gate(t);
  await assert.rejects(plugin.verifyRelease({}, box.context({ BRANCH_TRUCE_KDE_RELEASE_VERSION: '1.2.2' })), /built for 1.2.2/);
  await assert.rejects(plugin.prepare({}, box.context({ BRANCH_TRUCE_KDE_RELEASE_VERSION: '1.2.2' })), /built for 1.2.2/);
  await assert.rejects(plugin.verifyRelease({}, box.context({ BRANCH_TRUCE_KDE_RELEASE_REVISION: '0'.repeat(40) })), /revision/);
  await assert.rejects(plugin.verifyRelease({}, box.context({ BRANCH_TRUCE_KDE_RELEASE_RUN_ID: 'other-run' })), /runId/);
  for (const key of ['VERSION', 'REVISION', 'RUN_ID']) {
    await assert.rejects(plugin.verifyRelease({}, box.context({ [`BRANCH_TRUCE_KDE_RELEASE_${key}`]: undefined })), /Candidate/);
  }
  box.git('commit', '--quiet', '--allow-empty', '-m', 'fix: later');
  await assert.rejects(plugin.verifyRelease({}, box.context()), /revision/);
});

test('dry analysis accepts supported versions without artifacts and fails unsupported versions', async t => {
  const box = gate(t);
  rmSync(box.dist, { recursive: true });
  const ctx = { ...box.context({ BRANCH_TRUCE_KDE_RELEASE_VERSION: undefined }), options: { dryRun: true } };
  await plugin.verifyRelease({}, ctx);
  await assert.rejects(plugin.verifyRelease({}, { ...ctx, nextRelease: { version: '1.2.3-rc-1' } }), /exited with 2/);
});

/**
 * A repository for next-release.js with the release rules of .releaserc.json, the KDE plugin and
 * a local bare remote, so the dry run can check that it could push without reaching GitHub.
 */
function releaseRepository(t: { after: (fn: () => void) => void }) {
  const repo = repository(t);
  const remote = join(repo.root, 'remote.git');
  execFileSync('git', ['init', '--quiet', '--bare', '--initial-branch', 'main', remote]);
  const config = JSON.parse(readFileSync(RELEASERC, 'utf8'));
  const keep = ['@semantic-release/commit-analyzer', '@semantic-release/release-notes-generator'];
  const plugins = config.plugins.filter((entry: string | [string]) => keep.includes(Array.isArray(entry) ? entry[0] : entry));
  assert.equal(plugins.length, keep.length, '.releaserc.json lost a plugin these tests rely on');
  writeFileSync(join(repo.cwd, '.releaserc.json'), JSON.stringify({ ...config, plugins: [...plugins, PLUGIN], repositoryUrl: pathToFileURL(remote).href }));
  repo.git('add', '.releaserc.json');
  repo.git('commit', '--quiet', '-m', 'chore: release configuration');
  repo.git('remote', 'add', 'origin', pathToFileURL(remote).href);
  const commit = (message: string) => repo.git('commit', '--quiet', '--allow-empty', '-m', message);
  const tag = (name: string) => repo.git('tag', name);
  // Only these variables, so a CI run of this test does not look like a release run to env-ci.
  const nextRelease = (env: Record<string, string> = {}) => {
    repo.git('push', '--quiet', '--tags', 'origin', 'main');
    return spawnSync(process.execPath, [NEXT_RELEASE, '--analyze'], {
      cwd: repo.cwd,
      env: { PATH: process.env.PATH, HOME: repo.root, GIT_CONFIG_NOSYSTEM: '1', ...env },
      encoding: 'utf8',
    });
  };
  return { ...repo, commit, tag, nextRelease };
}

test('next-release prints only the first version when nothing was released yet', (t) => {
  const repo = releaseRepository(t);
  repo.commit('feat: first feature');
  const result = repo.nextRelease();
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, '1.0.0\n');
  assert.match(result.stderr, /There is no previous release/);
});

test('next-release bumps the last release by the release rules', (t) => {
  for (const [message, next] of [
    ['fix: a bug', '1.2.4'],
    ['refactor: tidy up', '1.2.4'],
    ['feat: a feature', '1.3.0'],
  ] as const) {
    const repo = releaseRepository(t);
    repo.commit('feat: earlier');
    repo.tag('v1.2.3');
    repo.commit(message);
    const result = repo.nextRelease();
    assert.equal(result.status, 0, result.stderr);
    assert.equal(result.stdout, `${next}\n`, message);
  }
});

test('next-release prints nothing when the commits warrant no release', (t) => {
  const repo = releaseRepository(t);
  repo.commit('feat: earlier');
  repo.tag('v1.2.3');
  repo.commit('docs: explain');
  repo.commit('chore: tidy');
  const result = repo.nextRelease();
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, '');
});

test('next-release fails, printing nothing, when the KDE plugin refuses the version', (t) => {
  const repo = releaseRepository(t);
  repo.commit('feat: earlier');
  repo.tag('v1.2.3');
  repo.commit('fix: a bug');
  const result = repo.nextRelease({ BRANCH_TRUCE_KDE_RELEASE_VERSION: '1.3.0' });
  assert.notEqual(result.status, 0);
  assert.equal(result.stdout, '');
  assert.match(result.stderr, /The KDE packages were built for 1\.3\.0, but the next release is 1\.2\.4/);
});

test('semantic-release rejects stale candidates before an earlier prepare hook can mutate files', t => {
  const repo = releaseRepository(t);
  const marker = join(repo.cwd, 'prepare-mutation');
  const sentinel = join(repo.cwd, 'mutation.cjs');
  writeFileSync(sentinel, `exports.prepare = () => require('node:fs').writeFileSync(${JSON.stringify(marker)}, 'mutated');`);
  const configPath = join(repo.cwd, '.releaserc.json');
  const config = JSON.parse(readFileSync(configPath, 'utf8'));
  config.plugins.unshift(sentinel);
  writeFileSync(configPath, JSON.stringify(config));
  repo.commit('feat: earlier'); repo.tag('v1.2.3'); repo.commit('fix: next');
  // The remote is disposable and local. This setup never reaches GitHub.
  repo.nextRelease();
  for (const [version, revision] of [['1.3.0', repo.git('rev-parse', 'HEAD')], ['1.2.4', '0'.repeat(40)]]) {
    const result = spawnSync(process.execPath, ['--input-type=module', '-e',
      `import semanticRelease from ${JSON.stringify(pathToFileURL(join(HERE, '..', 'node_modules/semantic-release/index.js')).href)}; await semanticRelease({dryRun:false, ci:false});`], {
      cwd: repo.cwd, encoding: 'utf8', env: { PATH: process.env.PATH, HOME: repo.root, GIT_CONFIG_NOSYSTEM: '1',
        BRANCH_TRUCE_KDE_RELEASE_VERSION: version!, BRANCH_TRUCE_KDE_RELEASE_REVISION: revision!, BRANCH_TRUCE_KDE_RELEASE_RUN_ID: 'fixture-run' },
    });
    assert.notEqual(result.status, 0, 'stale candidate reached prepare');
    assert.match(result.stderr, /built for|Candidate revision/);
    assert.equal(existsSync(marker), false, 'prepare mutated release metadata before validation');
  }
});
