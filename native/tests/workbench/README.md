# DayZ mip capture

`mip-goldens.json` contains owned synthetic BGRA inputs and the exact output levels from the
DayZ Workbench 1.29.163709 texture importer. The executable SHA-256 is recorded in the capture.
No Workbench binaries or disassembly are distributed.

The capture executes the installed importer's machine code in Unicorn, starting at its texture
conversion function (RVA `0xfcc0d0`), with decoded BGRA8/BGRX8 input and `Conversion=None`.
It never starts the executable, its Windows entry point, a window, or an operating-system process.
The original profile dispatch, mip generator and NVTT arithmetic execute unchanged. The harness
supplies raw-image storage and metadata properties, allocation/copy/free, ordinary CRT math,
and no-op logging/profiling. Unhandled imports fail. Addresses are accepted only for the recorded
binary fingerprint. This is an importer comparison, **not** a Workbench UI or DayZ visual smoke.

The 56 cases cover 8x8, 7x5, 1x7, 7x1, 2x2 and 1x1 inputs, Box/Kaiser, both tiling modes,
Filter/ColorNoise, varying alpha and opaque BGRX. Each case is executed once in a fresh emulator
and twice in a reused emulator; the three results must be identical. Expected pixels never come
from `edds_core`. CTest converts these sources, then the independent reader checks every decoded
level, dimensions, order and COPY container, including each valid RemoveMips boundary.

Observed for this DayZ build:

- `MipMapFunction=ColorNoise` (enum 2) takes the same filtering path as Filter (enum 0).
  RGB and alpha are deterministic. A separate `Swizzling=ColorNoise` enum invokes alpha noise;
  that swizzle is outside this ticket and is not enabled here.
- `TiledTexture=false` clamps out-of-bounds samples; true repeats them. Mip 0 is unchanged.
  Box never needs samples outside the image. Kaiser makes the border difference observable.
- Workbench sets Kaiser width 3, **stretch 4, alpha 1**, with 32 subsamples. The earlier
  interpretation exchanged alpha and stretch. The chain remains in normalized float samples;
  the uncompressed output rounds to UNORM16 then drops the low eight bits. Packing a level
  does not quantize the samples used to generate the next level.

To reproduce in a separate Python environment with `pefile==2024.8.26` and `unicorn==2.1.4`:

```powershell
py capture.py 'F:/SteamLibrary/steamapps/common/DayZ Tools/Bin/Workbench/workbenchApp.exe' reproduced.json
```

Compare the `rows` with `mip-goldens.json`; a different Workbench build needs fresh address and
property-registration evidence before updating this oracle. Do not regenerate expected pixels
with the converter under test. The harness is optional research tooling; building, testing and
shipping the converter does not require Python, Unicorn, pefile or Workbench.

## Swizzling captures

`swizzle-goldens.json` adds 261 cases (29 for each of the nine mappings), captured by
`capture_swizzling.py` with the same arguments and binary fingerprint. Inputs include asymmetric
RGBA, RGB, opaque declared alpha, NPOT and single-axis sizes, normalization before and after mip
filtering, Kaiser borders, removal, and every supported LDR conversion. Each runs once fresh and
twice in a reused emulator. No expected pixels are produced by this converter.

For `Conversion=None` the capture is the importer's final raw image. For other conversions the
harness intercepts the final channel/BC encoder boundary, recording its requested DXGI format
and exact BGRA input. Those captures are **not Workbench-compressed BC bytes**. The independent
reader checks lossless output exactly and decodes lossy output against the captured input with
per-channel mean absolute error at most 8 for RGB and 6 for alpha (byte units). Missing runtime
channels must still be exactly zero/opaque. BC inputs use smooth asymmetric color/alpha ramps;
uncompressed inputs exercise high-frequency channel differences. Every level, including 1x1,
is checked through direct CLI and batch output.

Observed mappings, written as output `(R,G,B,A)` from input `(r,g,b,a)`:

| Swizzling | Byte mapping after mip filtering |
| --- | --- |
| AlphaToRGB | `(a,a,a,255)` |
| SMDIToGS | `(b,g,0,255)` |
| NormalMap_NOHQ | `(0,g,0,255-r)` |
| NormalMapGA | `(0,g,0,r)` |
| TerrainNormalSpecular_SYxX | `(a,g,0,r)` |
| NormalSpecularMapXYZS | Identity in this importer build |
| TerrainLayerTexture / TerrainSuperTexture | Identity at RemoveMips=0; special resampling below |
| AmbientSpecularMapGA | Identity at RemoveMips=0; special reduction below |

AlphaToRGB and SMDIToGS precede every format branch. Normal/terrain-normal mappings and the two
terrain resamplers run only for None/DXT; other LDR branches preserve their input channels. This
is the captured dispatch, not a fallback to a different recipe. The five byte mappings select
BGRA8 for None and BC3 for DXT, even when they write a constant opaque alpha.
The remaining mappings retain declared source alpha in DXT too: opaque RGBA chooses BC3,
while RGB chooses BC1. This is covered separately from varying alpha and opaque RGB.

Terrain layer/super require generated mips for None/DXT. With removal they first resize the source
to `max(8, dimension >> RemoveMips)`, then generate the ordinary chain. Layer uses endpoint-aligned
bilinear sampling and duplicates three border texels. Super preserves the three-texel source
border, bilinearly resamples its interior and copies rounded border samples. Ambient removal
uses component-wise minimum RGB and maximum alpha per integer source block, except in the Red
branch, where ordinary removal applies. The admitted special-removal inputs are unsupplied,
at least 8x8, and for ambient declare alpha; smaller or incompatible sources explicitly refuse.

These captures also correct normalization: the boolean stage expands a byte as `2*b/255-1`,
normalizes RGB, and truncates/saturates `(unit+1)*128.5`. MipMapFunction=Normalize instead normalizes
the filtered float vectors, packs them through UNORM16, and retains unquantized floats for the
next level. Alpha is unchanged by either normalization stage.

These importer and codec checks do not replace the pending minimal-DayZ visual smoke.

The public-domain NVIDIA implementation provides supplementary arithmetic documentation:
[FloatImage.cpp](https://github.com/castano/nvidia-texture-tools/blob/master/src/nvimage/FloatImage.cpp),
[Filter.cpp](https://github.com/castano/nvidia-texture-tools/blob/master/src/nvimage/Filter.cpp),
and [CompressorRGB.cpp](https://github.com/castano/nvidia-texture-tools/blob/master/src/nvtt/CompressorRGB.cpp).
