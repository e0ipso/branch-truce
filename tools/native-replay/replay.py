#!/usr/bin/env python3
"""Generate the approved fixture or replay its real native controls on private sway."""
import argparse
from contextlib import contextmanager
import fcntl
import json
import math
import os
from pathlib import Path
import selectors
import re
import threading
import signal
import socket
import subprocess
import sys
import tempfile
import time

# Keep the source checkout read only, including when it has no bytecode cache.
sys.dont_write_bytecode = True

from calibration import build_provenance, capture_inputs, colour_scheme, compare, load_contract, validate_profile
from fixture import REFERENCE, git, make_fixture, reference_bytes, sha256, unmerged

HERE = Path(__file__).resolve().parent


def require(condition, message):
    if not condition:
        raise RuntimeError(message)


def write_json(path, value):
    Path(path).write_text(json.dumps(value, indent=2) + '\n')


@contextmanager
def replay_lock(path=None):
    # Keep the inode in place: unlinking a flock file lets a third caller bypass a waiter.
    if path is None:
        path = Path('/tmp') / f'branch-truce-native-replay-{os.getuid()}.lock'
    fd = os.open(path, os.O_CREAT | os.O_RDWR | os.O_NOFOLLOW, 0o600)
    try:
        require(os.fstat(fd).st_uid == os.getuid(), 'Replay lock belongs to another user')
        fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        yield
    finally:
        os.close(fd)


class OwnedResources:
    """Own only this invocation's private directory and process groups."""
    def __init__(self, cleanup_receipt=None):
        self.cleanup_receipt = cleanup_receipt
        self.directory = tempfile.TemporaryDirectory(prefix='bt-r-', dir='/var/tmp')
        self.root = Path(self.directory.name)
        self.processes = []
        self.logs = []
        self.threads = []

    def spawn(self, argv, *, env, log, pass_fds=(), pipe=False):
        stream = Path(log).open('wb')
        self.logs.append(stream)
        process = subprocess.Popen(argv, env=env, stdin=subprocess.DEVNULL,
                                   stdout=subprocess.PIPE if pipe else stream,
                                   stderr=subprocess.STDOUT, start_new_session=True,
                                   pass_fds=pass_fds)
        self.processes.append(process)
        return process, stream

    def close(self):
        for process in reversed(self.processes):
            if process.poll() is None:
                try:
                    os.killpg(process.pid, signal.SIGTERM)
                except ProcessLookupError:
                    pass
                try:
                    process.wait(timeout=5)
                except subprocess.TimeoutExpired:
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    process.wait(timeout=5)
        for thread in self.threads:
            thread.join(timeout=5)
            require(not thread.is_alive(), "Owned log reader did not finish")
        for process in self.processes:
            if process.stdout:
                process.stdout.close()
        for log in self.logs:
            log.close()
        self.directory.cleanup()
        require(not self.root.exists(), f'Private replay directory survived cleanup: {self.root}')
        if self.cleanup_receipt:
            write_json(self.cleanup_receipt, dict(private_root=str(self.root),
                private_root_removed=not self.root.exists(),
                owned_processes_reaped=all(process.returncode is not None for process in self.processes),
                unrelated_outputs_touched=False))

    def __enter__(self):
        return self

    def __exit__(self, *_):
        self.close()


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
            observed += chunk
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

    def toolbar(self, target):
        return self.command('toolbar-click', target=target)

    def source_menu(self, side, line, gesture='right-click'):
        return self.command('source-menu', target=side + 'Text', line=line, gesture=gesture)

    def close(self):
        self.reader.close()
        self.channel.close()


def item(state, name):
    matches = [row for row in state['items'] if row['name'] == name and row.get('onScreen', True)]
    require(len(matches) == 1, f'{name}: expected one reachable control, observed {matches}')
    return matches[0]


def verify_prominent_actions(state):
    for name in ('saveDraft', 'markResolved'):
        control = item(state, name)
        require(control['x'] >= 0 and control['y'] >= 0
                and control['x'] + control['width'] <= state['width']
                and control['y'] + control['height'] <= state['height'],
                f'{name} must remain prominent and fully reachable: {control}')


