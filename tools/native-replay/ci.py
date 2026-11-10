#!/usr/bin/env python3
"""Collect every native size in capture-profile.json and keep failures and pending acceptance visible."""
import argparse
from datetime import datetime, timezone
import os
from pathlib import Path
import subprocess
import shutil
import signal
import sys

if __name__ == '__main__':
    # Keep the source checkout read only when run as a script, including without -B.
    sys.dont_write_bytecode = True

import capture_profile  # noqa: E402
from fixture import sha256  # noqa: E402
from ledger import assemble  # noqa: E402
from processes import interrupt, run  # noqa: E402
from reports import write_json  # noqa: E402


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    # The replay that runs is the one in --source, so its profile sets the run shape.
    shape = capture_profile.run_shape(capture_profile.load(args.source / 'tools/native-replay/capture-profile.json'))
    heights = shape.heights
    args.output.mkdir(exist_ok=False)
    (args.output / 'inputs').mkdir()
    (args.output / 'executables').mkdir()
    retained = {}
    for source, destination in [
        (args.build / 'src/app/branch-truce', args.output / 'executables/branch-truce'),
        (args.source / 'tools/native-replay/capture-profile.json', args.output / 'inputs/capture-profile.json'),
        (args.source / 'tools/native-replay/reference-contract.json', args.output / 'inputs/reference-contract.json'),
        (args.source / 'tools/native-replay/reference/provenance.json', args.output / 'inputs/fixture-provenance.json'),
        (args.source / 'tools/native-replay/fidelity-ledger.json', args.output / 'inputs/fidelity-ledger.json')]:
        shutil.copy2(source, destination)
        retained[destination.relative_to(args.output).as_posix()] = sha256(destination)
    context = dict(
        started_at_utc=datetime.now(timezone.utc).isoformat(),
        source=str(args.source), build=str(args.build),
        source_head_observed=run(['git', '-C', str(args.source), 'rev-parse', 'HEAD'],
                                 timeout=10, max_output=65536, check=True).stdout.decode().strip(),
        os_release=Path('/etc/os-release').read_text(),
        ci={key: os.environ.get(key) for key in
            ('BRANCH_TRUCE_CI_RUN_URL', 'BRANCH_TRUCE_CI_RUN_ATTEMPT', 'BRANCH_TRUCE_CI_JOB',
             'BRANCH_TRUCE_CI_FORMAT', 'BRANCH_TRUCE_CI_SOURCE_REVISION')},
        physical_desktop=False, retained_artifact_sha256=retained, checks=[])
    receipt = args.output / 'ci-context.json'

    def save():
        write_json(receipt, context)

    def invoke(command, log, timeout):
        try:
            # Replay stops its own app/compositor groups and readers before it can exit.
            result = run(command, cwd=args.source, timeout=timeout, max_output=16 * 1024 * 1024,
                         log=args.output / log, cleanup_grace=60)
            status = result.returncode
        except (OSError, subprocess.SubprocessError) as error:
            with (args.output / log).open('ab') as stream:
                stream.write(f'\n{type(error).__name__}: {error}\n'.encode())
            status = 1
        context['checks'].append(dict(argv=command, cwd=str(args.source), exit_code=status, log=log))
        save()
        return status

    save()
    for height in heights:
        command = [sys.executable, str(args.source / 'tools/native-replay/replay.py'), 'capture',
                   '--source', str(args.source), '--build', str(args.build),
                   '--height', str(height), '--repeat', str(shape.repeat), '--output', str(args.output / str(height))]
        if height == heights[0]:
            command.append('--documentation')
        status = invoke(command, f'{height}.log', timeout=1800)
        for filename in ('libnative_replay.so', 'native-replay-image'):
            source = args.output / str(height) / 'driver-build' / filename
            if source.is_file():
                destination = args.output / 'executables' / f'{height}-{filename}'
                shutil.copy2(source, destination)
                retained[destination.relative_to(args.output).as_posix()] = sha256(destination)
        save()
        # Verification is independent, so a failed capture cannot suppress the other size.
        if status == 0:
            command = [sys.executable, str(args.source / 'tools/native-replay/calibration.py'),
                       '--verify', str(args.output / str(height))]
            invoke(command, f'{height}-integrity.log', timeout=120)
    ledger = assemble(args.output, shape=shape)
    write_json(args.output / 'acceptance-ledger.json', ledger)
    context['finished_at_utc'] = datetime.now(timezone.utc).isoformat()
    context['acceptance'] = 'pending original-image and per-callout independent inspection'
    save()
    for check in context['checks']:
        print(f'exit {check["exit_code"]}: {check["argv"]}; log {args.output / check["log"]}', flush=True)
    return 1 if any(check['exit_code'] for check in context['checks']) else 0


if __name__ == '__main__':
    signal.signal(signal.SIGTERM, interrupt)
    raise SystemExit(main())
