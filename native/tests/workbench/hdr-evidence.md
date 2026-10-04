# HDR and cubemap evidence

Captured from DayZ Tools Workbench 1.29.163709, executable SHA-256
`74b74d1abe1f7f91e989d33dbfdcf97f9517a3a7df4dcb808c38991fd1df5cbe`.
The capture script refuses another binary rather than reusing its addresses.

`capture_hdr.py WORKBENCH_EXE OUTPUT_JSON` requires the same pefile/Unicorn environment
as `capture.py`. No Workbench binaries or third-party runtime codecs are distributed.
`hdr-goldens.json` contains owned RGBE inputs and outputs produced by executing the
installed decoder, importer and EDDS writer. Each importer case is checked in a fresh
instance and twice in a reused instance.

| Boundary executed | Observation | Product contract |
| --- | --- | --- |
| Radiance loader `0x140d88630` | `#?RADIANCE` and `#?RGBE`, `FORMAT=32-bit_rle_rgbe`, `-Y height +X width`; flat and planar scanline RLE decode to floats above 1 | Only these encodings/orientation; comments are permitted. Other header transforms and XYZE are refused. |
| Importer `0x140fcc0d0`, `Conversion=None` (0) | The RGBA32F image stays float and is the importer's own output, at any size: 12x8, 6x6, 3x5, 2x2 and 1x1 are accepted, and odd sides halve down to 1. Its chain equals what HDRCompression hands its encoder for the same profile. | `None` writes RGBA32F (legacy FourCC 116) at any size. |
| Importer, `HDRCompression` (7) | Requires RGBA32F input; the encoder is asked for DXGI 95. Sides must be powers of two of at least 4: 4x4, 8x4, 4x8 and 16x8 are accepted, 12x8, 6x6, 3x5, 2x2 and 1x1 refused. | `HDRCompression` means unsigned BC6H, for Radiance sources only, with the same size rule. |
| Cubemap stage | A 2:1 panorama of power-of-two width at least 16: 16x8 and 32x16 are accepted, 24x12, 8x4, 32x8 and 16x16 refused. Side `width/4`, face order +X,-X,+Y,-Y,+Z,-Z; float samples match all captured faces, for both conversions | The same rule, with the exact -Y/+X equirectangular orientation. |
| Mip stage | Float Box and Kaiser chains, each with tiling and with clamping, preserve radiance | Filter only; no unproven float swizzles or normalization. |
| Encoder `0x140f4cdb0` | Receives the filtered floats unchanged, radiance above 65504 included (257024 in the capture). ConversionQuality 0, 0.403 and 1 arrive as floats at parameters +24 | See below: the encoder saturates, and quality is search effort. |
| Writer `0x140429a30` | Legacy FOURCC 116 for RGBA32F; DX10 95 for BC6H; all faces concatenated inside each mip, smallest mip first | COPY/LZ4 encode the aggregated mip. Complete cube caps2 `0xfe00`; DX10 miscFlag 4 and **arraySize 6**. |

Projection is captured, not inferred from the word “cubemap”. With `u=2*x/size`,
`v=2*y/size`, face directions are `(1,1-v,1-u)`, `(-1,1-v,u-1)`, `(u-1,1,v-1)`,
`(u-1,-1,1-v)`, `(u-1,1-v,1)`, `(1-u,1-v,-1)`. Longitude uses `atan2(x,z)`, latitude
uses `atan2(y,hypot(z,x))`; bilinear sampling wraps longitude and clamps latitude.
These are corner samples, without a half-pixel offset. Native float comparisons allow
`max(1e-5, abs(expected)*5e-6)` for platform math differences.

## The BC6H encoder

For the importer captures the encoder boundary is intercepted, to read its parameters and
keep its float input; the importer goldens do **not** contain Workbench BC6H blocks.
Separate `containers` entries execute the installed writer with controlled zero BC6H blocks
to establish the compressed header/table topology.

Run unintercepted (`encoder_options` in the goldens), the encoder hands its codec three named
options as strings before it starts worker threads, which a single-threaded emulator cannot
run: `Quality`, the ConversionQuality printed with two decimals (0.403 becomes `0.40`),
`ModeMask` `207` and `NumThreads` `8`, both fixed. Next to them the executable holds the rest of
that codec's option names: `UseChannelWeighting`, `WeightR`/`G`/`B`, `UseAdaptiveWeighting`,
`DXT1UseAlpha`, `AlphaThreshold`, `CompressionSpeed`, `MultiThreading`, `ColourRestrict` and
`AlphaRestrict`. These are AMD Compressonator's codec parameters. Its unsigned BC6H encoder
clamps endpoints to 0–65504, and its `Quality` scales the endpoint search (`clampF16Max` and
`optQuantAnD_d` in its `cmp_core/shaders/bc6_encode_kernel.cpp`). Accordingly the
converter saturates radiance above 65504 rather than refusing it, and ConversionQuality buys
search effort in its own encoder; no byte equality with Workbench is claimed.

The native encoder writes mode 11 only: one region with two 10-bit endpoints. `ModeMask`
selects a subset of modes in Workbench's codec, and the single-region mode is the converter's
limit. A block whose colours lie near one line in RGB is reproduced closely; one whose colours
do not keeps up to about 29% relative error, `|decoded - wanted| / (1 + wanted)`, in the
captured chains. The CLI test holds every face and level to Workbench's encoder input at a
35% maximum and a 2% mean square, so it catches a broken face, level or block, not a lost
refinement.

`../edds/bc6-goldens.json` covers all 14 legal modes and four reserved modes, with 12
owned deterministic blocks each (seed 806). Float RGB expectations were captured with
`bcdec_bc6h_float`, independent of production code; the reference URL and header SHA-256
are embedded in the fixture. Reserved modes return black RGB/opaque alpha as hardware
specifies. The CLI tests decode the blocks the converter writes with their own mode-11
decoder, written from the format's specification, never through the converter's preview.

## Release verification

Automated tests cover source subtypes, float faces/mips against Workbench for every captured
filter, tiling, cube and mip choice, the sizes and panoramas Workbench accepts and refuses,
independently decoded BC6H faces and mips against Workbench's encoder input, saturation above
65504, all BC6H decoder modes, encoded float preview, metadata/GUID, stale revisions,
cancellation, malformed/truncated/oversized data, partial allocation cleanup and container
topology. `hdr_fixtures.py CLI OUTPUT_FOLDER` materializes sources and EDDS outputs for
sanitizer fuzz seeds and manual smoke. The package workflow stages the tested binary and
verifies the VSIX binary hash; its HDR suite runs again on the extracted packaged executable.

**Workbench UI-open and minimal-DayZ visual smoke remain unverified.** Executing captured
engine routines is not a substitute for these two visual checks. Before release, open the four
generated `hdr-*-2d/cube.edds` fixtures in Workbench and exercise them in a minimal DayZ
scene, checking orientation, brightness and the mip chain, and whether RGBA32F loads at all.
