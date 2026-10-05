"""Check a generated SDF font against fontTools: every glyph's outline and the kerning pairs.

The generator is not its own oracle. fontTools reads the same TrueType file independently: the
composed outline of every glyph, and the GPOS pairs a shaper applies for the `kern` feature of
the default language systems of DFLT, latn and cyrl (PairPos 1 and 2, Extension, first matching
subtable per lookup), or a format 0 `kern` table when there is no GPOS. The check then rebuilds
each glyph from the atlas the way the engine's shader does — bilinear, median of three, ink from
0.5 — at four times atlas resolution, and compares it with the outline filled by the nonzero rule.

A glyph mismatch within half a pixel of the outline is antialiasing, so a stroke thinner than a
pixel that the field loses whole hides inside that band. The check measures those losses on its
own: the outline shrunk by a quarter pixel against the ink, or all of the outline when the ink
misses more than half of it. A glyph losing 2 square pixels or more must be in the generator's
`thin` list. The generator also lists strokes shaved less deeply than a quarter pixel, which this
resolution cannot tell from antialiasing, so a listed glyph this check sees whole is no fault.

usage:
  py font_check.py ENFUSION_EXE FONT.ttf CHARACTERS.txt SIZE
                   [--golden PAIRS.json] [--preview OUT.png TEXT]

Needs fontTools and numpy. `--golden` writes the fontTools pairs of this font, size and character
set, which is how `font-kerning-golden.json` for the synthetic fixture was made.
"""

import argparse
import json
import pathlib
import struct
import subprocess
import sys
import tempfile
import zlib

import numpy
from fontTools.ttLib import TTFont

from edds_reader import EddsFile


