# HDR Pattern Generator

> **Developer tool** — This is a standalone D3D11 test pattern generator for WGC HDR capture validation. It is not needed for normal use of Looking Glass.

Standalone Windows D3D11 scRGB test pattern for validating WGC HDR capture
paths. It renders into an `R16G16B16A16_FLOAT` swapchain so Windows HDR/WGC
should see linear values above SDR white.

Build:

```sh
cmake -S host -B host/build-wgc-deploy -DBUILD_TEST_HDR_PATTERN=ON
cmake --build host/build-wgc-deploy --target test-hdr-pattern -j$(nproc)
```

Run on the Windows guest:

```powershell
.\test-hdr-pattern.exe --fullscreen
```

For dirty-region capture tests, use static mode so the pattern presents once and
then sleeps until the window is resized or repainted:

```powershell
.\test-hdr-pattern.exe --fullscreen --static
```

Controls:

- `Esc`: quit
- `F11`: toggle borderless fullscreen

The image labels each ramp, neutral patch, highlight inset, and color patch with
the intended scRGB value. Neutral labels also include approximate nit levels
using the convention `scRGB 1.0 = 80 nits`.

The grayscale HDR ramp and per-channel red/green/blue HDR ramps run from
`scRGB 0.0` to `32.0`. Each ramp has an `SDR MAX 1.0 / 80N / EV9.3` marker so it
is easy to see where an SDR conversion path clips or rolls off. Values above
`1.0` are HDR highlights and should retain separable detail in capture paths
that preserve HDR range.

Each ramp also has a horizontal `MAX STRIPE` drawn at the ramp maximum. If a
capture or display path crushes values above some threshold, the stripe blends
into the gradient at the point where the path can no longer distinguish higher
values.

Ramps include ruler-style ticks: SDR ramps have tenths, and HDR ramps have
one-scRGB minor ticks with four-scRGB major ticks. The labeled ticks include
denser nit/EV reference points for visual measurement from captures or photos.

Ramp tick labels show scRGB value, approximate nits, and photography-style EV
using `EV = log2(nits / 0.125)`. In this convention `scRGB 1.0` is `80 nits`,
or about `EV9.3`. The difference between two EV labels still gives stops above
or below the other value.

Nits are computed as `scRGB * 80`. EV values are rounded to one decimal place;
the decimal is part of the exposure value, so `EV 13.3` means about 13.3 stops
above `0.125 nits`.

The lower portion contains two-channel color-space squares with axes from
`scRGB 0.0` to `4.0`, useful for spotting hue shifts, chroma clipping, and
matrix/range mistakes.
