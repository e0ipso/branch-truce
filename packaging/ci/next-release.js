/**
 * --analyze prints the next main release version, or nothing for a successful
 * no-release analysis. Errors propagate. Only this mode loads semantic-release
 * and requires a release-write token for its push preflight.
 *
 * The default mode selects a validation revision/version as JSON. PRs need
 * neither credentials nor release analysis. Main selection requires analysis
 * of this exact revision, even when that analysis selected no release.
 */
import { execFileSync } from 'node:child_process';
import { readFileSync } from 'node:fs';

if (process.argv[2] === '--analyze') {
  const { default: semanticRelease } = await import('semantic-release');
  const result = await semanticRelease({ dryRun: true }, { stdout: process.stderr, stderr: process.stderr });
  if (result?.nextRelease) process.stdout.write(`${result.nextRelease.version}\n`);
} else {
  if (process.argv.length !== 2) throw new Error('usage: next-release.js [--analyze]');
  const event = process.env.BRANCH_TRUCE_CI_EVENT;
  if (!['pull_request', 'push'].includes(event)) throw new Error(`Unsupported validation event ${event}`);
  const revision = execFileSync('git', ['rev-parse', 'HEAD'], { encoding: 'utf8' }).trim();
  let candidate = '';
  if (event === 'push') {
    if (process.env.BRANCH_TRUCE_ANALYZED_REVISION !== revision || process.env.BRANCH_TRUCE_ANALYZED_VERSION === undefined) {
      throw new Error('Release analysis is missing or belongs to another revision');
    }
    candidate = process.env.BRANCH_TRUCE_ANALYZED_VERSION;
  }
  const version = candidate || JSON.parse(readFileSync('package.json', 'utf8')).version;
  if (typeof version !== 'string' || !/^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(-[A-Za-z]+(\.[1-9]\d*)?)?$/.test(version)) {
    throw new Error(`Unsupported native package version ${version}`);
  }
  process.stdout.write(`${JSON.stringify({ version, revision, publish: Boolean(candidate) })}\n`);
}
