bc7e.ispc - BC7 encoder by Binomial LLC / Richard Geldreich, Jr.
Copyright (C) 2018-2021 Binomial LLC. Apache License 2.0 (see LICENSE-Apache-2.0.txt).
Source: https://github.com/richgel999/bc7enc_rdo

Modified for PixelPress HAP: opaque mode-6 blocks only use p-bits 1/1 so their alpha decodes to
exactly 255 (three places, marked "PixelPress"). Compiled with Intel ISPC for ARM NEON (Apple
Silicon) and SSE4/AVX2 (x64, runtime dispatch).
