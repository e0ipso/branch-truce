"""Bounded captured commands and cleanup of exclusively owned process groups."""
import os
import selectors
import signal
import subprocess
import time


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


def run(argv, *, env, timeout=30, max_output=4 * 1024 * 1024):
    """Capture bytes with a combined output budget and deadline; cancellation always cleans up."""
    process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
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
        return subprocess.CompletedProcess(argv, process.returncode, *map(bytes, output))
    finally:
        try:
            stop_group(process, grace=0.2)
        finally:
            process.stdout.close()
            process.stderr.close()
