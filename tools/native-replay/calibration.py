"""Read-only provenance and explicit, unmasked native/reference diagnostics."""
import json
import sys
sys.dont_write_bytecode = True
import configparser
import math
from pathlib import Path
import shutil
import subprocess

from fixture import sha256

HERE = Path(__file__).resolve().parent
CONTRACT = HERE / 'reference-contract.json'
PROFILE = HERE / 'capture-profile.json'


def load_contract(source):
    contract = json.loads(CONTRACT.read_text())
    validate_contract(contract)
    for name, digest in contract['references'].items():
        if sha256(Path(source) / 'docs/ui-mockup' / name) != digest:
            raise ValueError(f'Approved reference changed: {name}')
    return contract


def validate_contract(contract):
    if contract['version'] != 1:
        raise ValueError('Unknown contract version')
    if (contract['reference_pixel_scale'] != 1.5 or contract['reference_logical_size'] != [1280, 720]
            or contract['reference_decoded_size'] != [1920, 1080]):
        raise ValueError('Reference raster geometry requires a reviewed comparison-grid change')
    regions = contract['regions']
    for name, rect in regions.items():
        if len(rect) != 4 or not all(type(n) is int for n in rect) or min(rect) < 0 or min(rect[2:]) == 0:
            raise ValueError(f'Invalid reference region: {name}')
        if rect[0]+rect[2] > 1280 or rect[1]+rect[3] > 720:
            raise ValueError(f'Reference region out of bounds: {name}')
    for exception in contract['exceptions']:
        for key in ('region', 'reference', 'reference_sha256', 'observed_native_value', 'reason', 'acceptance_rule', 'kind'):
            if not exception.get(key):
                raise ValueError(f'Exception missing {key}')
        if exception['region'] not in regions or exception['region'] == 'window':
            raise ValueError('Exceptions must name a narrow region')
        if exception['reference_sha256'] != contract['references'].get(exception['reference']):
            raise ValueError('Exception reference hash mismatch')
        allowed = {'truthful-identity': {'source_identity', 'incoming_identity'},
                   'truthful-metadata': {'footer_facts'}, 'renderer': {'glyph'},
                   'background-role-fill': {'accepted_fill'}}
        if exception['region'] not in allowed.get(exception['kind'], set()):
            raise ValueError('Exception region is too broad or unrelated to its kind')
        if exception['kind'] not in ('truthful-identity', 'truthful-metadata', 'renderer', 'background-role-fill'):
            raise ValueError('Unrecognized exception kind')
        if 'mask' in exception or 'threshold' in exception:
            raise ValueError('Exceptions never mask pixels or set similarity thresholds')
    return contract


def build_provenance(source, build):
    executable = build / 'src/app/branch-truce'
    cache = build / 'CMakeCache.txt'
    values = {}
    if cache.exists():
        for line in cache.read_text().splitlines():
            if '=' in line and not line.startswith(('#', '//')):
                k, v = line.split('=', 1)
                values[k.split(':')[0]] = v
    build_source = Path(values.get('CMAKE_HOME_DIRECTORY', '/unavailable'))
    # Record observed source identity separately from the binary's embedded build identity.
    # A source checkout's HEAD alone cannot prove which bytes produced an executable.
    def git_at(root, *args):
        result = subprocess.run(['git', '-C', str(root), *args], capture_output=True, text=True)
        return result.stdout.strip() if result.returncode == 0 else None
    source_files = [p for p in (source / 'src').rglob('*') if p.is_file()]
    equal = bool(source_files) and all((build_source / p.relative_to(source)).is_file()
        and sha256(p) == sha256(build_source / p.relative_to(source)) for p in source_files)
    return dict(executable=str(executable), executable_sha256=sha256(executable),
                source_checkout=str(source), source_head_observed=git_at(source, 'rev-parse', 'HEAD'),
                build_source_observed=str(build_source), build_source_head_observed=git_at(build_source, 'rev-parse', 'HEAD'),
                current_src_matches_build_source=equal,
                embedded_build_identity='not observed; executable SHA identifies the exact artifact',
                manifests={name: sha256(build / name) for name in ('CMakeCache.txt', 'build.ninja', 'compile_commands.json')
                           if (build / name).is_file()},
                cmake_observations={k: v for k, v in values.items() if k in
                    ('CMAKE_BUILD_TYPE', 'CMAKE_CXX_COMPILER', 'BRANCH_TRUCE_WITH_SYNTAX_HIGHLIGHTING')})


