#!/usr/bin/env python3
"""Attach available native evidence to the pending per-callout acceptance ledger."""
import argparse
from copy import deepcopy
import json
from pathlib import Path
import sys

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
TEMPLATE = HERE.parents[1] / 'tests/acceptance/fidelity-ledger.json'


def assemble(root, template=None):
    root = Path(root)
    ledger = deepcopy(template if template is not None else json.loads(TEMPLATE.read_text()))
    files = sorted(path.relative_to(root).as_posix() for path in root.rglob('*') if path.is_file()
                   and 'driver-build' not in path.relative_to(root).parts)
    ledger['evidence_root'] = str(root)
    ledger['available_artifacts'] = files
    ledger['originals_inspected'] = False
    ledger['status'] = 'pending-independent-inspection'
    # Preserve every measured residual. Even zero pixel differences cannot approve
    # semantic, interaction or physical-desktop obligations.
    residuals = []
    for path in root.rglob('comparison.json'):
        comparison = json.loads(path.read_text())
        for name, region in comparison.get('regions', {}).items():
            residuals.append(dict(report=path.relative_to(root).as_posix(), region=name,
                                  status=region['status'], differing_pixels=region['differing_pixels'],
                                  native_rect=region['native_rect'],
                                  reference_logical_rect=region['reference_logical_rect'],
                                  count_unit=region['count_unit']))
    ledger['measured_regions'] = residuals
    complete_runs = {}
    for height in (720, 800):
        report = root / str(height) / 'verification.json'
        if not report.is_file():
            complete_runs[height] = False
            continue
        replay = json.loads(report.read_text())
        complete_runs[height] = (replay.get('cleanup_confirmed') is True
            and len(replay.get('captures', [])) >= 8
            and len(replay.get('comparisons', [])) >= 4
            and all(row['differing_pixels'] == 0 for row in replay['comparisons']))
    for callout in ledger['callouts']:
        callout['outcome'] = 'pending-independent-inspection'
        callout['original_inspection'] = None
        callout['evidence'] = [name for name in files if name.endswith(
            ('.png', '-state.json', 'verification.json', 'provenance.json', 'actions.jsonl', 'last-reply.json', 'callout-regions.json'))]
        callout['residuals'] = [row for row in residuals if row['region'] in callout['calibrated_regions']
                                and row['status'] != 'identical']
        callout['missing_evidence'] = []
        for height in (720, 800):
            if not complete_runs[height]:
                callout['missing_evidence'].append(f'{height}: successful complete replay receipt unavailable')
        if not callout['calibrated_regions']:
            callout['missing_evidence'].append('shared-component inspection; no invented PNG target')
    ledger['runtime_outcomes'] = 'read replay/CTest receipts; evidence collection does not approve callouts'
    ledger['physical_desktop'] = 'unavailable: private headless sway is not Plasma/KWin/AT-SPI validation'
    ledger['installed_documentation_evidence'] = (
        'pending: attach current per-target installed validation and inspect fresh documentation PNGs; '
        'development replay and package lifecycle logs do not substitute for installed UI observations')
    return ledger


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--captures', required=True, type=Path)
    parser.add_argument('--output', required=True, type=Path)
    args = parser.parse_args()
    with args.output.open('x') as stream:
        json.dump(assemble(args.captures), stream, indent=2)
        stream.write('\n')


if __name__ == '__main__':
    main()
