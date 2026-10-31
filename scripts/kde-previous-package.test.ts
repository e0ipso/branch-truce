import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync, spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { chmodSync, existsSync, mkdirSync, mkdtempSync, readFileSync, readdirSync, rmSync, symlinkSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Every validation event can reuse a verified published native predecessor.
// These tests run it with a stand-in `gh` that serves releases from a throwaway directory: `release
// list` prints the tags, `release view <tag>` the release's assets, `release download` copies the
// asset's bytes, and marker files make a call fail.

const HERE = dirname(fileURLToPath(import.meta.url));
const SCRIPT = join(HERE, '..', 'packaging', 'ci', 'previous-package.sh');
const RELEASERC = JSON.parse(readFileSync(join(HERE, '..', '.releaserc.json'), 'utf8'));
const GITHUB_ASSETS: { path: string; label: string }[] = RELEASERC.plugins.find(
  (entry: unknown) => Array.isArray(entry) && entry[0] === '@semantic-release/github',
)[1].assets;
const LABEL = {
  'kubuntu-deb': GITHUB_ASSETS.find((asset) => asset.path.endsWith('-ubuntu26.04.deb'))!.label,
  'fedora-rpm': 'branch-truce RPM for Fedora 44 (x86_64)',
  'arch-pkg': 'branch-truce package for Arch Linux (x86_64)',
};

const GH = `#!/usr/bin/env bash
printf '%s\\n' "$*" >> "$FIXTURE/gh.log"
case "$1 $2" in
    "release list")
        [ ! -e "$FIXTURE/fail-list" ] || { echo "HTTP 403: Resource not accessible" >&2; exit 1; }
        cat "$FIXTURE/tags" ;;
    "release view")
        [ -f "$FIXTURE/releases/$3.json" ] || { echo "release not found" >&2; exit 1; }
        cat "$FIXTURE/releases/$3.json" ;;
    "release download")
        tag="$3"; shift 3
        while [ $# -gt 0 ]; do
            case "$1" in --pattern) pattern="$2" ;; --dir) dir="$2" ;; esac
            shift 2
        done
        if [ -e "$FIXTURE/fail-download" ]; then
            [ ! -e "$FIXTURE/partial" ] || head -c 3 "$FIXTURE/files/$tag/$pattern" > "$dir/$pattern"
            echo "connection reset" >&2
            exit 1
        fi
        cp "$FIXTURE/files/$tag/$pattern" "$dir/" ;;
    *) echo "unexpected gh $*" >&2; exit 64 ;;
esac
`;

type Asset = { name: string; label?: string; state?: string; digest?: string | null; size?: number; content?: string };
type Release = { tag: string; assets: Asset[] };

const sha256 = (content: string) => createHash('sha256').update(content).digest('hex');
const kubuntu = (native: string, extra: Partial<Asset> = {}): Asset => ({
  name: `branch-truce_${native}_amd64-ubuntu26.04.deb`,
  label: LABEL['kubuntu-deb'],
  ...extra,
});
const fedora = (native: string, extra: Partial<Asset> = {}): Asset => ({
  name: `branch-truce-${native}-1.fc44.x86_64.rpm`,
  label: LABEL['fedora-rpm'],
  ...extra,
});

/** A throwaway directory with the stand-in `gh` and the given releases, deleted when the test ends. */
function fixture(t: { after: (fn: () => void) => void }, releases: Release[], tags = releases.map((r) => r.tag)) {
  const root = mkdtempSync(join(tmpdir(), 'branch-truce-previous-package-'));
  t.after(() => rmSync(root, { recursive: true, force: true }));
  const bin = join(root, 'bin');
  const data = join(root, 'gh');
  const output = join(root, 'previous');
  mkdirSync(bin);
  mkdirSync(join(data, 'releases'), { recursive: true });
  writeFileSync(join(bin, 'gh'), GH);
  chmodSync(join(bin, 'gh'), 0o755);
  writeFileSync(join(data, 'tags'), tags.map((tag) => `${tag}\n`).join(''));
  for (const release of releases) {
    mkdirSync(join(data, 'files', release.tag), { recursive: true });
    const assets = release.assets.map((asset) => {
      const content = asset.content ?? `${release.tag} ${asset.name}`;
      writeFileSync(join(data, 'files', release.tag, asset.name), content);
      return {
        name: asset.name,
        label: asset.label ?? '',
        state: asset.state ?? 'uploaded',
        digest: asset.digest === undefined ? `sha256:${sha256(content)}` : asset.digest,
        size: asset.size ?? Buffer.byteLength(content),
      };
    });
    writeFileSync(join(data, 'releases', `${release.tag}.json`), JSON.stringify({ assets }));
  }
  const mark = (name: 'fail-list' | 'fail-download' | 'partial') => writeFileSync(join(data, name), '');
  const forget = (tag: string) => rmSync(join(data, 'releases', `${tag}.json`));
  const run = (format: 'kubuntu-deb' | 'fedora-rpm' | 'arch-pkg', version: string, path = `${bin}:${process.env.PATH}`) => {
    const result = spawnSync(SCRIPT, ['--format', format, '--version', version, '--output', output], {
      env: { PATH: path, FIXTURE: data, HOME: root },
      encoding: 'utf8',
    });
    const log = join(data, 'gh.log');
    const gh = existsSync(log) ? readFileSync(log, 'utf8').trimEnd().split('\n') : [];
    const left = existsSync(output) ? readdirSync(output) : [];
    return { ...result, gh, left, output, data };
  };
  return { root, bin, mark, forget, run };
}

const FALLBACK = /the upgrade check uses a build of this source as 0\.0\.0 instead\n$/;

test('selects the Fedora predecessor separately from Kubuntu packages in the same release', (t) => {
  const box = fixture(t, [
    { tag: 'v1.2.2', assets: [kubuntu('1.2.2')] },
    { tag: 'v1.2.1', assets: [kubuntu('1.2.1'), fedora('1.2.1')] },
  ]);
  const result = box.run('fedora-rpm', '1.2.3');
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, `${join(result.output, fedora('1.2.1').name)}\n`);
  assert.deepEqual(result.left, [fedora('1.2.1').name]);
});

