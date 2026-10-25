/**
 * semantic-release plugin that releases the branch-truce Kubuntu 26.04 and Tuxedo OS 24.04 DEBs
 * under the shared Branch Truce version, attached to the same v<version> GitHub release. It builds
 * nothing: the release workflow (.github/workflows/ci.yml) works out the next version with a dry
 * run (packaging/ci/next-release.js), builds and checks each package for it with
 * packaging/scripts/container-check.sh on a runner of its own, and puts both packages in dist/kde,
 * each with a manifest, before semantic-release runs for real.
 *
 * - verifyRelease: packaging/scripts/native-version.sh accepts the next version and, when
 *   BRANCH_TRUCE_KDE_RELEASE_VERSION is set, the next version is that one. It runs in the dry run
 *   too, before the changelog, package.json, tag or release are written.
 * - prepare: dist/kde holds exactly the Kubuntu 26.04 DEB, Tuxedo OS 24.04 DEB of the next version
 *   and their manifests, kde-kubuntu-deb.json and kde-tuxedo-deb.json, and nothing else. Each
 *   manifest names its format, the version, the source revision, the package file and its SHA-256,
 *   and must match the package and the revision being released (HEAD). This plugin runs before
 *   @semantic-release/git, so a failure stops the release before anything is pushed.
 */
import { spawn } from 'node:child_process';
import { createHash } from 'node:crypto';
import { readFileSync, readdirSync } from 'node:fs';
import { join } from 'node:path';

const PACKAGING_DIR = 'packaging';
const OUTPUT_DIR = 'dist/kde';

function run(command, args, { cwd, env }) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd, env, stdio: ['ignore', 'pipe', 'inherit'] });
    let stdout = '';
    child.stdout.on('data', (chunk) => {
      stdout += chunk;
    });
    child.on('error', reject);
    child.on('close', (code) => {
      if (code === 0) resolve(stdout.trim());
      else reject(new Error(`${command} ${args.join(' ')} exited with ${code}`));
    });
  });
}

function nativeVersion(version, context) {
  return run(join(PACKAGING_DIR, 'scripts/native-version.sh'), [version], context);
}

export async function verifyRelease(pluginConfig, { cwd, env, nextRelease, logger }) {
  const native = await nativeVersion(nextRelease.version, { cwd, env });
  const expected = env.BRANCH_TRUCE_KDE_RELEASE_VERSION;
  if (expected && expected !== nextRelease.version) {
    throw new Error(`The KDE packages were built for ${expected}, but the next release is ${nextRelease.version}`);
  }
  logger.log('KDE packages for %s will have version %s', nextRelease.version, native);
}

export async function prepare(pluginConfig, { cwd, env, nextRelease, logger }) {
  const output = join(cwd, OUTPUT_DIR);
  const native = await nativeVersion(nextRelease.version, { cwd, env });
  const revision = await run('git', ['rev-parse', 'HEAD'], { cwd, env });
  let files = [];
  try {
    files = readdirSync(output).sort();
  } catch (error) {
    if (error.code !== 'ENOENT') throw error;
  }

  const kubuntu = `branch-truce_${native}_amd64-ubuntu26.04.deb`;
  const tuxedo = `branch-truce_${native}_amd64-tuxedo24.04.deb`;
  const expected = [kubuntu, tuxedo, 'kde-kubuntu-deb.json', 'kde-tuxedo-deb.json'];
  const complete = files.length === expected.length && expected.every((name) => files.includes(name));
  if (!complete) {
    throw new Error(
      `Expected ${kubuntu}, ${tuxedo}, kde-kubuntu-deb.json and kde-tuxedo-deb.json in ${OUTPUT_DIR}, found: ${files.join(', ') || 'nothing'}`,
    );
  }

  for (const [format, name] of [
    ['kubuntu-deb', kubuntu],
    ['tuxedo-deb', tuxedo],
  ]) {
    const manifestFile = `kde-${format}.json`;
    let manifest;
    try {
      manifest = JSON.parse(readFileSync(join(output, manifestFile), 'utf8'));
    } catch (error) {
      throw new Error(`${OUTPUT_DIR}/${manifestFile} is not valid JSON: ${error.message}`);
    }
    const sha256 = createHash('sha256').update(readFileSync(join(output, name))).digest('hex');
    const want = { format, version: nextRelease.version, revision, package: name, sha256 };
    for (const [key, value] of Object.entries(want)) {
      if (manifest[key] !== value) {
        throw new Error(`${OUTPUT_DIR}/${manifestFile} has ${key} ${JSON.stringify(manifest[key])}, expected ${JSON.stringify(value)}`);
      }
    }
  }
  logger.log('Releasing %s and %s, built and checked from %s', kubuntu, tuxedo, revision);
}
