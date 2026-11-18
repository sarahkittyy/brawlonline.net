# Magic Kernel Sharp output resampling

Select **Graphics → Enhancements → Output Resampling → Magic Kernel Sharp 2021** to use
the new filter. Existing setting defaults are unchanged. **Area Sampling remains the
recommendation for downscaling** until quality, GPU time, and frame pacing have been
compared in actual game workloads.
In `GFX.ini`, the equivalent setting is `OutputResampling = 9` under `[Enhancements]`.
Values 0–6 retain their existing meanings; the shader's legacy values 7 and 8 are reserved.

The implementation uses John Costella's [Magic Kernel Sharp 2021](https://johncostella.com/magic/).
Resampling uses the quadratic B-spline (Magic Kernel); sharpening uses
`[-1, 6, -35, 204, -35, 6, -1] / 144` on each changed axis. There is one new mode,
with no separate 2013 mode, sigmoid, or anti-ringing clamp.

`OutputResamplingSharpness` in the same INI section blends each axis's sharpening filter
with the unsharpened image. It defaults to `1.0` (100%) and is limited to `[0, 1]`.
It has no UI control. Each changed axis blends its own 1D filter, so with both axes active
a value between 0 and 1 is a little milder than a blend of the whole 2D filter would be;
0 and 1 are the same either way.

## Passes

An axis that shrinks is reduced and then sharpened on the smaller grid. An axis that grows
is sharpened on the source grid and then enlarged. `CreateOutputResamplingPlan`
(`VideoCommon/OutputResampling.h`) turns the source and target sizes into at most three
passes of three kinds:

| Kind | What it does | Fetches per destination pixel |
| --- | --- | --- |
| Table | One axis: Magic Kernel reduction, alone or together with Sharp 2021 on the reduced grid | 3 × ratio, or 9 × ratio with Sharp 2021, plus one table fetch each |
| Sharp | One axis of Sharp 2021 | 7 |
| Enlarge | Magic Kernel enlargement of one or both axes | 3 or 9 |

| Scaling | Pass 1 | Pass 2 | Pass 3 |
| --- | --- | --- | --- |
| Both axes shrink | Table: reduce *a* | Table: reduce and sharpen *b* | Sharp *a* |
| One axis shrinks | Table: reduce *a* | Sharp *a* | |
| Both axes grow | Sharp *x* | Sharp *y* | Enlarge *x* and *y* |
| One axis grows | Sharp *a* | Enlarge *a* | |
| *a* shrinks, *b* grows | Sharp *b* | Table: reduce and sharpen *a* | Enlarge *b* |

For 2560×1920 to 1920×1080 that is a 6-tap vertical reduction into a 2560×1080 target, a
12-tap horizontal reduction-and-sharpen into 1920×1080, and the 7-tap vertical sharpen
in the final draw.

Only the first pass decodes RGB, with the same `LinearizeSample` helper and configured game
gamma as the existing output filters, without assuming sRGB. It is the pass with the fewest
taps: a plain reduction or one axis of sharpening. When both axes shrink, the first pass takes
the axis that leaves the smaller intermediate (2560×1080 instead of 1920×1920 above), and the
second axis's reduction and sharpening are one set of weights.

Every tap is a point fetch (`texelFetch`), so the result does not depend on the sampler's
subtexel precision. A Table pass reads its weights from a small R32F texture with one row
per destination pixel along the axis: the index of the first source texel, then the weights.
`CreateOutputResamplingTable` computes them in double precision when the sizes or the
sharpness change (they stay on the GPU otherwise): each reduced pixel's Magic Kernel weights
are normalized on their own, taps past the image edge are folded into the edge texel, and
Sharp 2021 mixes the reduced pixels *i* − 3 … *i* + 3 clamped to the image, exactly as
separate reduce and sharpen passes would. Kernel ends that land on a texel with zero weight
are dropped, and rows near the end of the source are slid back so the shader never reads
outside the source rectangle. The filter does not truncate its support for large reductions.

`PostProcessing` compiles a copy of the default shader for each kind of pass in use (kind,
axis, whether it decodes, whether it is the last), with those as constants; only the tap
count and the Sharp 2021 weights are uniforms. A resize therefore changes weights and
uniforms and compiles nothing, unless it changes the kind of scaling. The copies are
compiled on first use and dropped when another output resampling mode is selected. The
table loop takes its taps in groups of 8, 4, 2 and 1, because the GPU overlaps the fetches
within a group but not across the turns of a loop. The ordinary default shader contains
none of this code.

Two persistent RGBA16F targets hold filter intermediates. Their sizes, layer counts, the
pass plan and the tables update when the source or destination changes. Negative values
survive all intermediate passes. Each intermediate pass rounds its result to the nearest
half-float itself: some GPUs round towards zero when they write one, which
darkens every pass by about 0.025% and turns 1.0 into 0.9995. The final pass floors negative
RGB before output encoding; positive overshoot is retained for HDR. No local anti-ringing
clamp is applied. The final pass draws where the default pipeline would have: to the
existing next stage, or, when a user post-processing shader is active, to its existing
intermediary buffer. Source reads clamp to the visible rectangle and output sizes are per
stereo view. A 1:1 copy needs no filter intermediates, including when the source has XFB
stride padding. If a texture, table or pass shader cannot be made, Area Sampling uses the
original gamma-encoded source, including when an earlier filter pass already ran.