class FntFont:
    """The generated .fnt: its size, glyph cell, atlas boxes and KERN pairs."""

    def __init__(self, data: bytes):
        assert data[:4] == b'FORM' and data[8:12] == b'FNT5'

        # Big-endian sized chunks after the FORM header.
        chunks, at = {}, 12
        while at < len(data):
            tag, size = data[at : at + 4].decode(), struct.unpack('>I', data[at + 4 : at + 8])[0]
            chunks[tag] = data[at + 8 : at + 8 + size]
            at += 8 + size

        # HEAD: a sized name, then the field description that starts with the size and the cell.
        head = chunks['HEAD']
        name_size = struct.unpack('<I', head[:4])[0]
        self.size, _, _, self.cell = struct.unpack_from('<iiBi', head, 4 + name_size)

        # GLPS: the character ranges, whose characters TCRD describes in the same order.
        glyphs = chunks['GLPS']
        ranges = struct.unpack('<I', glyphs[12:16])[0]
        codes = []
        for index in range(ranges):
            first, length, _ = struct.unpack('<IHH', glyphs[16 + 8 * index : 24 + 8 * index])
            codes += range(first, first + length)

        # Each box: atlas x, y, width, height, bearing x, bearing y and advance.
        self.boxes = {}
        for index, code in enumerate(codes):
            self.boxes[code] = struct.unpack('<HHHHhhh', chunks['TCRD'][14 * index : 14 * index + 14])

        kern = chunks.get('KERN', b'')
        self.pairs = [struct.unpack('<HHi', kern[8 * index : 8 * index + 8]) for index in range(len(kern) // 8)]

    @classmethod
    def read(cls, path: pathlib.Path) -> 'FntFont':
        """The .fnt at `path`."""
        return cls(pathlib.Path(path).read_bytes())


class SdfAtlas:
    """The atlas as a field of RGB distances, and the engine's shader that reads ink from it."""

    def __init__(self, edds: EddsFile):
        assert edds.mip_count == 1, 'the atlas must have one level'
        level = edds.levels()[0]
        bgra = numpy.frombuffer(level.pixels, dtype=numpy.uint8).reshape(level.height, level.width, 4)
        self.field = bgra[:, :, [2, 1, 0]].astype(numpy.float64) / 255.0

    def ink(self, u: numpy.ndarray, v: numpy.ndarray) -> numpy.ndarray:
        """The shader at atlas point (u, v): bilinear between texel centres, median of three."""
        height, width, _ = self.field.shape
        x, y = u - 0.5, v - 0.5
        x0, y0 = numpy.floor(x).astype(int), numpy.floor(y).astype(int)
        fx, fy = (x - x0)[..., None], (y - y0)[..., None]

        def texel(dx, dy):
            return self.field[numpy.clip(y0 + dy, 0, height - 1), numpy.clip(x0 + dx, 0, width - 1)]

        channels = (texel(0, 0) * (1 - fx) + texel(1, 0) * fx) * (1 - fy) + (texel(0, 1) * (1 - fx) + texel(1, 1) * fx) * fy
        return numpy.median(channels, axis=-1) >= 0.5


class GlyphOutline:
    """A TrueType glyph's contours as line segments in atlas pixels; a curve is 32 of them."""

    CURVE_PIECES = 32

    def __init__(self, coordinates: list, ends: list, flags: list, scale: float):
        segments, start = [], 0
        for end in ends:
            points = [(coordinates[index][0] * scale, coordinates[index][1] * scale, flags[index] & 1) for index in range(start, end + 1)]
            start = end + 1
            if len(points) < 2:
                continue

            segments += self.contour_segments(self.spelled(points))

        self.segments = numpy.array(segments, dtype=numpy.float64).reshape(-1, 4)

    @staticmethod
    def spelled(points: list) -> list:
        """The contour with the on-curve point implied between two off-curve ones made explicit,
        starting and ending at its first on-curve point."""
        spelled = []
        for index, point in enumerate(points):
            following = points[(index + 1) % len(points)]
            spelled.append(point)
            if not point[2] and not following[2]:
                spelled.append(((point[0] + following[0]) / 2, (point[1] + following[1]) / 2, 1))

        first = next(index for index, point in enumerate(spelled) if point[2])
        return spelled[first:] + spelled[:first] + [spelled[first]]

    @classmethod
    def contour_segments(cls, spelled: list) -> list:
        """Lines as they are, quadratic curves as `CURVE_PIECES` lines each."""
        segments = []
        index = 0
        while index + 1 < len(spelled):
            a, b = spelled[index], spelled[index + 1]
            if b[2]:
                segments.append((a[0], a[1], b[0], b[1]))
                index += 1
                continue

            c = spelled[index + 2]
            previous = a
            for step in range(1, cls.CURVE_PIECES + 1):
                t = step / cls.CURVE_PIECES
                x = (1 - t) ** 2 * a[0] + 2 * (1 - t) * t * b[0] + t * t * c[0]
                y = (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * b[1] + t * t * c[1]
                segments.append((previous[0], previous[1], x, y))
                previous = (x, y)
            index += 2

        return segments

    def filled(self, xs: numpy.ndarray, ys: numpy.ndarray) -> numpy.ndarray:
        """The nonzero rule at every (x, y) of two equally shaped arrays."""
        winding = numpy.zeros(xs.shape, dtype=numpy.int32)
        for x0, y0, x1, y1 in self.segments:
            if y0 == y1:
                continue

            up = (y0 <= ys) & (ys < y1)
            down = (y1 <= ys) & (ys < y0)
            crossing = x0 + (ys - y0) * (x1 - x0) / (y1 - y0)
            right = crossing > xs
            winding += (up & right).astype(numpy.int32) - (down & right).astype(numpy.int32)

        return winding != 0


class GlyphComparison:
    """Every glyph's ink against its outline: faults away from the outline, and losses."""

    SUPERSAMPLE = 4
    BOUNDARY_NOISE = 2

    def __init__(self, font: TTFont, fnt: FntFont, atlas: SdfAtlas, cmap: dict, scale: float):
        self.font = font
        self.fnt = fnt
        self.atlas = atlas
        self.cmap = cmap
        self.scale = scale

    def run(self) -> tuple[dict, dict]:
        """Fault sample counts of the glyphs that have any, and every glyph's lost square pixels."""
        glyf = self.font['glyf']
        faults, losses = {}, {}
        for code, (x, y, w, h, bx, by, _) in sorted(self.fnt.boxes.items()):
            name = self.cmap.get(code)
            if name is None or w == 0 or h == 0:
                continue

            # The outline and the ink on one supersampled grid over the box and a 5-pixel margin.
            coordinates, ends, flags = glyf[name].getCoordinates(glyf)
            outline = GlyphOutline(list(coordinates), list(ends), list(flags), self.scale)
            left = x + (self.fnt.cell - w) * 0.5
            top = y + (self.fnt.cell - h) * 0.5
            columns = (numpy.arange((w + 10) * self.SUPERSAMPLE) + 0.5) / self.SUPERSAMPLE
            rows = (numpy.arange((h + 10) * self.SUPERSAMPLE) + 0.5) / self.SUPERSAMPLE
            gx, gy = numpy.meshgrid(bx - 5 + columns, by + 5 - rows)
            truth = outline.filled(gx, gy)
            ink = self.atlas.ink(left + (gx - bx), top + (by - gy))

            # Disagreement within half a pixel of the outline is antialiasing, not a fault.
            wrong = truth != ink
            near = numpy.zeros_like(truth)
            for dy in range(-self.BOUNDARY_NOISE, self.BOUNDARY_NOISE + 1):
                for dx in range(-self.BOUNDARY_NOISE, self.BOUNDARY_NOISE + 1):
                    shifted = numpy.roll(numpy.roll(truth, dy, axis=0), dx, axis=1)
                    near |= shifted != truth

            count = int((wrong & ~near).sum())
            if count:
                faults[code] = count
            losses[code] = self.lost_area(truth, ink)

        return faults, losses

    @classmethod
    def lost_area(cls, truth: numpy.ndarray, ink: numpy.ndarray) -> float:
        """Square pixels of the outline the ink misses: shrunk by a quarter pixel, or all of it."""
        inner = truth.copy()
        for axis in (0, 1):
            for step in (-1, 1):
                inner &= numpy.roll(truth, step, axis=axis)

        missed = truth & ~ink
        lost = missed if 2 * missed.sum() > truth.sum() else inner & ~ink
        return lost.sum() / cls.SUPERSAMPLE**2


class KerningOracle:
    """The pairs a shaper applies between the font's characters, read by fontTools."""

    SCRIPTS = ('DFLT', 'latn', 'cyrl')
    NO_REQUIRED_FEATURE = 0xFFFF

    def __init__(self, font: TTFont, codes: list[int], cmap: dict, scale: float):
        self.font = font
        self.scale = scale
        self.glyphs = {code: cmap[code] for code in codes if code in cmap and code <= 0xFFFF}

    def pairs(self) -> list[tuple[int, int, int]]:
        """(left, right, adjustment) for every nonzero pair, rounded and in KERN order."""
        if 'GPOS' in self.font:
            totals = self.gpos_totals()
        elif 'kern' in self.font:
            totals = self.kern_table_totals()
        else:
            totals = {}

        pairs = [(left, right, self.rounded(value * self.scale)) for (left, right), value in totals.items()]
        return sorted((pair for pair in pairs if pair[2] != 0), key=lambda pair: (pair[0] << 16) | pair[1])

    def kern_lookups(self, table) -> set[int]:
        """The lookups of the `kern` feature in the default language system of each known script."""
        lookups = set()
        features = table.FeatureList.FeatureRecord
        for script in table.ScriptList.ScriptRecord:
            # Text with no language set: the default language system only, never a language's own.
            system = script.Script.DefaultLangSys
            if script.ScriptTag not in self.SCRIPTS or system is None:
                continue

            indices = list(system.FeatureIndex)
            if system.ReqFeatureIndex != self.NO_REQUIRED_FEATURE:
                indices.append(system.ReqFeatureIndex)
            for index in indices:
                if features[index].FeatureTag == 'kern':
                    lookups.update(features[index].Feature.LookupListIndex)

        return lookups

    @staticmethod
    def pair_subtables(lookup) -> list:
        """The lookup's pair-adjustment subtables, unwrapped from Extension, with their coverage."""
        prepared = []
        for subtable in lookup.SubTable:
            if lookup.LookupType == 9:
                if subtable.ExtensionLookupType != 2:
                    continue
                subtable = subtable.ExtSubTable
            elif lookup.LookupType != 2:
                continue

            covered = {name: position for position, name in enumerate(subtable.Coverage.glyphs)}
            prepared.append((subtable, covered))

        return prepared

    def gpos_totals(self) -> dict[tuple[int, int], int]:
        """Each pair's summed adjustment in font units over the `kern` lookups of GPOS."""
        table = self.font['GPOS'].table
        totals = {}
        for index in sorted(self.kern_lookups(table)):
            prepared = self.pair_subtables(table.LookupList.Lookup[index])
            for left, left_name in self.glyphs.items():
                # Within one lookup the first subtable that applies to a pair is the only one.
                done = set()
                for subtable, covered in prepared:
                    if left_name not in covered:
                        continue

                    if subtable.Format == 1:
                        self.add_glyph_pairs(totals, done, left, subtable.PairSet[covered[left_name]])
                    else:
                        self.add_class_pairs(totals, done, left, left_name, subtable)

        return totals

    def add_glyph_pairs(self, totals: dict, done: set, left: int, pair_set) -> None:
        """PairPos format 1: the adjustments listed for `left` by second glyph."""
        values = {record.SecondGlyph: getattr(record.Value1, 'XAdvance', 0) if record.Value1 else 0 for record in pair_set.PairValueRecord}
        for right, right_name in self.glyphs.items():
            if right not in done and right_name in values:
                totals[(left, right)] = totals.get((left, right), 0) + (values[right_name] or 0)
                done.add(right)

    def add_class_pairs(self, totals: dict, done: set, left: int, left_name: str, subtable) -> None:
        """PairPos format 2: the adjustment between the classes of `left` and each right glyph."""
        first = subtable.ClassDef1.classDefs.get(left_name, 0) if subtable.ClassDef1 else 0
        if first >= subtable.Class1Count:
            return

        for right, right_name in self.glyphs.items():
            second = subtable.ClassDef2.classDefs.get(right_name, 0) if subtable.ClassDef2 else 0
            if right in done or second >= subtable.Class2Count:
                continue

            record = subtable.Class1Record[first].Class2Record[second]
            value = getattr(record.Value1, 'XAdvance', 0) if record.Value1 else 0
            totals[(left, right)] = totals.get((left, right), 0) + (value or 0)
            done.add(right)

    def kern_table_totals(self) -> dict[tuple[int, int], int]:
        """Each pair's adjustment in font units from the horizontal format 0 subtables of `kern`."""
        totals = {}
        for table in self.font['kern'].kernTables:
            if table.format != 0 or table.coverage & 0x06 or not table.coverage & 0x01:
                continue

            for left, left_name in self.glyphs.items():
                for right, right_name in self.glyphs.items():
                    value = table.kernTable.get((left_name, right_name))
                    if value is not None:
                        # A subtable with the override bit replaces the sum so far.
                        totals[(left, right)] = value if table.coverage & 0x08 else totals.get((left, right), 0) + value

        return totals

    @staticmethod
    def rounded(value: float) -> int:
        """Rounded half away from zero."""
        return int(value + 0.5) if value >= 0 else -int(-value + 0.5)


class GrayscalePng:
    """An 8-bit grayscale PNG written with the standard library."""

    SIGNATURE = b'\x89PNG\r\n\x1a\n'

    @classmethod
    def write(cls, path: pathlib.Path, pixels: numpy.ndarray) -> None:
        """Writes `pixels`, rows of 0..255 values, as one unfiltered, compressed image."""
        height, width = pixels.shape
        rows = b''.join(b'\x00' + bytes(row) for row in pixels.astype(numpy.uint8))
        header = struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)
        pathlib.Path(path).write_bytes(
            cls.SIGNATURE + cls.chunk(b'IHDR', header) + cls.chunk(b'IDAT', zlib.compress(rows, 9)) + cls.chunk(b'IEND', b'')
        )

    @staticmethod
    def chunk(tag: bytes, data: bytes) -> bytes:
        """One chunk: its length, tag, data and CRC."""
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))


