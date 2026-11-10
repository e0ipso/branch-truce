import { test } from 'node:test';
import assert from 'node:assert/strict';
import { run } from './metadata-command.js';

const context = { cwd: process.cwd(), env: process.env };

test('metadata commands preserve stdout, stderr and exit diagnostics', async () => {
  assert.equal(await run(process.execPath, ['-e', 'process.stdout.write("€\\n")'], context), '€');
  await assert.rejects(run(process.execPath, ['-e', 'process.stderr.write("native diagnostic\\n"); process.exit(17)'], context), /exited with 17/);
  await assert.rejects(run('/missing/metadata-command', [], context), /ENOENT/);
});

test('metadata commands enforce deadlines, output bounds and cancellation', async () => {
  await assert.rejects(run(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], context, { timeout: 100 }), /timed out/);
  await assert.rejects(run(process.execPath, ['-e', 'process.stdout.write("x".repeat(65536))'], context, { maxOutput: 1024 }), /output bytes/);
  const controller = new AbortController();
  const pending = run(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { ...context, signal: controller.signal });
  controller.abort();
  await assert.rejects(pending, /cancelled/);
  await assert.rejects(run(process.execPath, ['-e', 'setInterval(() => {}, 1000)'], { ...context, signal: controller.signal }), /cancelled/);
});

test('a metadata leader exiting early still leaves its descendants bounded', async () => {
  await assert.rejects(run(process.execPath, ['-e',
    'require("node:child_process").spawn(process.execPath, ["-e", "setInterval(() => {}, 1000)"], {stdio: "inherit"}); process.exit(0)'],
    context, { timeout: 300 }), /timed out/);
});
