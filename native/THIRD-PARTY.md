# Third-party notices

The native executable has no third-party runtime dependencies.

Mip filtering follows the public-domain NVIDIA Texture Tools `FloatImage.cpp`, `Filter.cpp`,
and `CompressorRGB.cpp` by Ignacio Castaño (castanyo@yahoo.es), as checked against the installed
DayZ importer. Their public-domain notice is preserved here; no NVTT library is bundled.

Source: https://github.com/castano/nvidia-texture-tools/tree/master/src

The font area's multi-channel distance field follows the method Viktor Chlumský published with
msdfgen: edges coloured so that sharp corners split channels, pseudo-distance beyond an edge's
ends, and the median of three channels. It is an independent C17 implementation; no msdfgen code
is used or bundled.

Source: https://github.com/Chlumsky/msdfgen

The optional fixture-capture script uses pefile and Unicorn, and the font check uses fontTools and
numpy, from the developer's Python environment. None of these tools, nor any Workbench executable
or font, is shipped in the extension.
