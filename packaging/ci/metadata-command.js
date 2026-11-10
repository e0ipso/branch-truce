/** Bounded metadata commands. Each call owns a new session and leaves stderr on stderr. */
import { spawn } from 'node:child_process';

/**
 * @param {string} command
 * @param {string[]} args
 * @param {{cwd: string, env: NodeJS.ProcessEnv, signal?: AbortSignal}} context
 * @param {{timeout?: number, maxOutput?: number}} [limits]
 * @returns {Promise<string>}
 */
export function run(command, args, { cwd, env, signal }, { timeout = 30_000, maxOutput = 4 * 1024 * 1024 } = {}) {
  return new Promise((resolve, reject) => {
    const child = spawn(command, args, { cwd, env, detached: true, stdio: ['ignore', 'pipe', 'pipe'] });
    /** @type {Buffer[]} */
    const output = [];
    let size = 0;
    /** @type {Error | undefined} */
    let failure;
    const stop = () => {
      if (child.pid) {
        try { process.kill(-child.pid, 'SIGKILL'); }
        catch (error) { if (/** @type {NodeJS.ErrnoException} */ (error).code !== 'ESRCH') failure ??= /** @type {Error} */ (error); }
      }
    };
    const cancel = () => { failure ??= new Error(`${command} cancelled`); stop(); };
    const timer = setTimeout(() => { failure ??= new Error(`${command} timed out after ${timeout}ms`); stop(); }, timeout);
    const interrupt = () => { process.exitCode = 130; cancel(); };
    const terminate = () => { process.exitCode = 143; cancel(); };
    process.once('SIGINT', interrupt);
    process.once('SIGTERM', terminate);
    signal?.addEventListener('abort', cancel, { once: true });
    if (signal?.aborted) cancel();
    for (const stream of [child.stdout, child.stderr]) stream.on('data', (/** @type {Buffer} */ chunk) => {
      size += chunk.length;
      if (size > maxOutput) { failure ??= new Error(`${command} exceeded ${maxOutput} output bytes`); stop(); }
      else if (stream === child.stdout) output.push(chunk);
      else process.stderr.write(chunk);
    });
    child.on('error', error => { failure ??= error; });
    child.on('close', (code, killedBy) => {
      clearTimeout(timer);
      signal?.removeEventListener('abort', cancel);
      process.removeListener('SIGINT', interrupt);
      process.removeListener('SIGTERM', terminate);
      stop();
      if (failure) reject(failure);
      else if (code === 0) resolve(Buffer.concat(output).toString('utf8').trim());
      else reject(Object.assign(new Error(`${command} ${args.join(' ')} exited with ${code ?? killedBy}`),
        { status: code, signal: killedBy }));
    });
  });
}