test('selects the Kubuntu predecessor when the release also contains a Fedora RPM', (t) => {
  const box = fixture(t, [{ tag: 'v1.2.2', assets: [kubuntu('1.2.2'), fedora('1.2.2')] }]);
  const result = box.run('kubuntu-deb', '1.2.3');
  assert.equal(result.status, 0, result.stderr);
  assert.deepEqual(result.left, [kubuntu('1.2.2').name]);
});

function assertFallback(result: ReturnType<ReturnType<typeof fixture>['run']>, reason: RegExp) {
  assert.equal(result.status, 0, result.stderr);
  assert.equal(result.stdout, '');
  assert.match(result.stderr, reason);
  assert.match(result.stderr, FALLBACK);
  assert.deepEqual(result.left, []);
}

test('picks the newest strictly older release with one asset of the format by package version order', (t) => {
  const box = fixture(
    t,
    [
      { tag: 'v1.2.9', assets: [kubuntu('1.2.9')] },
      { tag: 'v1.4.0', assets: [kubuntu('1.4.0')] },
      { tag: 'v1.3.0', assets: [kubuntu('1.3.0')] },
      { tag: 'v1.3.0-rc.1', assets: [kubuntu('1.3.0~rc1', { label: 'branch-truce DEB for Kubuntu 24.04 (amd64)' })] },
      { tag: 'v1.2.11', assets: [kubuntu('1.2.11', { digest: null })] },
      { tag: 'v1.2.10', assets: [kubuntu('1.2.10'), fedora('1.2.10')] },
    ],
    ['v1.2.9', 'v1.4.0', 'nightly', 'v1.3.0', 'v1.3.0-rc.1', 'v1.2.11', 'v1.2.10', 'v1.2.3-rc-1'],
  );
  const result = box.run('kubuntu-deb', '1.3.0');
  assert.equal(result.status, 0, result.stderr);
  const file = join(result.output, 'branch-truce_1.2.10_amd64-ubuntu26.04.deb');
  assert.equal(result.stdout, `${file}\n`);
  assert.equal(readFileSync(file, 'utf8'), 'v1.2.10 branch-truce_1.2.10_amd64-ubuntu26.04.deb');
  assert.deepEqual(result.left, ['branch-truce_1.2.10_amd64-ubuntu26.04.deb']);
  assert.deepEqual(result.gh.slice(0, 4), [
    'release list --exclude-drafts --exclude-pre-releases --limit 1000 --json tagName --jq .[].tagName',
    'release view v1.3.0-rc.1 --json assets',
    'release view v1.2.11 --json assets',
    'release view v1.2.10 --json assets',
  ]);
  assert.equal(result.gh.length, 5);
  assert.ok(
    result.gh[4]!.startsWith(`release download v1.2.10 --pattern branch-truce_1.2.10_amd64-ubuntu26.04.deb --dir ${result.output}/.download.`),
    result.gh[4],
  );
  assert.match(result.stderr, /v1\.3\.0-rc\.1 has no single "branch-truce DEB for Kubuntu 26\.04 \(amd64\)" asset for 1\.3\.0~rc1; trying older/);
  assert.match(result.stderr, /v1\.2\.11 has no single .* asset for 1\.2\.11; trying older/);
  assert.match(result.stderr, /branch-truce_1\.2\.10_amd64-ubuntu26\.04\.deb from v1\.2\.10, SHA-256 [0-9a-f]{64}, for the upgrade to 1\.3\.0\n$/);
});