def verify_window(state):
    if not any(row['name'] == 'comparisonView' for row in state['items']):
        return
    pitch = item(state, 'comparisonView')['rowHeight']
    for name in ('stage2Viewport', 'resultViewport', 'stage3Viewport'):
        pane = item(state, name)
        require(pitch > 0 and pane['height'] >= 8 * pitch, f'{name} has fewer than eight rows: {pane}')
    for name in ('stage2Identity', 'stage3Identity'):
        identity = item(state, name)
        require(not identity['truncated'], f'{name} truncated at {state["width"]}x{state["height"]}: {identity}')


def verify_decided_controls(state):
    for scope in ('section/1/392/446', 'section/2/392/450'):
        for kind in ('Accept', 'Ignore', 'Menu'):
            require(not any(row['name'] == f'choice{kind}-{scope}' and row.get('onScreen', True)
                            for row in state['items']), f'Decided range retains {kind} chrome: {scope}')


def verify_line_cue(state, pane, line, kind, fill=True):
    details = state['renderedDetails']
    glyphs = [row for row in details if row.get('line') == line and row.get('kind')
              and pane + 'GutterMarkerSlot' in row.get('ancestors', [])]
    require(len(glyphs) == 1 and glyphs[0]['kind'] == kind,
            f'{pane}:{line} must have exactly one {kind} glyph: {glyphs}')
    backgrounds = [row for row in details if row.get('line') == line
                   and pane + 'BackgroundsLines' in row.get('ancestors', [])]
    require((len(backgrounds) == 1 and backgrounds[0].get('kind') == kind) if fill else not backgrounds,
            f'{pane}:{line} background differs from {kind}, fill={fill}: {backgrounds}')


def verify_text(state, repo):
    require(state['path'] == 'src/net/retry.cpp', 'Wrong text file selected')
    verify_prominent_actions(state)
    verify_window(state)
    verify_decided_controls(state)
    for pane, line, kind, fill in [('stage3', 4, 'added', True), ('result', 4, 'added', True),
                                   ('stage2', 16, 'changed', True), ('result', 22, 'changed', True),
                                   ('stage2', 19, 'accepted', True), ('result', 25, 'accepted', True),
                                   ('stage3', 20, 'ignored', False)]:
        verify_line_cue(state, pane, line, kind, fill)
    require(int(state['progress']['conflicts']) == 1, 'Expected one actual conflict region')
    require(int(state['progress']['undecided']) == 8, 'Separate source decisions changed')
    require(item(state, 'undecidedCounter')['text'] == '1 conflict left', 'Conflict stepper count differs')
    for name, side in [('stage2Text', 'left'), ('stage3Text', 'right')]:
        require(item(state, name)['text'].encode() == reference_bytes(f'expected-{side}.cpp'), f'{side} source bytes differ')
    require(state['resultText'].encode() == reference_bytes('expected-result.cpp'), 'Result bytes differ')
    require(len(state['resultText'].splitlines()) == 30, 'Expected 30 result lines')
    blocks = state['markerBlocks']
    require(len(blocks) == 1 and blocks[0]['folded'] and blocks[0]['resultLineBegin'] == 11
            and blocks[0]['resultLineEnd'] == 18, 'Expected folded marker lines 11 through 17')
    numbers = [r['number'] for r in item(state, 'resultGutter')['rows']]
    require(11 in numbers and numbers[numbers.index(11)+1] == 18, 'Visible gutter must jump from 11 to 18')
    for side, line, decision in [('stage2', 19, 'accepted'), ('stage3', 20, 'ignored')]:
        require(any(r['side'] == side and r['scope'] == 'section' and r['sourceLineBegin'] == line
                    and r['state'] == decision for r in state['regions']), f'Missing {decision} contribution')
    require(item(state, 'workflowStatus')['text'] == '1 of 3 files resolved', 'Release notes not resolved')
    require(sorted(unmerged(repo)) == ['assets/logo.png', 'src/net/retry.cpp'], 'Unexpected unresolved Git index')
    require(git(repo, 'show', ':0:docs/release-notes.md').stdout == b'# Release notes\n\nAdd retry timeouts.\n',
            'Release notes staged bytes differ')
    require(all(item(state, name).get('contentY') == 0 for name in ('stage2Viewport', 'stage3Viewport', 'resultViewport')),
            'Comparison panes did not return to the top')
    manifest = json.loads((repo.parent / 'fixture.json').read_text())
    for side, branch in [('stage2', 'main'), ('stage3', 'incoming')]:
        require(manifest[branch][:10] in item(state, side + 'Identity')['text'], 'Source commit identity differs')
    require(not any(r['name'] == 'commandMessage' for r in state['items']), 'Unexpected command failure')


