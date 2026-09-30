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

The public-domain NVIDIA implementation provides supplementary arithmetic documentation:
[FloatImage.cpp](https://github.com/castano/nvidia-texture-tools/blob/master/src/nvimage/FloatImage.cpp),
[Filter.cpp](https://github.com/castano/nvidia-texture-tools/blob/master/src/nvimage/Filter.cpp),
and [CompressorRGB.cpp](https://github.com/castano/nvidia-texture-tools/blob/master/src/nvtt/CompressorRGB.cpp).
