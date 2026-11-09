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
 * Errors print one line on stderr: usage and configuration errors exit 2,
 * failed checks exit 1.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

const USAGE = 'usage: node packaging/ci/next-release.js [--analyze]';
const NATIVE_VERSION = fileURLToPath(new URL('../scripts/native-version.sh', import.meta.url));

/** A bad invocation or configuration, reported with exit status 2. */
export class UsageError extends Error {}

/** The next release version semantic-release would publish, or '' for none. */
export async function analyze() {
  const { default: semanticRelease } = await import('semantic-release');
  const result = await semanticRelease({ dryRun: true }, { stdout: process.stderr, stderr: process.stderr });
  return result?.nextRelease?.version ?? '';
}

/** Whether native-version.sh, which owns the packageable version grammar, accepts VERSION. */
export function packageable(version) {
  try {
    execFileSync(NATIVE_VERSION, [version], { stdio: 'ignore' });
    return true;
  } catch {
    return false;
  }
}

/**
 * The validation selection for EVENT at REVISION. A push needs the analysis of
 * this revision (ANALYZED_REVISION, ANALYZED_VERSION, '' for no release); a pull
 * request builds PACKAGE_VERSION without publishing.
 */
export function select({ event, revision, analyzedRevision, analyzedVersion, packageVersion }) {
  if (!['pull_request', 'push'].includes(event)) throw new UsageError(`Unsupported validation event ${event}`);
  let candidate = '';
  if (event === 'push') {
    if (analyzedRevision !== revision || analyzedVersion === undefined) {
      throw new Error('Release analysis is missing or belongs to another revision');
    }
    candidate = analyzedVersion;
  }
  const version = candidate || packageVersion;
  if (typeof version !== 'string' || !packageable(version)) throw new Error(`Unsupported native package version ${version}`);
  return { version, revision, publish: Boolean(candidate) };
}

async function main(argv) {
  if (argv.length === 1 && (argv[0] === '-h' || argv[0] === '--help')) {
    process.stdout.write(`${USAGE}\n`);
  } else if (argv.length === 1 && argv[0] === '--analyze') {
    const version = await analyze();
    if (version) process.stdout.write(`${version}\n`);
  } else if (argv.length === 0) {
    const selection = select({
      event: process.env.BRANCH_TRUCE_CI_EVENT,
      revision: execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim(),
      analyzedRevision: process.env.BRANCH_TRUCE_ANALYZED_REVISION,
      analyzedVersion: process.env.BRANCH_TRUCE_ANALYZED_VERSION,
      packageVersion: JSON.parse(readFileSync('package.json', 'utf8')).version,
    });
    process.stdout.write(`${JSON.stringify(selection)}\n`);
  } else {
    throw new UsageError(USAGE);
  }
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  main(process.argv.slice(2)).catch(error => {
    process.stderr.write(`error: ${error.message}\n`);
    process.exitCode = error instanceof UsageError ? 2 : 1;
  });
}