def verify_binary(state):
    require(state['path'] == 'assets/logo.png', 'Wrong binary file selected')
    verify_prominent_actions(state)
    require(item(state, 'selectedProblem')['text'] == 'Binary files are resolved outside Branch Truce',
            'Binary heading lost its exclusion meaning')
    explanation = item(state, 'selectedProblem')['explanation']
    require('logo.png' in explanation and 'git add assets/logo.png' in explanation
            and 'Git' in explanation, 'Concrete binary guidance missing')
    require(not item(state, 'markResolved')['enabled'] and not item(state, 'saveDraft')['enabled'],
            'Binary save/resolution must stay disabled')
    require(item(state, 'workflowStatus')['text'] == '1 of 3 files resolved', 'Resolved count changed')
    require(item(state, 'fileFacts')['text'] == 'PNG image · 18.20 KiB', 'Wrong binary facts')


def callout_closeups(state, shot, image_tool, output):
    output.mkdir()
    regions = {}
    for name in ('fileList', 'selectedPath', 'fileToolBar', 'stage2Identity', 'stage3Identity',
                 'stage2Gutter', 'resultGutter', 'stage3Gutter', 'stage2Bands', 'stage3Bands',
                 'stage2LeftEdge', 'stage2RightEdge', 'stage3LeftEdge', 'stage3RightEdge', 'selectedProblem'):
        matches = [row for row in state['items'] if row['name'] == name]
        if len(matches) != 1:
            continue
        row = matches[0]
        x, y = max(0, math.floor(row['x'])), max(0, math.floor(row['y']))
        right = min(state['width'], math.ceil(row['x'] + row['width']))
        bottom = min(state['height'], math.ceil(row['y'] + row['height']))
        if right <= x or bottom <= y:
            continue
        rect = [x, y, right-x, bottom-y]
        destination = output / f'{name}.png'
        subprocess.run([str(image_tool), '--crop', str(shot), str(destination), *map(str, rect)], check=True)
        regions[name] = dict(observed_scene_rect=[row[key] for key in ('x', 'y', 'width', 'height')],
                             native_pixel_crop=rect, file=destination.name,
                             acceptance='pending original-image inspection; native diagnostic only')
    write_json(output / 'callout-regions.json', dict(original=str(shot), regions=regions,
               transforms='exact native pixels, no scaling, overlays or target substitution'))


