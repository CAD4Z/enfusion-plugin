"""Black-box HDR tests; the independent reader uses only Python's standard library."""
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = sys.argv.pop(1)
GOLDENS = json.loads((pathlib.Path(__file__).parents[1] / 'workbench/hdr-goldens.json').read_text())


class HdrConversion(unittest.TestCase):
    def test_all_captured_rgbe_encodings_preserve_float_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            for fixture in GOLDENS['decoder']:
                source.write_bytes(bytes.fromhex(fixture['source']))
                result = subprocess.run([CLI, 'edds', 'convert', '--machine', '--protocol', '1',
                    '--input', str(source), '--output', str(output), '--format-compress', 'copy',
                    '--generate-mips', 'false'], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                pixels = struct.unpack('<' + 'f' * (fixture['width']*fixture['height']*4), output.read_bytes()[136:])
                self.assertEqual([p for i,p in enumerate(pixels) if i%4!=3], fixture['rgb'])

    def test_quality_refines_a_colour_ramp_without_changing_hdr_format(self):
        import base64
        rgbe = bytes(v for y in range(4) for x in range(4) for v in [32+x*32,160-x*32,32,132])
        expected = [v/16 for i,v in enumerate(rgbe) if i%4!=3]
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'ramp.hdr'; output = source.with_suffix('.edds')
            source.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n'+rgbe)
            errors = []
            for quality in ['0','1']:
                result = subprocess.run([CLI, 'edds', 'convert', '--machine', '--protocol', '1',
                    '--input', str(source), '--output', str(output), '--conversion', 'hdr-compression',
                    '--conversion-quality', quality, '--generate-mips', 'false'], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads(result.stdout)['pixelFormat'], 'BC6H')
                result = subprocess.run([CLI, 'edds', 'preview', '--machine', '--protocol', '1',
                    '--input', str(output), '--mip', '0', '--float'], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                values = struct.unpack('<64f', base64.b64decode(json.loads(result.stdout)['pixelsBase64']))
                rgb = [v for i,v in enumerate(values) if i%4!=3]
                errors.append(sum(((a-b)/(1+b))**2 for a,b in zip(rgb,expected))/len(rgb))
                self.assertGreater(max(rgb),1)
            self.assertLess(errors[1], errors[0])
            self.assertLess(errors[1], 0.005)

    def test_workbench_bc6h_container_topology_is_readable(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'workbench.edds'
            for row in GOLDENS['containers']:
                output.write_bytes(bytes.fromhex(row['bc6_container']))
                result = subprocess.run([CLI, 'edds', 'inspect', '--machine', '--protocol', '1',
                    '--input', str(output)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                facts = json.loads(result.stdout)
                self.assertEqual(facts['dds']['arraySize'], row['faces'])
                self.assertEqual(facts['faceCount'], row['faces'])
                self.assertTrue(facts['previewSupported'])

    def test_parser_subtypes_truncations_and_oversized_headers_never_mutate_a_pair(self):
        import random
        fixture = bytes.fromhex(GOLDENS['decoder'][0]['source'])
        cases = [b'', b'#?RADIANCE\n', fixture[:-1], fixture + b'junk',
            fixture.replace(b'-Y 8 +X 16', b'-Y 32768 +X 32768'),
            fixture.replace(b'FORMAT=32-bit_rle_rgbe', b'FORMAT=32-bit_rle_xyze'),
            fixture.replace(b'-Y', b'+Y'), fixture.replace(b'+X', b'-X'),
            fixture.replace(b'FORMAT=', b'EXPOSURE=2\nFORMAT=')]
        for row in GOLDENS['decoder']:
            data = bytes.fromhex(row['source'])
            cases += [data[:i] for i in random.Random(808).sample(range(len(data)), 20)]
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            meta = source.with_suffix('.edds.meta')
            source.write_bytes(fixture)
            args = [CLI, 'edds', 'convert', '--machine', '--protocol', '1', '--input', str(source),
                '--output', str(output), '--metadata', str(meta), '--resource-name', 'Fixture/source.edds',
                '--source-file', source.name, '--guid', '0123456789ABCDEF']
            subprocess.run(args, capture_output=True, check=True)
            pair = (output.read_bytes(), meta.read_bytes())
            for data in cases:
                source.write_bytes(data)
                result = subprocess.run(args, capture_output=True, text=True)
                self.assertIn(result.returncode, [3,4], result.stdout + result.stderr)
                self.assertIn(json.loads(result.stdout)['error']['code'], ['invalid-hdr', 'unsupported-hdr-subtype',
                    'unsupported-hdr-orientation', 'image-size-limit', 'trailing-hdr-data'])
                self.assertEqual((output.read_bytes(), meta.read_bytes()), pair)
                self.assertEqual(sorted(p.name for p in pathlib.Path(directory).iterdir()),
                    ['source.edds', 'source.edds.meta', 'source.hdr'])

    def test_hdr_recipe_round_trip_faces_stale_input_and_cancellation(self):
        import base64
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds'); meta = source.with_suffix('.edds.meta')
            source.write_bytes(bytes.fromhex(GOLDENS['decoder'][0]['source']))
            args = [CLI, 'edds', 'convert', '--machine', '--protocol', '1', '--input', str(source),
                '--output', str(output), '--metadata', str(meta), '--resource-name', 'Fixture/source.edds',
                '--source-file', source.name, '--guid', '0123456789ABCDEF',
                '--conversion', 'hdr-compression', '--generate-cubemap', 'true', '--conversion-quality', '0.403']
            result = subprocess.run(args, capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            inspection = subprocess.run([CLI, 'edds', 'inspect', '--machine', '--protocol', '1',
                '--input', str(output), '--metadata', str(meta)], capture_output=True, text=True)
            self.assertEqual(inspection.returncode, 0, inspection.stdout + inspection.stderr)
            facts = json.loads(inspection.stdout)
            self.assertEqual(facts['faceCount'], 6)
            self.assertEqual(facts['dds']['arraySize'], 6)
            self.assertEqual(facts['dds']['pitchOrLinearSize'], 16)
            self.assertEqual(output.read_bytes()[:148], bytes.fromhex(GOLDENS['containers'][1]['bc6_container'])[:148])
            self.assertEqual(facts['metadata']['recipe']['GenerateCubemap'], True)
            self.assertEqual(facts['metadata']['recipe']['ConversionQuality'], 0.403)
            self.assertEqual(facts['pixelFormat'], 'BC6H')
            self.assertIn('HDRResourceClass', meta.read_text())
            preview = subprocess.run([CLI, 'edds', 'preview', '--machine', '--protocol', '1',
                '--input', str(output), '--mip', '0', '--all-faces'], capture_output=True, text=True)
            self.assertEqual(preview.returncode, 0, preview.stdout + preview.stderr)
            faces = json.loads(preview.stdout)['facesBase64']
            self.assertEqual(len(faces), 6)
            self.assertEqual(len(base64.b64decode(faces[0])), 4*4*4)
            self.assertGreater(len(set(faces)), 1)
            pair = (output.read_bytes(), meta.read_bytes())
            cancel = source.with_name('cancel'); cancel.write_text('cancel')
            for extra in [['--expect-source-revision', '0:0', '--expect-output-revision', '0:0', '--expect-metadata-revision', '0:0'],
                          ['--cancel-file', str(cancel)], ['--swizzling', 'alpha-to-rgb']]:
                result = subprocess.run(args + extra, capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, result.stdout)
                self.assertEqual((output.read_bytes(), meta.read_bytes()), pair)
                self.assertEqual(len(list(pathlib.Path(directory).iterdir())), 4)

    def test_all_bc6h_modes_against_independent_float_decoder(self):
        import base64
        rows = json.loads(pathlib.Path(__file__).with_name('bc6-goldens.json').read_text())['rows']
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'constant.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n' + bytes([128, 64, 32, 132]) * 16)
            subprocess.run([CLI, 'edds', 'convert', '--machine', '--protocol', '1', '--input', str(source),
                '--output', str(output), '--conversion', 'hdr-compression', '--format-compress', 'copy',
                '--generate-mips', 'false'], check=True, capture_output=True)
            header = output.read_bytes()[:156]
            for row in rows:
                with self.subTest(mode=row['mode'], block=row['block']):
                    output.write_bytes(header + bytes.fromhex(row['block']))
                    result = subprocess.run([CLI, 'edds', 'preview', '--machine', '--protocol', '1',
                        '--input', str(output), '--mip', '0', '--float'], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    values = struct.unpack('<64f', base64.b64decode(json.loads(result.stdout)['pixelsBase64']))
                    self.assertEqual([v for i,v in enumerate(values) if i%4!=3], row['rgb'])

    def test_bc6h_preserves_hdr_and_reports_actual_float_preview(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'constant.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n' + bytes([128, 64, 32, 132]) * 16)
            result = subprocess.run([CLI, 'edds', 'convert', '--machine', '--protocol', '1',
                '--input', str(source), '--output', str(output), '--conversion', 'hdr-compression',
                '--format-compress', 'copy', '--generate-mips', 'false'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = output.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data, 128)[0], 95)
            block = int.from_bytes(data[156:172], 'little')
            self.assertEqual(block & 31, 3)  # Mode 11: untransformed 10-bit endpoints.
            endpoints = [(block >> (5 + i * 10)) & 1023 for i in range(6)]
            def unquant(v):
                return 0 if v == 0 else 65535 if v == 1023 else ((v << 16) + 32768) >> 10
            endpoints = list(map(unquant, endpoints))
            weights = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]
            at = 65
            expected = []
            for pixel in range(16):
                bits = 3 if pixel == 0 else 4
                w = weights[(block >> at) & ((1 << bits) - 1)]
                at += bits
                for c, target in enumerate([8, 4, 2]):
                    half = (((endpoints[c] * (64 - w) + endpoints[c + 3] * w + 32) >> 6) * 31) >> 6
                    value = struct.unpack('<e', struct.pack('<H', half))[0]
                    self.assertAlmostEqual(value, target, delta=target * 0.025)
                    expected.append(value)
                expected.append(1.0)
            result = subprocess.run([CLI, 'edds', 'preview', '--machine', '--protocol', '1',
                '--input', str(output), '--mip', '0', '--float'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            import base64
            preview = json.loads(result.stdout)
            actual = struct.unpack('<64f', base64.b64decode(preview['pixelsBase64']))
            self.assertEqual(list(actual), expected)

    def test_float_faces_and_mips_match_workbench(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(bytes.fromhex(GOLDENS['decoder'][0]['source']))
            for row in GOLDENS['rows']:
                if row['quality'] != 1 or (not row['generate'] and row['filter'] != 0):
                    continue
                with self.subTest(cubemap=row['cubemap'], generate=row['generate'], filter=row['filter']):
                    result = subprocess.run([CLI, 'edds', 'convert', '--machine', '--protocol', '1',
                        '--input', str(source), '--output', str(output), '--format-compress', 'copy',
                        '--generate-mips', str(row['generate']).lower(), '--generate-cubemap', str(row['cubemap']).lower(),
                        '--mipmap-filter', 'kaiser' if row['filter'] == 2 else 'box',
                        '--tiled-texture', str(row['tiled']).lower()], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    data = output.read_bytes()
                    count = struct.unpack_from('<I', data, 28)[0]
                    faces = 6 if row['cubemap'] else 1
                    self.assertEqual(struct.unpack_from('<I', data, 112)[0], 0xfe00 if faces == 6 else 0)
                    captured = bytes.fromhex(row['float_container'])
                    self.assertEqual(data[:128], captured[:128])
                    offset = 128 + count * 8
                    for level in range(count - 1, -1, -1):
                        tag, size = struct.unpack_from('<4sI', data, 128 + (count - level - 1) * 8)
                        self.assertEqual(tag, b'COPY')
                        wanted = b''.join(bytes.fromhex(row['levels'][face * count + level]['rgba']) for face in range(faces))
                        self.assertEqual(size, len(wanted))
                        expected = struct.unpack('<' + 'f' * (size // 4), wanted)
                        actual = struct.unpack_from('<' + 'f' * (size // 4), data, offset)
                        for a, b in zip(actual, expected):
                            self.assertAlmostEqual(a, b, delta=max(0.00001, abs(b) * 0.000005))
                        offset += size
                    self.assertEqual(offset, len(data))

    def test_radiance_dynamic_range_survives_uncompressed_conversion(self):
        fixture = GOLDENS['decoder'][0]
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(bytes.fromhex(fixture['source']))
            result = subprocess.run([CLI, 'edds', 'convert', '--machine', '--protocol', '1',
                '--input', str(source), '--output', str(output), '--format-compress', 'copy',
                '--generate-mips', 'false'], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = output.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data, 84)[0], 116)
            self.assertEqual(data[128:132], b'COPY')
            pixels = struct.unpack('<' + 'f' * (fixture['width'] * fixture['height'] * 4), data[136:])
            rgb = [p for i, p in enumerate(pixels) if i % 4 != 3]
            self.assertEqual(rgb, fixture['rgb'])
            self.assertGreater(max(rgb), 1)


if __name__ == '__main__':
    unittest.main()
