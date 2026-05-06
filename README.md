
![cullpp](https://codeberg.org/attachments/c1755a2b-ba09-4c1e-b78c-a1a8dae5f1e6)

![Wayland](https://img.shields.io/badge/Wayland-v1.0+-yellow.svg)
![C++17](https://img.shields.io/badge/C%2B%2B17-purple.svg)

Interactive region selector for wlroots-based Wayland compositors. Outputs geometry data to stdout for integration with screenshot, recording, and window management tools or custom scripts.

## Current design goals

- fast pointer response
- predictable multi-output behavior
- simple shell integration
- safe rendering over aggressive hacks

## Build

### Dependencies

- `wayland-client`
- `wayland-cursor`
- `cairo`
- `xkbcommon`
- generated protocol sources in `out/protocol` must be present

### Build with Ninja

```sh
ninja
./cullpp -h

# install
ninja -v install
```

## Usage

### Basic region selection

```sh
cullpp
# verbose mode frame timing, damage regions
cullpp -v
```

```sh
# screenshot
grim -g "$(cullpp)" ~/screenshot.png

# region recording
wf-recorder -g "$(cullpp)" -f output.mp4

# window placement (sway)
swaymsg "resize set $(cullpp -f '%w %h'); move position $(cullpp -f '%x %y')"

# screenshot straight into clipboard history tools
grim -g "$(cullpp)" - | wl-copy

# OCR a selected region
grim -g "$(cullpp)" -t ppm - | tesseract stdin stdout

# OCR and clean the output a bit
grim -g "$(cullpp)" -t ppm - | tesseract stdin stdout | sed '/^[[:space:]]*$/d'

# point-pick a single pixel and print its color
grim -g "$(cullpp -p -f '%x,%y 1x1')" -t ppm - | magick - -format '%[pixel:p{0,0}]\n' info:

# emit JSON geometry for scripts
cullpp -f '{"x":%x,"y":%y,"w":%w,"h":%h,"right":%X,"bottom":%Y,"output":"%o","label":"%l"}' | jq .

# pick a full output as a snap rect and capture that monitor
grim -g "$(cullpp -o)" selected-output.png
```

## Format tokens

| Token | Meaning |
|------|--------|
| `%x` | left edge |
| `%y` | top edge |
| `%w` | width |
| `%h` | height |
| `%X` | right edge |
| `%Y` | bottom edge |
| `%o` | output name with largest overlap |
| `%l` | matching stdin/snap label |
| `%%` | literal percent |

### Default output

```sh
%x,%y %wx%h
```

## Stdin snap rectangles

### Input format

```
x,y wxh
x,y wxh label
```

### Example

```sh
printf '0,0 1920x1080 left\n1920,0 1920x1080 right\n' | cullpp -r
```

## Notes

- The renderer currently favors correctness and low-latency input handling over overly aggressive repaint tricks.
- Multi-output behavior is still the most sensitive area. Test `-o`, crosshair, and drag-across-monitor flows on your compositor before publishing a release.
- If you use command substitution, the printed result is emitted after the Wayland teardown path completes.

## License under [MIT](LICENSE)
