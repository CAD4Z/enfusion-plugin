# Legacy target research gate — 2026-10-04

The legacy targets are **not implemented**. The current Workbench can produce complete controlled files
for `EnfusionDDS_LZ4`, but its `EnfusionDDS_LZ0` compression path faults before writing a file.
Neither target is enabled by this evidence-only change. A successful LZO writer oracle and the
Workbench-open/DayZ visual checks remain required before claiming the requested support.

## Reproduce

Use the Python environment described in [README.md](README.md), with the same fingerprinted
DayZ Workbench 1.29.163709 executable:

```powershell
py -B capture_targets.py '<DayZ Tools>/Bin/Workbench/workbenchApp.exe' reproduced-targets.json
py -B check_target_captures.py reproduced-targets.json
py -B check_target_captures.py reproduced-targets.json --cli ../../.build/Release/enfusion.exe
```

`capture_targets.py` runs the original metadata target dispatch at RVA `0xfcf2f0`, its container
writer at `0x429a30`, and its original compressors. The harness supplies decoded image storage,
metadata lookup, a writable memory stream and ordinary allocation/CRT services. It substitutes
the Windows stack probe because the emulator stack is already mapped. It does not patch target
selection, header creation, compression, table creation or the writer fault. Unhandled calls,
unrecognized faults and instruction-limit failures stop capture rather than creating a golden.
The executable's Windows entry point is never run; this is not a Workbench UI crash report or
a Workbench-open/DayZ visual smoke.

`target-captures.json` records 84 cases. Each is run in a fresh emulator, then twice in another
reused emulator; the three results must agree. Its seven inputs are owned synthetic BGRA/BGRX
images, including NPOT and independently planted mip levels, a payload below the compression
boundary, one exactly at that boundary, and a multi-block LZ4 payload. They exercise `Copy`,
`Fastest`, `Medium`, `Best`, and thresholds 0, 80 and 100. This is a container-writer capture;
it does not exercise source decoding, GPU encoding, swizzling or mip generation.

Sources and complete files are deduplicated in the JSON. Each complete file is addressed by its
SHA-256. A `writer-fault` contains only diagnostic facts and the codec payload observed before
the fault; it has **no EDDS file**. Those codec bytes must not be mistaken for a completed capture.

`check_target_captures.py` uses only the Python standard library. Independently of the converter
and capture harness, it checks hashes, DDS fields, masks, ENF1 tables, smallest-first storage,
COPY and chained LZ4 framing, complete file boundaries and every planted byte of every mip.
With `--cli`, it also checks inspect container facts and every decoded RGBA mip through the
given executable. The expected samples come from the synthetic source, never from the converter.

## Observations

- The registered target values are `EnfusionDDS=0`, `DirectXDDS=1`, `EnfusionDDS_LZ0=2` and
  `EnfusionDDS_LZ4=3`. The spelling is `LZ0` (zero) in metadata, but the codec/container is LZO.
- In this pinned build, all 28 modern/LZ4 pairs produce byte-identical complete files. They use
  the ordinary DDS header with `ENF1` at byte 36, eight-byte table entries, smallest-first mip
  payloads, and `COPY` or `LZ4 ` tags. Thus a standalone file cannot distinguish those two
  authoring targets; the metadata recipe must retain the choice. This is observed equality,
  not a guessed alias or a claim about every Workbench release.
- `Copy` produces identical files for all three EDDS target choices. Mips shorter than 64 bytes
  also bypass compression. At 64 bytes, a non-Copy mode already invokes the selected compressor.
- The LZO branch uses `LZO ` as its intended table tag. All 18 captured attempts to compress
  with target 2 fault, including `CompressTreshold=0`: the ratio test occurs after compression.
  Therefore threshold zero is not a workaround for the writer failure.
- The 66 completed files have independently verified header/container/pixel data. The 18 failures
  prove a blocker, not LZO file compatibility. No completed LZO-container fixture was obtained.

## Exact LZO failure

An 8x8 constant BGRA source `(3,5,7,255)`, `TargetFormat=EnfusionDDS_LZ0`,
`FormatCompress=Fastest`, `CompressTreshold=80` suffices.

At writer RVA `0x42a567`, the LZO call receives the output-length pointer at frame offset `+4`.
The mip-table pointer starts at frame offset `+8`. The called LZO compressor stores an eight-byte
length, overlapping the low four bytes of that pointer. For this source the length is 16 and
the observed codec payload is `05030507ff030507ff20d70c00110000`. On return at `0x42a56c`, the
previously nonzero table pointer is zero in the mapped emulator stack. At `0x42a573`, the writer
tries to store the `LZO ` tag through it and faults. No bytes reached the output stream.

These facts support an ABI-width mismatch between the writer's four-byte length slot and the
compressor's eight-byte output-length store. They do not establish that every installed build
fails, nor replace reproducing the problem in an actual Workbench process. The harness deliberately
does not repair the pointer or rewrite the captured bytes to conceal this failure.

## Remaining work

Obtain a working version-pinned LZO writer or independently verifiable complete Workbench LZO
fixtures with their source/profile. Then establish its reader/writer contract, malformed-data
limits and profile interactions, and implement the shared target capability across native CLI,
metadata, batch and editor. Keep `DirectXDDS` explicitly refused before writing. Preserve GUIDs
and atomic replacement, verify both targets from the packaged executable, and perform the
Workbench-open and minimal-DayZ visual smoke. The current refusal remains in place until that
work is completed; these captures alone do not support either legacy target.

## Verification of this evidence

When the captures were recorded, typecheck, lint, the extension tests with `EDDS_TEST_CONVERTER`
set to a freshly built executable, the native CTest suites and the Windows packaging smoke passed.
The independent checker and the CLI inspect/preview checks passed again with the executable
extracted from the VSIX. None of this substitutes for implementing the target options, fuzzing
a future LZO parser, or the outstanding visual compatibility checks.
