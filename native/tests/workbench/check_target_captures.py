"""Independently read successful target captures; optionally exercise a packaged CLI.

Uses only Python's standard library. No converter or capture-oracle code is imported.
"""

import argparse
import base64
import hashlib
import json
import pathlib
import subprocess
import sys
import tempfile

from edds_reader import EddsFile


class ConverterCli:
    """The packaged executable, asked for machine-readable EDDS answers."""

    def __init__(self, path: pathlib.Path):
        self.path = path

    def edds(self, *arguments: str) -> dict:
        """The parsed JSON of one successful `enfusion edds` command."""
        result = subprocess.run(
            [str(self.path), 'edds', *arguments, '--machine', '--protocol', '1'],
            capture_output=True,
            text=True,
            check=True,
        )
        return json.loads(result.stdout)


class TargetCaptureCheck:
    """Checks a target capture against its own planted sources, and optionally against the CLI."""

    WORKBENCH_SHA256 = '74b74d1abe1f7f91e989d33dbfdcf97f9517a3a7df4dcb808c38991fd1df5cbe'
    COMPRESSIONS = ['Copy', 'Fastest', 'Medium', 'Best']

    # Of 7 sources x 3 targets x 4 compressions, the LZO target faults wherever it compresses: in
    # the three compressing modes, for the six sources of 64 bytes or more.
    WRITTEN = 66
    FAULTS = 18

    def __init__(self, capture: dict, cli: ConverterCli | None):
        self.capture = capture
        self.cli = cli

    def run(self) -> None:
        """Checks every row; a mismatch fails an assertion."""
        assert self.capture['executable_sha256'] == self.WORKBENCH_SHA256

        files = {digest: bytes.fromhex(encoded) for digest, encoded in self.capture['files'].items()}
        for digest, data in files.items():
            assert hashlib.sha256(data).hexdigest() == digest

        written = 0
        faults = 0
        results = {}
        with tempfile.TemporaryDirectory(prefix='enfusion-target-captures-') as directory:
            for index, row in enumerate(self.capture['rows']):
                source = self.capture['sources'][row['source']]
                result = row['result']
                results[(row['source'], row['compression'], row['target'])] = result

                if result['status'] == 'writer-fault':
                    self.check_fault(row, result)
                    faults += 1
                    continue

                assert result['status'] == 'written'
                data = files[result['edds_sha256']]
                containers = self.check_file(EddsFile(data), source)
                written += 1

                if self.cli:
                    path = pathlib.Path(directory) / f'{index}.edds'
                    path.write_bytes(data)
                    self.check_cli(path, source, containers)

        assert (written, faults) == (self.WRITTEN, self.FAULTS)
        self.check_targets_agree(results)

        print(
            f'{written} complete files decoded independently; {faults} LZO writer failures retained as failures.'
            + (' CLI inspect and every mip preview also matched.' if self.cli else '')
        )

    @staticmethod
    def check_fault(row: dict, result: dict) -> None:
        """An LZO writer failure is kept as a failure: no file, only the codec bytes before it."""
        assert row['target'] == 'EnfusionDDS_LZ0' and row['compression'] != 'Copy'
        assert result['fault_rva'] == '0x42a573'
        assert result['length_overlaps_table_pointer'] and result['table_pointer_zeroed']
        assert result['file_bytes_written'] == 0 and 'edds_sha256' not in result
        assert len(bytes.fromhex(result['codec_payload'])) == result['stored_bytes']

    @staticmethod
    def check_file(edds: EddsFile, source: dict) -> list[str]:
        """Checks the header and every planted sample; returns the containers, largest first."""
        levels = source['levels']
        first = levels[0]
        alpha = source['alpha']

        # A BGRA8 or BGRX8 header of the source's size and level count.
        assert edds.word(76) == 32
        assert (edds.width, edds.height, edds.mip_count) == (first['width'], first['height'], len(levels))
        assert edds.word(80) == (0x41 if alpha else 0x40)
        assert edds.word(88) == 32 and edds.word(20) == first['width'] * 4
        assert [edds.word(at) for at in (92, 96, 100, 104)] == [0xFF0000, 0xFF00, 0xFF, 0xFF000000 if alpha else 0]

        stored = edds.levels()
        for level, expected in zip(stored, levels):
            assert level.pixels == bytes.fromhex(expected['bgra'])

        return [level.container for level in stored]

    def check_cli(self, path: pathlib.Path, source: dict, containers: list[str]) -> None:
        """The CLI reports the same containers and previews every level as the planted RGBA."""
        inspection = self.cli.edds('inspect', '--input', str(path))
        assert [mip['container'] for mip in inspection['mips']] == containers

        for level, expected in enumerate(source['levels']):
            preview = self.cli.edds('preview', '--input', str(path), '--mip', str(level))
            assert base64.b64decode(preview['pixelsBase64']) == self.rgba_of(bytes.fromhex(expected['bgra']), source['alpha'])

    @staticmethod
    def rgba_of(bgra: bytes, alpha: bool) -> bytes:
        """BGRA samples as the preview's RGBA; BGRX reads as opaque."""
        return bytes(
            value for at in range(0, len(bgra), 4) for value in (bgra[at + 2], bgra[at + 1], bgra[at], bgra[at + 3] if alpha else 255)
        )

    def check_targets_agree(self, results: dict) -> None:
        """EnfusionDDS writes what EnfusionDDS_LZ4 writes, and for Copy what the LZO one writes."""
        for source in range(len(self.capture['sources'])):
            for compression in self.COMPRESSIONS:
                assert results[(source, compression, 'EnfusionDDS')] == results[(source, compression, 'EnfusionDDS_LZ4')]
            assert results[(source, 'Copy', 'EnfusionDDS')] == results[(source, 'Copy', 'EnfusionDDS_LZ0')]

    @classmethod
    def main(cls, arguments: list[str]) -> int:
        """The command line: a capture file and, optionally, the CLI to compare with."""
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument('capture', type=pathlib.Path)
        parser.add_argument('--cli', type=pathlib.Path)
        options = parser.parse_args(arguments)

        capture = json.loads(options.capture.read_text(encoding='utf-8'))
        cls(capture, ConverterCli(options.cli.resolve()) if options.cli else None).run()
        return 0


if __name__ == '__main__':
    sys.exit(TargetCaptureCheck.main(sys.argv[1:]))