def capture_one(args, driver, out, theme, repeat):
    captures = []
    with OwnedResources(out / 'cleanup.json') as owned:
        env = private_environment(owned.root / 'home')
        fixture_root = owned.root / 'fixture'
        fixture = make_fixture(fixture_root)
        repo = fixture_root / 'retry-service'
        write_json(out / 'fixture.json', fixture)
        scheme = 'BreezeLight' if theme == 'light' else 'BreezeDark'
        colours = Path('/usr/share/color-schemes') / f'{scheme}.colors'
        (Path(env['XDG_CONFIG_HOME']) / 'kdeglobals').write_text(colours.read_text() +
            f'\n[General]\nColorScheme={scheme}\nfont=Noto Sans,10,-1,5,50,0,0,0,0,0\nfixed=Hack,9,-1,5,50,0,0,0,0,0\n')
        config = owned.root / 'sway.conf'
        config.write_text(f'output * mode 1280x{args.height} scale 1\n'
                          'default_border none\ndefault_floating_border none\n'
                          'seat seat0 fallback true\nfocus_follows_mouse no\n')
        sway_env = dict(env, WLR_BACKENDS='headless', WLR_LIBINPUT_NO_DEVICES='1',
                        WLR_RENDERER='pixman', WLR_HEADLESS_OUTPUTS='1')
        compositor, log = owned.spawn(['sway', '--verbose', '--config', str(config)],
                                      env=sway_env, log=out / 'compositor.log', pipe=True)
        display = await_compositor(compositor, log)
        # The readiness prefix has been consumed. Send later logs directly to the owned log.
        # Keep draining the pipe on a thread so compositor diagnostics cannot fill it.
        def drain():
            while chunk := compositor.stdout.read(65536):
                log.write(chunk)
                log.flush()
        drain_thread = threading.Thread(target=drain, daemon=True)
        owned.threads.append(drain_thread)
        drain_thread.start()
        sockets = list(Path(env['XDG_RUNTIME_DIR']).glob('sway-ipc.*.sock'))
        require(len(sockets) == 1, 'Expected one private sway socket')
        env.update(WAYLAND_DISPLAY=str(Path(env['XDG_RUNTIME_DIR']) / display),
                   SWAYSOCK=str(sockets[0]), QT_QPA_PLATFORM='wayland')
        outputs = json.loads(subprocess.check_output(['swaymsg', '-t', 'get_outputs', '-r'], env=env))
        require(len(outputs) == 1 and outputs[0]['active'] and outputs[0]['rect']['width'] == 1280
                and outputs[0]['rect']['height'] == args.height, 'Private output geometry differs')
        parent, child = socket.socketpair()
        client = NativeClient(parent, out)
        app_env = dict(env, LD_PRELOAD=str(driver), BRANCH_TRUCE_REPLAY_FD=str(child.fileno()))
        try:
            app, _ = owned.spawn([str(args.build / 'src/app/branch-truce'), '--mergetool-path',
                                  'src/net/retry.cpp'.encode().hex(), str(repo)], env=app_env,
                                 log=out / 'app.log', pass_fds=(child.fileno(),))
            child.close()
            client.command(path='src/net/retry.cpp', target='choiceAccept-section/1/392/446')
            client.command('click', target='choiceAccept-section/1/392/446')
            client.command('click', target='choiceIgnore-section/2/392/450')
            client.command('click-path', target='docs/release-notes.md')
            client.command(path='docs/release-notes.md', target='choiceAccept-section/1/17/37')
            client.command('click', target='choiceAccept-section/1/17/37')
            client.command('click', target='choiceIgnore-section/2/17/46')
            client.toolbar('markResolved')
            client.command(target='workflowStatus', text='1 of 3 files resolved')
            client.command('click-path', target='src/net/retry.cpp')
            client.command(path='src/net/retry.cpp')
            client.toolbar('undoCommand')
            undone = client.command()
            write_json(out / 'undo-state.json', undone)
            require(any(row['name'] == 'choiceIgnore-section/2/392/450' and row.get('onScreen', True)
                        for row in undone['items']), 'Undo must restore undecided controls')
            client.toolbar('redoCommand')
            for side, line, gesture in [('stage2', 19, 'Menu'), ('stage3', 20, 'Shift+F10'),
                                         ('stage2', 10, 'right-click')]:
                menu = client.source_menu(side, line, gesture)
                require(any(row['name'].startswith('choiceResolve-') for row in menu['items']),
                        f'{gesture} did not open a real range menu at {side}:{line}')
                write_json(out / f'{side}-{line}-menu-state.json', menu)
                client.command('escape')
            client.command('click', target='stage2Label')
            state = client.command('neutral')
            verify_text(state, repo)
            for kind in ('text', 'binary'):
                if kind == 'binary':
                    client.command('click-path', target='assets/logo.png')
                    state = client.command('neutral', path='assets/logo.png')
                    verify_binary(state)
                    client.command('click-accessible', target='Technical details')
                    details = client.command()
                    diagnostic = item(details, 'selectedProblemDiagnostic')
                    require(diagnostic['text'] == details['supportDetail'] and 'UTF-8' in diagnostic['text'],
                            'Technical details changed or lost the captured decoder diagnostic')
                    require(diagnostic['readOnly'] and diagnostic['textFormat'] == 0,
                            'Technical details must remain selectable plain text')
                    write_json(out / 'binary-details-state.json', details)
                    client.command('escape')
                state = client.command('capture-ready', path='src/net/retry.cpp' if kind == 'text' else 'assets/logo.png')
                if kind == 'text':
                    verify_text(state, repo)
                else:
                    verify_binary(state)
                require(state['width'] == 1280 and state['height'] == args.height, 'Window geometry differs')
                name = f'{theme}-{kind}'
                shot = out / f'{name}.png'
                subprocess.run(['grim', '-o', outputs[0]['name'], str(shot)], env=env, check=True)
                image = json.loads(subprocess.check_output([str(driver.parent / 'native-replay-image'), str(shot)]))
                require((image['width'], image['height']) == (1280, args.height), 'Capture geometry differs')
                decoded = image['decoded_sha256']
                metadata = dict(theme=theme, kind=kind, repeat=repeat, height=args.height,
                                fixture=fixture, fixture_sha256=sha256(out / 'fixture.json'), executable_sha256=sha256(args.build / 'src/app/branch-truce'),
                                driver_sha256=sha256(driver), capture_sha256=sha256(shot), decoded_sha256=decoded,
                                capture_environment=dict(colour_scheme=scheme, kdeglobals_sha256=sha256(Path(env['XDG_CONFIG_HOME']) / 'kdeglobals'),
                                                         output=outputs[0], renderer='pixman', quick_backend='software',
                                                         os_release=Path('/etc/os-release').read_text(), physical_desktop=False,
                                                         scheme_roles=colour_scheme(Path(env['XDG_CONFIG_HOME']) / 'kdeglobals')),
                                reference_provenance_sha256=sha256(REFERENCE / 'provenance.json'))
                metadata.update(capture_profile_sha256=sha256(HERE / 'capture-profile.json'),
                                reference_contract_sha256=sha256(HERE / 'reference-contract.json'))
                validate_profile(state, metadata)
                write_json(out / f'{name}-state.json', dict(metadata=metadata, state=state))
                callout_closeups(state, shot, driver.parent / 'native-replay-image', out / f'{name}-callouts')
                compare(args.source, shot, state, driver.parent / 'native-replay-image', out / f'{name}-comparison')
                captures.append(dict(file=str(shot), **metadata))
            if getattr(args, 'documentation', False) and repeat == 1 and theme == 'light':
                docs = args.output / 'documentation-readme'
                docs.mkdir()
                preview_env = dict(env, PREVIEW_FILE='notes.md', PREVIEW_SIZE='1440x900',
                                   PREVIEW_OUTPUT=str(docs))
                with (docs / 'preview.log').open('wb') as log:
                    subprocess.run([str(args.source / 'tools/preview-app.sh'),
                                    str(args.build / 'src/app/branch-truce')],
                                   env=preview_env, stdout=log, stderr=subprocess.STDOUT, check=True)
                write_json(docs / 'provenance.json', dict(
                    command=['tools/preview-app.sh', str(args.build / 'src/app/branch-truce')],
                    environment={key: preview_env[key] for key in ('PREVIEW_FILE', 'PREVIEW_SIZE')},
                    captured_at_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
                    executable_sha256=sha256(args.build / 'src/app/branch-truce'),
                    build_provenance='../build-provenance.json', physical_desktop=False,
                    images={path.name: sha256(path) for path in docs.glob('*.png')},
                    acceptance='pending original-image inspection; development build, not installed validation'))
            client.command('quit')
            require(app.wait(timeout=10) == 0, 'Application exit failed')
        except BaseException as error:
            # Keep the actual failing scene even when a semantic/readiness check
            # stops the replay before its normal capture boundary.
            failure = subprocess.run(['grim', '-o', outputs[0]['name'], str(out / 'failure.png')],
                                     env=env, capture_output=True)
            write_json(out / 'failure.json', dict(error=str(error), capture_exit=failure.returncode,
                capture_stderr=failure.stderr.decode(errors='replace'),
                scene='actual private output at failure; not an approved final state'))
            raise
        finally:
            child.close()
            client.close()
        # Stop the owned compositor before joining its log reader and deleting private paths.
        if compositor.poll() is None:
            compositor.terminate()
        compositor.wait(timeout=10)
        drain_thread.join(timeout=5)
        require(not drain_thread.is_alive(), 'Compositor log reader did not finish')
    return captures


