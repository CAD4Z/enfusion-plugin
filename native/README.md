# Native executable

`enfusion` is the extension's C17 process. Its first argument names an area — `edds` converts and
inspects textures, `font` makes SDF fonts from TrueType — and each area is a static library linked
into the one executable. It has no
third-party runtime dependencies and is linked with the static MSVC runtime. The extension invokes
only the installed Windows x64 executable at `dist/native/win32-x64/enfusion.exe`, with
`shell: false`.

## Build and test

```powershell
cmake -S native -B native/.build -A x64
cmake --build native/.build --config Release --parallel
ctest --test-dir native/.build -C Release --output-on-failure
cmake --install native/.build --config Release --prefix dist/native/win32-x64
```

## Layout

```text
include/edds/     the texture library's API: EDDS, batches, the worker pool, the memory budget
include/font/     the font library's API
src/cli/          the executable: the area dispatch, the edds and font commands, the publish transaction
src/edds/         edds_core: the EDDS container, the mip pipeline, GPU encoders, metadata, the pool
src/edds/decode/  the source decoders, one per resource class: png, tga, jpeg, tiff, dds
src/font/         font_core: TrueType reading, kerning, outlines, the MSDF field, FNT5
tests/edds/       EDDS fixtures, the independent reference reader, core and black-box CLI tests
tests/font/       font fixtures, the independent reference, core and black-box CLI tests
tests/workbench/  captures from DayZ Workbench and fontTools that the tests are held to
fuzz/             libFuzzer targets for both areas
```

The code is formatted by `clang-format -i` with `native/.clang-format` (clang-format 19, the one
Visual Studio 2022 Build Tools carry): every branch and loop has braces and a body on lines of its
own, and line breaks are otherwise the author's.

A library's own headers sit beside its sources and are included by name; another area's are
reached only through `include/`. The one exception is the meta text reader in `src/edds`, which
`font_core` and the executable also see: both areas write their recipes in that format.

Set `ENFUSION_BUILD_FUZZER=ON` with Clang to build the libFuzzer/AddressSanitizer/UBSan target. The
synthetic fixtures and their independent expected reader live under `native/tests`; production
code is not used to create expected values. CI materializes those fixtures, including a maximum-
integer boundary case, as an owned seed corpus and passes the corpus directory to libFuzzer.

## Versioned CLI

```text
enfusion protocol --machine
enfusion edds inspect --machine --protocol 1 --input PATH
enfusion edds inspect --machine --protocol 1 --input PATH --metadata PATH.edds.meta [--identity-only]
enfusion edds preview --machine --protocol 1 --mip N --input PATH
enfusion edds batch --machine --protocol 1 [--cancel-file PATH] < jobs.ndjson
enfusion edds convert --machine --protocol 1 --input SOURCE.png --output RESULT.edds \
  --target-format enfusion-dds --format-compress fastest --compress-threshold 80 \
  --remove-mips 0 --conversion color-hq-compression --conversion-quality 0.403 \
  --swizzling none --contains-mips false --generate-mips true --normalize false \
  --mipmap-function filter --mipmap-filter box --tiled-texture true [--cancel-file PATH]
```

`--identity-only` reads a metadata file whose recipe this converter cannot run for its identity
alone: the GUID and the names are validated as always, and the unsupported recipe is reported
under `unsupportedMetadata` instead of refusing the whole inspection.

`protocol` answers for the whole executable: the protocol version and, per area, its commands.
An area owns its commands, flags and JSON; a new area is a new first-level subcommand and changes
nothing the others accept or print.

Machine output is protocol-versioned JSON on stdout. Batch stdin and stdout are NDJSON: one header,
1–256 complete job values, and one end record are validated before encoding starts; stdout carries
start, progress, diagnostic, result, and completion events. Framing is the reader's own: where a
pipe split the bytes carries no protocol meaning, a line over 256 KiB is refused once rather than
half-framed, and progress steps below a twentieth are dropped so one shared stdout cannot flood.