test('falls back when gh is not installed', (t) => {
  const box = fixture(t, [{ tag: 'v1.2.2', assets: [kubuntu('1.2.2')] }]);
  const tools = join(box.root, 'tools');
  mkdirSync(tools);
  for (const tool of ['bash', 'dirname']) {
    symlinkSync(execFileSync('bash', ['-c', `command -v ${tool}`], { encoding: 'utf8' }).trim(), join(tools, tool));
  }
  const result = box.run('kubuntu-deb', '1.2.3', tools);
  assertFallback(result, /^previous package: gh is not installed;/);
  assert.deepEqual(result.gh, []);
});

test('falls back when the releases cannot be listed', (t) => {
  const box = fixture(t, [{ tag: 'v1.2.2', assets: [kubuntu('1.2.2')] }]);
  box.mark('fail-list');
  assertFallback(box.run('kubuntu-deb', '1.2.3'), /cannot list the releases \(HTTP 403: Resource not accessible\);/);
});

test('falls back when there are no releases', (t) => {
  assertFallback(
    fixture(t, []).run('kubuntu-deb', '1.2.3'),
    /no published release older than 1\.2\.3 has a "branch-truce DEB for Kubuntu 26\.04 \(amd64\)" asset;/,
  );
});

test('falls back when the assets of the release cannot be read', (t) => {
  const box = fixture(t, [{ tag: 'v1.2.2', assets: [kubuntu('1.2.2')] }, { tag: 'v1.2.1', assets: [kubuntu('1.2.1')] }]);
  box.forget('v1.2.2');
  const result = box.run('kubuntu-deb', '1.2.3');
  assertFallback(result, /cannot read the assets of v1\.2\.2;/);
  assert.equal(result.gh.filter((call) => call.startsWith('release view')).length, 1);
});

test('falls back when no older release has a package for the target', (t) => {
  const box = fixture(t, [
    { tag: 'v1.3.0', assets: [kubuntu('1.3.0')] },
    { tag: 'v1.2.3', assets: [kubuntu('1.2.3')] },
    { tag: 'v1.2.2', assets: [kubuntu('1.2.2', { label: 'branch-truce DEB for Kubuntu 24.04 (amd64)' })] },
    { tag: 'v1.2.1', assets: [fedora('1.2.1')] },
  ]);
  const result = box.run('kubuntu-deb', '1.2.3');
  assertFallback(result, /no published release older than 1\.2\.3 has a "branch-truce DEB for Kubuntu 26\.04 \(amd64\)" asset;/);
  assert.equal(result.gh.some((call) => call.startsWith('release download')), false);
});

