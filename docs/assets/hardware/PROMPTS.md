# Hardware artwork sources

The board front/back photograph and pinout diagram were supplied by the user and are retained unchanged. The strip product reference was also supplied by the user and copied unchanged.

## Current wiring diagrams

`pro-mini-ws2812b-wiring.svg` and `pro-mini-ws2812b-wiring.en.svg` are editable compositions; the corresponding PNG files are rendered from those SVG files. Each SVG embeds the original board photograph without changing its image bytes, clips to the back view, and scales it uniformly by 1.1. Pin labels and right-angle wire paths are separate vector elements.

The strip illustration was generated with image_gen and corrected against the supplied strip reference. The input pads are GND, DIN, 5V from top to bottom; each gap between adjacent LEDs contains three solder pads. Black connects GND to GND, green connects GPIO4 to DIN, and red connects 5V to 5V. The crossing wires have no junction.

The Chinese caption is “背面 · USB 朝上”; the English caption is “Back view · USB at top”. Standalone USB power annotations, the large data-direction arrow/caption, and the footer were removed at the user's request. Printed arrows on the strip remain.

### Final strip and wire geometry prompt

Keep five LEDs and three copper solder pads with a cut line between every adjacent pair. All strip pad stacks are top GND, middle data, bottom +5V. The input data pad is DIN; the output is DOUT; internal data joints are DO → DI. Arrows point right. Draw three independent unbranched wires with horizontal and vertical segments and square 90-degree turns: board 5V to bottom strip 5V in red, board GND to top strip GND in black, board GPIO4 to middle strip DIN in green. Black may cross over green without a junction dot. Preserve the original motherboard photo; do not regenerate its traces, pads, proportions, or silkscreen.
