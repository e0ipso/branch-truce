import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFileSync } from 'node:child_process';
import { readFileSync, readdirSync } from 'node:fs';
import { createRequire } from 'node:module';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Facts that several files state must agree with the one table that owns them: the release formats
// (packaging/formats.tsv), the installed files (packaging/installed-files.txt), the runtime QML
// modules (packaging/runtime-modules.txt) and the version floors (packaging/minimum-versions.tsv).
// Reads text files only.

const ROOT = join(dirname(fileURLToPath(import.meta.url)), '..');
const read = (path: string) => readFileSync(join(ROOT, path), 'utf8');
// The lines of a list, and the rows of a table after its header line, which names the columns.
const lines = (path: string) => read(path).split('\n').filter((line) => line.trim() && !line.startsWith('#'))
  .map((line) => line.split('\t'));
const rows = (path: string) => lines(path).slice(1);
const header = (path: string) => lines(path)[0]!;
const require = createRequire(import.meta.url);

const FORMATS = rows('packaging/formats.tsv').map((cells) => Object.fromEntries(
  header('packaging/formats.tsv').map((name, index) => [name, cells[index]]))) as Record<string, string | undefined>[];
const formatsScript = (...args: string[]) => execFileSync(join(ROOT, 'packaging/scripts/formats.sh'), args, { encoding: 'utf8' });

test('formats.tsv has complete rows whose family scripts and Dockerfiles exist', () => {
  assert.ok(FORMATS.length > 0);
  assert.deepEqual(formatsScript('ids').trim().split('\n'), FORMATS.map((format) => format.id), 'formats.sh reads every row');
  for (const format of FORMATS) {
    assert.ok(Object.values(format).every((value) => value), `${format.id} has every column`);
    assert.ok(['tilde', 'plain'].includes(format.version_style!), `${format.id} version style`);
    assert.ok(['0', '1'].includes(format.native_fidelity!), `${format.id} native_fidelity`);
    for (const path of [`packaging/scripts/build-${format.family}.sh`, `packaging/scripts/pkg-${format.family}.sh`,
      `packaging/${format.dockerfile}`]) {
      assert.doesNotThrow(() => read(path), `${format.id} needs ${path}`);
    }
    assert.ok(format.filename!.includes('{version}') && format.filename!.endsWith(`.${format.extension}`), format.id);
  }
});

test('the release assets and the CI matrix are the formats of formats.tsv', () => {
  const releaserc = JSON.parse(read('.releaserc.json'));
  const github = releaserc.plugins.find((entry: unknown) => Array.isArray(entry) && entry[0] === '@semantic-release/github');
  assert.deepEqual(github[1].assets, FORMATS.map((format) => ({
    path: `dist/kde/${format.filename!.replace('{version}', '*')}`,
    label: format.label,
  })));
  const workflow = require('js-yaml').load(read('.github/workflows/ci.yml'));
  assert.equal(workflow.jobs.kde.strategy.matrix, '${{ fromJSON(needs.select.outputs.matrix) }}');
  assert.deepEqual(JSON.parse(formatsScript('matrix')).include,
    FORMATS.map((format) => ({ format: format.id, target: format.target, native_fidelity: format.native_fidelity })));
  assert.equal(FORMATS.filter((format) => format.native_fidelity === '1').length, 1, 'one calibrated native replay leg');
});

test('validation targets name a DEB release format and a Dockerfile with a target stage', () => {
  const targets = rows('packaging/validation-targets.tsv');
  assert.deepEqual(header('packaging/validation-targets.tsv'), ['target', 'format', 'dockerfile', 'label']);
  assert.ok(targets.length > 0);
  for (const [target, format, dockerfile, label] of targets) {
    assert.ok(label, `${target} has every column`);
    assert.equal(FORMATS.find((row) => row.id === format)?.family, 'deb', `${target} installs a DEB format`);
    assert.match(read(`packaging/${dockerfile}`), /^FROM \S+ AS target$/m, `${target}: packaging/${dockerfile} has a target stage`);
  }
});

test('the RPM file list is installed-files.txt', () => {
  const spec = read('packaging/rpm/branch-truce.spec');
  const files = spec.slice(spec.indexOf('%files')).split('\n').slice(1)
    .filter((line) => line && !line.startsWith('%license'))
    .map((line) => line.replace('%{_bindir}', '/usr/bin').replace('%{_datadir}', '/usr/share')
      .replace('%{app_id}', 'com.canpicasoft.branch-truce'));
  assert.deepEqual(files.sort(), lines('packaging/installed-files.txt').map(([path]) => `/usr/${path}`).sort());
});

test('runtime-modules.txt covers every QML import and every QML module the RPM requires', () => {
  const qml = new Set(lines('packaging/runtime-modules.txt').filter(([kind]) => kind === 'qml').map(([, module]) => module));
  const ui = join(ROOT, 'src/ui');
  for (const file of readdirSync(ui).filter((name) => name.endsWith('.qml'))) {
    for (const [, module] of readFileSync(join(ui, file), 'utf8').matchAll(/^import ([A-Za-z][\w.]*)/gm)) {
      if (!module!.startsWith('BranchTruce')) assert.ok(qml.has(module!), `src/ui/${file} imports ${module}`);
    }
  }
  for (const [, module] of read('packaging/rpm/branch-truce.spec').matchAll(/^Requires:\s+qt6qml\(([^)]+)\)/gm)) {
    assert.ok(qml.has(module!), `the spec requires ${module}`);
  }
});

test('the PKGBUILD depends on each floor of minimum-versions.tsv', () => {
  const pkgbuild = read('packaging/arch/PKGBUILD');
  assert.deepEqual(header('packaging/minimum-versions.tsv'), ['component', 'minimum', ...new Set(FORMATS.map((format) => format.family!))]);
  for (const [component, minimum, , , arch] of rows('packaging/minimum-versions.tsv')) {
    const floor = minimum!.split('.').slice(0, 2).join('.');
    assert.ok(pkgbuild.includes(`'${arch}>=${floor}'`), `${component}: the PKGBUILD lacks ${arch}>=${floor}`);
  }
});

test('package.json declares every package the scripts and CI modules import', () => {
  const manifest = JSON.parse(read('package.json'));
  const declared = new Set(Object.keys({ ...manifest.dependencies, ...manifest.devDependencies }));
  const sources = [
    ...readdirSync(join(ROOT, 'scripts')).filter((name) => name.endsWith('.ts')).map((name) => `scripts/${name}`),
    ...readdirSync(join(ROOT, 'packaging/ci')).filter((name) => name.endsWith('.js')).map((name) => `packaging/ci/${name}`),
  ];
  const specifier = /(?:^import\s[^'"]*|\bimport\(|\brequire\()\s*['"]([^'"]+)['"]/gm;
  let checked = 0;
  for (const source of sources) {
    for (const [, name] of read(source).matchAll(specifier)) {
      if (name!.startsWith('.') || name!.startsWith('node:')) continue;
      const pkg = name!.split('/').slice(0, name!.startsWith('@') ? 2 : 1).join('/');
      assert.ok(declared.has(pkg), `${source} imports ${pkg}, which package.json does not declare`);
      checked += 1;
    }
  }
  assert.ok(checked > 0, 'no package import found');
});
