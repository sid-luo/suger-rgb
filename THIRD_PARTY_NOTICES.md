# Third-party notices

## ESP32-C3 Pro Mini enclosure

The three STL files in `docs/models/esp32-c3-pro-mini/` are based on **Case for ESP32-C3 PRO MINI ESP32-C3FH4** by [Mattia Carli](https://makerworld.com/zh/@MattiaCarli).

Source: [MakerWorld model 2008412, print profile 2163305](https://makerworld.com/zh/models/2008412-case-for-esp32-c3-pro-mini-esp32-c3fh4#profileId-2163305)

License: [Creative Commons Attribution–NonCommercial–ShareAlike 4.0 International (CC BY-NC-SA 4.0)](https://creativecommons.org/licenses/by-nc-sa/4.0/). This license applies to the enclosure models independently of the project's software GPL license.

- `Case_ESP32-C3_PRO_MINI_V01.stl`: original combined body and lid, renamed without changing file contents.
- `Case_ESP32-C3_PRO_MINI_V01_Body.stl` and `Case_ESP32-C3_PRO_MINI_V01_Lid.stl`: separated by the Suger RGB maintainer and repositioned so each part is centered on X/Y with its base at Z=0. No scaling or enclosure geometry changes.

The split files are shared under the same CC BY-NC-SA 4.0 license. Retain attribution, the source and license links, and the description of changes when redistributing these models.

## Inter typeface

The `su` and `e` vector outlines in the Suger RGB logo are derived from Inter ExtraBold with optical size 32 and weight 800. The firmware does not ship the complete font binary.

Source: <https://github.com/google/fonts/tree/main/ofl/inter>

Upstream project: <https://github.com/rsms/inter>

```text
Copyright 2020 The Inter Project Authors (https://github.com/rsms/inter)
```

Inter is provided under the SIL Open Font License, Version 1.1. See [`licenses/Inter-OFL-1.1.txt`](licenses/Inter-OFL-1.1.txt).

## WLED v0.14.4 effects

Parts of `main/led_controller.c` are modified, standalone adaptations of the following effects and helpers from WLED v0.14.4:

- Fade / triangular luminance envelope
- Color Twinkles
- Twinklefox
- Chase Rainbow
- Theater Chase Rainbow

Source: <https://github.com/wled/WLED/blob/v0.14.4/wled00/FX.cpp>

Pinned release license: <https://github.com/wled/WLED/blob/v0.14.4/LICENSE>

The code was modified in 2026 for a small ESP-IDF C firmware, fixed glucose colors, a non-zero low-brightness floor, a bounded LED count, and a project-wide output-current budget. It is not a copy of or replacement for the complete WLED firmware.

The notices associated with the upstream source include:

```text
Copyright (c) 2016 Harm Aldick
Copyright (c) 2016 Christian Schwinne
```

WLED v0.14.4 and the relevant upstream effect source are provided under the MIT License:

```text
MIT License

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

The current WLED main branch uses a different license. Suger RGB intentionally references the historical v0.14.4 tag above; do not replace the source link with `main` without reviewing the licensing impact.

## ESP Web Tools 10.4.0

The browser installer bundles the `dist/web` build of ESP Web Tools 10.4.0.

Source: <https://github.com/esphome/esp-web-tools/tree/10.4.0>

ESP Web Tools is provided under the Apache License 2.0. The bundled license is at [`docs/installer/vendor/esp-web-tools/LICENSE`](docs/installer/vendor/esp-web-tools/LICENSE).
