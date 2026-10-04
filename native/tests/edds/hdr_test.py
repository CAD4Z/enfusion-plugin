"""Black-box HDR tests; the independent reader uses only Python's standard library."""
import base64
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import unittest

CLI = sys.argv.pop(1)
GOLDENS = json.loads((pathlib.Path(__file__).parents[1] / 'workbench/hdr-goldens.json').read_text())

# The weights of BC6H's 4-bit indices, out of 64, as the format specifies them.
WEIGHTS = [0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64]


def mode11(block):
    """The 16 RGB samples of one mode-11 BC6H block, decoded from the format's specification alone."""
    bits = int.from_bytes(block, 'little')
    if bits & 31 != 3:
        raise AssertionError(f'expected a mode-11 block, found mode code {bits & 31}')

    def unquantize(value):
        return 0 if value == 0 else 65535 if value == 1023 else ((value << 16) + 32768) >> 10

    def half(a, b, weight):
        bits16 = (((a * (64 - weight) + b * weight + 32) >> 6) * 31) >> 6
        return struct.unpack('<e', struct.pack('<H', bits16))[0]

    endpoints = [unquantize((bits >> (5 + i * 10)) & 1023) for i in range(6)]
    samples, at = [], 65
    for pixel in range(16):
        width = 3 if pixel == 0 else 4
        weight = WEIGHTS[(bits >> at) & ((1 << width) - 1)]
        at += width
        samples.append([half(endpoints[c], endpoints[c + 3], weight) for c in range(3)])
    return samples


