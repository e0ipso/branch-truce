"""Fixture integrity, readiness protocol, serialization and resource ownership."""
from contextlib import contextmanager, nullcontext
import io
import json
import os
from pathlib import Path
import shutil
import signal
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import Mock, patch
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/native-replay'))
from NativeCalibrationTests import ContractTests
from calibration import cleanup_confirmed, collect_shot
import capture_profile
from ledger import assemble
from fixture import PATHS, GitError, git, make_fixture, png, reference_bytes, unmerged
import reports
from processes import OutputLimitError, OwnedResources, run
from collection import replay_lock
from desktop import NativeClient, application, await_compositor, private_environment, write_kdeglobals
from scene import item, verify_line_cue


class PrivateBaseTestCase(unittest.TestCase):
    """Keeps every owned private directory and receipt under a temporary base directory."""
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.base = Path(directory.name)

    def owned(self, cleanup_receipt=None):
        return OwnedResources(cleanup_receipt, base_dir=self.base)

    @contextmanager
    def client_reply(self, response, diagnostics=None):
        parent, child = socket.socketpair()
        client, requests = NativeClient(parent, diagnostics), []
        def respond():
            with child, child.makefile('rb') as reader:
                requests.append(json.loads(reader.readline()))
                child.sendall(response)
        worker = threading.Thread(target=respond)
        worker.start()
        try:
            yield client, requests
        finally:
            client.close()
            worker.join(timeout=5)
        self.assertFalse(worker.is_alive())


