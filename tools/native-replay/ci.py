#!/usr/bin/env python3
"""Collect both required native sizes and keep failures and pending acceptance visible."""
import argparse
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import subprocess
import shutil
import sys

sys.dont_write_bytecode = True
from ledger import assemble
from fixture import sha256


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('source', 'build', 'output'):
        parser.add_argument('--' + name, required=True, type=Path)
    args = parser.parse_args()
    args.output.mkdir(exist_ok=False)
    (args.output / 'inputs').mkdir()
    (args.output / 'executables').mkdir()
    retained = {}
    for source, destination in [
        (args.build / 'src/app/branch-truce', args.output / 'executables/branch-truce'),
        (args.source / 'tools/native-replay/capture-profile.json', args.output / 'inputs/capture-profile.json'),
        (args.source / 'tools/native-replay/reference-contract.json', args.output / 'inputs/reference-contract.json'),
        (args.source / 'tools/native-replay/reference/provenance.json', args.output / 'inputs/fixture-provenance.json'),
        (args.source / 'tests/acceptance/fidelity-ledger.json', args.output / 'inputs/fidelity-ledger.json')]:
        shutil.copy2(source, destination)
        retained[destination.relative_to(args.output).as_posix()] = sha256(destination)
    context = dict(
        started_at_utc=datetime.now(timezone.utc).isoformat(),
        source=str(args.source), build=str(args.build),
        source_head_observed=subprocess.check_output(
            ['git', '-C', str(args.source), 'rev-parse', 'HEAD'], text=True).strip(),
        os_release=Path('/etc/os-release').read_text(),
        ci={key: os.environ.get(key) for key in
            ('BRANCH_TRUCE_CI_RUN_URL', 'BRANCH_TRUCE_CI_RUN_ATTEMPT', 'BRANCH_TRUCE_CI_JOB',
             'BRANCH_TRUCE_CI_FORMAT', 'BRANCH_TRUCE_CI_SOURCE_REVISION')},
        physical_desktop=False, retained_artifact_sha256=retained, checks=[])
    receipt = args.output / 'ci-context.json'

    def save():
        receipt.write_text(json.dumps(context, indent=2) + '\n')

    save()
    for height in (720, 800):
        command = [sys.executable, str(args.source / 'tools/native-replay/replay.py'), 'capture',
                   '--source', str(args.source), '--build', str(args.build),
                   '--height', str(height), '--repeat', '2', '--output', str(args.output / str(height))]
        if height == 720:
            command.append('--documentation')
        log = args.output / f'{height}.log'
        with log.open('wb') as stream:
            result = subprocess.run(command, cwd=args.source, stdout=stream, stderr=subprocess.STDOUT)
        context['checks'].append(dict(argv=command, cwd=str(args.source), exit_code=result.returncode,
                                      log=log.name))
        for filename in ('libnative_replay.so', 'native-replay-image'):
            source = args.output / str(height) / 'driver-build' / filename
            if source.is_file():
                destination = args.output / 'executables' / f'{height}-{filename}'
                shutil.copy2(source, destination)
                retained[destination.relative_to(args.output).as_posix()] = sha256(destination)
        save()
        # Verification is independent, so a failed capture cannot suppress the other size.
        if result.returncode == 0:
            command = [sys.executable, str(args.source / 'tools/native-replay/calibration.py'),
                       '--verify', str(args.output / str(height))]
            with (args.output / f'{height}-integrity.log').open('wb') as stream:
                checked = subprocess.run(command, cwd=args.source, stdout=stream, stderr=subprocess.STDOUT)
            context['checks'].append(dict(argv=command, cwd=str(args.source), exit_code=checked.returncode,
                                          log=f'{height}-integrity.log'))
            save()
    image_tool = args.output / '720/driver-build/native-replay-image'
    if image_tool.is_file():
        command = [sys.executable, str(args.source / 'tests/tools/NativeCalibrationTests.py'),
                   '--image-tool', str(image_tool)]
        with (args.output / 'calibration-tests.log').open('wb') as stream:
            checked = subprocess.run(command, cwd=args.source, stdout=stream, stderr=subprocess.STDOUT)
        context['checks'].append(dict(argv=command, cwd=str(args.source), exit_code=checked.returncode,
                                      log='calibration-tests.log'))
        save()
    ledger = assemble(args.output)
    (args.output / 'acceptance-ledger.json').write_text(json.dumps(ledger, indent=2) + '\n')
    context['finished_at_utc'] = datetime.now(timezone.utc).isoformat()
    context['acceptance'] = 'pending original-image and per-callout independent inspection'
    save()
    for check in context['checks']:
        print(f'exit {check["exit_code"]}: {check["argv"]}; log {args.output / check["log"]}', flush=True)
    return 1 if any(check['exit_code'] for check in context['checks']) else 0


if __name__ == '__main__':
    raise SystemExit(main())
