/**
 * Publishes only the three native packages validated by this workflow run.
 * verifyRelease checks candidates before any prepare hook can edit release
 * metadata. prepare repeats the gate before @semantic-release/git pushes.
 * A dry analysis validates packageable versions without requiring artifacts.
 */
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { lstatSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const FORMATS = ['kubuntu-deb', 'fedora-rpm', 'arch-pkg'];
const OUTPUT_DIR = 'dist/kde';

function run(command, args, { cwd, env }) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd, env, stdio: ['ignore', 'pipe', 'inherit'] });
    let stdout = '';
    child.stdout.on('data', chunk => { stdout += chunk; });
    child.on('error', reject);
    child.on('close', code => {
      if (code === 0) resolve(stdout.trim());
      else reject(new Error(`${command} ${args.join(' ')} exited with ${code}`));
    });
  });
}

async function identities(version, context) {
  const info = [];
  for (const format of FORMATS) {
    info.push(JSON.parse(await run('packaging/scripts/package-info.sh', ['--format', format, '--version', version], context)));
  }
  return info;
}

function checkCandidateVersion({ env, nextRelease }) {
  const expected = env.BRANCH_TRUCE_KDE_RELEASE_VERSION;
  if (expected && expected !== nextRelease.version) {
    throw new Error(`The KDE packages were built for ${expected}, but the next release is ${nextRelease.version}`);
  }
}

async function checkMetadata(file, info, context) {
  let got;
  let expected;
  if (info.format === 'kubuntu-deb') {
    got = await run('dpkg-deb', ['--show', '--showformat', '${Package}|${Version}|${Architecture}', file], context);
    expected = `branch-truce|${info.nativeVersion}|${info.architecture}`;
  } else if (info.format === 'fedora-rpm') {
    got = await run('rpm', ['-qp', '--qf', '%{NAME}|%{EPOCHNUM}|%{VERSION}-%{RELEASE}|%{ARCH}', file], context);
    expected = `branch-truce|0|${info.nativeVersion}|${info.architecture}`;
  } else {
    const text = await run('bsdtar', ['-xOf', file, '.PKGINFO'], context);
    // Require exactly one value of each identity field. Duplicate native
    // metadata must not be hidden by a last-value-wins parser.
    const value = key => text.split('\n').filter(line => line.startsWith(`${key} = `)).map(line => line.slice(key.length + 3));
    got = JSON.stringify([value('pkgname'), value('pkgver'), value('arch')]);
    expected = JSON.stringify([['branch-truce'], [info.nativeVersion], [info.architecture]]);
  }
  if (got !== expected) throw new Error(`${info.filename} native metadata ${JSON.stringify(got)}, expected ${JSON.stringify(expected)}`);
}

async function validateArtifacts(context, info) {
  const { cwd, env, nextRelease, logger } = context;
  checkCandidateVersion(context);
  if (!env.BRANCH_TRUCE_KDE_RELEASE_VERSION) throw new Error('Candidate build version is required');
  const revision = await run('git', ['rev-parse', 'HEAD'], context);
  if (env.BRANCH_TRUCE_KDE_RELEASE_REVISION !== revision) {
    throw new Error(`Candidate revision ${env.BRANCH_TRUCE_KDE_RELEASE_REVISION} does not match checked out revision ${revision}`);
  }
  const runId = env.BRANCH_TRUCE_KDE_RELEASE_RUN_ID;
  if (!runId) throw new Error('Candidate workflow runId is required');
  const output = join(cwd, OUTPUT_DIR);
  let files = [];
  try { files = readdirSync(output).sort(); }
  catch (error) { if (error.code !== 'ENOENT') throw error; }
  const expected = info.flatMap(item => [item.filename, `kde-${item.format}.json`]);
  if (files.length !== expected.length || !expected.every(name => files.includes(name))) {
    throw new Error(`Expected ${expected.join(', ')} in ${OUTPUT_DIR}, found: ${files.join(', ') || 'nothing'}`);
  }
  for (const name of expected) {
    if (!lstatSync(join(output, name)).isFile()) throw new Error(`${OUTPUT_DIR}/${name} must be a regular file`);
  }
  for (const item of info) {
    const manifestFile = `kde-${item.format}.json`;
    let manifest;
    try { manifest = JSON.parse(readFileSync(join(output, manifestFile), 'utf8')); }
    catch (error) { throw new Error(`${OUTPUT_DIR}/${manifestFile} is not valid JSON: ${error.message}`); }
    const file = join(output, item.filename);
    const sha256 = createHash('sha256').update(readFileSync(file)).digest('hex');
    const want = { format: item.format, version: nextRelease.version, nativeVersion: item.nativeVersion,
      architecture: item.architecture, revision, runId, package: item.filename, sha256 };
    for (const [key, value] of Object.entries(want)) {
      if (manifest?.[key] !== value) {
        throw new Error(`${OUTPUT_DIR}/${manifestFile} has ${key} ${JSON.stringify(manifest?.[key])}, expected ${JSON.stringify(value)}`);
      }
    }
    await checkMetadata(file, item, context);
  }
  logger.log('Releasing %s, built and checked from %s in run %s', info.map(item => item.filename).join(', '), revision, runId);
}

export async function verifyRelease(pluginConfig, context) {
  checkCandidateVersion(context);
  const info = await identities(context.nextRelease.version, context);
  if (!context.options?.dryRun || context.env.BRANCH_TRUCE_KDE_RELEASE_VERSION) await validateArtifacts(context, info);
  else context.logger.log('Native packages accept version %s', context.nextRelease.version);
}

export async function prepare(pluginConfig, context) {
  checkCandidateVersion(context);
  await validateArtifacts(context, await identities(context.nextRelease.version, context));
}