This is additional GPU work and memory. Area Sampling's existing implementation also
restores averaged gamma-space luminance, which should be included in visual comparisons.

## Tests

`dolphin/Tools/test-magic-kernel-sharp.py` renders the shipped shader through EGL/OpenGL,
one program per kind of pass as `PostProcessing` compiles them, and compares it against a
double-precision Magic Kernel plus Sharp 2021 reference made of dense matrices. Install
`numpy` and `moderngl` to run it; an EGL implementation is required. Cases include integer,
fractional and greater-than-16x reductions, enlargements, mixed directions, unchanged axes,
one-pixel dimensions, cropped images, constant colors, checkerboard antialiasing, signed
lobes, linear output, separate source layers, strength, non-sRGB game gamma, and final-only
color correction and HDR scaling. Single table, sharpen and enlarge passes are compared
with the reference rounded to a half-float. `OutputResamplingTest` checks the C++ pass
planner for every scale direction and the C++ tables against a dense reference, including
tiny and extreme sizes and partial sharpness.

In the emulator itself (P+, a paused match, Vulkan and OpenGL, the window captured from
the screen), the presented picture was compared with the
same reference computed from the raw XFB of the same frame, for 4× to 1080p and 720p, 3× to
1080p (one axis shrinks), 1× to 1080p (both grow) and 2× to 1600×900 (one shrinks, one
grows). The mean difference is 0.21–0.23 of an 8-bit code, which is the output rounding;
at most 0.001% of the values are more than one code away, and the largest difference is
about 3 codes (6 when enlarging), next to black where the output gamma stretches small errors.
Switching to Area Sampling and back, changing the sharpness, the internal resolution, a
user post-processing shader and side-by-side stereo while running all gave the expected
picture again.

## GPU measurements

An isolated OpenGL/EGL benchmark on one GPU used
three raw 2560×1920 XFB captures of P+ matches at 4× internal resolution (not in the
repository). Each method had persistent
targets and resident inputs. After warming up, 21 randomized rounds timed batches of
192 frames with GPU elapsed-time queries. Values below are the median of the three
scene medians, in milliseconds per output frame.

| Output | Default hardware bilinear | Gamma-correct Bilinear | Area Sampling | MKS 2021 |
| --- | ---: | ---: | ---: | ---: |
| 1280×720 | 0.007 | 0.030 | 0.089 | 0.163 |
| 1920×1080 | 0.016 | 0.067 | 0.158 | 0.299 |

At 1080p, MKS adds approximately 0.28 ms over Default, or about 1.7% of a 60 Hz frame
budget, and costs 1.9× Area Sampling. Its three scene medians ranged from 0.282 to
0.356 ms. Measured draw by draw, the vertical reduction with linearization takes about
0.17 ms, the horizontal reduction and sharpen 0.09 ms, and the final sharpen 0.04 ms. The
two intermediate textures consume approximately 36.9 MiB at this source/output size
(21.1 MiB for 720p).

Other scalings, one capture each (11 rounds):

| Source | Output | Passes | Gamma-correct Bilinear | Area Sampling | MKS 2021 |
| --- | --- | --- | ---: | ---: | ---: |
| 640×480 | 1920×1080 | sharp, sharp, enlarge | 0.062 | 0.089 | 0.062 |
| 640×480 | 2560×1440 | sharp, sharp, enlarge | 0.112 | 0.159 | 0.090 |
| 1280×960 | 1920×1080 | sharp, sharp, enlarge | 0.064 | 0.093 | 0.124 |
| 1280×960 | 2560×1440 | sharp, sharp, enlarge | 0.117 | 0.169 | 0.154 |
| 1920×1440 | 1920×1080 | table, sharp | 0.065 | 0.123 | 0.100 |
| 1920×1440 | 2560×1440 | sharp, enlarge | 0.116 | 0.167 | 0.169 |

What decided the layout, on the GPU measured. A first version (a horizontal Magic Kernel pass with
point taps, a vertical one with bilinear pairs and a 49-tap 2D sharpen, in one shader that
branched on uniforms) took 0.332 ms at 720p and 0.600 ms at 1080p in the same benchmark:

- Bytes written and read per frame cost as much as the taps. Even a plain copy through one
  more 1920×1080 RGBA16F target costs 0.05 ms or more, so the passes are few and the first
  one leaves the smaller of the two possible intermediates.
- A loop over taps with its count in a uniform block runs each fetch after the previous one.
  Groups of taps written out in a row make a table pass a quarter to a third cheaper.
- A shader that selects the pass with uniforms keeps code the pass does not need (the final
  sharpen pass cost twice as much inside it as on its own).
- Kernel weights from a table cost less than computing the quadratic per tap, and make the
  combined reduce-and-sharpen pass no more work per tap than a plain reduction.

These timings exclude compilation, allocation, uploads, readback, PNG encoding,
game rendering and presentation. They measure the current shader pipeline, rather
than end-to-end Dolphin FPS, another GPU or another graphics backend. The recommendation above
is unchanged pending actual game frame-time and motion comparisons.

Run it with `dolphin/Tools/benchmark-output-resampling.py --source-dir <captures>
--output <file>.json`. The tool needs `numpy`, `moderngl`, `Pillow`, EGL, and raw XFB
screenshots named `*-source-4x.png`; `--pattern` and `--outputs` select other captures
and output sizes. Raw timings, driver and shader information,
source hashes and sampling settings are recorded in its JSON.