One batch is one process over one worker pool: at most eight threads, never more than there are
jobs or cores, and one shared 512 MiB budget for live codec heap allocations, including allocation
bookkeeping. File size supplies only the initial quota. Source buffers, decoded pixels, mip and
GPU buffers, container compression and metadata parsing all reserve their actual bytes before
allocation; growing a buffer charges both copies while they coexist. If an attempt needs more
room, it releases its memory and sibling temps before retrying with a larger quota. Waiting workers
never retain decoded images, and cancellation is checked again after admission and before retry.
An image that needs more than the whole budget runs alone rather than being refused. The budget
is not a process RSS limit: bounded job records, thread stacks and the C runtime are separate.
`EDDS_CONVERT_WORKERS` pins the worker count for a test or a diagnostic run; it is not a protocol
field and never a texture-profile one.

The extension passes a private cancellation file to `convert` and `batch` alike; the codec polls it
at its existing cancellation points, then the extension force-kills only after a grace period and
recovers whatever pair the process left under its own id (see the publish transaction below). A
batch the converter refuses as a whole — its header, a job line, the stream — is answered with one
`error` record, the same shape a single command refuses with. Diagnostics are also written for a
human on stderr. Exit categories are stable: `0` success, `2` invalid invocation, `3` invalid
input, `4` unsupported preview format, `5` cancellation and `6` internal failure.

## EDDS: supported slice and hard limits

Inspection accepts the common DDS header, its optional DX10 extension, and the Enfusion `ENF1`
mip table. It reports actual table order, `COPY`/`LZ4` storage, offsets, stored sizes, decoded sizes
and LZ4 block counts. Preview decodes every runtime format this converter can also write — BGRA8,
BGRX8, R8, R8G8, DXT1, DXT5, BC4, BC5 and BC7 — from either container, for two-dimensional,
single-surface textures, and normalizes them to top-to-bottom RGBA8. It shows what the file holds:
a single-channel format leaves green and blue at zero rather than repeating red across them. Block
formats decode through their padding, so a mip whose size is not a multiple of four and the 2x2 and
1x1 mips at the end of a chain come back at exactly their own dimensions. BC7 decodes all eight of
its block modes. Other pixel formats remain inspectable; preview returns `unsupported-format` and
never manufactures pixels.

Inputs are bounded to 1 GiB, dimensions to 32768 on either axis, mip levels to 32, decoded selected
mips to 64 MiB, LZ4 streams to 1024 blocks and each stored LZ4 block to 1 MiB. Every offset and size
is checked before reads or allocation; trailing bytes, malformed final-block markers and decoded
size mismatches are invalid input.

Conversion accepts five source resource classes DayZ Workbench registers, by the one extension
that names each: non-interlaced 8-bit RGB/RGBA `.png`; uncompressed true-color 24/32-bit `.tga`;
baseline sequential 8-bit Huffman `.jpg`, greyscale or YCbCr, one scan, luma 1x1/2x1/1x2/2x2 over
1x1 chroma; and single-page 8-bit chunky `.tiff` in strips, uncompressed or LZW/Deflate/PackBits,
greyscale BlackIsZero or RGB with an optional unassociated alpha extra sample. `.dds` admits one
controlled two-dimensional LDR surface in legacy BGRX/BGRA, DXT1/DXT5, or DX10 R8/RG8,
BC1/BC3/BC4/BC5/BC7/BGRA/BGRX UNORM layout, with either only the top level or a complete tight
largest-to-smallest chain. Any other subtype is
refused by its own code rather than decoded on a guess, and an extension outside that set — `.jpeg`
and `.tif` included, which Workbench does not register — is `unsupported-source-extension` before
anything is read. JPEG carries no alpha; TIFF alpha comes from the file, never from the profile.
A PNG `tRNS` colour key on an RGB image makes the pixels of that colour transparent and the image
one with alpha; a suggested `PLTE` on RGB or RGBA changes no pixel and is passed over. However
many IDAT chunks a PNG splits its image data into, the data is assembled with one allocation.

