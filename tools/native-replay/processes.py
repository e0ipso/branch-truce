"""Bounded captured commands and cleanup of exclusively owned process groups."""
from contextlib import ExitStack
import os
from pathlib import Path
import shutil
import tempfile
import threading
import selectors
import signal
import subprocess
import time

from reports import write_json


def interrupt(signum, _frame):
    raise SystemExit(128 + signum)


class OutputLimitError(subprocess.SubprocessError):
    def __init__(self, limit, output):
        super().__init__(f'command exceeded {limit} output bytes')
        self.stdout, self.stderr = map(bytes, output)


def stop_group(process, grace=5):
    """Stop the session created for PROCESS even after its leader has exited; reap the leader."""
    def send(sig):
        try:
            os.killpg(process.pid, sig)
        except ProcessLookupError:
            pass
    send(signal.SIGTERM)
    try:
        process.wait(timeout=grace)
    except subprocess.TimeoutExpired:
        pass
    finally:
        send(signal.SIGKILL)
    process.wait(timeout=5)


def run(argv, *, env=None, cwd=None, timeout=30, max_output=4 * 1024 * 1024,
        check=False, log=None, cleanup_grace=5):
    """Capture bounded output; allow a supervising child its cleanup_grace before forced termination."""
    process = subprocess.Popen(argv, env=env, cwd=cwd, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, start_new_session=True)
    output = [bytearray(), bytearray()]
    deadline = time.monotonic() + timeout
    try:
        with selectors.DefaultSelector() as selector:
            for index, stream in enumerate((process.stdout, process.stderr)):
                selector.register(stream, selectors.EVENT_READ, index)
            while selector.get_map():
                remaining = deadline - time.monotonic()
                if remaining <= 0 or not (events := selector.select(remaining)):
                    raise subprocess.TimeoutExpired(argv, timeout, *map(bytes, output))
                for key, _ in events:
                    chunk = os.read(key.fd, 65536)
                    if not chunk:
                        selector.unregister(key.fileobj)
                        continue
                    if sum(map(len, output)) + len(chunk) > max_output:
                        raise OutputLimitError(max_output, output)
                    output[key.data].extend(chunk)
            try:
                process.wait(timeout=max(0, deadline - time.monotonic()))
            except subprocess.TimeoutExpired:
                raise subprocess.TimeoutExpired(argv, timeout, *map(bytes, output)) from None
        result = subprocess.CompletedProcess(argv, process.returncode, *map(bytes, output))
        if check:
            result.check_returncode()
        return result
    finally:
        try:
            stop_group(process, grace=cleanup_grace)
        finally:
            process.stdout.close()
            process.stderr.close()
            if log is not None:
                Path(log).write_bytes(b''.join(output))


class OwnedResources:
    """Own only this invocation's private directory and process groups.

    The directory goes under base_dir, which defaults to /var/tmp: a short path keeps the private
    Wayland and sway sockets under the AF_UNIX length limit, whatever TMPDIR says.
    """
    def __init__(self, cleanup_receipt=None, base_dir=Path('/var/tmp')):
        self.cleanup_receipt = cleanup_receipt
        self.streams = ExitStack()
        self.root = Path(tempfile.mkdtemp(prefix='bt-r-', dir=base_dir))
        self.streams.callback(shutil.rmtree, self.root)
        self.processes = []
        self.threads = []
        self.reader_errors = []

    def spawn(self, argv, *, env, log, pass_fds=(), pipe=False):
        stream = self.streams.enter_context(Path(log).open('wb'))
        process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE if pipe else stream,
                                   stderr=subprocess.STDOUT, start_new_session=True,
                                   pass_fds=pass_fds)
        self.processes.append(process)
        if process.stdout:
            self.streams.enter_context(process.stdout)
        return process, stream

    def start_reader(self, target, *args):
        def run():
            try:
                target(*args)
            except Exception as error:
                self.reader_errors.append(error)
        reader = threading.Thread(target=run, daemon=True)
        reader.start()
        self.threads.append(reader)
        return reader

    def close(self):
        """Attempt every stop/join before releasing storage; retain it if an owner may still be live."""
        errors = []
        for process in reversed(self.processes):
            try:
                stop_group(process)
            except Exception as error:
                errors.append(f'Process {process.pid}: {type(error).__name__}: {error}')
        processes_stopped = not errors
        for thread in self.threads:
            try:
                thread.join(timeout=5)
                if thread.is_alive():
                    raise RuntimeError('Owned log reader did not finish')
            except Exception as error:
                errors.append(str(error))
        stopped = not errors
        if stopped:
            try:
                self.streams.close()
            except Exception as error:
                errors.append(str(error))
        errors += [f'Owned log reader failed: {type(error).__name__}: {error}' for error in self.reader_errors]
        if self.cleanup_receipt:
            write_json(self.cleanup_receipt, dict(private_root=str(self.root),
                private_root_removed=not self.root.exists(), owned_readers_stopped=all(not t.is_alive() for t in self.threads),
                owned_processes_reaped=processes_stopped and all(p.returncode is not None for p in self.processes),
                errors=errors))
        if errors:
            raise RuntimeError('; '.join(errors))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()
