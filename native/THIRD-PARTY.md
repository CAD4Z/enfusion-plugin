# Third-party notices

The native executable has no third-party runtime dependencies.

Mip filtering follows the public-domain NVIDIA Texture Tools `FloatImage.cpp`, `Filter.cpp`,
and `CompressorRGB.cpp` by Ignacio Castaño (castanyo@yahoo.es), as checked against the installed
DayZ importer. Their public-domain notice is preserved here; no NVTT library is bundled.

Source: https://github.com/castano/nvidia-texture-tools/tree/master/src

The font area's multi-channel distance field (`src/font/msdf.c`) adapts to C17 the edge
colouring, the signed and pseudo-distance to line and quadratic segments, and the cubic and
quadratic equation solvers of msdfgen by Viktor Chlumský. No msdfgen library is bundled. Its MIT
notice is preserved here:

Source: https://github.com/Chlumsky/msdfgen

```text
MIT License

Copyright (c) 2014 - 2025 Viktor Chlumsky

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

The optional fixture-capture script uses pefile and Unicorn, and the font check uses fontTools and
numpy, from the developer's Python environment. None of these tools, nor any Workbench executable
or font, is shipped in the extension.

BC6H endpoint bit-layout tables in `src/edds/bc6_tables.h` derive from Microsoft DirectXTex
`BC6HBC7.cpp`. No DirectXTex codec or runtime library is linked.

Source: https://github.com/microsoft/DirectXTex/blob/main/DirectXTex/BC6HBC7.cpp

```text
    MIT License

    Copyright (c) Microsoft Corporation.

    Permission is hereby granted, free of charge, to any person obtaining a copy
    of this software and associated documentation files (the "Software"), to deal
    in the Software without restriction, including without limitation the rights
    to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
    copies of the Software, and to permit persons to whom the Software is
    furnished to do so, subject to the following conditions:

    The above copyright notice and this permission notice shall be included in all
    copies or substantial portions of the Software.

    THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
    IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
    FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
    AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
    LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
    OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
    SOFTWARE

```