def bc6_surface(data, width, height):
    """The RGB samples of one BC6H surface, row by row, each 4x4 block decoded with mode11."""
    across = (width + 3) // 4
    rows = [[None] * width for _ in range(height)]
    for by in range((height + 3) // 4):
        for bx in range(across):
            for i, rgb in enumerate(mode11(data[(by * across + bx) * 16:][:16])):
                x, y = bx * 4 + i % 4, by * 4 + i // 4
                if x < width and y < height:
                    rows[y][x] = rgb
    return [rgb for row in rows for rgb in row]


def levels_of(data):
    """(width, height, stored bytes) of every level of a COPY EDDS, largest first, from its own header."""
    header = 148 if data[84:88] == b'DX10' else 128
    height, width = struct.unpack_from('<II', data, 12)
    count = struct.unpack_from('<I', data, 28)[0]
    offset, stored = header + count * 8, {}
    for entry in range(count):
        tag, size = struct.unpack_from('<4sI', data, header + entry * 8)
        assert tag == b'COPY', tag
        stored[count - entry - 1] = data[offset:offset + size]
        offset += size
    assert offset == len(data)
    return [(max(width >> level, 1), max(height >> level, 1), stored[level]) for level in range(count)]


def floats_of(hex_text):
    data = bytes.fromhex(hex_text)
    return struct.unpack('<' + 'f' * (len(data) // 4), data)


def edds(*args):
    return subprocess.run([CLI, 'edds', *args], capture_output=True, text=True)


def convert(source, output, *profile):
    return edds('convert', '--machine', '--protocol', '1', '--input', str(source), '--output', str(output), *profile)


class HdrConversion(unittest.TestCase):
    def test_all_captured_rgbe_encodings_preserve_float_samples(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            for fixture in GOLDENS['decoder']:
                source.write_bytes(bytes.fromhex(fixture['source']))
                result = convert(source, output, '--format-compress', 'copy', '--generate-mips', 'false')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                pixels = struct.unpack('<' + 'f' * (fixture['width']*fixture['height']*4), output.read_bytes()[136:])
                self.assertEqual([p for i,p in enumerate(pixels) if i%4!=3], fixture['rgb'])

    def test_quality_refines_a_colour_ramp_without_changing_hdr_format(self):
        rgbe = bytes(v for y in range(4) for x in range(4) for v in [32+x*32,160-x*32,32,132])
        expected = [v/16 for i,v in enumerate(rgbe) if i%4!=3]
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'ramp.hdr'; output = source.with_suffix('.edds')
            source.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n'+rgbe)
            errors = []
            for quality in ['0','1']:
                result = convert(source, output, '--conversion', 'hdr-compression', '--conversion-quality', quality,
                    '--format-compress', 'copy', '--generate-mips', 'false')
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertEqual(json.loads(result.stdout)['pixelFormat'], 'BC6H')
                # The block is decoded here, from the specification, not by the converter's own decoder.
                width, height, stored = levels_of(output.read_bytes())[0]
                rgb = [value for sample in bc6_surface(stored, width, height) for value in sample]
                errors.append(sum(((a-b)/(1+b))**2 for a,b in zip(rgb,expected))/len(rgb))
                self.assertGreater(max(rgb),1)
            self.assertLess(errors[1], errors[0])
            self.assertLess(errors[1], 0.005)

    def test_workbench_bc6h_container_topology_is_readable(self):
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'workbench.edds'
            for row in GOLDENS['containers']:
                output.write_bytes(bytes.fromhex(row['bc6_container']))
                result = edds('inspect', '--machine', '--protocol', '1', '--input', str(output))
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
            inspection = edds('inspect', '--machine', '--protocol', '1', '--input', str(output), '--metadata', str(meta))
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
            preview = edds('preview', '--machine', '--protocol', '1', '--input', str(output), '--mip', '0', '--all-faces')
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
                    result = edds('preview', '--machine', '--protocol', '1', '--input', str(output), '--mip', '0', '--float')
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    values = struct.unpack('<64f', base64.b64decode(json.loads(result.stdout)['pixelsBase64']))
                    self.assertEqual([v for i,v in enumerate(values) if i%4!=3], row['rgb'])

    def test_bc6h_preserves_hdr_and_reports_actual_float_preview(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'constant.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(b'#?RADIANCE\nFORMAT=32-bit_rle_rgbe\n\n-Y 4 +X 4\n' + bytes([128, 64, 32, 132]) * 16)
            result = convert(source, output, '--conversion', 'hdr-compression', '--format-compress', 'copy', '--generate-mips', 'false')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = output.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data, 128)[0], 95)
            samples = mode11(data[156:172])
            for sample in samples:
                for value, target in zip(sample, [8, 4, 2]):
                    self.assertAlmostEqual(value, target, delta=target * 0.025)
            result = edds('preview', '--machine', '--protocol', '1', '--input', str(output), '--mip', '0', '--float')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            actual = struct.unpack('<64f', base64.b64decode(json.loads(result.stdout)['pixelsBase64']))
            self.assertEqual(list(actual), [value for sample in samples for value in sample + [1.0]])

    def test_float_faces_and_mips_match_workbench(self):
        # Conversion=None keeps the float chain; every captured filter, tiling, cube and mip choice.
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(bytes.fromhex(GOLDENS['decoder'][0]['source']))
            rows = [row for row in GOLDENS['rows'] if row['conversion'] == 0]
            self.assertEqual(len(rows), 10)
            for row in rows:
                with self.subTest(cubemap=row['cubemap'], generate=row['generate'], filter=row['filter'], tiled=row['tiled']):
                    result = convert(source, output, '--format-compress', 'copy',
                        '--generate-mips', str(row['generate']).lower(), '--generate-cubemap', str(row['cubemap']).lower(),
                        '--mipmap-filter', 'kaiser' if row['filter'] == 2 else 'box',
                        '--tiled-texture', str(row['tiled']).lower())
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    data = output.read_bytes()
                    faces = 6 if row['cubemap'] else 1
                    self.assertEqual(data[:128], bytes.fromhex(row['float_container'])[:128])
                    levels = levels_of(data)
                    for level, (width, height, stored) in enumerate(levels):
                        wanted = b''.join(bytes.fromhex(row['levels'][face * len(levels) + level]['rgba']) for face in range(faces))
                        self.assertEqual(len(stored), len(wanted))
                        expected = struct.unpack('<' + 'f' * (len(wanted) // 4), wanted)
                        actual = struct.unpack('<' + 'f' * (len(stored) // 4), stored)
                        for a, b in zip(actual, expected):
                            self.assertAlmostEqual(a, b, delta=max(0.00001, abs(b) * 0.000005))

    def test_bc6h_faces_and_mips_stay_within_a_bounded_error_of_workbench_input(self):
        # Workbench's own BC6H blocks are not captured: its float input to the encoder is. Every face
        # and level the converter writes is decoded here and held to that input. The bound is the
        # mode-11 encoder's: a block whose colours do not lie on one line keeps up to about 29%
        # relative error (measured 0.29 at most, mean square 0.015), so this catches a broken face,
        # level or block, not a lost refinement. Without mips the filter is moot and refused.
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(bytes.fromhex(GOLDENS['decoder'][0]['source']))
            rows = [row for row in GOLDENS['rows']
                    if row['conversion'] == 7 and row['quality'] == 1 and (row['generate'] or row['filter'] == 0)]
            self.assertEqual(len(rows), 10)
            for row in rows:
                with self.subTest(cubemap=row['cubemap'], generate=row['generate'], filter=row['filter'], tiled=row['tiled']):
                    result = convert(source, output, '--conversion', 'hdr-compression', '--format-compress', 'copy',
                        '--generate-mips', str(row['generate']).lower(), '--generate-cubemap', str(row['cubemap']).lower(),
                        '--mipmap-filter', 'kaiser' if row['filter'] == 2 else 'box',
                        '--tiled-texture', str(row['tiled']).lower())
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    levels = levels_of(output.read_bytes())
                    faces = 6 if row['cubemap'] else 1
                    for level, (width, height, stored) in enumerate(levels):
                        per_face = len(stored) // faces
                        for face in range(faces):
                            decoded = bc6_surface(stored[face * per_face:][:per_face], width, height)
                            wanted = floats_of(row['levels'][face * len(levels) + level]['rgba'])
                            errors = [abs(a - b) / (1 + b) for i, sample in enumerate(decoded)
                                      for a, b in zip(sample, wanted[i * 4:i * 4 + 3])]
                            self.assertLess(max(errors), 0.35, (face, level))
                            self.assertLess(sum(e * e for e in errors) / len(errors), 0.02, (face, level))

    def test_sizes_and_panoramas_are_accepted_exactly_as_workbench_accepts_them(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            for row in GOLDENS['dimensions']:
                with self.subTest(conversion=row['conversion'], width=row['width'], height=row['height']):
                    source.write_bytes(bytes.fromhex(row['source']))
                    result = convert(source, output, '--conversion', 'hdr-compression' if row['conversion'] == 7 else 'none',
                        '--format-compress', 'copy', '--generate-mips', 'true')
                    if not row['accepted']:
                        self.assertEqual(result.returncode, 4, result.stdout)
                        self.assertEqual(json.loads(result.stdout)['error']['code'], 'unsupported-hdr-dimensions')
                        continue
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    if 'levels' not in row:
                        continue
                    levels = levels_of(output.read_bytes())
                    self.assertEqual([(w, h) for w, h, _ in levels], [(l['width'], l['height']) for l in row['levels']])
                    for (_, _, stored), wanted in zip(levels, row['levels']):
                        expected = floats_of(wanted['rgba'])
                        actual = struct.unpack('<' + 'f' * (len(stored) // 4), stored)
                        self.assertEqual(len(actual), len(expected))
                        for a, b in zip(actual, expected):
                            self.assertAlmostEqual(a, b, delta=max(0.00001, abs(b) * 0.000005))
            for row in GOLDENS['panoramas']:
                with self.subTest(panorama=(row['width'], row['height'])):
                    source.write_bytes(bytes.fromhex(row['source']))
                    result = convert(source, output, '--conversion', 'hdr-compression', '--generate-cubemap', 'true',
                        '--generate-mips', 'false')
                    if row['accepted']:
                        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                    else:
                        self.assertEqual(result.returncode, 4, result.stdout)
                        self.assertEqual(json.loads(result.stdout)['error']['code'], 'unsupported-cubemap-topology')

    def test_radiance_above_the_half_float_range_saturates_like_workbench(self):
        # Workbench passes such radiance to its encoder unchanged; the encoder saturates it.
        captured = GOLDENS['above_half']
        self.assertGreater(captured['encoder_input_max'], 65504)
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'sun.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(bytes.fromhex(captured['source']))
            result = convert(source, output, '--conversion', 'hdr-compression', '--format-compress', 'copy', '--generate-mips', 'false')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            width, height, stored = levels_of(output.read_bytes())[0]
            decoded = bc6_surface(stored, width, height)
            self.assertEqual(max(max(sample) for sample in decoded), 65504)
            for i, sample in enumerate(decoded):
                if i % width < 2:
                    self.assertEqual(sample, [65504, 65504, 65504], i)

    def test_radiance_dynamic_range_survives_uncompressed_conversion(self):
        fixture = GOLDENS['decoder'][0]
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(bytes.fromhex(fixture['source']))
            result = convert(source, output, '--format-compress', 'copy', '--generate-mips', 'false')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = output.read_bytes()
            self.assertEqual(struct.unpack_from('<I', data, 84)[0], 116)
            self.assertEqual(data[128:132], b'COPY')
            pixels = struct.unpack('<' + 'f' * (fixture['width'] * fixture['height'] * 4), data[136:])
            rgb = [p for i, p in enumerate(pixels) if i % 4 != 3]
            self.assertEqual(rgb, fixture['rgb'])
            self.assertGreater(max(rgb), 1)

    def test_a_float_sample_that_is_not_a_number_is_refused_by_name(self):
        with tempfile.TemporaryDirectory() as directory:
            source = pathlib.Path(directory) / 'source.hdr'
            output = source.with_suffix('.edds')
            source.write_bytes(bytes.fromhex(GOLDENS['decoder'][0]['source']))
            result = convert(source, output, '--format-compress', 'copy', '--generate-mips', 'false')
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            data = bytearray(output.read_bytes())
            data[136:140] = struct.pack('<f', float('nan'))
            output.write_bytes(bytes(data))
            for flag in [[], ['--float']]:
                result = edds('preview', '--machine', '--protocol', '1', '--input', str(output), '--mip', '0', *flag)
                self.assertEqual(result.returncode, 3, result.stdout)
                self.assertEqual(json.loads(result.stdout)['error']['code'], 'invalid-float-sample')

    def test_all_faces_beyond_the_preview_limit_return_the_first_face_and_say_why(self):
        # A DXT1 cube of six 1680x1680 faces: each face fits the preview, all six would not.
        side = 1680
        face = side * side // 2
        header = bytearray(128)
        header[0:4] = b'DDS '
        struct.pack_into('<7I', header, 4, 124, 0x000a1007, side, side, face, 0, 1)
        header[36:40] = b'ENF1'
        struct.pack_into('<2I4s', header, 76, 32, 0x4, b'DXT1')
        struct.pack_into('<2I', header, 108, 0x1008, 0xfe00)
        with tempfile.TemporaryDirectory() as directory:
            output = pathlib.Path(directory) / 'cube.edds'
            output.write_bytes(bytes(header) + b'COPY' + struct.pack('<I', face * 6) + bytes(face * 6))
            result = edds('preview', '--machine', '--protocol', '1', '--input', str(output), '--mip', '0', '--all-faces')
            self.assertEqual(result.returncode, 0, result.stdout[:500] + result.stderr)
            preview = json.loads(result.stdout)
            self.assertNotIn('facesBase64', preview)
            self.assertIn('only +X', preview['facesOmitted'])
            self.assertEqual(len(base64.b64decode(preview['pixelsBase64'])), side * side * 4)


if __name__ == '__main__':
    unittest.main()
