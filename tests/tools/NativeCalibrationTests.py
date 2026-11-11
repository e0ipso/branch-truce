"""Reference immutability, exception boundaries and deterministic pixel diagnostics."""
import argparse
import copy
import json
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest import mock
import zlib

sys.dont_write_bytecode = True
ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools/native-replay'))
from calibration import (ImageTool, build_provenance, compare, load_contract, native_regions, rect_argument, validate_contract,
                         validate_profile)
from fixture import png, sha256


class ContractTests(unittest.TestCase):
    def setUp(self):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.contract = load_contract(ROOT)
        self.profile = json.loads((ROOT / 'tools/native-replay/capture-profile.json').read_text())

    def profile_state(self, scheme='BreezeLight', palette=None):
        profile = self.profile
        return {'profile': dict(profile['pinned_observations'], fonts=profile['pinned_fonts'],
                                configuredColourScheme=scheme, palette=self.profile['pinned_palettes'][scheme] if palette is None else palette)}

    def test_all_four_png_and_html_references_are_hash_pinned(self):
        self.assertEqual(set(self.contract['references']), {f'{t}-{k}.{ext}' for t in ('light', 'dark')
            for k in ('text', 'binary') for ext in ('png', 'html')})

    def test_reference_tampering_is_rejected_without_rewriting_it(self):
        shutil.copytree(ROOT / 'docs/ui-mockup', self.root / 'docs/ui-mockup')
        victim = self.root / 'docs/ui-mockup/light-text.png'
        victim.write_bytes(victim.read_bytes()+b'changed')
        before = victim.read_bytes()
        with self.assertRaisesRegex(ValueError, 'reference changed'):
            load_contract(self.root)
        self.assertEqual(victim.read_bytes(), before)

    def test_each_exception_requires_region_hash_value_reason_and_rule(self):
        for entry in self.contract['exceptions']:
            for key in ('region', 'reference', 'reference_sha256', 'observed_native_value', 'reason', 'acceptance_rule'):
                candidate = copy.deepcopy(self.contract)
                del candidate['exceptions'][self.contract['exceptions'].index(entry)][key]
                with self.assertRaisesRegex(ValueError, 'Exception missing'):
                    validate_contract(candidate)

    def test_broad_or_wrong_hash_exceptions_and_masks_are_rejected(self):
        for update in ({'region': 'window'}, {'region': 'absent'}, {'region': 'sidebar'}, {'reference_sha256': '0'*64},
                       {'kind': 'theme-excuse'}, {'mask': [0, 0, 1280, 720]}, {'threshold': 0.99}, {'threshold': 0}):
            candidate = copy.deepcopy(self.contract)
            candidate['exceptions'][0].update(update)
            with self.assertRaises(ValueError):
                validate_contract(candidate)

    def test_unknown_exception_kind_is_named(self):
        candidate = copy.deepcopy(self.contract)
        candidate['exceptions'][0]['kind'] = 'theme-excuse'
        with self.assertRaisesRegex(ValueError, 'Unrecognized exception kind: theme-excuse'):
            validate_contract(candidate)

    def test_geometry_outside_reference_is_rejected(self):
        for rect in ([0, 0, 1281, 720], [-1, 0, 30, 30], [0, 0, 0, 5], [0, 0, 1.5, 4]):
            candidate = copy.deepcopy(self.contract)
            candidate['regions']['pane_header'] = rect
            with self.assertRaises(ValueError):
                validate_contract(candidate)

    def test_profile_rejects_style_dpr_scheme_and_font_drift(self):
        state = self.profile_state()
        metadata = {'capture_environment': {'output': {'scale': 1}, 'colour_scheme': 'BreezeLight'}}
        self.assertTrue(validate_profile(state, metadata))
        for key, value in [('devicePixelRatio', 2), ('quickControlsStyle', 'Fusion'), ('configuredColourScheme', 'BreezeDark')]:
            changed = copy.deepcopy(state)
            changed['profile'][key] = value
            with self.assertRaises(ValueError):
                validate_profile(changed, metadata)
        changed = copy.deepcopy(state)
        changed['profile']['fonts']['small']['pointSize'] = 9
        with self.assertRaises(ValueError):
            validate_profile(changed, metadata)

    def test_profile_accepts_recorded_light_and_dark_palettes(self):
        for scheme, palette in self.profile['pinned_palettes'].items():
            with self.subTest(scheme=scheme):
                state = self.profile_state(scheme, palette)
                metadata = {'capture_environment': {'output': {'scale': 1}, 'colour_scheme': scheme}}
                self.assertTrue(validate_profile(state, metadata))

    def test_profile_rejects_wrong_palette_when_configured_name_matches(self):
        for requested, observed in [('BreezeLight', 'BreezeDark'), ('BreezeDark', 'BreezeLight')]:
            with self.subTest(requested=requested):
                state = self.profile_state(requested, self.profile['pinned_palettes'][observed])
                metadata = {'capture_environment': {'output': {'scale': 1}, 'colour_scheme': requested}}
                with self.assertRaisesRegex(ValueError, 'Observed application palette'):
                    validate_profile(state, metadata)

    def test_profile_requires_all_pinned_observed_palette_roles(self):
        palette = self.profile['pinned_palettes']['BreezeLight']
        metadata = {'capture_environment': {'output': {'scale': 1}, 'colour_scheme': 'BreezeLight'}}
        for role in palette:
            for value in [None, '#000000']:
                with self.subTest(role=role, value=value):
                    changed = dict(palette)
                    if value is None:
                        del changed[role]
                    else:
                        changed[role] = value
                    state = self.profile_state(palette=changed)
                    with self.assertRaisesRegex(ValueError, 'Observed application palette'):
                        validate_profile(state, metadata)

    def test_supplemental_footer_geometry_uses_native_height(self):
        state = {'width': 1280, 'height': 800, 'items': [
            dict(name='Page', x=0, y=48, width=1280, height=721),
            dict(name='fileList', x=0, y=82, width=252, height=687),
            dict(name='fileToolBar', x=603, y=48, width=677, height=34),
            dict(name='fileFacts', x=1100, y=778, width=180, height=15)]}
        regions = native_regions(state)
        self.assertEqual(regions['footer'], [0, 770, 1280, 30])
        self.assertEqual(regions['file_header'], [253, 48, 1027, 34])

    def test_build_provenance_does_not_invent_a_compiled_revision(self):
        (self.root / 'src/app').mkdir(parents=True)
        executable = self.root / 'src/app/branch-truce'
        executable.write_bytes(b'fixture executable identity')
        observed = build_provenance(self.root, self.root)
        self.assertEqual(observed['executable_sha256'], sha256(executable))
        self.assertIsNone(observed['source_head_observed'])
        self.assertFalse(observed['current_src_matches_build_source'])
        self.assertIn('not observed', observed['embedded_build_identity'])

    def write_cache(self, root, **entries):
        (root / 'src/app').mkdir(parents=True)
        (root / 'src/app/branch-truce').write_bytes(b'fixture executable identity')
        lines = ['# This is the CMakeCache file.', '//Highlight syntax in the comparison panes',
                 *(f'{key}:STRING={value}' for key, value in entries.items())]
        (root / 'CMakeCache.txt').write_text('\n'.join(lines) + '\n')

    def test_build_provenance_records_the_syntax_highlighting_option(self):
        self.write_cache(self.root, CMAKE_HOME_DIRECTORY=self.root, CMAKE_BUILD_TYPE='Debug',
                         CMAKE_CXX_COMPILER='/usr/bin/c++', BRANCH_TRUCE_SYNTAX_HIGHLIGHTING='OFF')
        observed = build_provenance(self.root, self.root)
        self.assertEqual(observed['cmake_observations'], dict(
            CMAKE_BUILD_TYPE='Debug', CMAKE_CXX_COMPILER='/usr/bin/c++', BRANCH_TRUCE_SYNTAX_HIGHLIGHTING='OFF'))

    def test_build_provenance_rejects_a_cache_without_an_observed_option(self):
        self.write_cache(self.root, CMAKE_HOME_DIRECTORY=self.root, CMAKE_BUILD_TYPE='Debug',
                         CMAKE_CXX_COMPILER='/usr/bin/c++', BRANCH_TRUCE_WITH_SYNTAX_HIGHLIGHTING='ON')
        with self.assertRaisesRegex(KeyError, 'CMakeCache.txt lacks BRANCH_TRUCE_SYNTAX_HIGHLIGHTING'):
            build_provenance(self.root, self.root)

    def test_changed_reference_scale_requires_review(self):
        changed = copy.deepcopy(self.contract)
        changed['reference_pixel_scale'] = 1
        with self.assertRaisesRegex(ValueError, 'raster geometry'):
            validate_contract(changed)


class PixelTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.a, self.b = self.root / 'a.png', self.root / 'b.png'
        self.a.write_bytes(png((100, 80, 60)))
        self.b.write_bytes(png((90, 60, 30)))

    def run_tool(self, *args):
        return subprocess.run([str(IMAGE_TOOL), *map(str, args)], capture_output=True, text=True)

    def tool_json(self, *args):
        result = self.run_tool(*args)
        self.assertEqual(result.returncode, 0, result.stderr)
        return json.loads(result.stdout)

    def artifacts(self, name, a, b, left=(0, 0, 4, 3), right=(0, 0, 4, 3), replication=(1, 1), grid=None):
        # Without a declared grid, keep the whole replicated reference region.
        grid = grid or (0, 0, left[2] * replication[0], left[3] * replication[0])
        return self.run_tool('artifacts', '--reference-rect=' + rect_argument(left),
                             '--native-rect=' + rect_argument(right),
                             f'--reference-replication={replication[0]}', f'--native-replication={replication[1]}',
                             '--reference-grid=' + rect_argument(grid), a, b, self.root / name)

    def assertUsageError(self, result, detail):
        self.assertEqual(result.returncode, 2, result.stderr)
        self.assertEqual(result.stdout, '')
        self.assertIn(detail, result.stderr)
        self.assertIn('native-replay-image', result.stderr)

    def test_help_and_usage_errors(self):
        for args in (('--help',), ('-h',)):
            result = self.run_tool(*args)
            self.assertEqual((result.returncode, result.stderr), (0, ''))
            self.assertIn('artifacts REFERENCE NATIVE PREFIX', result.stdout)
        for command in ('decode', 'compare', 'crop', 'artifacts'):
            result = self.run_tool(command, '--help')
            self.assertEqual((result.returncode, result.stderr), (0, ''), command)
            self.assertIn(command, result.stdout)
        self.assertUsageError(self.run_tool(), 'a command is required')
        self.assertUsageError(self.run_tool('--crop', self.a), "unknown command '--crop'")
        self.assertUsageError(self.run_tool('decode'), 'decode takes 1 arguments')
        self.assertUsageError(self.run_tool('compare', self.a), 'compare takes 2 arguments')
        self.assertUsageError(self.run_tool('crop', self.a, self.root / 'x.png'), '--rect is required')
        self.assertUsageError(self.run_tool('crop', '--rect=1,2,x,3', self.a, self.root / 'x.png'),
                              "--rect must be X,Y,WIDTH,HEIGHT integers, not '1,2,x,3'")
        self.assertUsageError(self.run_tool('crop', '--size=4', self.a, self.root / 'x.png'), 'size')
        self.assertUsageError(self.artifacts('replication', self.a, self.b, replication=(4, 1)),
                              "--reference-replication must be 1, 2 or 3, not '4'")
        self.assertFalse(list(self.root.glob('x.png')) + list(self.root.glob('replication-*')))

    def test_artifacts_require_the_full_argument_set(self):
        # Replication and the reference grid have no defaults; the comparison grid is always explicit.
        result = self.run_tool('artifacts', '--reference-rect=0,0,4,3', '--native-rect=0,0,4,3',
                               self.a, self.b, self.root / 'partial')
        self.assertUsageError(result, '--reference-replication is required')
        result = self.run_tool('artifacts', '--reference-rect=0,0,4,3', '--native-rect=0,0,4,3',
                               '--reference-replication=1', '--native-replication=1', self.a, self.b,
                               self.root / 'partial')
        self.assertUsageError(result, '--reference-grid is required')
        self.assertFalse(list(self.root.glob('partial-*')))

    def test_unreadable_inputs_fail_with_a_message(self):
        missing = self.root / 'missing.png'
        for args in (('decode', missing), ('compare', self.a, missing), ('crop', '--rect=0,0,1,1', missing, self.root / 'x.png')):
            result = self.run_tool(*args)
            self.assertEqual((result.returncode, result.stdout), (1, ''), args)
            self.assertIn(f'cannot decode {missing}', result.stderr)

    def test_native_closeups_preserve_original_pixels_and_reject_outside_bounds(self):
        original = self.a.read_bytes()
        crop = self.root / 'native-closeup.png'
        subprocess.run([str(IMAGE_TOOL), 'crop', '--rect=1,2,4,3', str(self.a), str(crop)], check=True)
        decoded = self.tool_json('decode', crop)
        self.assertEqual((decoded['width'], decoded['height']), (4, 3))
        expected = self.artifacts('expected-closeup', self.a, self.a, (1, 2, 4, 3), (1, 2, 4, 3))
        self.assertEqual(expected.returncode, 0, expected.stderr)
        reference = str(self.root / 'expected-closeup-native.png')
        compared = self.tool_json('compare', crop, reference)
        self.assertEqual(compared['differing_pixels'], 0)
        bad = self.root / 'invalid-closeup.png'
        result = self.run_tool('crop', '--rect=-1,0,4,3', self.a, bad)
        self.assertEqual(result.returncode, 1)
        self.assertIn('--rect -1,0,4,3 is empty or outside the image', result.stderr)
        self.assertFalse(bad.exists())
        self.assertEqual(self.a.read_bytes(), original)

    def test_identical_and_changed_pixels_are_exact_and_repeatable(self):
        originals = [sha256(p) for p in (self.a, self.b)]
        same = json.loads(self.artifacts('same', self.a, self.a).stdout)
        different = json.loads(self.artifacts('one', self.a, self.b).stdout)
        again = json.loads(self.artifacts('two', self.a, self.b).stdout)
        self.assertEqual(same['differing_pixels'], 0)
        self.assertEqual(different['differing_pixels'], 12)
        self.assertEqual(different['unmatched_extent_pixels'], 0)
        self.assertEqual(different['overlay_top_left'], '#ff5f462d')
        self.assertEqual(different['difference_top_left'], '#ff0a141e')
        for suffix in ('reference', 'native', 'overlay', 'difference'):
            self.assertEqual((self.root / f'one-{suffix}.png').read_bytes(),
                             (self.root / f'two-{suffix}.png').read_bytes())
        # Decode through Qt too, so a stable hash alone cannot hide wrong dimensions.
        dimensions = self.tool_json('decode', different['overlay'])
        self.assertEqual((dimensions['width'], dimensions['height']), (4, 3))
        self.assertEqual([sha256(p) for p in (self.a, self.b)], originals)
        self.assertEqual(again['differing_pixels'], 12)

    def test_unmatched_extents_are_counted_without_rescaling(self):
        result = json.loads(self.artifacts('extent', self.a, self.a, right=(0, 0, 2, 3)).stdout)
        self.assertEqual(result['unmatched_extent_pixels'], 6)
        self.assertEqual(result['differing_pixels'], 6)

    def test_reference_and_native_scale_are_explicit_without_interpolation(self):
        # A uniform 2x2 reference cell block versus 3x3 native cells represents
        # the same logical extent after reference x3 / native x2 replication.
        scaled = self.artifacts('scale', self.a, self.a, (0, 0, 2, 2), (0, 0, 3, 3), replication=(3, 2))
        self.assertEqual(scaled.returncode, 0, scaled.stderr)
        result = json.loads(scaled.stdout)
        self.assertEqual(result['differing_pixels'], 0)
        self.assertEqual(result['unmatched_extent_pixels'], 0)
        self.assertEqual(result['reference_common_grid_rect'], [0, 0, 6, 6])
        original = self.tool_json('decode', self.root / 'scale-reference.png')
        overlay = self.tool_json('decode', result['overlay'])
        self.assertEqual(original['width'], 2)
        self.assertEqual(overlay['width'], 6)

    def test_out_of_bounds_crops_fail_without_artifacts(self):
        result = self.artifacts('invalid', self.a, self.b, left=(60, 0, 5, 3))
        self.assertEqual(result.returncode, 1)
        self.assertIn('--reference-rect 60,0,5,3 is empty or outside the image', result.stderr)
        self.assertFalse(list(self.root.glob('invalid-*')))

    def compare_bar_region(self, name, rect, changed=False, logical_size=None, bar=(2, 2, 4, 4)):
        x, y, width, height = rect
        size = logical_size or (x+width, y+height+1)
        root = self.root / name
        source = root / 'source'
        references = source / 'docs/ui-mockup'
        references.mkdir(parents=True)
        native = root / 'native'
        native.mkdir()

        def write_png(path, dimensions, bar, changed_pixel=False):
            raw = bytearray()
            for py in range(dimensions[1]):
                raw.append(0)
                for px in range(dimensions[0]):
                    colour = (0, 0, 0) if bar[0] <= px < bar[2] and bar[1] <= py < bar[3] else (255, 255, 255)
                    raw.extend((255, 0, 0) if changed_pixel and (px, py) == (2, 2) else colour)
            def chunk(kind, data):
                return struct.pack('>I', len(data))+kind+data+struct.pack('>I', zlib.crc32(kind+data))
            path.write_bytes(b'\x89PNG\r\n\x1a\n'
                +chunk(b'IHDR', struct.pack('>IIBBBBB', *dimensions, 8, 2, 0, 0, 0))
                +chunk(b'IDAT', zlib.compress(raw))+chunk(b'IEND', b''))

        reference_size = tuple((n*3+1)//2 for n in size)
        reference, shot = references / 'light-text.png', native / 'light-text.png'
        # Even bar edges have exact 1.5x raster coordinates without interpolation.
        write_png(reference, reference_size, tuple(n*3//2 for n in bar))
        write_png(shot, size, bar, changed)
        originals = [sha256(reference), sha256(shot)]
        contract = dict(reference_pixel_scale=1.5, reference_decoded_size=list(reference_size),
                        regions={'file_header': list(rect)}, exceptions=[])
        state = dict(width=size[0], height=size[1], profile={'devicePixelRatio': 1}, items=[
            dict(name='Page', x=0, y=y, width=size[0], height=size[1]-y-1),
            dict(name='fileList', x=0, y=y, width=x-1, height=height),
            dict(name='fileToolBar', x=x, y=y, width=width, height=height),
            dict(name='fileFacts', x=0, y=size[1]-1, width=size[0], height=1, text='fixture')])
        with mock.patch('calibration.load_contract', return_value=contract):
            report = compare(source, shot, state, ImageTool(IMAGE_TOOL), root / 'comparison')
        self.assertEqual([sha256(reference), sha256(shot)], originals)
        return report['regions']['file_header']

    def test_identical_regions_align_for_odd_and_even_origins_and_dimensions(self):
        for index, rect in enumerate(((1, 1, 5, 3), (1, 2, 4, 3), (2, 1, 3, 4), (2, 2, 4, 4))):
            with self.subTest(rect=rect):
                region = self.compare_bar_region(f'aligned-{index}', rect)
                self.assertEqual(region['differing_pixels'], 0)
                self.assertEqual(region['unmatched_extent_pixels'], 0)
                self.assertEqual(region['status'], 'identical')
                overlay = self.tool_json('decode', region['overlay'])
                self.assertEqual((overlay['width'], overlay['height']), (3*rect[2], 3*rect[3]))

    def test_aligned_odd_region_still_detects_a_real_changed_pixel(self):
        region = self.compare_bar_region('changed-bar', (1, 1, 5, 3), changed=True)
        self.assertEqual(region['differing_pixels'], 9)
        self.assertEqual(region['unmatched_extent_pixels'], 0)
        self.assertEqual(region['status'], 'outstanding')

    def test_identical_file_header_at_reference_resolution_has_no_false_differences(self):
        region = self.compare_bar_region('file-header', (263, 46, 1017, 40),
                                        logical_size=(1280, 720), bar=(300, 0, 310, 720))
        self.assertEqual(region['differing_pixels'], 0)
        self.assertEqual(region['unmatched_extent_pixels'], 0)

    def test_invalid_common_grid_crop_fails_without_artifacts(self):
        for index, rect in enumerate(((-1, 0, 6, 6), (0, 0, 9, 6), (0, 0, 6, 0), (0, 0, 6, 7))):
            with self.subTest(rect=rect):
                result = self.artifacts(f'invalid-grid-{index}', self.a, self.b, replication=(2, 3), grid=rect)
                self.assertEqual(result.returncode, 1)
                self.assertIn('--reference-grid', result.stderr)
                self.assertFalse(list(self.root.glob(f'invalid-grid-{index}-*')))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--image-tool', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    IMAGE_TOOL = args.image_tool.resolve()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
