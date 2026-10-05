"""Capture the installed importer's mip chains for owned sources; see README.md.

usage: py capture.py WORKBENCH_EXE OUTPUT_JSON
"""

import sys

from oracle import Capture, Oracle, SourcePattern


class MipCapture(Capture):
    """Every mip case once in a fresh emulator and twice in a reused one; all three must agree."""

    # (alpha, sizes, mip functions): Filter (0) and ColorNoise (2) with varying alpha, and
    # ColorNoise alone when opaque.
    CASES = [
        (True, [(8, 8), (7, 5), (1, 7), (7, 1), (2, 2), (1, 1)], [0, 2]),
        (False, [(8, 8), (7, 5)], [2]),
    ]

    def capture(self) -> dict:
        return dict(
            workbench_sha256=self.executable.fingerprint,
            rows=self.rows(),
            repeatability='Each case captured in a fresh emulator and twice in a reused emulator; all three outputs identical.',
        )

    def rows(self) -> list[dict]:
        """One row per source, mip function, tiling and Box (0) or Kaiser (2) filter."""
        rows = []
        for alpha, sizes, functions in self.CASES:
            for width, height in sizes:
                pixels = SourcePattern.bgra(width, height, alpha)
                for function in functions:
                    for tiled in [False, True]:
                        for mip_filter in [0, 2]:
                            rows.append(self.row(width, height, pixels, alpha, function, tiled, mip_filter))
                            print(width, height, function, tiled, mip_filter, 'repeatable', flush=True)

        return rows

    def row(self, width: int, height: int, pixels: bytes, alpha: bool, function: int, tiled: bool, mip_filter: int) -> dict:
        """One case's source and output levels, after checking that it repeats."""
        arguments = (width, height, pixels, function, tiled, mip_filter)
        fmt = 87 if alpha else 88
        levels = Oracle(self.executable).convert(*arguments, fmt=fmt)

        reused = Oracle(self.executable)
        for _ in range(2):
            assert reused.convert(*arguments, fmt=fmt) == levels

        row = dict(
            width=width, height=height, source_bgra=pixels.hex(), mip_function=function, tiled=tiled, mip_filter=mip_filter, levels=levels
        )
        if not alpha:
            row['alpha'] = False

        return row


if __name__ == '__main__':
    sys.exit(MipCapture.main(sys.argv[1:], __doc__))