for (const partial of [false, true]) {
  test(`falls back when the download fails${partial ? ' after writing part of the file' : ''}`, (t) => {
    const box = fixture(t, [{ tag: 'v1.2.2', assets: [kubuntu('1.2.2')] }]);
    box.mark('fail-download');
    if (partial) box.mark('partial');
    const result = box.run('kubuntu-deb', '1.2.3');
    assertFallback(result, /downloading branch-truce_1\.2\.2_amd64-ubuntu26\.04\.deb from v1\.2\.2 failed;/);
    assert.match(result.stderr, /connection reset/);
  });
}

for (const [what, asset] of [
  ['size', kubuntu('1.2.2', { size: 999 })],
  ['SHA-256', kubuntu('1.2.2', { digest: `sha256:${sha256('something else')}` })],
] as const) {
  test(`fails, leaving nothing behind, when the download does not match its published ${what}`, (t) => {
    const box = fixture(t, [{ tag: 'v1.2.2', assets: [asset] }]);
    const result = box.run('kubuntu-deb', '1.2.3');
    assert.equal(result.status, 1);
    assert.equal(result.stdout, '');
    assert.match(
      result.stderr,
      /^error: branch-truce_1\.2\.2_amd64-ubuntu26\.04\.deb from v1\.2\.2 does not match its published size \d+ and SHA-256 [0-9a-f]{64}$/m,
    );
    assert.doesNotMatch(result.stderr, FALLBACK);
    assert.deepEqual(result.left, []);
  });
}

test('removes a stale file of the same name when the download does not verify', (t) => {
  const box = fixture(t, [{ tag: 'v1.2.2', assets: [kubuntu('1.2.2', { size: 999 })] }]);
  const output = join(box.root, 'previous');
  mkdirSync(output);
  writeFileSync(join(output, 'branch-truce_1.2.2_amd64-ubuntu26.04.deb'), 'stale');
  const result = box.run('kubuntu-deb', '1.2.3');
  assert.equal(result.status, 1);
  assert.deepEqual(result.left, []);
});

for (const format of ['fedora-rpm', 'arch-pkg'] as const) {
  test(`${format} selects the newest strictly older target and treats corrupt bytes as fatal`, t => {
    const asset = (version: string, extra: Partial<Asset> = {}): Asset => ({
      name: format === 'fedora-rpm' ? `branch-truce-${version}-1.fc44.x86_64.rpm` : `branch-truce-${version.replace('~', '')}-1-x86_64.pkg.tar.zst`,
      label: LABEL[format], ...extra,
    });
    const releases = [
      { tag: 'v1.3.0', assets: [asset('1.3.0')] },
      { tag: 'v1.2.10', assets: [asset('1.2.10'), asset('1.2.10')] },
      { tag: 'v1.2.9', assets: [asset('1.2.9', { label: 'wrong platform' })] },
      { tag: 'v1.2.8', assets: [asset('1.2.8', { name: 'wrong-architecture' })] },
      { tag: 'v1.3.0-rc.2', assets: [asset('1.3.0~rc2')] },
      { tag: 'v1.3.0-rc.10', assets: [asset('1.3.0~rc10')] },
      { tag: 'v1.2.7', assets: [asset('1.2.7')] },
    ];
    const box = fixture(t, releases);
    const result = box.run(format, '1.3.0');
    assert.equal(result.status, 0, result.stderr);
    assert.equal(result.stdout, `${join(result.output, asset('1.3.0~rc10').name)}\n`);
    assert.match(result.stderr, /from v1.3.0-rc.10, SHA-256/);
    const corrupt = fixture(t, [{ tag: 'v1.2.2', assets: [asset('1.2.2', { digest: `sha256:${sha256('corrupt')}` })] }]).run(format, '1.3.0');
    assert.equal(corrupt.status, 1, corrupt.stderr);
    assert.match(corrupt.stderr, /does not match its published/);
    assert.doesNotMatch(corrupt.stderr, FALLBACK);
    assert.deepEqual(corrupt.left, []);
    assertFallback(fixture(t, []).run(format, '1.3.0'), /no published release older/);
  });
}
