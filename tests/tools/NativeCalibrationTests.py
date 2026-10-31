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
from calibration import load_contract, validate_contract, validate_profile, native_regions, build_provenance, compare
from fixture import png, sha256


class ContractTests(unittest.TestCase):
    def setUp(self):
        self.contract = load_contract(ROOT)

    def test_all_four_png_and_html_references_are_hash_pinned(self):
        self.assertEqual(set(self.contract['references']), {f'{t}-{k}.{ext}' for t in ('light', 'dark')
            for k in ('text', 'binary') for ext in ('png', 'html')})

    def test_reference_tampering_is_rejected_without_rewriting_it(self):
        with tempfile.TemporaryDirectory(dir='/var/tmp') as directory:
            source = Path(directory)
            shutil.copytree(ROOT / 'docs/ui-mockup', source / 'docs/ui-mockup')
            victim = source / 'docs/ui-mockup/light-text.png'
            victim.write_bytes(victim.read_bytes()+b'changed')
            before = victim.read_bytes()
            with self.assertRaisesRegex(ValueError, 'reference changed'):
                load_contract(source)
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

    def test_geometry_outside_reference_is_rejected(self):
        for rect in ([0, 0, 1281, 720], [-1, 0, 30, 30], [0, 0, 0, 5], [0, 0, 1.5, 4]):
            candidate = copy.deepcopy(self.contract)
            candidate['regions']['pane_header'] = rect
            with self.assertRaises(ValueError):
                validate_contract(candidate)

    def test_profile_rejects_style_dpr_scheme_and_font_drift(self):
        profile = json.loads((ROOT / 'tools/native-replay/capture-profile.json').read_text())
        state = {'profile': dict(profile['pinned_observations'], fonts=profile['pinned_fonts'],
                                 configuredColourScheme='BreezeLight', palette=profile['pinned_palettes']['BreezeLight'])}
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
        profile = json.loads((ROOT / 'tools/native-replay/capture-profile.json').read_text())
        for scheme, palette in profile['pinned_palettes'].items():
            with self.subTest(scheme=scheme):
                state = {'profile': dict(profile['pinned_observations'], fonts=profile['pinned_fonts'],
                                         configuredColourScheme=scheme, palette=palette)}
                metadata = {'capture_environment': {'output': {'scale': 1}, 'colour_scheme': scheme}}
                self.assertTrue(validate_profile(state, metadata))

    def test_profile_rejects_wrong_palette_when_configured_name_matches(self):
        profile = json.loads((ROOT / 'tools/native-replay/capture-profile.json').read_text())
        for requested, observed in [('BreezeLight', 'BreezeDark'), ('BreezeDark', 'BreezeLight')]:
            with self.subTest(requested=requested):
                state = {'profile': dict(profile['pinned_observations'], fonts=profile['pinned_fonts'],
                                         configuredColourScheme=requested, palette=profile['pinned_palettes'][observed])}
                metadata = {'capture_environment': {'output': {'scale': 1}, 'colour_scheme': requested}}
                with self.assertRaisesRegex(ValueError, 'Observed application palette'):
                    validate_profile(state, metadata)

    def test_profile_requires_all_pinned_observed_palette_roles(self):
        profile = json.loads((ROOT / 'tools/native-replay/capture-profile.json').read_text())
        palette = profile['pinned_palettes']['BreezeLight']
        metadata = {'capture_environment': {'output': {'scale': 1}, 'colour_scheme': 'BreezeLight'}}
        for role in palette:
            for value in [None, '#000000']:
                with self.subTest(role=role, value=value):
                    changed = dict(palette)
                    if value is None:
                        del changed[role]
                    else:
                        changed[role] = value
                    state = {'profile': dict(profile['pinned_observations'], fonts=profile['pinned_fonts'],
                                             configuredColourScheme='BreezeLight', palette=changed)}
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
        with tempfile.TemporaryDirectory(dir='/var/tmp') as directory:
            root = Path(directory)
            (root / 'src/app').mkdir(parents=True)
            executable = root / 'src/app/branch-truce'
            executable.write_bytes(b'fixture executable identity')
            observed = build_provenance(root, root)
            self.assertEqual(observed['executable_sha256'], sha256(executable))
            self.assertIsNone(observed['source_head_observed'])
            self.assertFalse(observed['current_src_matches_build_source'])
            self.assertIn('not observed', observed['embedded_build_identity'])

    def test_changed_reference_scale_requires_review(self):
        changed = copy.deepcopy(self.contract)
        changed['reference_pixel_scale'] = 1
        with self.assertRaisesRegex(ValueError, 'raster geometry'):
            validate_contract(changed)


class PixelTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory(dir='/var/tmp')
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.a, self.b = self.root / 'a.png', self.root / 'b.png'
        self.a.write_bytes(png((100, 80, 60)))
        self.b.write_bytes(png((90, 60, 30)))

    def artifacts(self, name, a, b, left=(0, 0, 4, 3), right=(0, 0, 4, 3)):
        result = subprocess.run([str(IMAGE_TOOL), '--artifacts', str(a), str(b), str(self.root / name),
                                 *map(str, left), *map(str, right)], capture_output=True)
        return result

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
        dimensions = json.loads(subprocess.check_output([str(IMAGE_TOOL), different['overlay']]))
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
        command = [str(IMAGE_TOOL), '--artifacts', str(self.a), str(self.a), str(self.root / 'scale'),
                   '0', '0', '2', '2', '0', '0', '3', '3', '3', '2']
        result = json.loads(subprocess.check_output(command))
        self.assertEqual(result['differing_pixels'], 0)
        self.assertEqual(result['unmatched_extent_pixels'], 0)
        original = json.loads(subprocess.check_output([str(IMAGE_TOOL), str(self.root / 'scale-reference.png')]))
        overlay = json.loads(subprocess.check_output([str(IMAGE_TOOL), result['overlay']]))
        self.assertEqual(original['width'], 2)
        self.assertEqual(overlay['width'], 6)

    def test_out_of_bounds_crops_fail_without_artifacts(self):
        result = self.artifacts('invalid', self.a, self.b, left=(60, 0, 5, 3))
        self.assertEqual(result.returncode, 2)
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
            report = compare(source, shot, state, IMAGE_TOOL, root / 'comparison')
        self.assertEqual([sha256(reference), sha256(shot)], originals)
        return report['regions']['file_header']

    def test_identical_regions_align_for_odd_and_even_origins_and_dimensions(self):
        for index, rect in enumerate(((1, 1, 5, 3), (1, 2, 4, 3), (2, 1, 3, 4), (2, 2, 4, 4))):
            with self.subTest(rect=rect):
                region = self.compare_bar_region(f'aligned-{index}', rect)
                self.assertEqual(region['differing_pixels'], 0)
                self.assertEqual(region['unmatched_extent_pixels'], 0)
                self.assertEqual(region['status'], 'identical')
                overlay = json.loads(subprocess.check_output([str(IMAGE_TOOL), region['overlay']]))
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
                prefix = self.root / f'invalid-grid-{index}'
                result = subprocess.run([str(IMAGE_TOOL), '--artifacts', str(self.a), str(self.b), str(prefix),
                    '0', '0', '4', '3', '0', '0', '4', '3', '2', '3', *map(str, rect)], capture_output=True)
                self.assertEqual(result.returncode, 2)
                self.assertFalse(list(self.root.glob(f'invalid-grid-{index}-*')))


if __name__ == '__main__':
    parser = argparse.ArgumentParser()
    parser.add_argument('--image-tool', type=Path, required=True)
    args, remaining = parser.parse_known_args()
    IMAGE_TOOL = args.image_tool.resolve()
    unittest.main(argv=[sys.argv[0], *remaining], verbosity=2)
