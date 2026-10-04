# HDR and cubemap evidence

Captured 2026-10-04 from DayZ Tools Workbench 1.29.163709, executable SHA-256
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
| Importer `0x140fcc0d0` | Conversion enum 7 requires RGBA32F input; encoder parameters request DXGI 95 | `HDRCompression` means unsigned BC6H; LDR sources cannot select it. |
| Encoder boundary `0x140f4cdb0` | ConversionQuality 0, 0.403, 1 arrive unchanged as float at parameters +24 | Stable quality property controls search effort; no claim of byte-identical Workbench encoding. |
| Cubemap stage | 2:1 panorama, side `width/4`, face order +X,-X,+Y,-Y,+Z,-Z; float samples match all captured faces | Power-of-two width at least 16, exact -Y/+X equirectangular orientation. |
| Mip stage | Float Box and Kaiser/clamp chains preserve radiance | Filter only; no unproven float swizzles or normalization. |
| Writer `0x140429a30` | Legacy FOURCC 116 for RGBA32F; DX10 95 for BC6H; all faces concatenated inside each mip, smallest mip first | COPY/LZ4 encode the aggregated mip. Complete cube caps2 `0xfe00`; DX10 miscFlag 4 and **arraySize 6**. |

Projection is captured, not inferred from the word “cubemap”. With `u=2*x/size`,
`v=2*y/size`, face directions are `(1,1-v,1-u)`, `(-1,1-v,u-1)`, `(u-1,1,v-1)`,
`(u-1,-1,1-v)`, `(u-1,1-v,1)`, `(1-u,1-v,-1)`. Longitude uses `atan2(x,z)`, latitude
uses `atan2(y,hypot(z,x))`; bilinear sampling wraps longitude and clamps latitude.
These are corner samples, without a half-pixel offset. Native float comparisons allow
`max(1e-5, abs(expected)*5e-6)` for platform math differences.

The encoder boundary is intercepted to inspect parameters and preserve its float input;
the importer goldens do **not** contain Workbench BC6H blocks. Separate `containers`
entries execute the installed writer with controlled zero BC6H blocks to establish the
compressed header/table topology. Native encoding uses its own mode-11 endpoint fitting
and deterministic refinement. Quality increases the search budget, not the GPU format.

`../edds/bc6-goldens.json` covers all 14 legal modes and four reserved modes, with 12
owned deterministic blocks each (seed 806). Float RGB expectations were captured with
`bcdec_bc6h_float`, independent of production code; the reference URL and header SHA-256
are embedded in the fixture. Reserved modes return black RGB/opaque alpha as hardware
specifies. The CLI test also independently decodes emitted mode-11 blocks with Python
and requires less than 2.5% error for the constant HDR fixture `(8,4,2)`.

## Release verification

Automated tests cover source subtypes, float faces/mips against Workbench, all BC6H
decoder modes, encoded float preview, metadata/GUID, stale revisions, cancellation,
malformed/truncated/oversized data, partial allocation cleanup and container topology.
`hdr_fixtures.py CLI OUTPUT_FOLDER` materializes sources and EDDS outputs for sanitizer
fuzz seeds and manual smoke. The package workflow stages the tested binary and verifies
the VSIX binary hash; its HDR suite runs again on the extracted packaged executable.

Local verification on 2026-10-04:

- Windows Release CTest: 10/10 suites passed, including nine HDR black-box cases.
- Extension typecheck and lint passed; full native-enabled test run: 587/587, no skips.
- Clang 18 on Ubuntu 24.04 with ASan/UBSan: HDR, core and memory suites passed;
  2,000 libFuzzer iterations over the owned HDR/BC6H corpus completed without a finding.
- VSIX allowlist, HDR controls in bundled editors, executable identity, nine HDR cases
  on the extracted executable, and clean VS Code install/activation smoke passed.
- Tested executable SHA-256 (build, staged artifact and VSIX entry):
  `11fc8c99b11689543fac7ad8b8063ac84a55c38d5d4832bc478ac87dd77ce237`.

**Workbench UI-open and minimal-DayZ visual smoke remain unverified.** On this machine
the Windows Computer Use native pipe could not be connected, including after a session
reset. Executing captured engine routines is not a substitute for these two visual checks.
Before release, open the four generated `hdr-*-2d/cube.edds` fixtures in Workbench and
exercise them in a minimal DayZ scene, checking orientation, brightness and the mip chain.