It writes EnfusionDDS with a floor-halved NPOT Box or Kaiser mip chain. Supplied DDS levels are used
only with `--contains-mips true --generate-mips false`; otherwise only the decoded top level enters
the generated path. `--remove-mips` removes levels from the large end after that path has completed.
`--mipmap-function color-noise` follows the same deterministic filter path as `filter` in the
observed DayZ build. `--tiled-texture false` clamps border samples; the default true repeats them.
Both modes accept Box and Kaiser. The [importer captures](tests/workbench/README.md) document their
exact pixels and repeatability, including unchanged mip 0.
Boolean `--normalize` normalizes source vectors before filtering, while
`--mipmap-function normalize` normalizes each reduced level after Box filtering. `--conversion`
selects the runtime
format: `none` writes 32-bit BGRX/BGRA, `dxt-compression` BC1 or BC3, `red` `R8_UNORM`,
`red-hq-compression` BC4, `red-green` `R8G8_UNORM`, `red-green-hq-compression` BC5 and
`color-hq-compression` BC7. `hdr-compression` is recognized and refused rather than replaced with
the nearest LDR format. A block format declares its top mip as a linear size and an uncompressed
one as a pitch, and every block format stores whole `ceil(w/4)*ceil(h/4)` blocks, padding the edge
by repeating the last real column and row. With `Swizzling=None`, the `none` / `dxt-compression` branch is read off
each source image's own samples: `none` follows whether the source declares an alpha channel, and
`dxt-compression` writes BC3 only when some sample is actually below fully opaque.

`--conversion-quality` is a fraction of one written the way a recipe writes it (`1`, `0.5`,
`0.403`, at most three decimals), and it buys encoder search rather than a different format. It is
accepted only where a compressed encoder reads it; against `none`, `red` or `red-green` any value
other than `1` is refused before anything is written.

`Copy` always uses `COPY`; `Fastest`, `Medium` and `Best` select lossless `COPY` or
independent-block `LZ4` per mip using the declared `CompressTreshold` percentage (equality selects
LZ4). The container is applied over the runtime format and never changes a decoded pixel of it.
Unsupported known profile values are refused rather than substituted.