def capture(args):
    require((args.source / 'tools/native-replay/replay.py').resolve() == Path(__file__).resolve(),
            '--source must name the checkout containing this replay tool')
    require((args.build / 'src/app/branch-truce').is_file(), '--build must contain src/app/branch-truce')
    require(args.repeat >= 2, 'Capture requires at least two clean replays for pixel comparison')
    load_contract(args.source)
    with replay_lock():
        args.output.mkdir(exist_ok=False)
        write_json(args.output / 'build-provenance.json', build_provenance(args.source, args.build))
        driver_build = args.output / 'driver-build'
        subprocess.run(['cmake', '-S', str(HERE), '-B', str(driver_build)], check=True)
        subprocess.run(['cmake', '--build', str(driver_build), '--parallel', '2'], check=True)
        driver = driver_build / 'libnative_replay.so'
        records = []
        for repeat in range(args.repeat):
            for theme in ('light', 'dark'):
                out = args.output / f'replay-{repeat+1}-{theme}'
                out.mkdir()
                records += capture_one(args, driver, out, theme, repeat+1)
        comparisons = []
        for theme in ('light', 'dark'):
            for kind in ('text', 'binary'):
                selected = [r for r in records if r['theme'] == theme and r['kind'] == kind]
                for row in selected[1:]:
                    comparison = json.loads(subprocess.check_output([str(driver_build / 'native-replay-image'),
                                                                      selected[0]['file'], row['file']]))
                    comparisons.append(dict(theme=theme, kind=kind, repeat=row['repeat'],
                                            differing_pixels=comparison['differing_pixels']))
        write_json(args.output / 'verification.json', dict(captures=records, comparisons=comparisons,
            source=str(args.source), build=str(args.build), height=args.height,
            environment=dict(platform='wayland', renderer='pixman', quick_backend='software'),
            cleanup_confirmed=True, source_inputs=capture_inputs(args.source),
            artifact_hashes={str(p.relative_to(args.output)): sha256(p) for p in args.output.rglob('*')
                             if p.is_file() and 'driver-build' not in p.relative_to(args.output).parts}))
        require(all(row['differing_pixels'] == 0 for row in comparisons), 'Clean replays differ in decoded pixels')
        print(f'{len(records)} native captures; {len(comparisons)} comparisons, zero differing decoded pixels; cleanup confirmed')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='mode', required=True)
    fixture = sub.add_parser('fixture', help='Create a new deterministic merge without starting the app')
    fixture.add_argument('--output', required=True, type=Path)
    native = sub.add_parser('capture', help='Build the external driver and replay the real application twice')
    native.add_argument('--source', required=True, type=Path)
    native.add_argument('--build', required=True, type=Path)
    native.add_argument('--output', required=True, type=Path)
    native.add_argument('--height', type=int, choices=(720, 800), default=720)
    native.add_argument('--documentation', action='store_true', help='Also retain the fresh README preview pair')
    native.add_argument('--repeat', type=int, default=2)
    args = parser.parse_args()
    def interrupted(signum, _frame):
        raise SystemExit(128 + signum)
    signal.signal(signal.SIGTERM, interrupted)
    for key in ('source', 'build', 'output'):
        if hasattr(args, key):
            setattr(args, key, getattr(args, key).resolve())
    if args.mode == 'fixture':
        print(json.dumps(make_fixture(args.output), indent=2))
    else:
        capture(args)


if __name__ == '__main__':
    main()
