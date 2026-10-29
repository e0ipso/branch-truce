import { execFileSync } from 'node:child_process';
import { lstatSync, readFileSync } from 'node:fs';
import { fileURLToPath } from 'node:url';

// Fails when a tracked file cites a Sift ticket ID or a plan under the agent planning
// directory. Those trackers are not part of the published repository, so a reference to one leads a
// reader nowhere. Run it with `npm run check:tracker-refs`; CI runs it on every PR and push.
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

const ROOT = fileURLToPath(new URL('../', import.meta.url));

const isPlanExcluded = (path: string) =>
  PLAN_EXCLUDED.some((entry) => (entry.endsWith('/') ? path.startsWith(entry) : path === entry));

function trackedFiles(): string[] {
  const out = execFileSync('git', ['ls-files', '-z'], {
    cwd: ROOT,
    encoding: 'utf8',
    maxBuffer: 64 * 1024 * 1024,
  });
  return out.split('\0').filter((path) => path && !path.startsWith(SIFT_ROOT));
}

/** The file's text, or null for a path that is not a readable regular text file. */
function readText(path: string): string | null {
  try {
    if (!lstatSync(`${ROOT}${path}`).isFile()) return null;
    const bytes = readFileSync(`${ROOT}${path}`);
    return bytes.includes(0) ? null : bytes.toString('utf8');
  } catch (error) {
    // A tracked file deleted in the working tree has nothing to scan.
    if ((error as NodeJS.ErrnoException).code === 'ENOENT') return null;
    throw error;
  }
}

const hits: string[] = [];
for (const path of trackedFiles()) {
  const text = readText(path);
  if (text === null) continue;
  text.split('\n').forEach((line, index) => {
    if (TICKET.test(line) || (!isPlanExcluded(path) && line.includes(PLANS_PATH))) {
      hits.push(`${path}:${index + 1}`);
    }
  });
}

if (hits.length > 0) {
  console.error(
    `${hits.length} tracked line${hits.length === 1 ? '' : 's'} cite a ticket ID or a plan path:`,
  );
  for (const hit of hits) console.error(hit);
  console.error('Remove the reference. Ticket IDs belong only inside Sift.');
  process.exit(1);
}
console.log('No ticket or plan references in tracked files.');