class TextPreview:
    """A line of text drawn the way the engine draws it, KERN applied, for a visual check."""

    def __init__(self, fnt: FntFont, atlas: SdfAtlas):
        self.fnt = fnt
        self.atlas = atlas

    def write(self, text: str, path: pathlib.Path, scale: int = 2) -> None:
        """Draws `text` at `scale` x atlas pixels, black on white, into the PNG at `path`."""
        kerning = {(left, right): value for left, right, value in self.fnt.pairs}
        line = self.fnt.size
        width = sum(self.fnt.boxes.get(ord(letter), (0,) * 7)[6] for letter in text) + 2 * line
        canvas = numpy.zeros((int(line * 1.6 * scale), int(width * scale)), dtype=numpy.float64)
        pen, baseline = line * 0.5, line * 1.2

        for index, letter in enumerate(text):
            box = self.fnt.boxes.get(ord(letter))
            if box is None:
                continue

            x, y, w, h, bx, by, advance = box
            if w and h:
                left, top = x + (self.fnt.cell - w) * 0.5, y + (self.fnt.cell - h) * 0.5
                x0, y0 = int((pen + bx - 5) * scale), int((baseline - by - 5) * scale)
                columns = (numpy.arange((w + 10) * scale) + 0.5) / scale
                rows = (numpy.arange((h + 10) * scale) + 0.5) / scale
                gx, gy = numpy.meshgrid(columns, rows)
                ink = self.atlas.ink(left - 5 + gx, top - 5 + gy)
                region = canvas[y0 : y0 + ink.shape[0], x0 : x0 + ink.shape[1]]
                region[...] = numpy.maximum(region, ink[: region.shape[0], : region.shape[1]])

            pen += advance
            if index + 1 < len(text):
                pen += kerning.get((ord(letter), ord(text[index + 1])), 0)

        GrayscalePng.write(path, 255 - canvas * 255)


