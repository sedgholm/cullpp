![cullpp](https://codeberg.org/attachments/2f40b394-8333-4b64-8bc7-e0c008f3874e)

![C++17](https://img.shields.io/badge/C%2B%2B17-purple?style=flat-square)
![Wayland](https://img.shields.io/badge/Wayland-v1.0+-yellow?style=flat-square)

Interactive region selector for wlroots-based wayland compositors. outputs geometry data to stdout.

## build

deps: `wayland-client`, `wayland-cursor`, `cairo`, `xkbcommon` + generated protocol sources in `out/protocol`

```
ninja
./cullpp -h

ninja -v install
```

## usage

basic:
```
cullpp
cullpp -v
```

pipe it:
```
wf-recorder -g "$(cullpp)" -f output.mp4
# upload files
grim -g "$(cullpp)" - | curl -s -F "files[]=@-" https://uguu.se/upload
```

ocr:
```
grim -g "$(cullpp)" -t ppm - | tesseract stdin stdout
grim -g "$(cullpp)" -t ppm - | tesseract stdin stdout | sed '/^[[:space:]]*$/d'
```

pixel pick:
```
grim -g "$(cullpp -p -f '%x,%y 1x1')" -t ppm - | magick - -format '%[pixel:p{0,0}]\n' info:
```

json:
```
cullpp -f '{"x":%x,"y":%y,"w":%w,"h":%h,"right":%X,"bottom":%Y,"output":"%o","label":"%l"}' | jq .
```

full output pick:
```
grim -g "$(cullpp -o)" selected-output.png
```

## format tokens

%x left, %y top, %w width, %h height, %X right, %Y bottom, %o output name, %l snap label, %% literal

default output:
```
%x,%y %wx%h
```

## stdin snap rectangles

input format:
```
x,y wxh
x,y wxh label
```

example:
```
printf '0,0 1920x1080 left\n1920,0 1920x1080 right\n' | cullpp -r
```

## env

CULLPP_RESELECT_MOD  reselect modifier def: Shift

set it:
```
export CULLPP_RESELECT_MOD=Control
```

or one-off:
```
CULLPP_RESELECT_MOD=Control cullpp
```
> [!TIP]
> Defaults live in `main.cpp` if you want to build your own preset.
## volunteer

found a bug? want to patch something? open an issue on codeberg:

https://codeberg.org/qel/cullpp/issues

or hit up the srht mailing list: [~flnt/list@lists.sr.ht]

## notes

- renderer favors correctness and low-latency input over aggressive repaint tricks
- multi-output is the most sensitive area. test -o, crosshair, and drag-across-monitor on your compositor before shipping
- with command substitution the result prints after wayland teardown completes

## License under [MIT](LICENSE)

