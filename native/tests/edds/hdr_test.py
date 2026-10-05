"""Black-box HDR tests; the independent reader uses only Python's standard library.

usage: py hdr_test.py ENFUSION_EXE [unittest options]
"""

import base64
import json
import pathlib
import random
import struct
import sys
import tempfile
import unittest

from hdr_harness import Bc6hMode11, Converter, CopyEdds, HdrGoldens, RadianceFile


class HdrConversion(unittest.TestCase):
    """HDR conversion, inspection and preview against Workbench's captures and the specs."""

    converter: Converter

    # Profile flags most tests share.
    COPY = ('--format-compress', 'copy')
    NO_MIPS = ('--generate-mips', 'false')
    HDR_COMPRESSION = ('--conversion', 'hdr-compression')

    # A registered conversion: the .edds.meta and the identity it records.
    RESOURCE_NAME = 'Fixture/source.edds'
    GUID = '0123456789ABCDEF'

    @classmethod
    def setUpClass(cls):
        cls.goldens = HdrGoldens.workbench()

    def setUp(self):
        self.folder = pathlib.Path(self.enterContext(tempfile.TemporaryDirectory()))
        self.source = self.folder / 'source.hdr'
        self.output = self.source.with_suffix('.edds')
        self.meta = self.source.with_suffix('.edds.meta')

    def expect_success(self, result) -> None:
        """The command exited 0; its output says why when it did not."""
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def expect_refusal(self, result, exit_code: int, code: str) -> None:
        """The command exited `exit_code` with the machine error `code`."""
        self.assertEqual(result.returncode, exit_code, result.stdout)
        self.assertEqual(json.loads(result.stdout)['error']['code'], code)

    def expect_floats_close(self, actual, expected) -> None:
        """Float samples within the tolerance of a float pipeline that matches Workbench."""
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, delta=max(0.00001, abs(b) * 0.000005))

    def registration(self) -> list[str]:
        """The flags that make a conversion write its .edds.meta with a fixed identity."""
        return ['--metadata', str(self.meta), '--resource-name', self.RESOURCE_NAME, '--source-file', self.source.name, '--guid', self.GUID]

    @staticmethod
    def mip_profile(row: dict) -> list[str]:
        """The mip, cube, filter and tiling flags of one captured importer row."""
        return [
            '--generate-mips',
            str(row['generate']).lower(),
            '--generate-cubemap',
            str(row['cubemap']).lower(),
            '--mipmap-filter',
            'kaiser' if row['filter'] == 2 else 'box',
            '--tiled-texture',
            str(row['tiled']).lower(),
        ]

    def test_all_captured_rgbe_encodings_preserve_float_samples(self):
        for fixture in self.goldens['decoder']:
            self.source.write_bytes(bytes.fromhex(fixture['source']))
            result = self.converter.convert(self.source, self.output, *self.COPY, *self.NO_MIPS)
            self.expect_success(result)

            pixels = CopyEdds.floats(self.output.read_bytes()[136:])
            self.assertEqual(len(pixels), fixture['width'] * fixture['height'] * 4)
            self.assertEqual(CopyEdds.rgb(pixels), fixture['rgb'])

    def test_quality_refines_a_colour_ramp_without_changing_hdr_format(self):
        rgbe = bytes(value for y in range(4) for x in range(4) for value in [32 + x * 32, 160 - x * 32, 32, 132])
        expected = [value / 16 for value in CopyEdds.rgb(rgbe)]
        self.source.write_bytes(RadianceFile.flat(4, 4, rgbe))

        errors = []
        for quality in ['0', '1']:
            result = self.converter.convert(
                self.source, self.output, *self.HDR_COMPRESSION, '--conversion-quality', quality, *self.COPY, *self.NO_MIPS
            )
            self.expect_success(result)
            self.assertEqual(json.loads(result.stdout)['pixelFormat'], 'BC6H')

            # The block is decoded here, from the specification, not by the converter's own decoder.
            width, height, stored = CopyEdds.levels(self.output.read_bytes())[0]
            rgb = [value for sample in Bc6hMode11.surface(stored, width, height) for value in sample]
            errors.append(sum(((a - b) / (1 + b)) ** 2 for a, b in zip(rgb, expected)) / len(rgb))
            self.assertGreater(max(rgb), 1)

        self.assertLess(errors[1], errors[0])
        self.assertLess(errors[1], 0.005)

    def test_workbench_bc6h_container_topology_is_readable(self):
        for row in self.goldens['containers']:
            self.output.write_bytes(bytes.fromhex(row['bc6_container']))
            result = self.converter.inspect(self.output)
            self.expect_success(result)

            facts = json.loads(result.stdout)
            self.assertEqual(facts['dds']['arraySize'], row['faces'])
            self.assertEqual(facts['faceCount'], row['faces'])
            self.assertTrue(facts['previewSupported'])

    def test_parser_subtypes_truncations_and_oversized_headers_never_mutate_a_pair(self):
        fixture = bytes.fromhex(self.goldens['decoder'][0]['source'])
        cases = [
            b'',
            b'#?RADIANCE\n',
            fixture[:-1],
            fixture + b'junk',
            fixture.replace(b'-Y 8 +X 16', b'-Y 32768 +X 32768'),
            fixture.replace(b'FORMAT=32-bit_rle_rgbe', b'FORMAT=32-bit_rle_xyze'),
            fixture.replace(b'-Y', b'+Y'),
            fixture.replace(b'+X', b'-X'),
            fixture.replace(b'FORMAT=', b'EXPOSURE=2\nFORMAT='),
        ]
        for row in self.goldens['decoder']:
            data = bytes.fromhex(row['source'])
            cases += [data[:end] for end in random.Random(808).sample(range(len(data)), 20)]

        # A registered pair from the valid source, which no refused conversion may touch.
        self.source.write_bytes(fixture)
        self.expect_success(self.converter.convert(self.source, self.output, *self.registration()))
        pair = (self.output.read_bytes(), self.meta.read_bytes())

        refusals = ['invalid-hdr', 'unsupported-hdr-subtype', 'unsupported-hdr-orientation', 'image-size-limit', 'trailing-hdr-data']
        for data in cases:
            self.source.write_bytes(data)
            result = self.converter.convert(self.source, self.output, *self.registration())
            self.assertIn(result.returncode, [3, 4], result.stdout + result.stderr)
            self.assertIn(json.loads(result.stdout)['error']['code'], refusals)
            self.assertEqual((self.output.read_bytes(), self.meta.read_bytes()), pair)
            self.assertEqual(sorted(path.name for path in self.folder.iterdir()), ['source.edds', 'source.edds.meta', 'source.hdr'])

    def test_hdr_recipe_round_trip_faces_stale_input_and_cancellation(self):
        self.source.write_bytes(bytes.fromhex(self.goldens['decoder'][0]['source']))
        profile = [*self.registration(), *self.HDR_COMPRESSION, '--generate-cubemap', 'true', '--conversion-quality', '0.403']
        self.expect_success(self.converter.convert(self.source, self.output, *profile))

        # The recipe, the cube's header and the container's start as Workbench writes them.
        inspection = self.converter.inspect(self.output, '--metadata', str(self.meta))
        self.expect_success(inspection)
        facts = json.loads(inspection.stdout)
        self.assertEqual(facts['faceCount'], 6)
        self.assertEqual(facts['dds']['arraySize'], 6)
        self.assertEqual(facts['dds']['pitchOrLinearSize'], 16)
        self.assertEqual(self.output.read_bytes()[:148], bytes.fromhex(self.goldens['containers'][1]['bc6_container'])[:148])
        self.assertEqual(facts['metadata']['recipe']['GenerateCubemap'], True)
        self.assertEqual(facts['metadata']['recipe']['ConversionQuality'], 0.403)
        self.assertEqual(facts['pixelFormat'], 'BC6H')
        self.assertIn('HDRResourceClass', self.meta.read_text())

        # All six faces preview, and they differ.
        preview = self.converter.preview(self.output, '--mip', '0', '--all-faces')
        self.expect_success(preview)
        faces = json.loads(preview.stdout)['facesBase64']
        self.assertEqual(len(faces), 6)
        self.assertEqual(len(base64.b64decode(faces[0])), 4 * 4 * 4)
        self.assertGreater(len(set(faces)), 1)

        # A stale revision, a cancellation and a refused profile all leave the pair as it was.
        pair = (self.output.read_bytes(), self.meta.read_bytes())
        cancel = self.source.with_name('cancel')
        cancel.write_text('cancel')
        stale = ['--expect-source-revision', '0:0', '--expect-output-revision', '0:0', '--expect-metadata-revision', '0:0']
        for extra in [stale, ['--cancel-file', str(cancel)], ['--swizzling', 'alpha-to-rgb']]:
            result = self.converter.convert(self.source, self.output, *profile, *extra)
            self.assertNotEqual(result.returncode, 0, result.stdout)
            self.assertEqual((self.output.read_bytes(), self.meta.read_bytes()), pair)
            self.assertEqual(len(list(self.folder.iterdir())), 4)

    def test_all_bc6h_modes_against_independent_float_decoder(self):
        # A one-block BC6H file whose block is replaced by each golden block in turn.
        self.source.write_bytes(RadianceFile.constant())
        self.expect_success(self.converter.convert(self.source, self.output, *self.HDR_COMPRESSION, *self.COPY, *self.NO_MIPS))
        header = self.output.read_bytes()[:156]

        for row in HdrGoldens.bc6_blocks():
            with self.subTest(mode=row['mode'], block=row['block']):
                self.output.write_bytes(header + bytes.fromhex(row['block']))
                result = self.converter.preview(self.output, '--mip', '0', '--float')
                self.expect_success(result)
                values = struct.unpack('<64f', base64.b64decode(json.loads(result.stdout)['pixelsBase64']))
                self.assertEqual(CopyEdds.rgb(values), row['rgb'])

    def test_bc6h_preserves_hdr_and_reports_actual_float_preview(self):
        self.source.write_bytes(RadianceFile.constant())
        self.expect_success(self.converter.convert(self.source, self.output, *self.HDR_COMPRESSION, *self.COPY, *self.NO_MIPS))

        # BC6H (DXGI 95), whose one block holds the colour within 2.5%.
        data = self.output.read_bytes()
        self.assertEqual(struct.unpack_from('<I', data, 128)[0], 95)
        samples = Bc6hMode11.block(data[156:172])
        for sample in samples:
            for value, target in zip(sample, [8, 4, 2]):
                self.assertAlmostEqual(value, target, delta=target * 0.025)

        # The float preview reports exactly the stored block's samples, opaque.
        result = self.converter.preview(self.output, '--mip', '0', '--float')
        self.expect_success(result)
        actual = struct.unpack('<64f', base64.b64decode(json.loads(result.stdout)['pixelsBase64']))
        self.assertEqual(list(actual), [value for sample in samples for value in sample + [1.0]])

    def test_float_faces_and_mips_match_workbench(self):
        # Conversion=None keeps the float chain; every captured filter, tiling, cube and mip choice.
        self.source.write_bytes(bytes.fromhex(self.goldens['decoder'][0]['source']))
        rows = [row for row in self.goldens['rows'] if row['conversion'] == 0]
        self.assertEqual(len(rows), 10)

        for row in rows:
            with self.subTest(cubemap=row['cubemap'], generate=row['generate'], filter=row['filter'], tiled=row['tiled']):
                result = self.converter.convert(self.source, self.output, *self.COPY, *self.mip_profile(row))
                self.expect_success(result)

                data = self.output.read_bytes()
                faces = 6 if row['cubemap'] else 1
                self.assertEqual(data[:128], bytes.fromhex(row['float_container'])[:128])
                levels = CopyEdds.levels(data)
                for level, (_, _, stored) in enumerate(levels):
                    wanted = b''.join(bytes.fromhex(row['levels'][face * len(levels) + level]['rgba']) for face in range(faces))
                    self.assertEqual(len(stored), len(wanted))
                    self.expect_floats_close(CopyEdds.floats(stored), CopyEdds.floats(wanted))

    def test_bc6h_faces_and_mips_stay_within_a_bounded_error_of_workbench_input(self):
        # Workbench's own BC6H blocks are not captured: its float input to the encoder is. Every
        # face and level the converter writes is decoded here and held to that input. The bound is
        # the mode-11 encoder's: a block whose colours do not lie on one line keeps up to about 29%
        # relative error (measured 0.29 at most, mean square 0.015), so this catches a broken face,
        # level or block, not a lost refinement. Without mips the filter is moot and refused.
        self.source.write_bytes(bytes.fromhex(self.goldens['decoder'][0]['source']))
        rows = [
            row
            for row in self.goldens['rows']
            if row['conversion'] == 7 and row['quality'] == 1 and (row['generate'] or row['filter'] == 0)
        ]
        self.assertEqual(len(rows), 10)

        for row in rows:
            with self.subTest(cubemap=row['cubemap'], generate=row['generate'], filter=row['filter'], tiled=row['tiled']):
                result = self.converter.convert(self.source, self.output, *self.HDR_COMPRESSION, *self.COPY, *self.mip_profile(row))
                self.expect_success(result)

                levels = CopyEdds.levels(self.output.read_bytes())
                faces = 6 if row['cubemap'] else 1
                for level, (width, height, stored) in enumerate(levels):
                    per_face = len(stored) // faces
                    for face in range(faces):
                        decoded = Bc6hMode11.surface(stored[face * per_face :][:per_face], width, height)
                        wanted = CopyEdds.floats(bytes.fromhex(row['levels'][face * len(levels) + level]['rgba']))
                        errors = [
                            abs(a - b) / (1 + b)
                            for index, sample in enumerate(decoded)
                            for a, b in zip(sample, wanted[index * 4 : index * 4 + 3])
                        ]
                        self.assertLess(max(errors), 0.35, (face, level))
                        self.assertLess(sum(error * error for error in errors) / len(errors), 0.02, (face, level))

    def test_sizes_and_panoramas_are_accepted_exactly_as_workbench_accepts_them(self):
        for row in self.goldens['dimensions']:
            with self.subTest(conversion=row['conversion'], width=row['width'], height=row['height']):
                self.source.write_bytes(bytes.fromhex(row['source']))
                conversion = 'hdr-compression' if row['conversion'] == 7 else 'none'
                result = self.converter.convert(self.source, self.output, '--conversion', conversion, *self.COPY, '--generate-mips', 'true')
                if not row['accepted']:
                    self.expect_refusal(result, 4, 'unsupported-hdr-dimensions')
                    continue

                self.expect_success(result)
                if 'levels' not in row:
                    continue

                # Conversion=None: the float levels match the importer's own.
                levels = CopyEdds.levels(self.output.read_bytes())
                self.assertEqual(
                    [(width, height) for width, height, _ in levels], [(level['width'], level['height']) for level in row['levels']]
                )
                for (_, _, stored), wanted in zip(levels, row['levels']):
                    expected = CopyEdds.floats(bytes.fromhex(wanted['rgba']))
                    actual = CopyEdds.floats(stored)
                    self.assertEqual(len(actual), len(expected))
                    self.expect_floats_close(actual, expected)

        for row in self.goldens['panoramas']:
            with self.subTest(panorama=(row['width'], row['height'])):
                self.source.write_bytes(bytes.fromhex(row['source']))
                result = self.converter.convert(
                    self.source, self.output, *self.HDR_COMPRESSION, '--generate-cubemap', 'true', *self.NO_MIPS
                )
                if row['accepted']:
                    self.expect_success(result)
                else:
                    self.expect_refusal(result, 4, 'unsupported-cubemap-topology')

    def test_radiance_above_the_half_float_range_saturates_like_workbench(self):
        # Workbench passes such radiance to its encoder unchanged; the encoder saturates it.
        captured = self.goldens['above_half']
        self.assertGreater(captured['encoder_input_max'], 65504)
        self.source.write_bytes(bytes.fromhex(captured['source']))
        self.expect_success(self.converter.convert(self.source, self.output, *self.HDR_COMPRESSION, *self.COPY, *self.NO_MIPS))

        width, height, stored = CopyEdds.levels(self.output.read_bytes())[0]
        decoded = Bc6hMode11.surface(stored, width, height)
        self.assertEqual(max(max(sample) for sample in decoded), 65504)
        for index, sample in enumerate(decoded):
            if index % width < 2:
                self.assertEqual(sample, [65504, 65504, 65504], index)

    def test_radiance_dynamic_range_survives_uncompressed_conversion(self):
        fixture = self.goldens['decoder'][0]
        self.source.write_bytes(bytes.fromhex(fixture['source']))
        self.expect_success(self.converter.convert(self.source, self.output, *self.COPY, *self.NO_MIPS))

        # D3DFMT_A32B32G32R32F (116) in one COPY level.
        data = self.output.read_bytes()
        self.assertEqual(struct.unpack_from('<I', data, 84)[0], 116)
        self.assertEqual(data[128:132], b'COPY')
        pixels = CopyEdds.floats(data[136:])
        self.assertEqual(len(pixels), fixture['width'] * fixture['height'] * 4)
        rgb = CopyEdds.rgb(pixels)
        self.assertEqual(rgb, fixture['rgb'])
        self.assertGreater(max(rgb), 1)

    def test_a_float_sample_that_is_not_a_number_is_refused_by_name(self):
        self.source.write_bytes(bytes.fromhex(self.goldens['decoder'][0]['source']))
        self.expect_success(self.converter.convert(self.source, self.output, *self.COPY, *self.NO_MIPS))

        data = bytearray(self.output.read_bytes())
        data[136:140] = struct.pack('<f', float('nan'))
        self.output.write_bytes(bytes(data))
        for flag in [[], ['--float']]:
            self.expect_refusal(self.converter.preview(self.output, '--mip', '0', *flag), 3, 'invalid-float-sample')

    def test_all_faces_beyond_the_preview_limit_return_the_first_face_and_say_why(self):
        # A DXT1 cube of six 1680x1680 faces: each face fits the preview, all six would not.
        side = 1680
        face = side * side // 2
        header = bytearray(128)
        header[0:4] = b'DDS '
        struct.pack_into('<7I', header, 4, 124, 0x000A1007, side, side, face, 0, 1)
        header[36:40] = b'ENF1'
        struct.pack_into('<2I4s', header, 76, 32, 0x4, b'DXT1')
        struct.pack_into('<2I', header, 108, 0x1008, 0xFE00)
        cube = self.folder / 'cube.edds'
        cube.write_bytes(bytes(header) + b'COPY' + struct.pack('<I', face * 6) + bytes(face * 6))

        result = self.converter.preview(cube, '--mip', '0', '--all-faces')
        self.assertEqual(result.returncode, 0, result.stdout[:500] + result.stderr)
        preview = json.loads(result.stdout)
        self.assertNotIn('facesBase64', preview)
        self.assertIn('only +X', preview['facesOmitted'])
        self.assertEqual(len(base64.b64decode(preview['pixelsBase64'])), side * side * 4)

    @classmethod
    def main(cls, arguments: list[str]) -> int:
        """The command line: the executable under test, then unittest's own options."""
        cls.converter = Converter(arguments[0])
        program = unittest.main(argv=[sys.argv[0], *arguments[1:]], exit=False)
        return 0 if program.result.wasSuccessful() else 1


if __name__ == '__main__':
    sys.exit(HdrConversion.main(sys.argv[1:]))
