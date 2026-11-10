"""Collect theme repeats, documentation previews and their verification receipts."""
from contextlib import contextmanager
import fcntl
from functools import partial
import os
from pathlib import Path
import time

from calibration import (ImageTool, build_provenance, capture_inputs, cleanup_confirmed,
                         collect_shot, colour_scheme, load_contract)
import capture_profile
from desktop import application, boot_compositor, screenshot, write_kdeglobals
from fixture import REFERENCE, make_fixture, sha256
from journey import SHOT_PATHS, capture_state, run_journey, show_binary
from processes import OwnedResources, run
from reports import write_json
from scene import require

HERE = Path(__file__).resolve().parent


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


def replay_once(*, app, driver, tool, source, out, theme, scheme, repeat, size, scheme_dir, fonts):
    """One clean replay: a new fixture, private home and compositor, the app and its shots."""
    with OwnedResources(out / 'cleanup.json') as owned:
        repo = owned.root / 'fixture/retry-service'
        fixture = make_fixture(repo.parent)
        write_json(out / 'fixture.json', fixture)
        env, output = boot_compositor(owned, size, out / 'compositor.log')
        kdeglobals = write_kdeglobals(env['XDG_CONFIG_HOME'], scheme, scheme_dir, fonts)
        context = dict(theme=theme, repeat=repeat, size=size, height=size[1], fixture=fixture,
            fixture_sha256=sha256(out / 'fixture.json'), executable_sha256=sha256(app), driver_sha256=sha256(driver),
            capture_environment=dict(colour_scheme=scheme, kdeglobals_sha256=sha256(kdeglobals),
                output=output, renderer='pixman', quick_backend='software',
                os_release=Path('/etc/os-release').read_text(), physical_desktop=False,
                scheme_roles=colour_scheme(kdeglobals)),
            reference_provenance_sha256=sha256(REFERENCE / 'provenance.json'),
            capture_profile_sha256=sha256(capture_profile.PROFILE),
            reference_contract_sha256=sha256(HERE / 'reference-contract.json'))
        shoot = partial(screenshot, env, output['name'])
        captures = []
        with application(owned, env, app=app, driver=driver, repo=repo, out=out, shoot=shoot) as client:
            run_journey(client, out)
            for kind in SHOT_PATHS:
                if kind == 'binary':
                    show_binary(client, out)
                captures.append(collect_shot(capture_state(client, kind, repo), context, kind,
                                             shoot=shoot, tool=tool, source=source, out=out))
    return captures


def documentation_preview(*, app, source, docs, size):
    """Run the README preview recipe for notes.md on its own private compositor."""
    with OwnedResources(docs / 'cleanup.json') as owned:
        env, _ = boot_compositor(owned, size, docs / 'compositor.log')
        preview_env = dict(env, PREVIEW_FILE='notes.md', PREVIEW_SIZE='1440x900',
                           PREVIEW_OUTPUT=str(docs))
        run([str(source / 'tools/preview-app.sh'), str(app)], env=preview_env,
            timeout=120, max_output=4 * 1024 * 1024, check=True, log=docs / 'preview.log')
    write_json(docs / 'provenance.json', dict(
        command=['tools/preview-app.sh', str(app)],
        environment={key: preview_env[key] for key in ('PREVIEW_FILE', 'PREVIEW_SIZE')},
        captured_at_utc=time.strftime('%Y-%m-%dT%H:%M:%SZ', time.gmtime()),
        executable_sha256=sha256(app),
        build_provenance='../build-provenance.json', physical_desktop=False,
        images={path.name: sha256(path) for path in docs.glob('*.png')},
        acceptance='pending original-image inspection; development build, not installed validation'))


def compare_repeats(records, tool, themes):
    """Compare each later repeat's decoded pixels with the first repeat of the same theme and kind."""
    comparisons = []
    for theme in themes:
        for kind in SHOT_PATHS:
            selected = [r for r in records if r['theme'] == theme and r['kind'] == kind]
            comparisons.extend(dict(theme=theme, kind=kind, repeat=row['repeat'],
                                    differing_pixels=tool.compare(selected[0]['file'], row['file'])['differing_pixels'])
                               for row in selected[1:])
    return comparisons


def capture(*, source, build, output, size, repeat, documentation, schemes, fonts, scheme_dir):
    """Replay and capture every theme repeat times at one size. Returns the verification.json report."""
    require((source / 'tools/native-replay').resolve() == HERE,
            '--source must name the checkout containing this replay tool')
    app = build / 'src/app/branch-truce'
    require(app.is_file(), '--build must contain src/app/branch-truce')
    require(repeat >= 2, 'Capture requires at least two clean replays for pixel comparison')
    load_contract(source)
    with replay_lock():
        output.mkdir(exist_ok=False)
        write_json(output / 'build-provenance.json', build_provenance(source, build))
        driver_build = output / 'driver-build'
        run(['cmake', '-S', str(HERE), '-B', str(driver_build)],
            timeout=120, max_output=4 * 1024 * 1024, check=True, log=driver_build.parent / 'driver-configure.log')
        run(['cmake', '--build', str(driver_build), '--parallel', '2'],
            timeout=300, max_output=4 * 1024 * 1024, check=True, log=driver_build.parent / 'driver-build.log')
        driver, tool = driver_build / 'libnative_replay.so', ImageTool(driver_build / 'native-replay-image')
        records, receipts = [], []
        for index in range(1, repeat + 1):
            for theme, scheme in schemes.items():
                out = output / f'replay-{index}-{theme}'
                out.mkdir()
                receipts.append(out / 'cleanup.json')
                records += replay_once(app=app, driver=driver, tool=tool, source=source, out=out, theme=theme,
                                       scheme=scheme, repeat=index, size=size, scheme_dir=scheme_dir, fonts=fonts)
        if documentation:
            docs = output / 'documentation-readme'
            docs.mkdir()
            receipts.append(docs / 'cleanup.json')
            documentation_preview(app=app, source=source, docs=docs, size=size)
        comparisons = compare_repeats(records, tool, schemes)
        cleaned = cleanup_confirmed(receipts)
        report = dict(captures=records, comparisons=comparisons,
            source=str(source), build=str(build), height=size[1],
            themes=list(schemes), kinds=list(SHOT_PATHS), repeat=repeat,
            environment=dict(platform='wayland', renderer='pixman', quick_backend='software'),
            cleanup_confirmed=cleaned, source_inputs=capture_inputs(source),
            artifact_hashes={str(p.relative_to(output)): sha256(p) for p in output.rglob('*')
                             if p.is_file() and 'driver-build' not in p.relative_to(output).parts})
        write_json(output / 'verification.json', report)
        require(all(row['differing_pixels'] == 0 for row in comparisons), 'Clean replays differ in decoded pixels')
        require(cleaned, 'A cleanup receipt does not confirm its private root removed and processes reaped')
    return report
