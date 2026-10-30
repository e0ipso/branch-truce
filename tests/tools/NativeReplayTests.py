"""Fixture integrity, readiness protocol, serialization and resource ownership."""
import io
import json
import os
from pathlib import Path
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import unittest
from unittest.mock import patch
import zlib

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'tools/native-replay'))
from fixture import PATHS, git, make_fixture, png, reference_bytes, unmerged
from replay import NativeClient, OwnedResources, await_compositor, private_environment, replay_lock


class FixtureTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)

    def test_exact_source_and_result_bytes(self):
        fixture = self.root / 'fixture with $ and spaces'
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
        first = make_fixture(self.root / 'one')
        second = make_fixture(self.root / 'two')
        self.assertEqual(first, second)
        repo = self.root / 'one/retry-service'
        for branch, key in [('main', 'main'), ('feature/backoff', 'incoming')]:
            self.assertEqual(git(repo, 'rev-parse', branch).stdout.decode().strip(), first[key])
            self.assertEqual(git(repo, 'rev-parse', branch + '^').stdout.decode().strip(), first['base'])

    def test_png_chunks_crc_dimensions_and_decoded_pixels(self):
        for colour in [(100, 100, 100), (61, 174, 233), (39, 174, 96)]:
            image = png(colour)
            self.assertEqual(len(image), 18637)
            self.assertEqual(image[:8], b'\x89PNG\r\n\x1a\n')
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
        source = self.root / 'tool source with spaces'
        original = Path(__file__).resolve().parents[2] / 'tools/native-replay'
        shutil.copytree(original, source, ignore=shutil.ignore_patterns('__pycache__'))
        before = {str(p.relative_to(source)): p.read_bytes() for p in source.rglob('*') if p.is_file()}
        env = {k: v for k, v in os.environ.items() if k != 'PYTHONDONTWRITEBYTECODE'}
        run = subprocess.run([sys.executable, str(source / 'replay.py'), 'fixture',
                              '--output', str(self.root / 'new fixture')],
                             cwd='/', env=env, capture_output=True)
        self.assertEqual(run.returncode, 0, run.stderr)
        after = {str(p.relative_to(source)): p.read_bytes() for p in source.rglob('*') if p.is_file()}
        self.assertEqual(before, after)
        self.assertFalse(list(source.rglob('__pycache__')))

    def test_existing_output_is_never_reused_or_removed(self):
        sentinel = self.root / 'sentinel'
        sentinel.write_bytes(b'untouched')
        with self.assertRaises(FileExistsError):
            make_fixture(self.root)
        self.assertEqual(sentinel.read_bytes(), b'untouched')

    def test_invoking_git_settings_and_repository_are_ignored(self):
        sentinel = self.root / 'sentinel'
        sentinel.write_bytes(b'untouched')
        attrs = self.root / 'git/attributes'
        attrs.parent.mkdir()
        attrs.write_text('*.cpp merge=union\n')
        config = self.root / 'config'
        config.write_text('[init]\n defaultBranch = hostile\n')
        env = dict(GIT_DIR=str(self.root / 'unrelated.git'), GIT_WORK_TREE=str(self.root),
                   GIT_CONFIG_GLOBAL=str(config), GIT_CONFIG_COUNT='1',
                   GIT_CONFIG_KEY_0='core.hooksPath', GIT_CONFIG_VALUE_0=str(self.root),
                   XDG_CONFIG_HOME=str(self.root))
        with patch.dict(os.environ, env):
            make_fixture(self.root / 'fixture')
            self.assertEqual(os.environ['GIT_DIR'], env['GIT_DIR'])
        self.assertEqual(config.read_text(), '[init]\n defaultBranch = hostile\n')
        self.assertEqual(sentinel.read_bytes(), b'untouched')
        self.assertFalse((self.root / 'unrelated.git').exists())


