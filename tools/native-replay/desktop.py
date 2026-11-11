"""Private desktop and app adapters. The supplied OwnedResources owns every process and reader."""
from contextlib import contextmanager
import json
import os
from pathlib import Path
import re
import selectors
import socket
import subprocess
import time

from processes import run
from reports import write_json
from scene import require


def private_environment(root):
    # Build a new environment rather than inheriting Git, Qt, HOME or preload overrides.
    for name in ('config', 'cache', 'data', 'runtime', 'tmp'):
        (root / name).mkdir(parents=True)
    (root / 'runtime').chmod(0o700)
    return dict(PATH='/usr/bin:/bin', HOME=str(root), TMPDIR=str(root / 'tmp'),
                XDG_CONFIG_HOME=str(root / 'config'), XDG_CACHE_HOME=str(root / 'cache'),
                XDG_DATA_HOME=str(root / 'data'), XDG_RUNTIME_DIR=str(root / 'runtime'),
                LC_ALL='C.UTF-8', GIT_CONFIG_NOSYSTEM='1', GIT_CONFIG_GLOBAL='/dev/null',
                GIT_TERMINAL_PROMPT='0', QT_QUICK_BACKEND='software',
                XDG_SESSION_TYPE='wayland', XDG_CURRENT_DESKTOP='KDE',
                KDE_SESSION_VERSION='6', KDE_FULL_SESSION='true')


def await_compositor(process, log, timeout=30):
    """Wait for the compositor's readiness signal, including early-exit diagnostics."""
    deadline = time.monotonic() + timeout
    observed = b''
    with selectors.DefaultSelector() as selector:
        selector.register(process.stdout, selectors.EVENT_READ)
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0 or not selector.select(remaining):
                raise TimeoutError('Private sway did not publish a Wayland display')
            chunk = os.read(process.stdout.fileno(), 65536)
            if not chunk:
                raise RuntimeError('Private sway exited before publishing a Wayland display')
            log.write(chunk)
            log.flush()
            observed = (observed + chunk)[-65536:]
            match = re.search(rb"Running compositor on wayland display '([^']+)'", observed)
            if match:
                return match[1].decode()


class NativeClient:
    def __init__(self, channel, diagnostics=None):
        self.diagnostics = diagnostics
        self.sequence = 0
        self.channel = channel
        self.channel.settimeout(40)
        self.reader = channel.makefile('rb')

    def command(self, action='wait', **kwargs):
        request = dict(action=action, **kwargs)
        self.sequence += 1
        if self.diagnostics:
            write_json(self.diagnostics / 'last-request.json', dict(sequence=self.sequence, **request))
        self.channel.sendall(json.dumps(request).encode() + b'\n')
        line = self.reader.readline(4 * 1024 * 1024)
        require(line.endswith(b'\n'), 'Native driver closed or returned an incomplete reply')
        reply = json.loads(line)
        if self.diagnostics:
            write_json(self.diagnostics / 'last-reply.json', reply)
            with (self.diagnostics / 'actions.jsonl').open('a') as log:
                log.write(json.dumps(dict(sequence=self.sequence, request=request, error=reply['error'])) + '\n')
        require(not reply['error'], reply['error'])
        return reply['state']

    def close(self):
        self.reader.close()
        self.channel.close()


def write_kdeglobals(config_home, scheme, scheme_dir, fonts):
    """Configure the private session's colour scheme and fonts. Returns the kdeglobals path."""
    colours = Path(scheme_dir) / f'{scheme}.colors'
    path = Path(config_home) / 'kdeglobals'
    path.write_text(colours.read_text() + f'\n[General]\nColorScheme={scheme}\n'
                    + ''.join(f'{key}={value}\n' for key, value in fonts.items()))
    return path


def boot_compositor(owned, size, log_path):
    env = private_environment(owned.root / 'home')
    config = owned.root / 'sway.conf'
    config.write_text(f'output * mode {size[0]}x{size[1]} scale 1\n'
                      'default_border none\ndefault_floating_border none\n'
                      'seat seat0 fallback true\nfocus_follows_mouse no\n')
    sway_env = dict(env, WLR_BACKENDS='headless', WLR_LIBINPUT_NO_DEVICES='1',
                    WLR_RENDERER='pixman', WLR_HEADLESS_OUTPUTS='1')
    process, log = owned.spawn(['sway', '--verbose', '--config', str(config)],
                               env=sway_env, log=log_path, pipe=True)
    display = await_compositor(process, log)
    # The readiness prefix has been consumed. Send later logs directly to the owned log.
    # Keep draining the pipe on a thread so compositor diagnostics cannot fill it.
    def drain(source, destination):
        while chunk := source.read(65536):
            destination.write(chunk)
            destination.flush()
    owned.start_reader(drain, process.stdout, log)
    sockets = list(Path(env['XDG_RUNTIME_DIR']).glob('sway-ipc.*.sock'))
    require(len(sockets) == 1, 'Expected one private sway socket')
    env = dict(env, WAYLAND_DISPLAY=str(Path(env['XDG_RUNTIME_DIR']) / display),
               SWAYSOCK=str(sockets[0]), QT_QPA_PLATFORM='wayland')
    outputs = json.loads(run(['swaymsg', '-t', 'get_outputs', '-r'], env=env,
                             timeout=10, max_output=1024 * 1024, check=True).stdout)
    require(len(outputs) == 1 and outputs[0]['active'] and outputs[0]['rect']['width'] == size[0]
            and outputs[0]['rect']['height'] == size[1], 'Private output geometry differs')
    return env, outputs[0]


@contextmanager
def application(owned, env, *, app, driver, repo, out, shoot):
    """Launch one driven app; retain failure evidence and close its protocol channels."""
    parent, child = socket.socketpair()
    client = NativeClient(parent, out)
    env = dict(env, LD_PRELOAD=str(driver), BRANCH_TRUCE_REPLAY_FD=str(child.fileno()))
    try:
        process, _ = owned.spawn([str(app), '--mergetool-path', 'src/net/retry.cpp'.encode().hex(), str(repo)],
                                 env=env, log=out / 'app.log', pass_fds=(child.fileno(),))
        child.close()
        yield client
        client.command('quit')
        require(process.wait_exit(10) == 0, 'Application exit failed')
    except BaseException as error:
        # Keep the actual failing scene even when a semantic/readiness check
        # stops the replay before its normal capture boundary.
        try:
            failure = shoot(out / 'failure.png', check=False)
            evidence = dict(capture_exit=failure.returncode, capture_stderr=failure.stderr.decode(errors='replace'))
        except (OSError, subprocess.SubprocessError) as capture_error:
            evidence = dict(capture_exit=None, capture_error=str(capture_error))
        write_json(out / 'failure.json', dict(error=str(error), **evidence,
            scene='actual private output at failure; not an approved final state'))
        raise
    finally:
        child.close()
        client.close()


def screenshot(env, output_name, path, *, check=True):
    return run(['grim', '-o', output_name, str(path)], env=env,
               timeout=15, max_output=65536, check=check)
