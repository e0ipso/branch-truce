/**
 * Publishes only the native packages validated by this workflow run, one per
 * row of packaging/formats.tsv. verifyRelease checks candidates before any
 * prepare hook can edit release metadata. prepare repeats the gate before
 * @semantic-release/git pushes. A dry analysis validates packageable versions
 * without requiring artifacts. Every package identity comes from
 * packaging/scripts/package-info.sh, and every package's own metadata is read
 * by packaging/scripts/package-identity.sh, as the builders do.
 */
import { run } from './metadata-command.js';
import { createHash } from 'node:crypto';
import { lstatSync, readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const OUTPUT_DIR = 'dist/kde';

/**
 * The part of semantic-release's plugin context this plugin reads.
 * @typedef {object} Context
 * @property {string} cwd the checkout being released
 * @property {Record<string, string | undefined>} env
 * @property {{ version: string }} nextRelease
 * @property {AbortSignal} [signal]
 * @property {{ dryRun?: boolean }} [options]
 * @property {{ log: (...args: unknown[]) => void }} logger
 *
 * One format's identity, as packaging/scripts/package-info.sh prints it.
 * @typedef {object} Identity
 * @property {string} format
 * @property {string} version
 * @property {string} nativeVersion
 * @property {string} filename
 * @property {string} architecture
 */

/**
 * @param {string} version
 * @param {Context} context
 * @returns {Promise<Identity[]>}
 */
async function identities(version, context) {
  const info = [];
  for (const format of (await run('packaging/scripts/formats.sh', ['ids'], context)).split('\n').filter(Boolean)) {
    info.push(JSON.parse(await run('packaging/scripts/package-info.sh', ['--format', format, '--version', version], context)));
  }
  return info;
}

/** @param {Context} context */
function checkCandidateVersion({ env, nextRelease }) {
  const expected = env.BRANCH_TRUCE_KDE_RELEASE_VERSION;
  if (expected && expected !== nextRelease.version) {
    throw new Error(`The KDE packages were built for ${expected}, but the next release is ${nextRelease.version}`);
  }
}

/**
 * @param {string} file
 * @param {Identity} info
 * @param {Context} context
 */
async function checkMetadata(file, info, context) {
  try {
    await run('packaging/scripts/package-identity.sh', ['--format', info.format, '--version', info.version, file], context);
  } catch (error) {
    throw new Error(`${info.filename} native metadata does not match ${info.format} ${info.version}: ${errorMessage(error)}`);
  }
}

/** @param {unknown} error */
function errorMessage(error) {
  return error instanceof Error ? error.message : String(error);
}

/**
 * @param {Context} context
 * @param {Identity[]} info
 */
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
  /** @type {string[]} */
  let files = [];
  try { files = readdirSync(output).sort(); }
  catch (error) { if (/** @type {NodeJS.ErrnoException} */ (error).code !== 'ENOENT') throw error; }
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
    catch (error) { throw new Error(`${OUTPUT_DIR}/${manifestFile} is not valid JSON: ${errorMessage(error)}`); }
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

/**
 * semantic-release's verifyRelease step.
 * @param {object} pluginConfig
 * @param {Context} context
 */
export async function verifyRelease(pluginConfig, context) {
  checkCandidateVersion(context);
  const info = await identities(context.nextRelease.version, context);
  if (!context.options?.dryRun || context.env.BRANCH_TRUCE_KDE_RELEASE_VERSION) await validateArtifacts(context, info);
  else context.logger.log('Native packages accept version %s', context.nextRelease.version);
}

/**
 * semantic-release's prepare step.
 * @param {object} pluginConfig
 * @param {Context} context
 */
export async function prepare(pluginConfig, context) {
  checkCandidateVersion(context);
  await validateArtifacts(context, await identities(context.nextRelease.version, context));
}