class FixtureTests(PrivateBaseTestCase):
    def test_exact_source_and_result_bytes(self):
        fixture = self.base / 'fixture with $ and spaces'
        make_fixture(fixture)
        for side, lines in [('left', 24), ('right', 25), ('result', 30)]:
            data = (fixture / f'expected-{side}.cpp').read_bytes()
            self.assertEqual(data, reference_bytes(f'expected-{side}.cpp'))
            self.assertEqual(len(data.splitlines()), lines)
            self.assertFalse(data.endswith(b'\n'))
        repo = fixture / 'retry-service'
        self.assertEqual(git(repo, 'show', ':2:src/net/retry.cpp').stdout, reference_bytes('expected-left.cpp'))
        self.assertEqual(git(repo, 'show', ':3:src/net/retry.cpp').stdout, reference_bytes('expected-right.cpp'))
        self.assertEqual(sorted(unmerged(repo)), sorted(PATHS))

    def test_history_is_reproducible_and_truthful(self):
        first = make_fixture(self.base / 'one')
        second = make_fixture(self.base / 'two')
        self.assertEqual(first, second)
        repo = self.base / 'one/retry-service'
        for branch, key in [('main', 'main'), ('feature/backoff', 'incoming')]:
            self.assertEqual(git(repo, 'rev-parse', branch).stdout.decode().strip(), first[key])
            self.assertEqual(git(repo, 'rev-parse', branch + '^').stdout.decode().strip(), first['base'])

    def test_unlisted_reference_names_the_provenance_file(self):
        with self.assertRaisesRegex(KeyError, 'missing.cpp is not listed in .*/provenance.json'):
            reference_bytes('missing.cpp')

    def test_failed_git_command_reports_its_stderr(self):
        with self.assertRaisesRegex(GitError, 'git rev-parse HEAD in .* exited 128: fatal: not a git repository'):
            git(self.base, 'rev-parse', 'HEAD')
        self.assertEqual(git(self.base, 'rev-parse', 'HEAD', check=False).returncode, 128)

    def test_png_chunks_crc_dimensions_and_decoded_pixels(self):
        for colour in [(100, 100, 100), (61, 174, 233), (39, 174, 96)]:
            image = png(colour)
            self.assertEqual((len(image), image[:8]), (18637, b'\x89PNG\r\n\x1a\n'))
            offset = 8
            decoded = b''
            kinds = []
            while offset < len(image):
                size = struct.unpack('>I', image[offset:offset+4])[0]
                kind, data = image[offset+4:offset+8], image[offset+8:offset+8+size]
                crc = struct.unpack('>I', image[offset+8+size:offset+12+size])[0]
                self.assertEqual(crc, zlib.crc32(kind + data) & 0xffffffff)
                kinds.append(kind)
                if kind == b'IHDR':
                    self.assertEqual(struct.unpack('>IIBBBBB', data), (64, 64, 8, 2, 0, 0, 0))
                if kind == b'IDAT':
                    decoded += zlib.decompress(data)
                offset += size + 12
            self.assertEqual(kinds, [b'IHDR', b'IDAT', b'tEXt', b'IEND'])
            self.assertEqual(decoded, (b'\x00' + bytes(colour) * 64) * 64)

    def test_entry_point_keeps_sources_unchanged_from_another_cwd(self):
        source = self.base / 'tool source with spaces'
        original = Path(__file__).resolve().parents[2] / 'tools/native-replay'
        shutil.copytree(original, source, ignore=shutil.ignore_patterns('__pycache__'))
        before = {str(p.relative_to(source)): p.read_bytes() for p in source.rglob('*') if p.is_file()}
        env = {k: v for k, v in os.environ.items() if k != 'PYTHONDONTWRITEBYTECODE'}
        run = subprocess.run([sys.executable, str(source / 'replay.py'), 'fixture',
                              '--output', str(self.base / 'new fixture')],
                             cwd='/', env=env, capture_output=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        after = {str(p.relative_to(source)): p.read_bytes() for p in source.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        self.assertFalse(list(source.rglob('__pycache__')))

    def test_existing_output_is_never_reused_or_removed(self):
        sentinel = self.base / 'sentinel'
        sentinel.write_bytes(b'untouched')
        with self.assertRaises(FileExistsError):
            make_fixture(self.base)
        self.assertEqual(sentinel.read_bytes(), b'untouched')

    def test_invoking_git_settings_and_repository_are_ignored(self):
        sentinel = self.base / 'sentinel'
        sentinel.write_bytes(b'untouched')
        attrs = self.base / 'git/attributes'
        attrs.parent.mkdir()
        attrs.write_text('*.cpp merge=union\n')
        config = self.base / 'config'
        config.write_text('[init]\n defaultBranch = hostile\n')
        env = dict(GIT_DIR=str(self.base / 'unrelated.git'), GIT_WORK_TREE=str(self.base),
                   GIT_CONFIG_GLOBAL=str(config), GIT_CONFIG_COUNT='1',
                   GIT_CONFIG_KEY_0='core.hooksPath', GIT_CONFIG_VALUE_0=str(self.base),
                   XDG_CONFIG_HOME=str(self.base))
        with patch.dict(os.environ, env):
            make_fixture(self.base / 'fixture')
            self.assertEqual(os.environ['GIT_DIR'], env['GIT_DIR'])
        self.assertEqual(config.read_text(), '[init]\n defaultBranch = hostile\n')
        self.assertEqual(sentinel.read_bytes(), b'untouched')
        self.assertFalse((self.base / 'unrelated.git').exists())


    def test_git_bounds_fail_with_its_typed_error_and_diagnostic(self):
        with patch('fixture.run', side_effect=subprocess.TimeoutExpired(['git'], 30, b'partial', b'git diagnostic')):
            with self.assertRaisesRegex(GitError, 'timed out.*git diagnostic'):
                git(self.base, 'status', check=False)
        with patch('fixture.run', side_effect=OutputLimitError(1024, (b'', b'output diagnostic'))):
            with self.assertRaisesRegex(GitError, 'output bytes.*output diagnostic'):
                git(self.base, 'status')


class ProcessTests(PrivateBaseTestCase):
    def test_capture_preserves_bytes_status_and_stderr(self):
        result = run([sys.executable, '-c', 'import sys; print("€"); print("diagnostic", file=sys.stderr); sys.exit(17)'],
                     env=dict(os.environ))
        self.assertEqual((result.returncode, result.stdout, result.stderr), (17, '€\n'.encode(), b'diagnostic\n'))

    def test_timeout_and_output_limits_are_bounded(self):
        with self.assertRaises(subprocess.TimeoutExpired) as failure:
            run([sys.executable, '-c', 'import sys,time; print("before timeout", file=sys.stderr, flush=True); time.sleep(60)'],
                env=dict(os.environ), timeout=0.2)
        self.assertEqual(failure.exception.stderr, b'before timeout\n')
        with self.assertRaises(OutputLimitError):
            run([sys.executable, '-c', 'import os; os.write(1, b"x" * 65536)'], env=dict(os.environ), max_output=1024)

    def test_checked_failure_keeps_bounded_log_and_working_directory(self):
        log = self.base / 'helper.log'
        with self.assertRaises(subprocess.CalledProcessError) as failure:
            run([sys.executable, '-c', 'import os,sys; print(os.getcwd()); print("failed", file=sys.stderr); sys.exit(17)'],
                cwd=self.base, timeout=5, max_output=1024, check=True, log=log)
        self.assertEqual(failure.exception.returncode, 17)
        self.assertEqual(failure.exception.stdout, (str(self.base) + '\n').encode())
        self.assertEqual(log.read_bytes(), failure.exception.stdout + b'failed\n')

    def test_cancellation_reaps_the_started_process(self):
        with patch('processes.selectors.DefaultSelector.select', side_effect=KeyboardInterrupt), \
             patch('processes.stop_group', wraps=__import__('processes').stop_group) as stop:
            with self.assertRaises(KeyboardInterrupt):
                run([sys.executable, '-c', 'import time; time.sleep(60)'], env=dict(os.environ))
        self.assertIsNotNone(stop.call_args.args[0].returncode)


class ResourceTests(PrivateBaseTestCase):
    def test_nested_owner_finishes_cleanup_before_outer_force_termination(self):
        script = '''
import json, os, signal, sys
from pathlib import Path
import processes
from processes import OwnedResources, interrupt, stop_group
processes.stop_group = lambda process: stop_group(process, grace=0.05)
signal.signal(signal.SIGTERM, interrupt)
with OwnedResources(base_dir=Path(sys.argv[1])) as owned:
    children = []
    for index in range(2):
        process, _ = owned.spawn([sys.executable, '-c',
            'import signal; signal.signal(signal.SIGTERM, signal.SIG_IGN); print("ready", flush=True); signal.pause()'],
            env=dict(os.environ), log=owned.root / str(index), pipe=True)
        assert process.stdout.readline() == b'ready\\n'
        children.append(process.pid)
    Path(sys.argv[2]).write_text(json.dumps(dict(root=str(owned.root), pids=children)))
    os.write(1, b'exceed')
    signal.pause()
'''
        ledger = self.base / 'owned.json'
        env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[2] / 'tools/native-replay'))
        try:
            with self.assertRaises(OutputLimitError):
                run([sys.executable, '-B', '-c', script, str(self.base), str(ledger)],
                    env=env, timeout=30, max_output=1, cleanup_grace=5)
            owned = json.loads(ledger.read_text())
            self.assertFalse(Path(owned['root']).exists())
            for pid in owned['pids']:
                with self.assertRaises(ProcessLookupError):
                    os.kill(pid, 0)
        finally:
            if ledger.exists():
                for pid in json.loads(ledger.read_text())['pids']:
                    try:
                        os.killpg(pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass

    def test_exited_leader_does_not_hide_a_surviving_child(self):
        unrelated = subprocess.Popen([sys.executable, '-c', 'import signal; signal.pause()'])
        self.addCleanup(lambda: (unrelated.terminate(), unrelated.wait()))
        with self.owned() as owned:
            process, _ = owned.spawn([sys.executable, '-c',
                'import os,signal; child=os.fork(); '
                'signal.signal(signal.SIGTERM, signal.SIG_IGN) if child == 0 else None; '
                'os._exit(0) if child else (print("child ready", flush=True), signal.pause())'],
                env=dict(os.environ), log=owned.root / 'log', pipe=True)
            self.assertEqual(process.stdout.readline(), b'child ready\n')
            self.assertEqual(process.wait(timeout=5), 0)
            owned.start_reader(lambda: process.stdout.read())
        self.assertIsNone(unrelated.poll())
        self.assertFalse(any(thread.is_alive() for thread in owned.threads))

    def test_private_directory_lives_under_the_given_base(self):
        with self.owned() as owned:
            self.assertEqual((owned.root.parent, owned.root.name.startswith('bt-r-')), (self.base, True))
        self.assertFalse(owned.root.exists())
        self.assertEqual(list(self.base.iterdir()), [])

    def test_kdeglobals_reads_the_scheme_from_the_given_directory(self):
        schemes = self.base / 'schemes'
        schemes.mkdir()
        (schemes / 'BreezeDark.colors').write_text('[Colors:Window]\nBackgroundNormal=32,35,38\n')
        fonts = capture_profile.run_shape(capture_profile.load()).kdeglobals_fonts
        path = write_kdeglobals(self.base, 'BreezeDark', schemes, fonts)
        self.assertEqual(path, self.base / 'kdeglobals')
        self.assertEqual(path.read_text(), '[Colors:Window]\nBackgroundNormal=32,35,38\n\n[General]\n'
                         'ColorScheme=BreezeDark\nfont=Noto Sans,10,-1,5,50,0,0,0,0,0\n'
                         'fixed=Hack,9,-1,5,50,0,0,0,0,0\n')


    def test_reader_failure_is_reported_after_private_cleanup(self):
        def broken_reader():
            raise OSError('broken sink')
        receipt = self.base / 'cleanup.json'
        with self.assertRaisesRegex(RuntimeError, 'Owned log reader failed: OSError: broken sink'):
            with self.owned(receipt) as owned:
                owned.start_reader(broken_reader)
        self.assertFalse(owned.root.exists())
        self.assertTrue(json.loads(receipt.read_text())['private_root_removed'])

    def test_failed_stop_attempts_every_owner_and_retains_storage_until_retry(self):
        import processes
        receipt = self.base / 'cleanup.json'
        owned = self.owned(receipt)
        first, stream = owned.spawn([sys.executable, '-c', 'import signal; signal.pause()'],
                                    env=dict(os.environ), log=owned.root / 'first.log')
        second, _ = owned.spawn([sys.executable, '-c', 'import signal; signal.pause()'],
                                 env=dict(os.environ), log=owned.root / 'second.log')
        reader = owned.start_reader(lambda: None)
        stop_group = processes.stop_group
        def stop(process):
            if process is second:
                raise PermissionError('stop refused')
            stop_group(process)
        try:
            with patch('processes.stop_group', side_effect=stop), self.assertRaisesRegex(RuntimeError, 'stop refused'):
                owned.close()
            self.assertIsNotNone(first.returncode)
            self.assertEqual((second.poll(), reader.is_alive(), stream.closed, owned.root.exists()),
                             (None, False, False, True))
            evidence = json.loads(receipt.read_text())
            self.assertEqual((evidence['owned_processes_reaped'], evidence['private_root_removed']), (False, False))
            self.assertIn('stop refused', evidence['errors'][0])
        finally:
            owned.close()
        self.assertEqual((stream.closed, owned.root.exists()), (True, False))

    def test_private_environment_does_not_modify_parent(self):
        before = dict(os.environ)
        with self.owned() as owned:
            env = private_environment(owned.root / 'home')
            self.assertEqual(Path(env['XDG_RUNTIME_DIR']).stat().st_mode & 0o777, 0o700)
            self.assertTrue(all(Path(env[key]).is_relative_to(owned.root)
                                for key in ('HOME', 'TMPDIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME',
                                            'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR')))
            self.assertTrue({'SWAYSOCK', 'LD_PRELOAD'}.isdisjoint(env))
        self.assertEqual(dict(os.environ), before)
        self.assertFalse(owned.root.exists())

    def test_exception_reaps_only_owned_process_and_removes_private_paths(self):
        unrelated = subprocess.Popen([sys.executable, '-c', 'import signal; signal.pause()'])
        self.addCleanup(lambda: (unrelated.terminate(), unrelated.wait()))
        with self.assertRaisesRegex(RuntimeError, 'fixture failure'):
            with self.owned() as owned:
                process, _ = owned.spawn([sys.executable, '-c', 'import signal; signal.pause()'],
                                         env=dict(os.environ), log=owned.root / 'process.log')
                (owned.root / 'owned-output').write_bytes(b'private')
                raise RuntimeError('fixture failure')
        self.assertIsNotNone(process.returncode)
        self.assertIsNone(unrelated.poll())
        self.assertFalse(owned.root.exists())

    def test_long_tmpdir_still_uses_short_owned_runtime(self):
        # The default base, not TMPDIR, keeps runtime socket paths short.
        with patch.dict(os.environ, {'TMPDIR': '/does/not/exist/' + 'long' * 60}):
            with OwnedResources() as owned:
                env = private_environment(owned.root / 'home')
                self.assertLess(len(env['XDG_RUNTIME_DIR'] + '/branch-truce/bt-mt-' + 'f' * 32), 107)

    def test_replay_lock_excludes_other_processes_and_releases(self):
        lock = self.base / 'replay.lock'
        script = ('import sys\nfrom collection import replay_lock\n'
                  'with replay_lock(sys.argv[1]):\n print("acquired")\n')
        env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[2] / 'tools/native-replay'))
        with replay_lock(lock):
            blocked = subprocess.run([sys.executable, '-B', '-c', script, str(lock)], env=env, capture_output=True)
            self.assertNotEqual(blocked.returncode, 0)
            self.assertIn(b'BlockingIOError', blocked.stderr)
        released = subprocess.run([sys.executable, '-B', '-c', script, str(lock)], env=env, capture_output=True)
        self.assertEqual(released.returncode, 0)
        self.assertEqual(released.stdout, b'acquired\n')


