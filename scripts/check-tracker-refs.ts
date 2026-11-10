import { execFileSync } from 'node:child_process';
import { lstatSync, readFileSync } from 'node:fs';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

// Fails when a tracked file cites a Sift ticket ID or a plan under the agent planning
// directory. Those trackers are not part of the published repository, so a reference to one leads a
// reader nowhere. Run it with `npm run check:tracker-refs`; CI runs it on every PR and push.
//
//   node scripts/check-tracker-refs.ts [--help]
//
// Prints `path:line` for each hit on stderr and exits 1, or prints a clean message and exits 0.
// Usage errors exit 2. findReferences() is the check itself, for code and tests; it reads nothing but
// what its caller passes in. main() lists the files with Git and reads them from the working tree.
//
// Neither pattern matches its own source: the regex has `\d`, not digits, and the path is joined.

const TICKET = /\bTRUCE-\d+\b/i;
const SIFT_ROOT = ['.ai', 'sift', ''].join('/');
const PLANS_PATH = ['.ai', 'strikethroo', 'plans'].join('/');

/** These exclusions apply only to plan paths. Ticket IDs are allowed only inside Sift. */
const PLAN_EXCLUDED = [
  '.ai/',
  '.agents/',
  '.claude/',
  '.cursor/',
  '.codex/',
  'skills-lock.json',
  '.kkignore',
  'tests/',
];

export const CLEAN_MESSAGE = 'No ticket or plan references in tracked files.';
const USAGE = 'usage: node scripts/check-tracker-refs.ts [--help]';

const isPlanExcluded = (path: string) =>
  PLAN_EXCLUDED.some((entry) => (entry.endsWith('/') ? path.startsWith(entry) : path === entry));

/** Whether LINE of the tracked file PATH cites a ticket ID or a plan path. */
export function isReference(path: string, line: string): boolean {
  return TICKET.test(line) || (!isPlanExcluded(path) && line.includes(PLANS_PATH));
}

/** The paths Git tracks or has staged under ROOT, outside the Sift tracker. Throws outside a repository. */
export function trackedFiles(root: string): string[] {
  const out = execFileSync('git', ['ls-files', '-z'], {
    cwd: root,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
  });
  return out.split('\0').filter((path) => path && !path.startsWith(SIFT_ROOT));
}

/** The file's text, or null for a path that is not a readable regular text file. Other errors throw. */
export function readText(root: string, path: string): string | null {
  try {
    if (!lstatSync(join(root, path)).isFile()) return null;
    const bytes = readFileSync(join(root, path));
    return bytes.includes(0) ? null : bytes.toString('utf8');
  } catch (error) {
    // A tracked file deleted in the working tree has nothing to scan.
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return null;
    throw error;
  }
}

/** Every `path:line` of PATHS, in their order, whose text cites a ticket ID or a plan path. READ gives a
 * path's text, or null to skip it. */
export function findReferences(paths: Iterable<string>, read: (path: string) => string | null): string[] {
  const hits: string[] = [];
  for (const path of paths) {
    const text = read(path);
    if (text === null) continue;
    text.split('\n').forEach((line, index) => {
      if (isReference(path, line)) hits.push(`${path}:${index + 1}`);
    });
  }
  return hits;
}

/** Runs the check for the repository around this script and returns the exit status. */
export function main(argv: string[], out = process.stdout, err = process.stderr): number {
  if (argv.length === 1 && (argv[0] === '-h' || argv[0] === '--help')) {
    out.write(`${USAGE}\n`);
    return 0;
  }
  if (argv.length > 0) {
    err.write(`${USAGE}\n`);
    return 2;
  }
  const root = fileURLToPath(new URL('../', import.meta.url));
  const hits = findReferences(trackedFiles(root), (path) => readText(root, path));
  if (hits.length === 0) {
    out.write(`${CLEAN_MESSAGE}\n`);
    return 0;
  }
  err.write(`${hits.length} tracked line${hits.length === 1 ? '' : 's'} cite a ticket ID or a plan path:\n`);
  for (const hit of hits) err.write(`${hit}\n`);
  err.write('Remove the reference. Ticket IDs belong only inside Sift.\n');
  return 1;
}

if (process.argv[1] && fileURLToPath(import.meta.url) === process.argv[1]) {
  process.exitCode = main(process.argv.slice(2));
}