class FontCheck:
    """Generates the font and holds its glyphs, thin list and kerning to fontTools."""

    # A glyph that loses this many square pixels or more is clearly broken.
    CLEARLY_THIN = 2.0

    def __init__(self, options: argparse.Namespace):
        self.options = options

    def generate(self, folder: pathlib.Path) -> tuple[FntFont, SdfAtlas, set[int]]:
        """The font, its atlas and the characters the generator reports as thin.

        No resource name is given, so the generator writes the font without a recipe, which needs
        no relative path from the temporary folder to the source font.
        """
        output = folder / f'SDF_Check{self.options.size}.fnt'
        result = subprocess.run(
            [
                self.options.enfusion,
                'font',
                'generate',
                '--machine',
                '--protocol',
                '1',
                '--input',
                str(pathlib.Path(self.options.font).resolve()),
                '--output',
                str(output),
                '--size',
                str(self.options.size),
                '--characters',
                str(pathlib.Path(self.options.characters).resolve()),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            raise SystemExit(result.stdout + result.stderr)

        thin = set(json.loads(result.stdout)['thin'])
        return FntFont.read(output), SdfAtlas(EddsFile.read(output.with_suffix('.edds'))), thin

    def run(self) -> int:
        """Prints the three comparisons; 1 when any of them fails."""
        with tempfile.TemporaryDirectory() as folder:
            fnt, atlas, reported_thin = self.generate(pathlib.Path(folder))

        font = TTFont(self.options.font)
        cmap = font.getBestCmap()
        scale = self.options.size / font['head'].unitsPerEm
        codes = sorted(fnt.boxes)
        faults, losses = GlyphComparison(font, fnt, atlas, cmap, scale).run()
        expected = KerningOracle(font, codes, cmap, scale).pairs()
        actual = sorted(fnt.pairs, key=lambda pair: (pair[0] << 16) | pair[1])

        faulty = ' '.join(f'U+{code:04X}:{count}' for code, count in sorted(faults.items()))
        print(f'glyphs {len(codes)}, faulty {len(faults)} {faulty}')

        # Every glyph this check sees losing a stroke must be on the generator's own thin list.
        losing = sum(lost >= self.CLEARLY_THIN for lost in losses.values())
        unreported = sorted(code for code, lost in losses.items() if lost >= self.CLEARLY_THIN and code not in reported_thin)
        listed = ' '.join(f'U+{code:04X}:{losses[code]:.2f}' for code in unreported)
        print(
            f'thin {len(reported_thin)} reported, {losing} losing {self.CLEARLY_THIN:g} square px or more, '
            f'unreported {len(unreported)} {listed}'
        )

        print(f'pairs {len(actual)}, fontTools {len(expected)}, {"equal" if actual == expected else "DIFFERENT"}')
        if actual != expected:
            missing, extra = sorted(set(expected) - set(actual)), sorted(set(actual) - set(expected))
            print('  only fontTools:', missing[:10], '\n  only KERN:', extra[:10])

        if self.options.golden:
            golden = {'font': pathlib.Path(self.options.font).name, 'size': self.options.size, 'pairs': [list(pair) for pair in expected]}
            pathlib.Path(self.options.golden).write_text(json.dumps(golden, indent=None) + '\n')

        if self.options.preview:
            TextPreview(fnt, atlas).write(self.options.preview[1], self.options.preview[0])

        return 1 if faults or actual != expected or unreported else 0

    @classmethod
    def main(cls, arguments: list[str]) -> int:
        """The command line in the module's usage."""
        parser = argparse.ArgumentParser()
        parser.add_argument('enfusion')
        parser.add_argument('font')
        parser.add_argument('characters')
        parser.add_argument('size', type=int)
        parser.add_argument('--golden')
        parser.add_argument('--preview', nargs=2, metavar=('PNG', 'TEXT'))
        return cls(parser.parse_args(arguments)).run()


if __name__ == '__main__':
    sys.exit(FontCheck.main(sys.argv[1:]))