class ReportTests(PrivateBaseTestCase):
    def test_report_is_replaced_whole_and_leaves_no_partial_file(self):
        path = self.base / 'verification.json'
        reports.write_json(path, dict(first=True))
        reports.write_json(path, dict(second=[1, 2]))
        self.assertEqual(path.read_text(), '{\n  "second": [\n    1,\n    2\n  ]\n}\n')
        self.assertEqual(sorted(p.name for p in self.base.iterdir()), ['verification.json'])

    def test_concurrent_reports_use_distinct_temporaries_and_keep_modes(self):
        path = self.base / 'shared.txt'
        path.write_text('previous')
        path.chmod(0o640)
        barrier = threading.Barrier(2)
        replace = os.replace
        partials, errors = [], []
        def publish(partial, destination):
            partials.append(partial)
            barrier.wait(timeout=5)
            replace(partial, destination)
        def writer(value):
            try:
                reports.write_text(path, value)
            except BaseException as error:
                errors.append(error)
        with patch('reports.os.replace', side_effect=publish):
            workers = [threading.Thread(target=writer, args=(value,)) for value in ('a' * 10000, 'b' * 10000)]
            for worker in workers:
                worker.start()
            for worker in workers:
                worker.join(timeout=10)
        self.assertEqual((errors, any(worker.is_alive() for worker in workers)), ([], False))
        self.assertEqual(len(set(partials)), 2)
        self.assertIn(path.read_text(), ('a' * 10000, 'b' * 10000))
        self.assertEqual(path.stat().st_mode & 0o777, 0o640)
        self.assertEqual(list(self.base.iterdir()), [path])

    def test_temporary_collision_preserves_the_other_writers_file(self):
        partial = self.base / '.report.occupied.partial'
        partial.write_text('another writer')
        with patch('reports.secrets.token_hex', return_value='occupied'), self.assertRaises(FileExistsError):
            reports.write_text(self.base / 'report', 'mine')
        self.assertEqual(partial.read_text(), 'another writer')

    def test_failed_write_keeps_the_previous_report(self):
        path = self.base / 'ci-context.json'
        reports.write_text(path, 'previous\n')
        with patch('reports.os.replace', side_effect=OSError('disk full')), self.assertRaisesRegex(OSError, 'disk full'):
            reports.write_text(path, 'next\n')
        self.assertEqual(path.read_text(), 'previous\n')
        self.assertEqual(sorted(p.name for p in self.base.iterdir()), ['ci-context.json'])