def observations(state):
    items = {r['name']: r for r in state['items']}
    def box(name):
        r = items[name]
        return [r[k] for k in ('x', 'y', 'width', 'height')]
    result = dict(logical_size=[state['width'], state['height']], profile=state['profile'],
                  sidebar_width=items['fileList']['width'], file_toolbar_height=items['fileToolBar']['height'],
                  footer_height=state['height']-sum(box('Page')[1::2])-1,
                  file_facts=items['fileFacts']['text'])
    if 'stage2Text' in items:
        header = items['stage2Label']['ancestorGeometry']
        result.update(row_pitch=items['comparisonView']['rowHeight'],
                      header_inset=items['stage2Label']['x']-header[0],
                      pane_header_color=items['stage2Label']['ancestorColor'],
                      syntax_formats=items['stage2Text']['syntaxFormats'],
                      pane_heights={name: items[name]['height'] for name in
                                    ('stage2Viewport', 'resultViewport', 'stage3Viewport')})
    return result


def native_regions(state):
    items = {r['name']: r for r in state['items']}
    width, height = state['width'], state['height']
    page = items['Page']
    sidebar = items['fileList']['width']
    footer_y = math.ceil(page['y']+page['height']+1)
    result = dict(window=[0, 0, width, height], header=[0, 0, width, int(page['y'])],
                  sidebar=[0, int(page['y']), int(sidebar), int(page['height'])],
                  file_header=[math.ceil(sidebar+1), int(page['y']), width-math.ceil(sidebar+1),
                               int(items['fileToolBar']['height'])],
                  footer=[0, footer_y, width, height-footer_y],
                  footer_facts=[int(items['fileFacts']['x']), footer_y,
                                width-int(items['fileFacts']['x']), height-footer_y])
    if 'stage2Text' in items:
        header = items['stage2Label']['ancestorGeometry']
        result['pane_header'] = [math.floor(header[0]), math.floor(header[1]),
                                  math.floor(header[2]), math.floor(header[3])]
        text = items['stage2Viewport']
        result['syntax'] = [math.floor(text['x']), math.floor(text['y']),
                             math.floor(text['width']), math.floor(min(text['height'], 160))]
        gutter = items['stage2Gutter']
        accepted = next(r for r in gutter['rows'] if r['number'] == 19)
        # A glyph-free patch at the left of the accepted row. No rescaling.
        result['accepted_fill'] = [math.ceil(gutter['x']+gutter['width']+state['profile']['units']['gridUnit']+state['profile']['units']['smallSpacing']), math.floor(gutter['y']+accepted['top']), 4, int(accepted['height'])]
        incoming = items['stage3Identity']
        result['incoming_identity'] = [math.floor(incoming['x']), math.floor(incoming['y']),
                                        math.floor(incoming['width']), math.floor(incoming['height'])]
        identity = items['stage2Identity']
        result['source_identity'] = [math.floor(identity['x']), math.floor(identity['y']),
                                      math.floor(identity['width']), math.floor(identity['height'])]
    else:
        result['placeholder'] = [math.ceil(sidebar+1), int(page['y']+items['fileToolBar']['height']),
                                 width-math.ceil(sidebar+1), int(page['height']-items['fileToolBar']['height'])]
    return result


def compare(source, shot, state, image_tool, output):
    contract = load_contract(source)
    output.mkdir(exist_ok=False)
    name = shot.name
    reference = source / 'docs/ui-mockup' / name
    decoded = json.loads(subprocess.check_output([str(image_tool), str(reference)]))
    if [decoded['width'], decoded['height']] != contract['reference_decoded_size']:
        raise ValueError('Reference decoded size differs from contract')
    # The originals are preserved alongside artifacts, without re-encoding.
    shutil.copyfile(reference, output / name)
    shutil.copyfile(shot, output / 'original-native.png')
    native = native_regions(state)
    scale = contract['reference_pixel_scale']
    regions = {}
    for region, rect in native.items():
        if region not in contract['regions']:
            continue
        prefix = output / region
        logical = contract['regions'][region]
        x, y, w, h = logical
        physical = [math.floor(x*scale), math.floor(y*scale),
                    math.ceil((x+w)*scale)-math.floor(x*scale),
                    math.ceil((y+h)*scale)-math.floor(y*scale)]
        reference_grid = [3*x-2*physical[0], 3*y-2*physical[1], 3*w, 3*h]
        command = [str(image_tool), '--artifacts', str(reference), str(shot), str(prefix),
                   *map(str, physical), *map(str, rect), '2', '3', *map(str, reference_grid)]
        result = json.loads(subprocess.check_output(command))
        regions[region] = dict(reference_logical_rect=logical, reference_pixel_rect=physical, native_rect=rect,
                               count_unit='common-grid cells at 3 cells per logical pixel',
                               **result, status='outstanding' if result['differing_pixels'] else 'identical',
                               reference_closeup=str(prefix)+'-reference.png', native_closeup=str(prefix)+'-native.png')
    report = dict(reference_pixel_scale=scale, native_pixel_scale=state['profile']['devicePixelRatio'],
                  alignment='Exact nearest-neighbour cell replication: reference x2, native x3. Reference cells are trimmed to the logical region; originals and closeups remain unscaled.',
                  reference_sha256=sha256(reference), native_sha256=sha256(shot),
                  original_reference=str(output / name), original_native=str(output / 'original-native.png'),
                  regions=regions, observations=observations(state),
                  exceptions=[e for e in contract['exceptions'] if e['reference'] == name],
                  policy='Diagnostics only. Exceptions do not subtract pixels; unexplained differences remain outstanding.')
    (output / 'comparison.json').write_text(json.dumps(report, indent=2)+'\n')
    return report


