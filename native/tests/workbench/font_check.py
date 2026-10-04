"""Check a generated SDF font against fontTools: every glyph's outline and the kerning pairs.

The generator is not its own oracle. fontTools reads the same TrueType file independently: the
composed outline of every glyph, and the GPOS pairs a shaper applies for the `kern` feature of
the default language systems of DFLT, latn and cyrl (PairPos 1 and 2, Extension, first matching
subtable per lookup), or a format 0 `kern` table when there is no GPOS. The check then rebuilds each glyph from the atlas the way the
engine's shader does — bilinear, median of three, ink from 0.5 — at four times atlas resolution,
and compares it with the outline filled by the nonzero rule.

A glyph mismatch within half a pixel of the outline is antialiasing, so a stroke thinner than a
pixel that the field loses whole hides inside that band. The check measures those losses on its
own: the outline shrunk by a quarter pixel against the ink, or all of the outline when the ink
misses more than half of it. A glyph losing 2 square pixels or more must be in the generator's
`thin` list. The generator also lists strokes shaved less deeply than a quarter pixel, which this
resolution cannot tell from antialiasing, so a listed glyph this check sees whole is no fault.

usage:
  py font_check.py ENFUSION_EXE FONT.ttf CHARACTERS.txt SIZE [--golden PAIRS.json] [--preview OUT.png TEXT]

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

SUPERSAMPLE = 4
BOUNDARY_NOISE = 2
FIELD_RANGE = 1.5 * 8 / 2 ** 0.5
CLEARLY_THIN = 2.0


def read_fnt(path):
    data = pathlib.Path(path).read_bytes()
    assert data[:4] == b'FORM' and data[8:12] == b'FNT5'
    chunks, at = {}, 12
    while at < len(data):
        tag, size = data[at:at + 4].decode(), struct.unpack('>I', data[at + 4:at + 8])[0]
        chunks[tag] = data[at + 8:at + 8 + size]
        at += 8 + size
    head = chunks['HEAD']
    name_size = struct.unpack('<I', head[:4])[0]
    field = head[4 + name_size:]
    size, _, kind, cell, a, b, r, bold, italic, c = struct.unpack('<iiBiffhBBf', field[:29])
    count, ranges = struct.unpack('<II', chunks['GLPS'][8:16])
    codes = []
    for index in range(ranges):
        first, length, _ = struct.unpack('<IHH', chunks['GLPS'][16 + 8 * index:24 + 8 * index])
        codes += range(first, first + length)
    boxes = {}
    for index, code in enumerate(codes):
        boxes[code] = struct.unpack('<HHHHhhh', chunks['TCRD'][14 * index:14 * index + 14])
    kern = chunks.get('KERN', b'')
    pairs = [struct.unpack('<HHi', kern[8 * index:8 * index + 8]) for index in range(len(kern) // 8)]
    return {'size': size, 'cell': cell, 'a': a, 'r': r, 'boxes': boxes, 'pairs': pairs}


def lz4_block(source, output):
    at = 0
    while at < len(source):
        token = source[at]
        at += 1
        literals = token >> 4
        if literals == 15:
            while True:
                more = source[at]
                at += 1
                literals += more
                if more != 255:
                    break
        output += source[at:at + literals]
        at += literals
        if at >= len(source):
            break
        offset = source[at] | source[at + 1] << 8
        at += 2
        match = (token & 15) + 4
        if token & 15 == 15:
            while True:
                more = source[at]
                at += 1
                match += more
                if more != 255:
                    break
        for _ in range(match):
            output.append(output[-offset])


def read_atlas(path):
    data = pathlib.Path(path).read_bytes()
    assert data[:4] == b'DDS ' and data[36:40] == b'ENF1'
    height, width = struct.unpack('<II', data[12:20])
    assert struct.unpack('<I', data[28:32])[0] == 1, 'the atlas must have one level'
    container, stored = data[128:132], struct.unpack('<I', data[132:136])[0]
    payload = data[136:136 + stored]
    if container == b'COPY':
        pixels = payload
    else:
        output, at = bytearray(), 4
        while True:
            descriptor = struct.unpack('<I', payload[at:at + 4])[0]
            at += 4
            lz4_block(payload[at:at + (descriptor & 0x7FFFFFFF)], output)
            at += descriptor & 0x7FFFFFFF
            if descriptor & 0x80000000:
                break
        pixels = bytes(output)
    bgra = numpy.frombuffer(pixels, dtype=numpy.uint8).reshape(height, width, 4)
    return bgra[:, :, [2, 1, 0]].astype(numpy.float64) / 255.0


def segments_of(coordinates, ends, flags, scale):
    """TrueType contours as line segments, curves cut into 32 pieces each."""
    segments, start = [], 0
    for end in ends:
        points = [(coordinates[index][0] * scale, coordinates[index][1] * scale, flags[index] & 1)
                  for index in range(start, end + 1)]
        start = end + 1
        if len(points) < 2:
            continue
        spelled = []
        for index, point in enumerate(points):
            following = points[(index + 1) % len(points)]
            spelled.append(point)
            if not point[2] and not following[2]:
                spelled.append(((point[0] + following[0]) / 2, (point[1] + following[1]) / 2, 1))
        first = next(index for index, point in enumerate(spelled) if point[2])
        spelled = spelled[first:] + spelled[:first] + [spelled[first]]
        index = 0
        while index + 1 < len(spelled):
            a, b = spelled[index], spelled[index + 1]
            if b[2]:
                segments.append((a[0], a[1], b[0], b[1]))
                index += 1
            else:
                c = spelled[index + 2]
                previous = a
                for step in range(1, 33):
                    t = step / 32
                    x = (1 - t) ** 2 * a[0] + 2 * (1 - t) * t * b[0] + t * t * c[0]
                    y = (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * b[1] + t * t * c[1]
                    segments.append((previous[0], previous[1], x, y))
                    previous = (x, y)
                index += 2
    return numpy.array(segments, dtype=numpy.float64).reshape(-1, 4)


def filled(segments, xs, ys):
    """Nonzero rule at every (x, y) of two equally shaped arrays."""
    winding = numpy.zeros(xs.shape, dtype=numpy.int32)
    for x0, y0, x1, y1 in segments:
        if y0 == y1:
            continue
        up = (y0 <= ys) & (ys < y1)
        down = (y1 <= ys) & (ys < y0)
        crossing = x0 + (ys - y0) * (x1 - x0) / (y1 - y0)
        right = crossing > xs
        winding += (up & right).astype(numpy.int32) - (down & right).astype(numpy.int32)
    return winding != 0


def rebuilt(field, u, v):
    """The shader at atlas coordinates (u, v): bilinear between texel centres, median of three."""
    height, width, _ = field.shape
    x, y = u - 0.5, v - 0.5
    x0, y0 = numpy.floor(x).astype(int), numpy.floor(y).astype(int)
    fx, fy = (x - x0)[..., None], (y - y0)[..., None]
    def at(dx, dy):
        return field[numpy.clip(y0 + dy, 0, height - 1), numpy.clip(x0 + dx, 0, width - 1)]
    channels = (at(0, 0) * (1 - fx) + at(1, 0) * fx) * (1 - fy) + (at(0, 1) * (1 - fx) + at(1, 1) * fx) * fy
    return numpy.median(channels, axis=-1) >= 0.5


def lost_area(truth, ink):
    """Square atlas pixels of the outline the ink misses: shrunk by a quarter pixel, or all of it."""
    inner = truth.copy()
    for axis in (0, 1):
        for step in (-1, 1):
            inner &= numpy.roll(truth, step, axis=axis)
    missed = truth & ~ink
    lost = missed if 2 * missed.sum() > truth.sum() else inner & ~ink
    return lost.sum() / SUPERSAMPLE ** 2


def check_glyphs(font, fnt, field, cmap, scale):
    """Glyphs whose ink is wrong away from the outline, by sample count; and what each one loses."""
    glyf = font['glyf']
    faults, losses = {}, {}
    for code, (x, y, w, h, bx, by, _) in sorted(fnt['boxes'].items()):
        name = cmap.get(code)
        if name is None or w == 0 or h == 0:
            continue
        coordinates, ends, flags = glyf[name].getCoordinates(glyf)
        segments = segments_of(list(coordinates), list(ends), list(flags), scale)
        left = x + (fnt['cell'] - w) * 0.5
        top = y + (fnt['cell'] - h) * 0.5
        columns = (numpy.arange((w + 10) * SUPERSAMPLE) + 0.5) / SUPERSAMPLE
        rows = (numpy.arange((h + 10) * SUPERSAMPLE) + 0.5) / SUPERSAMPLE
        gx, gy = numpy.meshgrid(bx - 5 + columns, by + 5 - rows)
        truth = filled(segments, gx, gy)
        ink = rebuilt(field, left + (gx - bx), top + (by - gy))
        wrong = truth != ink
        near = numpy.zeros_like(truth)
        for dy in range(-BOUNDARY_NOISE, BOUNDARY_NOISE + 1):
            for dx in range(-BOUNDARY_NOISE, BOUNDARY_NOISE + 1):
                shifted = numpy.roll(numpy.roll(truth, dy, axis=0), dx, axis=1)
                near |= shifted != truth
        count = int((wrong & ~near).sum())
        if count:
            faults[code] = count
        losses[code] = lost_area(truth, ink)
    return faults, losses


def gpos_pairs(font, codes, cmap):
    """Pairs a shaper applies between two characters, in font units, from GPOS."""
    table = font['GPOS'].table
    lookups = set()
    features = table.FeatureList.FeatureRecord
    for script in table.ScriptList.ScriptRecord:
        # Text with no language set: the default language system only, never a language's own.
        system = script.Script.DefaultLangSys
        if script.ScriptTag not in ('DFLT', 'latn', 'cyrl') or system is None:
            continue
        indices = list(system.FeatureIndex)
        if system.ReqFeatureIndex != 0xFFFF:
            indices.append(system.ReqFeatureIndex)
        for index in indices:
            if features[index].FeatureTag == 'kern':
                lookups.update(features[index].Feature.LookupListIndex)
    glyphs = {code: cmap[code] for code in codes if code in cmap and code <= 0xFFFF}
    totals = {}
    for index in sorted(lookups):
        lookup = table.LookupList.Lookup[index]
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
        for left, left_name in glyphs.items():
            done = set()
            # Within one lookup the first subtable that applies to a pair is the only one.
            for subtable, covered in prepared:
                if left_name not in covered:
                    continue
                if subtable.Format == 1:
                    values = {record.SecondGlyph: getattr(record.Value1, 'XAdvance', 0) if record.Value1 else 0
                              for record in subtable.PairSet[covered[left_name]].PairValueRecord}
                    for right, right_name in glyphs.items():
                        if right not in done and right_name in values:
                            totals[(left, right)] = totals.get((left, right), 0) + (values[right_name] or 0)
                            done.add(right)
                else:
                    first = subtable.ClassDef1.classDefs.get(left_name, 0) if subtable.ClassDef1 else 0
                    if first >= subtable.Class1Count:
                        continue
                    for right, right_name in glyphs.items():
                        second = subtable.ClassDef2.classDefs.get(right_name, 0) if subtable.ClassDef2 else 0
                        if right in done or second >= subtable.Class2Count:
                            continue
                        record = subtable.Class1Record[first].Class2Record[second]
                        value = getattr(record.Value1, 'XAdvance', 0) if record.Value1 else 0
                        totals[(left, right)] = totals.get((left, right), 0) + (value or 0)
                        done.add(right)
    return totals


def kern_table_pairs(font, codes, cmap):
    totals = {}
    glyphs = {code: cmap[code] for code in codes if code in cmap and code <= 0xFFFF}
    for table in font['kern'].kernTables:
        if table.format != 0 or table.coverage & 0x06 or not table.coverage & 0x01:
            continue
        for left, left_name in glyphs.items():
            for right, right_name in glyphs.items():
                value = table.kernTable.get((left_name, right_name))
                if value is not None:
                    totals[(left, right)] = value if table.coverage & 0x08 else totals.get((left, right), 0) + value
    return totals


def rounded(value):
    return int(value + 0.5) if value >= 0 else -int(-value + 0.5)


def expected_pairs(font, codes, cmap, scale):
    if 'GPOS' in font:
        totals = gpos_pairs(font, codes, cmap)
    elif 'kern' in font:
        totals = kern_table_pairs(font, codes, cmap)
    else:
        totals = {}
    pairs = [(left, right, rounded(value * scale)) for (left, right), value in totals.items()]
    return sorted((pair for pair in pairs if pair[2] != 0), key=lambda pair: (pair[0] << 16) | pair[1])


def write_png(path, pixels):
    height, width = pixels.shape
    rows = b''.join(b'\x00' + bytes(row) for row in pixels.astype(numpy.uint8))
    def chunk(tag, data):
        return struct.pack('>I', len(data)) + tag + data + struct.pack('>I', zlib.crc32(tag + data))
    pathlib.Path(path).write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', width, height, 8, 0, 0, 0, 0)) +
                                   chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def preview(fnt, field, text, path, scale=2):
    """The text drawn the way the engine draws it, KERN applied, at `scale` × atlas pixels."""
    kerning = {(left, right): value for left, right, value in fnt['pairs']}
    line = fnt['size']
    width = sum(fnt['boxes'].get(ord(letter), (0,) * 7)[6] for letter in text) + 2 * line
    canvas = numpy.zeros((int(line * 1.6 * scale), int(width * scale)), dtype=numpy.float64)
    pen, baseline = line * 0.5, line * 1.2
    for index, letter in enumerate(text):
        box = fnt['boxes'].get(ord(letter))
        if box is None:
            continue
        x, y, w, h, bx, by, advance = box
        if w and h:
            left, top = x + (fnt['cell'] - w) * 0.5, y + (fnt['cell'] - h) * 0.5
            x0, y0 = int((pen + bx - 5) * scale), int((baseline - by - 5) * scale)
            columns = (numpy.arange((w + 10) * scale) + 0.5) / scale
            rows = (numpy.arange((h + 10) * scale) + 0.5) / scale
            gx, gy = numpy.meshgrid(columns, rows)
            ink = rebuilt(field, left - 5 + gx, top - 5 + gy)
            region = canvas[y0:y0 + ink.shape[0], x0:x0 + ink.shape[1]]
            region[...] = numpy.maximum(region, ink[:region.shape[0], :region.shape[1]])
        pen += advance
        if index + 1 < len(text):
            pen += kerning.get((ord(letter), ord(text[index + 1])), 0)
    write_png(path, 255 - canvas * 255)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('enfusion')
    parser.add_argument('font')
    parser.add_argument('characters')
    parser.add_argument('size', type=int)
    parser.add_argument('--golden')
    parser.add_argument('--preview', nargs=2, metavar=('PNG', 'TEXT'))
    arguments = parser.parse_args()
    with tempfile.TemporaryDirectory() as folder:
        stem = 'SDF_Check%d' % arguments.size
        output = pathlib.Path(folder) / (stem + '.fnt')
        result = subprocess.run([arguments.enfusion, 'font', 'generate', '--machine', '--protocol', '1',
                                 '--input', str(pathlib.Path(arguments.font).resolve()), '--output', str(output),
                                 '--resource-name', 'Check/' + stem + '.fnt', '--size', str(arguments.size),
                                 '--characters', str(pathlib.Path(arguments.characters).resolve())],
                                capture_output=True, text=True)
        if result.returncode != 0:
            raise SystemExit(result.stdout + result.stderr)
        reported_thin = set(json.loads(result.stdout)['thin'])
        fnt = read_fnt(output)
        field = read_atlas(output.with_suffix('.edds'))
    font = TTFont(arguments.font)
    cmap = font.getBestCmap()
    scale = arguments.size / font['head'].unitsPerEm
    codes = sorted(fnt['boxes'])
    faults, losses = check_glyphs(font, fnt, field, cmap, scale)
    expected = expected_pairs(font, codes, cmap, scale)
    actual = sorted(fnt['pairs'], key=lambda pair: (pair[0] << 16) | pair[1])
    print('glyphs %d, faulty %d %s' % (len(codes), len(faults),
                                       ' '.join('U+%04X:%d' % item for item in sorted(faults.items()))))
    # Every glyph this check sees losing a stroke must be on the generator's own thin list.
    unreported = sorted(code for code, lost in losses.items() if lost >= CLEARLY_THIN and code not in reported_thin)
    print('thin %d reported, %d losing %g square px or more, unreported %d %s' % (
        len(reported_thin), sum(lost >= CLEARLY_THIN for lost in losses.values()), CLEARLY_THIN, len(unreported),
        ' '.join('U+%04X:%.2f' % (code, losses[code]) for code in unreported)))
    print('pairs %d, fontTools %d, %s' % (len(actual), len(expected), 'equal' if actual == expected else 'DIFFERENT'))
    if actual != expected:
        missing, extra = sorted(set(expected) - set(actual)), sorted(set(actual) - set(expected))
        print('  only fontTools:', missing[:10], '\n  only KERN:', extra[:10])
    if arguments.golden:
        pathlib.Path(arguments.golden).write_text(json.dumps({
            'font': pathlib.Path(arguments.font).name, 'size': arguments.size,
            'pairs': [list(pair) for pair in expected]}, indent=None) + '\n')
    if arguments.preview:
        preview(fnt, field, arguments.preview[1], arguments.preview[0])
    return 1 if faults or actual != expected or unreported else 0


if __name__ == '__main__':
    sys.exit(main())