class CaptureProfileTests(unittest.TestCase):
    def test_run_shape_comes_from_the_capture_profile(self):
        shape = capture_profile.run_shape(capture_profile.load())
        self.assertEqual(shape.schemes, {'light': 'BreezeLight', 'dark': 'BreezeDark'})
        self.assertEqual(shape.sizes, ((1280, 720), (1280, 800)))
        self.assertEqual((shape.heights, shape.size(800)), ((720, 800), (1280, 800)))
        self.assertEqual((shape.repeat, list(shape.kdeglobals_fonts)), (2, ['font', 'fixed']))
        with self.assertRaisesRegex(ValueError, '900 is not a logical height'):
            shape.size(900)

    def test_malformed_run_shape_is_rejected(self):
        profile = capture_profile.load()
        for change, message in [(lambda p: p.update(clean_replays=1), 'clean_replays'),
                                (lambda p: p['environment'].update(colour_schemes={}), 'colour_schemes'),
                                (lambda p: p['environment'].update(logical_sizes=[[1280, 0]]), 'logical_sizes'),
                                (lambda p: p['environment'].update(logical_sizes=[[1280, 720], [1440, 720]]),
                                 'repeat a height')]:
            candidate = json.loads(json.dumps(profile))
            change(candidate)
            with self.subTest(message=message), self.assertRaisesRegex(ValueError, message):
                capture_profile.run_shape(candidate)


