import { test } from 'node:test';
import assert from 'node:assert/strict';
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
const rows = (path: string) => read(path).split('\n').filter((line) => line && !line.startsWith('#')).map((line) => line.split('\t'));
const require = createRequire(import.meta.url);

const FORMATS = rows('packaging/formats.tsv').map(([id, family, extension, style, filename, native, architecture, dockerfile, label]) =>
  ({ id, family, extension, style, filename, native, architecture, dockerfile, label }));

test('formats.tsv has complete rows whose family scripts and Dockerfiles exist', () => {
  assert.ok(FORMATS.length > 0);
  for (const format of FORMATS) {
    assert.ok(format.label, `${format.id} has nine columns`);
    assert.ok(['tilde', 'plain'].includes(format.style!), `${format.id} version style`);
    for (const path of [`packaging/scripts/package-${format.family}.sh`, `packaging/scripts/pkg-${format.family}.sh`,
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
  assert.deepEqual(workflow.jobs.kde.strategy.matrix.include.map((leg: { format: string }) => leg.format),
    FORMATS.map((format) => format.id));
});

test('the RPM file list is installed-files.txt', () => {
  const spec = read('packaging/rpm/branch-truce.spec');
  const files = spec.slice(spec.indexOf('%files')).split('\n').slice(1)
    .filter((line) => line && !line.startsWith('%license'))
    .map((line) => line.replace('%{_bindir}', '/usr/bin').replace('%{_datadir}', '/usr/share')
      .replace('%{app_id}', 'com.canpicasoft.branch-truce'));
  assert.deepEqual(files.sort(), rows('packaging/installed-files.txt').map(([path]) => `/usr/${path}`).sort());
});

test('runtime-modules.txt covers every QML import and every QML module the RPM requires', () => {
  const qml = new Set(rows('packaging/runtime-modules.txt').filter(([kind]) => kind === 'qml').map(([, module]) => module));
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