`--swizzling` accepts `none` (default), `terrain-layer-texture`, `terrain-super-texture`,
`terrain-normal-specular-syxx`, `alpha-to-rgb`, `smdi-to-gs`, `normal-map-nohq`, `normal-map-ga`,
`normal-specular-map-xyzs`, and `ambient-specular-map-ga`. Canonical metadata preserves the exact
Workbench spelling. No mode is inferred from a filename. The [capture notes](tests/workbench/README.md#swizzling-captures)
define the verified mappings, format-dependent dispatch, alpha format selection and special
terrain/ambient RemoveMips behavior. Those special paths resize before chain generation; they
require an unsupplied source of at least 8x8, and ambient also requires declared alpha. Unsupported
settings or combinations refuse before publication. The editor disables incompatible profile
choices with an explanation; a mixed batch reports source-specific failures independently.

Registration is explicit: supplying `--metadata`, `--resource-name`, `--source-file` and `--guid`
together publishes a canonical EDDS/metadata pair; omitting all four publishes only EDDS and
refuses an existing sibling metadata file. The optional `--expect-*-revision size:mtime` (or
`missing`) triplet lets a caller bind the publish step to the filesystem snapshot it presented; the
time is in milliseconds, and one millisecond either way is the same write, because a caller reading
it through a JavaScript `Date` gets it rounded where this side truncates. The size must match
exactly. Metadata names and source files are written between quotes as they are, so a backslash in
one — a path written the Windows way — is refused; resource paths use forward slashes.
Both artifacts are built and flushed in sibling temporary files, then replaced with rollback.
A process-owned `pending` marker is flushed before originals move to backups; renaming it to
`committed` records publication of the complete pair. The marker is removed after the backups.
After a converter process dies — a `convert` as much as a `batch` — the host restores a pending pair
(including removing newly created members) or retains a committed pair, and it looks after every
batch, too, for the journal of a job whose own rollback failed. Recovery keeps backups until both
members are restored and reports a recovery failure instead of deleting the only surviving
originals. The input and the output must be different files, which on Windows is decided by the
system's own resolution of both paths and by the identity of a file that exists: a trailing dot
or space, an 8.3 name or a junction does not make the source its own destination.
`EDDS_CONVERT_FAIL` is a fault-injection hook for the black-box transaction tests; nothing in the
extension sets it.

## Fonts: SDF fonts from TrueType

```text
enfusion font generate --machine --protocol 1 --meta FONT.fnt.meta [--resource-name NAME] [--cancel-file PATH]
enfusion font generate --machine --protocol 1 --input SOURCE.ttf --output FONT.fnt --resource-name NAME \
  [--size N] [--characters FILE] [--guid HEX] [--cancel-file PATH]
enfusion font inspect --machine --protocol 1 --input FONT.fnt
enfusion font inspect --machine --protocol 1 --input SOURCE.ttf
```

`generate` turns a static TrueType font into what the engine draws an SDF font from: `FONT.fnt`
(FNT5) and its atlas `FONT.edds`. Beside them it writes the recipe `FONT.fnt.meta`, which is also
the font's Workbench identity:

```text
MetaFileClass {
 Name "{GUID}Mod/GUI/Fonts/SDF_SansRegular32.fnt"
 Configurations {
  FNTResourceClass PC {
   SourceFile "Sans-Regular.ttf"
   Characters "Sans.charset.txt"
   FontSize 32
  }
  FNTResourceClass XBOX_ONE : PC {
  }
  FNTResourceClass PS4 : PC {
  }
  FNTResourceClass LINUX : PC {
  }
 }
}
```

`--meta` rebuilds a font from its recipe; `--input` makes a new font and a new recipe. `SourceFile`
and `Characters` are relative to the folder of the recipe, with forward slashes. `FontSize` is the
atlas size in pixels, 8 to 40, 32 when absent. Without `Characters` the font holds the built-in
set: Basic Latin, Latin-1 and Cyrillic U+0400–U+045F. The recipe is always written back in
canonical form: unknown fields and other platforms are dropped, and only the GUID is carried over
character for character. A recipe whose GUID cannot be read is refused before anything is
written. A new font gets `--guid` or a random GUID no `.meta` beside it uses; a font that already
has a recipe keeps its GUID, and a different `--guid` is refused. The atlas is not registered: no
`.edds.meta` is written for it.

An interactive caller can bind generation to its confirmed files with `--expect-output-revision`,
`--expect-atlas-revision` and `--expect-metadata-revision`. Supply all three as `size:mtime` (bytes
and Unix milliseconds), or `missing`. They are checked before generation and again immediately
before publication; a changed file refuses the operation without replacing any destination.

A character file is UTF-8 text, and every character in it is in the font; line breaks, other
control characters and spaces only separate them. The space and U+25A1 □ are always in the font.
A font without □ gets a square frame drawn by the generator. Characters the font has no glyph for
are left out and listed under `missing` in the result; the generator's own glyphs are listed under
`drawn`.

What the engine is given:

- MSDF (type 2) with `R = 8` and a field that runs from 0 to 1 over `1.5 × R / √2` atlas pixels,
  which the engine's shader turns into an edge one screen pixel wide at the default text sharpness.
- The sign of the field follows the nonzero rule of the whole glyph, and distances are to the
  boundary of the union of its contours: where contours overlap, as in many Cyrillic and accented
  letters, the parts inside another contour are not an edge. Every texel at which the median of
  the three channels, or its bilinear interpolation, would disagree with the outline beyond a
  quarter pixel gets the true distance in all three channels.
- One square cell per glyph, the largest box plus 10, with one pixel of zeros between cells, in the
  smallest power-of-two atlas no taller than wide and at most 4096. Characters that share a glyph
  share its cell.
- Whole-pixel boxes from the exact outline extent at atlas size (`bx = floor(xMin)`,
  `by = ceil(yMax)`), placed so that the outline is exactly where it belongs after the engine
  centres the box in its cell, half pixels included. The advance is rounded.
- `HEAD` carries the cap height from `OS/2.sCapHeight`; without one, the top of the font's H, of
  Cyrillic En (U+041D) when it has no H, or 0.7 em when it has neither, whether or not the set
  holds them.
  `B = C = FontSize`, and the bold and italic flags are zero; the widget sets those itself.
- `KERN` holds the GPOS `PairPos` format 1 and 2 pairs (`Extension` included) of the `kern`
  feature in the default language system of the DFLT, latn and cyrl scripts — what text with no
  language set is kerned by — the first matching subtable per lookup, or a format 0 `kern` table
  when the font has no GPOS. A pair is the first glyph's `XAdvance` in whole atlas pixels; zeros,
  non-BMP characters and characters outside the set are left out. Pairs are sorted by
  `(left << 16) | right`, as the engine's binary search expects.
- The atlas is BGRA8 with alpha 255, LZ4 without loss and without mips, written through
  `edds_core` in the same process.

The three files are built and flushed in sibling temporaries and replaced together, with rollback
on any failure, and a rollback that could not put everything back says so and keeps the previous
files beside the new ones. There is no crash journal as there is for textures: a process killed
in the middle of the swap can leave the new atlas beside the old `.fnt`, with the old files kept
as `.enfusion-old` temporaries. The black-box tests make that transaction fail through the same
`EDDS_CONVERT_FAIL`, at stages named `font-*`. A font that cannot be made is refused with its
reason: CFF or CFF2 outlines (`unsupported-outline-format`), a variable font
(`variable-font-unsupported`), a collection (`font-collection-unsupported`), a `FontSize` outside 8
to 40 (`font-size-out-of-range`), and more cells than one 4096 atlas holds (`atlas-too-large`); the
exit categories are those of `edds`. Every offset and length in the TrueType file is checked
before it is followed. Hard limits: a font file of 64 MiB (`font-file-limit`); 16384 points, 4096
contours and 4096 components per glyph, components nested 16 deep (`glyph-size-limit`); 8192
characters (`character-set-limit`) and a character file of 1 MiB (`character-file-limit`); 2^20
kerning pairs and 4096 pair subtables in the `kern` feature (`kerning-limit`). A glyph whose box
does not fit an atlas is `glyph-too-large`.

A made font is reported as `font-generate` with its `guid`, `glyphCount`, `rangeCount`,
`pairCount`, `cell`, `atlasWidth` and `atlasHeight`, the code points the font lacks (`missing`)
and those the generator drew (`drawn`), and the `source` family and style. `inspect` of an `.fnt`
(`font-inspect`) reports its header — `name`, `size`, `type`, `cell`, `capHeight` (A),
`lineHeight` (B), `c` (C), `r`, `bold`, `italic` — with `glyphCount`, `pairCount` and the code
`ranges`; of a TrueType file (`font-source`), its `family` and `style`, typographic names first,
`unitsPerEm` and `glyphCount`, from which a caller names a new font.

Tests follow the EDDS rule: production code is never its own oracle. `enfusion-font-fixture`
writes synthetic TrueType fonts — overlapping contours shaped like Tse (U+0426), a contour with a hole,
composite glyphs with offsets, scale, a two-by-two matrix, nesting and point matching, GPOS pairs
behind PairPos 1, PairPos 2 and an Extension lookup next to pairs only a language's own system
enables, a variant with a `kern` table and no cap height, and the fonts that must be refused. `enfusion-font-reference` reads the result with its own FNT5, EDDS and LZ4
readers, rebuilds every glyph from the atlas with the shader's formula at four times atlas
resolution and compares it with its own rasterization of the planted outline, measures the field
range from the commonest step of the median, and checks `KERN` against the planted pairs and the
pairs fontTools read from the same font (`tests/workbench/font-kerning-golden.json`).
`tests/workbench/font_check.py` repeats both comparisons against fontTools for any real font.