class ReadinessTests(PrivateBaseTestCase):
    def test_waits_for_actual_compositor_publication(self):
        with self.owned() as owned:
            process, _ = owned.spawn([sys.executable, '-c',
                'import sys; print("not ready"); print("Running compositor on wayland display \'private-wayland\'", flush=True)'],
                env=dict(os.environ), log=owned.root / 'log', pipe=True)
            log = io.BytesIO()
            self.assertEqual(await_compositor(process, log), 'private-wayland')
            self.assertIn(b'not ready', log.getvalue())

    def test_compositor_early_exit_is_failure(self):
        with self.owned() as owned:
            process, _ = owned.spawn([sys.executable, '-c', 'print("failed startup")'],
                                    env=dict(os.environ), log=owned.root / 'log', pipe=True)
            with self.assertRaisesRegex(RuntimeError, 'before publishing'):
                await_compositor(process, io.BytesIO())

    def test_compositor_timeout_is_failure_and_still_cleans_up(self):
        with self.owned() as owned:
            process, _ = owned.spawn([sys.executable, '-c', 'import signal; signal.pause()'],
                                    env=dict(os.environ), log=owned.root / 'log', pipe=True)
            with self.assertRaises(TimeoutError):
                await_compositor(process, io.BytesIO(), timeout=0)
        self.assertFalse(owned.root.exists())
        self.assertIsNotNone(process.returncode)

    def test_image_helpers_enforce_deadlines_and_output_limits(self):
        from calibration import ImageTool
        tool_path = self.base / 'image-helper'
        tool_path.write_text(f'#!{sys.executable}\nimport time\ntime.sleep(60)\n')
        tool_path.chmod(0o700)
        tool = ImageTool(tool_path)
        bounded = lambda argv, **kw: run(argv, **dict(kw, timeout=0.1))
        for operation in (lambda: tool.decode('unused'), lambda: tool.crop('unused', 'unused', [0, 0, 1, 1])):
            with patch('calibration.run', side_effect=bounded), self.assertRaises(subprocess.TimeoutExpired):
                operation()
        tool_path.write_text(f'#!{sys.executable}\nimport os\nos.write(1, b"x" * (2 * 1024 * 1024))\n')
        with self.assertRaises(OutputLimitError):
            tool.decode('unused')

    def test_native_reply_requires_complete_successful_message(self):
        for response, exception in [(b'{"error":"not durable"}\n', RuntimeError),
                                    (b'{"error":""}', RuntimeError),
                                    (b'{"error":"","state":{"ready":true}}\n', None)]:
            with self.client_reply(response) as (client, requests):
                if exception:
                    with self.assertRaises(exception):
                        client.command(path='src/net/retry.cpp')
                else:
                    self.assertEqual(client.command(path='src/net/retry.cpp'), {'ready': True})
            self.assertEqual(requests[0]['path'], 'src/net/retry.cpp')