def colour_scheme(path):
    parser = configparser.ConfigParser(strict=False, interpolation=None)
    parser.optionxform = str
    parser.read(path)
    return {section: dict(parser[section]) for section in parser.sections() if section.startswith('Colors:')}


def validate_profile(state, metadata):
    profile = json.loads(PROFILE.read_text())
    actual = state['profile']
    for key, expected in profile['pinned_observations'].items():
        if actual.get(key) != expected:
            raise ValueError(f'Capture profile mismatch: {key}: {actual.get(key)!r} != {expected!r}')
    for key, expected in profile['pinned_fonts'].items():
        if any(actual['fonts'][key][field] != value for field, value in expected.items()):
            raise ValueError(f'Capture font mismatch: {key}')
    if metadata['capture_environment']['output']['scale'] != profile['environment']['scale']:
        raise ValueError('Compositor scale mismatch')
    requested_scheme = metadata['capture_environment']['colour_scheme']
    if requested_scheme != actual.get('configuredColourScheme'):
        raise ValueError('Configured colour scheme differs from requested scheme')
    expected_palette = profile['pinned_palettes'].get(requested_scheme)
    if expected_palette is None:
        raise ValueError(f'No pinned application palette for {requested_scheme!r}')
    observed_palette = actual.get('palette')
    if not isinstance(observed_palette, dict) or any(
            observed_palette.get(role) != value for role, value in expected_palette.items()):
        raise ValueError(f'Observed application palette differs from pinned {requested_scheme}')
    return True


def capture_inputs(source):
    paths = [p for folder in ('src', 'tools/native-replay') for p in (source / folder).rglob('*')
             if p.is_file() and '__pycache__' not in p.parts]
    paths.append(source / 'docs/ui-design.md')
    return {str(p.relative_to(source)): sha256(p) for p in sorted(paths)}


def verify_capture(output, source):
    report = json.loads((output / 'verification.json').read_text())
    if report['source_inputs'] != capture_inputs(source):
        raise ValueError('Capture inputs changed; new captures required')
    for path, digest in report['artifact_hashes'].items():
        if sha256(output / path) != digest:
            raise ValueError(f'Capture artifact changed: {path}')
    provenance = json.loads((output / 'build-provenance.json').read_text())
    if sha256(Path(provenance['executable'])) != provenance['executable_sha256']:
        raise ValueError('Capture executable changed')
    load_contract(source)
    for row in report['captures']:
        state = json.loads(Path(row['file']).with_name(Path(row['file']).stem+'-state.json').read_text())
        validate_profile(state['state'], state['metadata'])
        cleanup = json.loads((Path(row['file']).parent / 'cleanup.json').read_text())
        if not cleanup['private_root_removed'] or not cleanup['owned_processes_reaped'] or Path(cleanup['private_root']).exists():
            raise ValueError('Replay cleanup incomplete')
    if any(r['differing_pixels'] for r in report['comparisons']):
        raise ValueError('Native repeat pixels differ')
    print(f"{len(report['captures'])} captures, {len(report['comparisons'])} repeat comparisons: "
          'zero differing native pixels; current inputs/artifacts verified; cleanup confirmed')


if __name__ == '__main__':
    import argparse
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--verify', type=Path, required=True)
    args = parser.parse_args()
    verify_capture(args.verify.resolve(), HERE.parents[1])
