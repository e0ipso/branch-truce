/**
 * Picks the release version the CI packages are built for.
 *
 *   node packaging/ci/next-release.js --analyze
 *   node packaging/ci/next-release.js
 *
 * --analyze prints the next main release version, or nothing for a successful
 * no-release analysis. Only this mode loads semantic-release and requires a
 * release-write token for its push preflight.
 *
 * Without arguments it prints the validation revision and version as JSON.
 * PRs need neither credentials nor release analysis. Main selection requires
 * analysis of this exact revision, even when that analysis selected no
 * release. The version must be one packaging/scripts/native-version.sh accepts.
 *
 * Every path is relative to the checkout it runs in, the current directory:
 * package.json, Git's HEAD and packaging/scripts/native-version.sh.
 *
 * Errors print one line on stderr: usage and configuration errors exit 2,
 * failed checks exit 1.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const USAGE = 'usage: node packaging/ci/next-release.js [--analyze]';

/** A bad invocation or configuration, reported with exit status 2. */
export class UsageError extends Error {}

/**
 * The next release version semantic-release would publish, or '' for none.
 * @returns {Promise<string>}
 */
export async function analyze() {
  const { default: semanticRelease } = await import('semantic-release');
  const result = await semanticRelease({ dryRun: true }, { stdout: process.stderr, stderr: process.stderr });
  return result ? result.nextRelease.version : '';
}

/**
 * Whether the checkout ROOT's packaging/scripts/native-version.sh, which owns the packageable
 * version grammar, accepts VERSION.
 * @param {string} version
 * @param {string} root
 * @returns {boolean}
 */
export function packageable(version, root) {
  try {
    execFileSync(join(root, 'packaging/scripts/native-version.sh'), ['--style', 'tilde', version], { stdio: 'ignore' });
    return true;
  } catch (error) {
    if (error instanceof Error && 'status' in error && error.status === 2 && 'signal' in error && !error.signal) return false;
    throw error;
  }
}

/**
 * @typedef {object} SelectionInput
 * @property {string | undefined} event the GitHub event, pull_request or push
 * @property {string} revision the checked out revision
 * @property {string} [analyzedRevision] the revision the release analysis ran on
 * @property {string} [analyzedVersion] the analysed release version, '' for no release
 * @property {string} packageVersion the version in package.json
 *
 * @typedef {object} Selection
 * @property {string} version the version the packages are built for
 * @property {string} revision the revision they are built from
 * @property {boolean} publish whether the run publishes them
 */

/**
 * The validation selection for an event at a revision. A push needs the analysis of this
 * revision; a pull request builds packageVersion without publishing. ACCEPTS decides which
 * versions can be packaged. Throws UsageError for an unsupported event and Error otherwise.
 * @param {SelectionInput} input
 * @param {(version: string) => boolean} accepts
 * @returns {Selection}
 */
export function select({ event, revision, analyzedRevision, analyzedVersion, packageVersion }, accepts) {
  if (event !== 'pull_request' && event !== 'push') throw new UsageError(`Unsupported validation event ${event}`);
  let candidate = '';
  if (event === 'push') {
    if (analyzedRevision !== revision || analyzedVersion === undefined) {
      throw new Error('Release analysis is missing or belongs to another revision');
    }
    candidate = analyzedVersion;
  }
  const version = candidate || packageVersion;
  if (typeof version !== 'string' || !accepts(version)) throw new Error(`Unsupported native package version ${version}`);
  return { version, revision, publish: Boolean(candidate) };
}

/**
 * @param {string[]} argv
 * @param {string} root
 * @param {NodeJS.ProcessEnv} env
 */
async function main(argv, root, env) {
  if (argv.length === 1 && (argv[0] === '-h' || argv[0] === '--help')) {
    process.stdout.write(`${USAGE}\n`);
  } else if (argv.length === 1 && argv[0] === '--analyze') {
    const version = await analyze();
    if (version) process.stdout.write(`${version}\n`);
  } else if (argv.length === 0) {
    const selection = select({
      event: env.BRANCH_TRUCE_CI_EVENT,
      revision: execFileSync('git', ['rev-parse', 'HEAD'], { cwd: root, encoding: 'utf8' }).trim(),
      analyzedRevision: env.BRANCH_TRUCE_ANALYZED_REVISION,
      analyzedVersion: env.BRANCH_TRUCE_ANALYZED_VERSION,
      packageVersion: JSON.parse(readFileSync(join(root, 'package.json'), 'utf8')).version,
    }, version => packageable(version, root));
    process.stdout.write(`${JSON.stringify(selection)}\n`);
  } else {
    throw new UsageError(USAGE);
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main(process.argv.slice(2), process.cwd(), process.env).catch(error => {
    process.stderr.write(`error: ${error instanceof Error ? error.message : String(error)}\n`);
    process.exitCode = error instanceof UsageError ? 2 : 1;
  });
}
