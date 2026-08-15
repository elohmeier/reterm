# E1004 camera color calibration

The E1004 conversion palette is measured from the physical display rather
than copied from nominal pigment colors. The current profile was captured as
an Apple ProRAW DNG from an iPhone 15 Pro and is stored in
`profiles/e1004-IMG_5327/profile.json`.

The original DNG and rectified photographs are deliberately not stored in the
repository. They are large, may contain private surroundings, and are not
needed at runtime. The profile records the source SHA-256 so a local original
can be matched to the measurements.

## Reproduce a capture

Generate the indexed target and its machine-readable patch manifest:

```sh
uv run tools/make-color-calibration.py
```

The target contains large references for all six native panel indices, a
black/white ramp, and every unordered pair of inks at nine ordered-dither
ratios. Send this asset with the nominal palette so its indices are not color
corrected before measurement:

```sh
uv run tools/send-image.py /root/.cache/e1004-color-target.png \
  --nominal-palette --no-flash --transport http
```

Photograph the whole border and all four markers. Use ProRAW, the iPhone's 1x
camera, diffuse light, no flash, and a nearly parallel camera/display plane.
Analyze the untouched DNG with:

```sh
uv run tools/analyze-color-calibration.py /root/IMG_5327.DNG
```

The analyzer does not apply Apple's display tone curve. It detects the four
fiducials from the embedded JPEG, maps target coordinates into the 16-bit
LinearRaw planes, compensates the measured spatial black/white field, and
converts camera values through the DNG ColorMatrix2 with Bradford adaptation
to D65. Pairwise rows are normalized to their photographed endpoints to
isolate ink-mixture behavior from residual illumination gradients.

Private geometry and rectification diagnostics default to
`/root/.cache/e1004-calibration-IMG_5327`. The repository output contains the
numeric profile and a synthetic reconstruction of the measured patches.

## Current measured gamut

The six large solid references produced these D65-relative sRGB preview
values. They represent the appearance of reflective pigments under the
calibrated capture, not the electrical encoding values.

| Wire index | Ink | Measured sRGB | Working sRGB | CIE Lab |
| ---: | --- | --- | --- | --- |
| 0 | black | `112, 99, 121` | `0, 0, 0` | `43.87, 9.61, -10.29` |
| 1 | white | `255, 254, 254` | `255, 255, 255` | `99.78, 0.20, 0.09` |
| 2 | green | `119, 187, 145` | `46, 174, 96` | `70.41, -30.38, 14.18` |
| 3 | blue | `95, 145, 193` | `0, 118, 175` | `58.48, -2.81, -30.38` |
| 4 | red | `213, 26, 93` | `204, 0, 0` | `46.63, 70.06, 12.37` |
| 5 | yellow | `255, 215, 112` | `255, 209, 0` | `89.76, 11.71, 58.32` |

The reflective gamut is substantially lighter and less saturated than the
old nominal palette. Native-pixel ordered mixtures are also not perfectly
linear: after endpoint and illumination normalization, the mean camera-space
RMSE across intermediate pairwise patches is about `0.098`. The full measured
curves remain in the profile for future mixture-aware optimization.

The camera's measured black is an illuminated paper value, not input-space
gray. Directly using it for nearest-ink selection made 57–67% of the example
photo physical black, versus about 25% before calibration. The working values
therefore apply per-channel black-point compensation in linear light between
the measured black and white. This retains measured hue information without
crushing shadows.

`tools/send-image.py`, the Svelte editor, and the device-hosted fallback use
that black-point-compensated gamut for nearest-ink selection and error
diffusion. The Python tool's `--nominal-palette` option remains reserved for
index-exact test and calibration assets.
