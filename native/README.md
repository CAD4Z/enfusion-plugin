# Native executable

`enfusion` is the extension's C17 process. Its first argument names an area — `edds` converts and
inspects textures — and each area is a static library linked into the one executable. It has no
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

Set `ENFUSION_BUILD_FUZZER=ON` with Clang to build the libFuzzer/AddressSanitizer/UBSan target. The
synthetic fixtures and their independent expected reader live under `native/tests`; production
code is not used to create expected values. CI materializes those fixtures, including a maximum-
integer boundary case, as an owned seed corpus and passes the corpus directory to libFuzzer.

## Versioned CLI

```text
enfusion protocol --machine
enfusion edds inspect --machine --protocol 1 --input PATH
enfusion edds inspect --machine --protocol 1 --input PATH --metadata PATH.edds.meta
enfusion edds preview --machine --protocol 1 --mip N --input PATH
enfusion edds batch --machine --protocol 1 < jobs.ndjson
enfusion edds convert --machine --protocol 1 --input SOURCE.png --output RESULT.edds \
  --target-format enfusion-dds --format-compress fastest --compress-threshold 80 \
  --remove-mips 0 --conversion color-hq-compression --conversion-quality 0.403 \
  --swizzling none --contains-mips false --generate-mips true --normalize false \
  --mipmap-function filter --mipmap-filter box --tiled-texture true
```

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

The extension passes a private cancellation-file control to the process; the codec polls it at its
existing cancellation points, then the extension force-kills only after a grace period and removes
any matching sibling transaction temps. Diagnostics are also written for a human on stderr. Exit
categories are stable: `0` success, `2` invalid invocation, `3` invalid input, `4` unsupported
preview format, `5` cancellation and `6` internal failure.

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
`missing`) triplet lets a caller bind the publish step to the filesystem snapshot it presented.
Both artifacts are built and flushed in sibling temporary files, then replaced with rollback.
`EDDS_CONVERT_FAIL` is reserved for the black-box transaction tests.