class ResourceTests(unittest.TestCase):
    def test_private_environment_does_not_modify_parent(self):
        before = dict(os.environ)
        with OwnedResources() as owned:
            env = private_environment(owned.root / 'home')
            self.assertEqual(Path(env['XDG_RUNTIME_DIR']).stat().st_mode & 0o777, 0o700)
            self.assertTrue(all(Path(env[key]).is_relative_to(owned.root)
                                for key in ('HOME', 'TMPDIR', 'XDG_CONFIG_HOME', 'XDG_DATA_HOME',
                                            'XDG_CACHE_HOME', 'XDG_RUNTIME_DIR')))
            self.assertNotIn('SWAYSOCK', env)
            self.assertNotIn('LD_PRELOAD', env)
        self.assertEqual(dict(os.environ), before)
        self.assertFalse(owned.root.exists())

    def test_exception_reaps_only_owned_process_and_removes_private_paths(self):
        unrelated = subprocess.Popen([sys.executable, '-c', 'import signal; signal.pause()'])
        self.addCleanup(lambda: (unrelated.terminate(), unrelated.wait()))
        with self.assertRaisesRegex(RuntimeError, 'fixture failure'):
            with OwnedResources() as owned:
                process, _ = owned.spawn([sys.executable, '-c', 'import signal; signal.pause()'],
                                         env=dict(os.environ), log=owned.root / 'process.log')
                (owned.root / 'owned-output').write_bytes(b'private')
                raise RuntimeError('fixture failure')
        self.assertIsNotNone(process.returncode)
        self.assertIsNone(unrelated.poll())
        self.assertFalse(owned.root.exists())

    def test_long_tmpdir_still_uses_short_owned_runtime(self):
        with patch.dict(os.environ, {'TMPDIR': '/does/not/exist/' + 'long' * 60}):
            with OwnedResources() as owned:
                env = private_environment(owned.root / 'home')
                self.assertLess(len(env['XDG_RUNTIME_DIR'] + '/branch-truce/bt-mt-' + 'f' * 32), 107)

    def test_replay_lock_excludes_other_processes_and_releases(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        lock = Path(directory.name) / 'replay.lock'
        script = ('import sys\nfrom replay import replay_lock\n'
                  'with replay_lock(sys.argv[1]):\n print("acquired")\n')
        env = dict(os.environ, PYTHONPATH=str(Path(__file__).resolve().parents[2] / 'tools/native-replay'))
        with replay_lock(lock):
            blocked = subprocess.run([sys.executable, '-B', '-c', script, str(lock)], env=env, capture_output=True)
            self.assertNotEqual(blocked.returncode, 0)
            self.assertIn(b'BlockingIOError', blocked.stderr)
        released = subprocess.run([sys.executable, '-B', '-c', script, str(lock)], env=env, capture_output=True)
        self.assertEqual(released.returncode, 0)
        self.assertEqual(released.stdout, b'acquired\n')


class ReadinessTests(unittest.TestCase):
    def test_waits_for_actual_compositor_publication(self):
        with OwnedResources() as owned:
            process, _ = owned.spawn([sys.executable, '-c',
                'import sys; print("not ready"); print("Running compositor on wayland display \'private-wayland\'", flush=True)'],
                env=dict(os.environ), log=owned.root / 'log', pipe=True)
            log = io.BytesIO()
            self.assertEqual(await_compositor(process, log), 'private-wayland')
            self.assertIn(b'not ready', log.getvalue())

    def test_compositor_early_exit_is_failure(self):
        with OwnedResources() as owned:
            process, _ = owned.spawn([sys.executable, '-c', 'print("failed startup")'],
                                    env=dict(os.environ), log=owned.root / 'log', pipe=True)
            with self.assertRaisesRegex(RuntimeError, 'before publishing'):
                await_compositor(process, io.BytesIO())

    def test_compositor_timeout_is_failure_and_still_cleans_up(self):
        with OwnedResources() as owned:
            process, _ = owned.spawn([sys.executable, '-c', 'import signal; signal.pause()'],
                                    env=dict(os.environ), log=owned.root / 'log', pipe=True)
            with self.assertRaises(TimeoutError):
                await_compositor(process, io.BytesIO(), timeout=0)
        self.assertFalse(owned.root.exists())
        self.assertIsNotNone(process.returncode)

    def test_native_reply_requires_complete_successful_message(self):
        for response, exception in [(b'{"error":"not durable"}\n', RuntimeError),
                                    (b'{"error":""}', RuntimeError),
                                    (b'{"error":"","state":{"ready":true}}\n', None)]:
            parent, child = socket.socketpair()
            client = NativeClient(parent)
            def responder():
                with child, child.makefile('rb') as reader:
                    request = json.loads(reader.readline())
                    self.assertEqual(request['path'], 'src/net/retry.cpp')
                    child.sendall(response)
            worker = threading.Thread(target=responder)
            worker.start()
            try:
                if exception:
                    with self.assertRaises(exception):
                        client.command(path='src/net/retry.cpp')
                else:
                    self.assertEqual(client.command(path='src/net/retry.cpp'), {'ready': True})
            finally:
                client.close()
                worker.join(timeout=5)
            self.assertFalse(worker.is_alive())


if __name__ == '__main__':
    unittest.main(verbosity=2)
