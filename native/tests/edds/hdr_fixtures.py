"""Materialize owned HDR and BC6H seeds; also useful for manual Workbench/DayZ smoke.

usage: py hdr_fixtures.py ENFUSION_EXE OUTPUT_FOLDER
"""

import argparse
import pathlib
import sys

from hdr_harness import Converter, HdrGoldens, RadianceFile


class HdrFixtures:
    """The captured Radiance sources, their conversions, and one BC6H file per block mode."""

    def __init__(self, converter: Converter, folder: pathlib.Path):
        self.converter = converter
        self.folder = folder

    def convert(self, source: pathlib.Path, output: pathlib.Path, *profile: str) -> None:
        """Converts or stops the script with the converter's own report."""
        result = self.converter.convert(source, output, *profile)
        if result.returncode != 0:
            raise SystemExit(result.stdout + result.stderr)

    def write(self) -> None:
        """Writes every fixture into the folder."""
        self.folder.mkdir(parents=True, exist_ok=True)

        # The sources Workbench's decoder was captured on, as they are.
        for index, row in enumerate(HdrGoldens.workbench()['decoder']):
            (self.folder / f'source-{index}.hdr').write_bytes(bytes.fromhex(row['source']))

        # The first source as a float and a BC6H texture, flat and as a cube.
        for cube in [False, True]:
            for conversion in ['none', 'hdr-compression']:
                output = self.folder / f'hdr-{conversion}-{"cube" if cube else "2d"}.edds'
                self.convert(
                    self.folder / 'source-0.hdr',
                    output,
                    '--conversion',
                    conversion,
                    '--generate-cubemap',
                    str(cube).lower(),
                    '--format-compress',
                    'copy',
                )

        # A one-block BC6H file, then the same header around a golden block of every twelfth row.
        block_source = self.folder / 'bc6-block.hdr'
        block_source.write_bytes(RadianceFile.constant())
        output = self.folder / 'bc6-block.edds'
        self.convert(block_source, output, '--conversion', 'hdr-compression', '--format-compress', 'copy', '--generate-mips', 'false')
        header = output.read_bytes()[:156]
        for row in HdrGoldens.bc6_blocks()[::12]:
            (self.folder / f'bc6-mode-{row["mode"]}.edds').write_bytes(header + bytes.fromhex(row['block']))

    @classmethod
    def main(cls, arguments: list[str]) -> int:
        """The command line: the executable and the folder to fill."""
        parser = argparse.ArgumentParser(description=__doc__)
        parser.add_argument('enfusion', type=pathlib.Path)
        parser.add_argument('folder', type=pathlib.Path)
        options = parser.parse_args(arguments)

        cls(Converter(str(options.enfusion.resolve())), options.folder.resolve()).write()
        return 0


if __name__ == '__main__':
    sys.exit(HdrFixtures.main(sys.argv[1:]))