class AcceptanceEvidenceTests(PrivateBaseTestCase):
    def test_control_lookup_rejects_hidden_measurement_duplicates(self):
        visible = dict(name='saveDraft', onScreen=True, enabled=True)
        hidden = dict(name='saveDraft', onScreen=False, enabled=True)
        self.assertEqual(item(dict(items=[hidden, visible]), 'saveDraft'), visible)
        with self.assertRaisesRegex(RuntimeError, 'expected one reachable control'):
            item(dict(items=[hidden]), 'saveDraft')
        with self.assertRaisesRegex(RuntimeError, 'expected one reachable control'):
            item(dict(items=[visible, visible]), 'saveDraft')

    def test_line_cues_reject_missing_duplicate_and_filled_ignored_ranges(self):
        glyph = dict(line=20, kind='ignored', ancestors=['stage3GutterMarkerSlot'])
        fill = dict(glyph, ancestors=['stage3BackgroundsLines'])
        for details, valid in [([glyph], True), ([], False), ([glyph, glyph], False), ([glyph, fill], False)]:
            with self.subTest(details=details), nullcontext() if valid else self.assertRaises(RuntimeError):
                verify_line_cue(dict(renderedDetails=details), 'stage3', 20, 'ignored', fill=False)

    def test_artifact_collection_keeps_originals_metadata_crops_and_residuals(self):
        profile = capture_profile.load()
        state = dict(width=1280, height=720, profile=dict(profile['pinned_observations'], fonts=profile['pinned_fonts'],
            configuredColourScheme='BreezeLight', palette=profile['pinned_palettes']['BreezeLight']), items=[
            dict(name='Page', x=0, y=48, width=1280, height=641),
            dict(name='fileList', x=-0.5, y=48, width=252, height=641),
            dict(name='fileToolBar', x=253, y=48, width=1027, height=34),
            dict(name='fileFacts', x=1100, y=698, width=180, height=15, text='fixture')])
        context = dict(size=[1280, 720], theme='light', capture_environment=dict(output=dict(scale=1), colour_scheme='BreezeLight'))
        tool = Mock()
        tool.decode.side_effect = [dict(width=1280, height=720, decoded_sha256='decoded'), dict(width=1920, height=1080)]
        tool.artifacts.return_value = dict(differing_pixels=3)
        record = collect_shot(state, context, 'binary', shoot=lambda path: path.write_bytes(b'original'), tool=tool,
                              source=Path(__file__).resolve().parents[2], out=self.base)
        self.assertEqual(Path(record['file']).read_bytes(), b'original')
        self.assertEqual(json.loads((self.base / 'light-binary-state.json').read_text()), dict(state=state, metadata={
            key: value for key, value in record.items() if key != 'file'}))
        tool.crop.assert_any_call(Path(record['file']), self.base / 'light-binary-callouts/fileList.png', [0, 48, 252, 641])
        comparison = json.loads((self.base / 'light-binary-comparison/comparison.json').read_text())
        self.assertTrue(comparison['regions'] and all(row['status'] == 'outstanding' and row['differing_pixels'] == 3
                                                      for row in comparison['regions'].values()))

    def test_failed_native_action_retains_request_and_full_reply(self):
        response = dict(error='control clipped', state=dict(width=1280, height=720))
        with self.client_reply(json.dumps(response).encode() + b'\n', self.base) as (client, _):
            with self.assertRaisesRegex(RuntimeError, 'control clipped'):
                client.command('toolbar-click', target='undoCommand')
        self.assertEqual(json.loads((self.base / 'last-reply.json').read_text()), response)
        request = json.loads((self.base / 'last-request.json').read_text())
        self.assertEqual((request['action'], request['target']), ('toolbar-click', 'undoCommand'))

    def test_failure_capture_timeout_keeps_the_original_failure_receipt(self):
        def shoot(path, *, check):
            raise subprocess.TimeoutExpired(['grim'], 15)
        with self.owned() as owned, self.assertRaisesRegex(RuntimeError, 'primary action failure'):
            with application(owned, dict(os.environ), app=Path('/bin/true'), driver=self.base / 'unused.so',
                             repo=self.base, out=self.base, shoot=shoot):
                raise RuntimeError('primary action failure')
        evidence = json.loads((self.base / 'failure.json').read_text())
        self.assertEqual(evidence['error'], 'primary action failure')
        self.assertIsNone(evidence['capture_exit'])
        self.assertIn('timed out', evidence['capture_error'])

    def test_ledger_keeps_missing_runs_and_measured_residuals_outstanding(self):
        comparison = self.base / '720/replay-1-light/light-text-comparison'
        comparison.mkdir(parents=True)
        (comparison / 'comparison.json').write_text(json.dumps(dict(regions=dict(
            syntax=dict(status='outstanding', differing_pixels=12, native_rect=[0, 0, 4, 3],
                        reference_logical_rect=[0, 0, 4, 3], count_unit='common-grid cells')))))
        ledger = assemble(self.base)
        self.assertFalse(ledger['originals_inspected'])
        self.assertTrue(all(row['outcome'] == 'pending-independent-inspection' for row in ledger['callouts']))
        row = next(row for row in ledger['callouts'] if row['behavior'] == 'passage-kinds')
        self.assertEqual(row['residuals'][0]['differing_pixels'], 12)
        self.assertEqual(len(row['missing_evidence']), 2)
        self.assertIsNone(row['original_inspection'])

    def write_verification(self, root, height, repeat, captures=None, **changes):
        themes, kinds = ['light', 'dark'], ['text', 'binary']
        shots = [dict(theme=t, kind=k) for t in themes for k in kinds]
        report = dict(themes=themes, kinds=kinds, repeat=repeat, cleanup_confirmed=True,
                      captures=shots * repeat if captures is None else shots[:1] * captures,
                      comparisons=[dict(differing_pixels=0)] * (len(shots) * (repeat - 1)))
        report.update(changes)
        (root / str(height)).mkdir(parents=True, exist_ok=True)
        (root / str(height) / 'verification.json').write_text(json.dumps(report))

    def test_ledger_expects_every_theme_kind_and_repeat_the_report_records(self):
        self.write_verification(self.base, 720, 3)
        self.write_verification(self.base, 800, 2)
        self.assertTrue(all(row['missing_evidence'] == [] for row in assemble(self.base)['callouts']
                            if row['calibrated_regions']))
        # Three repeats need twelve shots and eight comparisons; eight and four are not enough.
        self.write_verification(self.base, 720, 3, captures=8, comparisons=[dict(differing_pixels=0)] * 4)
        self.write_verification(self.base, 800, 2, themes=['light'])
        missing = next(row for row in assemble(self.base)['callouts'] if row['calibrated_regions'])['missing_evidence']
        self.assertEqual(missing, ['720: successful complete replay receipt unavailable',
                                   '800: successful complete replay receipt unavailable'])

    def test_error_cleanup_publishes_only_actual_owned_disposition(self):
        receipt = self.base / 'cleanup.json'
        with self.assertRaisesRegex(RuntimeError, 'interrupted replay'):
            with self.owned(receipt) as owned:
                private = owned.root
                raise RuntimeError('interrupted replay')
        evidence = json.loads(receipt.read_text())
        self.assertFalse(private.exists())
        self.assertEqual(evidence, dict(private_root=str(private), private_root_removed=True,
                                        owned_processes_reaped=True, owned_readers_stopped=True, errors=[]))

    def test_cleanup_is_confirmed_only_by_complete_receipts(self):
        receipts = []
        for name in ('one', 'two'):
            receipts.append(self.base / f'{name}.json')
            with self.owned(receipts[-1]):
                pass
        self.assertTrue(cleanup_confirmed(receipts))
        self.assertFalse(cleanup_confirmed([]))
        self.assertFalse(cleanup_confirmed(receipts + [self.base / 'missing.json']))
        for change in (dict(owned_processes_reaped=False), dict(private_root_removed=False),
                       dict(owned_readers_stopped=False), dict(errors=['reader failed']),
                       dict(private_root=str(self.base))):
            with self.subTest(change=change):
                broken = self.base / 'broken.json'
                broken.write_text(json.dumps(dict(json.loads(receipts[0].read_text()), **change)))
                self.assertFalse(cleanup_confirmed(receipts + [broken]))


if __name__ == '__main__':
    unittest.main(verbosity=2)
