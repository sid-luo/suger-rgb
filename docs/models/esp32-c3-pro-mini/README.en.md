# ESP32-C3 Pro Mini board enclosure

[简体中文](README.md) · [Back to the full guide](../../GUIDE.en.md)

The original model is by **[Mattia Carli](https://makerworld.com/zh/@MattiaCarli)**: [Case for ESP32-C3 PRO MINI ESP32-C3FH4 on MakerWorld](https://makerworld.com/zh/models/2008412-case-for-esp32-c3-pro-mini-esp32-c3fh4#profileId-2163305). This enclosure holds the controller board; it is not one of the complete lamp housings shown in the project's AI concept images.

## Downloads

| File | Contents | When to use it |
| --- | --- | --- |
| [Original STL](Case_ESP32-C3_PRO_MINI_V01.stl) | Body and lid in one file | Your slicer or printing service supports two separate parts in one file |
| [Body STL](Case_ESP32-C3_PRO_MINI_V01_Body.stl) | Body only | Upload and print each part separately |
| [Lid STL](Case_ESP32-C3_PRO_MINI_V01_Lid.stl) | Lid only | Print together with the body |

If a printing service does not accept two separate parts in a single file, upload the **body** and **lid** separately. Print one of each. The combined file was not accepted when this project used JLC's printing service, which is why the split files are provided.

## Changes in the split files

The Suger RGB maintainer separated the original model into two files and repositioned each part so it is centered on X/Y with its base at Z=0. The parts were not scaled and the enclosure geometry was not redesigned. The original file was renamed for consistency; its contents are unchanged.

| Part | Outside dimensions when imported in millimeters |
| --- | --- |
| Body | Approximately 29.30 × 21.70 × 7.50 mm |
| Lid | Approximately 29.30 × 21.70 × 3.30 mm |

STL files do not store units, so check these dimensions when importing. Both split parts are positioned near their own origin. If importing them together, arrange them as separate objects to avoid overlap. Check your board's dimensions and connector positions before printing; geometry checks do not verify the printed fit.

## Attribution and license

Original designer: Mattia Carli. The original model and the split files in this directory are licensed under **[CC BY-NC-SA 4.0](https://creativecommons.org/licenses/by-nc-sa/4.0/)** (Attribution–NonCommercial–ShareAlike), rather than the project's software GPL license. Retain the designer credit, source link, license, and description of changes when sharing these files.
