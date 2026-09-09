# EDDS converter

`edds-convert` is the extension's C17 codec process. It has no third-party code and is linked with
the static MSVC runtime. The extension invokes only the installed Windows x64 executable at
`dist/native/win32-x64/edds-convert.exe`, with `shell: false`.

## Build and test

```powershell
cmake -S native -B native/.build -A x64
cmake --build native/.build --config Release --parallel
ctest --test-dir native/.build -C Release --output-on-failure
cmake --install native/.build --config Release --prefix dist/native/win32-x64
```

Set `EDDS_BUILD_FUZZER=ON` with Clang to build the libFuzzer/AddressSanitizer/UBSan target. The
synthetic fixtures and their independent expected reader live under `native/tests`; production
code is not used to create expected values. CI materializes those fixtures, including a maximum-
integer boundary case, as an owned seed corpus and passes the corpus directory to libFuzzer.

## Versioned CLI

```text
edds-convert protocol --machine
edds-convert inspect --machine --protocol 1 --input PATH
edds-convert preview --machine --protocol 1 --mip N --input PATH
```

Machine output is one protocol-versioned JSON value on stdout. Diagnostics are also written for a
human on stderr. Exit categories are stable: `0` success, `2` invalid invocation, `3` invalid
input, `4` unsupported preview format, `5` cancellation and `6` internal failure.

## Supported slice and hard limits

Inspection accepts the common DDS header, its optional DX10 extension, and the Enfusion `ENF1`
mip table. It reports actual table order, `COPY`/`LZ4` storage, offsets, stored sizes, decoded sizes
and LZ4 block counts. Preview decodes two-dimensional, single-surface BGRA8 and BGRX8 mips from
either container and normalizes them to top-to-bottom RGBA8. Other pixel formats remain
inspectable; preview returns `unsupported-format` and never manufactures pixels.

Inputs are bounded to 1 GiB, dimensions to 32768 on either axis, mip levels to 32, decoded selected
mips to 64 MiB, LZ4 streams to 1024 blocks and each stored LZ4 block to 1 MiB. Every offset and size
is checked before reads or allocation; trailing bytes, malformed final-block markers and decoded
size mismatches are invalid input.
