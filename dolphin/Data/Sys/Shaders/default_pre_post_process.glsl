/***** COLOR CORRECTION *****/

// Color Space references:
// https://www.unravel.com.au/understanding-color-spaces

// SMPTE 170M - BT.601 (NTSC-M) -> BT.709
mat3 from_NTSCM = transpose(mat3(
	0.939497225737661,		0.0502268452914346,		0.0102759289709032,
	0.0177558637510127,		0.965824605885027,		0.0164195303639603,
	-0.00162163209967010,	-0.00437400622653655,	1.00599563832621));

// ARIB TR-B9 (9300K+27MPCD with chromatic adaptation) (NTSC-J) -> BT.709
mat3 from_NTSCJ = transpose(mat3(
	0.768497526,		-0.210804164,	  0.000297427177,
	0.0397904068,		1.04825413,			0.00555809540,
	0.00147510506,	0.0328789241,		1.36515128));

// EBU - BT.470BG/BT.601 (PAL) -> BT.709
mat3 from_PAL = transpose(mat3(
	1.04408168421813,		-0.0440816842181253,	0.000000000000000,
	0.000000000000000,	1.00000000000000,			0.000000000000000,
	0.000000000000000,	0.0118044782106489,		0.988195521789351));

float3 LinearTosRGBGamma(float3 color)
{
	const float a = 0.055;

	for (int i = 0; i < 3; ++i)
	{
		float x = color[i];
		if (x <= 0.0031308)
			x = x * 12.92;
		else
			x = (1.0 + a) * pow(x, 1.0 / 2.4) - a;
		color[i] = x;
	}

	return color;
}

float3 Rec709_Luminance = float3(0.2126, 0.7152, 0.0722);

float Luminance(float3 color, bool native_color_space)
{
	if (native_color_space && OptionEnabled(correct_color_space))
	{
		if (game_color_space == 0)
			color *= from_NTSCM;
		else if (game_color_space == 1)
			color *= from_NTSCJ;
		else if (game_color_space == 2)
			color *= from_PAL;
	}
	return dot(color, Rec709_Luminance);
}

/***** COLOR SAMPLING *****/

float4 LinearizeSample(float4 color, float gamma)
{
	color.rgb = pow(color.rgb, float3(gamma));
	return color;
}

// Non filtered gamma corrected sample (nearest neighbor)
float4 QuickSample(float3 uvw, float gamma)
{
#if 0 // Test sampling range
	const float threshold = 0.00000001;
	float2 xy = uvw.xy * GetResolution();
	// Sampling outside the valid range, draw in yellow
	if (xy.x < (0.0 - threshold) || xy.x > (GetResolution().x + threshold) || xy.y < (0.0 - threshold) || xy.y > (GetResolution().y + threshold))
		return float4(1.0, 1.0, 0.0, 1);
	// Sampling at the edges, draw in purple
	if (xy.x < 1.0 || xy.x > (GetResolution().x - 1.0) || xy.y < 1.0 || xy.y > (GetResolution().y - 1.0))
		return float4(0.5, 0, 0.5, 1);
#endif

	return LinearizeSample(texture(samp1, uvw), gamma);
}
float4 QuickSample(float2 uv, float w, float gamma)
{
	return QuickSample(float3(uv, w), gamma);
}
float4 QuickSampleByPixel(float2 xy, float w, float gamma)
{
	float3 uvw = float3(xy * GetInvResolution(), w);
	return QuickSample(uvw, gamma);
}

/***** Bilinear Interpolation *****/

float4 BilinearSample(float3 uvw, float gamma)
{
	// This emulates the (bi)linear filtering done directly from GPUs HW.
	// Note that GPUs might natively filter red green and blue differently, but we don't do it.
	// They might also use different filtering between upscaling and downscaling.
	float2 source_size = GetResolution();
	float2 pixel = (uvw.xy * source_size) - 0.5; // Try to find the matching pixel top left corner

	// Find the integer and floating point parts
	float2 int_pixel = floor(pixel);
	float2 frac_pixel = fract(pixel);

	// Take 4 samples around the original uvw
	float4 c11 = QuickSampleByPixel(int_pixel + float2(0.5, 0.5), uvw.z, gamma);
	float4 c21 = QuickSampleByPixel(int_pixel + float2(1.5, 0.5), uvw.z, gamma);
	float4 c12 = QuickSampleByPixel(int_pixel + float2(0.5, 1.5), uvw.z, gamma);
	float4 c22 = QuickSampleByPixel(int_pixel + float2(1.5, 1.5), uvw.z, gamma);

	// Blend the 4 samples by their weight
	float4 avg_c = lerp(lerp(c11, c21, frac_pixel.x), lerp(c12, c22, frac_pixel.x), frac_pixel.y);

	// Calculate the average of the gamma space luminance, as that can be used to
	// properly determine the perceptual brightness the color should have.
	float l11 = pow(Luminance(c11.rgb, true), 1.0 / gamma);
	float l21 = pow(Luminance(c21.rgb, true), 1.0 / gamma);
	float l12 = pow(Luminance(c12.rgb, true), 1.0 / gamma);
	float l22 = pow(Luminance(c22.rgb, true), 1.0 / gamma);
	float target_avg_c_lum = lerp(lerp(l11, l21, frac_pixel.x), lerp(l12, l22, frac_pixel.x), frac_pixel.y);

	float avg_c_lum = Luminance(avg_c.rgb, true);
	if (avg_c_lum != 0.0)
	{
		avg_c.rgb *= pow(target_avg_c_lum, gamma) / avg_c_lum;
	}

	return avg_c;
}

/***** Bicubic Interpolation *****/

// Formula derived from:
// https://en.wikipedia.org/wiki/Mitchell%E2%80%93Netravali_filters#Definition
// Values from:
// https://guideencodemoe-mkdocs.readthedocs.io/encoding/resampling/#mitchell-netravali-bicubic
// Other references:
// https://www.codeproject.com/Articles/236394/Bi-Cubic-and-Bi-Linear-Interpolation-with-GLSL
// https://github.com/ValveSoftware/gamescope/pull/740
// https://stackoverflow.com/questions/13501081/efficient-bicubic-filtering-code-in-glsl
#define CUBIC_COEFF_GEN(B, C)																						\
	(mat4(/* t^0 */ ((B) / 6.0), (-(B) / 3.0 + 1.0), ((B) / 6.0), (0.0),	\
		/* t^1 */ (-(B) / 2.0 - (C)), (0.0), ((B) / 2.0 + (C)), (0.0),			\
		/* t^2 */ ((B) / 2.0 + 2.0 * (C)), (2.0 * (B) + (C)-3.0),						\
		(-5.0 * (B) / 2.0 - 2.0 * (C) + 3.0), (-(C)),												\
		/* t^3 */ (-(B) / 6.0 - (C)), (-3.0 * (B) / 2.0 - (C) + 2.0),				\
		(3.0 * (B) / 2.0 + (C)-2.0), ((B) / 6.0 + (C))))

float4 CubicCoeffs(float t, mat4 coeffs)
{
	return coeffs * float4(1.0, t, t * t, t * t * t);
}

float4 CubicMix(float4 c0, float4 c1, float4 c2, float4 c3, float4 coeffs)
{
	return c0 * coeffs[0] + c1 * coeffs[1] + c2 * coeffs[2] + c3 * coeffs[3];
}

// By Sam Belliveau. Public Domain license.
// Simple 16 tap, gamma correct, implementation of bicubic filtering.
float4 BicubicSample(float3 uvw, float gamma, mat4 coeffs)
{
	float2 pixel = (uvw.xy * GetResolution()) - 0.5;
	float2 int_pixel = floor(pixel);
	float2 frac_pixel = fract(pixel);

	float4 c00 = QuickSampleByPixel(int_pixel + float2(-0.5, -0.5), uvw.z, gamma);
	float4 c10 = QuickSampleByPixel(int_pixel + float2(+0.5, -0.5), uvw.z, gamma);
	float4 c20 = QuickSampleByPixel(int_pixel + float2(+1.5, -0.5), uvw.z, gamma);
	float4 c30 = QuickSampleByPixel(int_pixel + float2(+2.5, -0.5), uvw.z, gamma);

	float4 c01 = QuickSampleByPixel(int_pixel + float2(-0.5, +0.5), uvw.z, gamma);
	float4 c11 = QuickSampleByPixel(int_pixel + float2(+0.5, +0.5), uvw.z, gamma);
	float4 c21 = QuickSampleByPixel(int_pixel + float2(+1.5, +0.5), uvw.z, gamma);
	float4 c31 = QuickSampleByPixel(int_pixel + float2(+2.5, +0.5), uvw.z, gamma);

	float4 c02 = QuickSampleByPixel(int_pixel + float2(-0.5, +1.5), uvw.z, gamma);
	float4 c12 = QuickSampleByPixel(int_pixel + float2(+0.5, +1.5), uvw.z, gamma);
	float4 c22 = QuickSampleByPixel(int_pixel + float2(+1.5, +1.5), uvw.z, gamma);
	float4 c32 = QuickSampleByPixel(int_pixel + float2(+2.5, +1.5), uvw.z, gamma);

	float4 c03 = QuickSampleByPixel(int_pixel + float2(-0.5, +2.5), uvw.z, gamma);
	float4 c13 = QuickSampleByPixel(int_pixel + float2(+0.5, +2.5), uvw.z, gamma);
	float4 c23 = QuickSampleByPixel(int_pixel + float2(+1.5, +2.5), uvw.z, gamma);
	float4 c33 = QuickSampleByPixel(int_pixel + float2(+2.5, +2.5), uvw.z, gamma);

	float4 cx = CubicCoeffs(frac_pixel.x, coeffs);
	float4 cy = CubicCoeffs(frac_pixel.y, coeffs);

	float4 x0 = CubicMix(c00, c10, c20, c30, cx);
	float4 x1 = CubicMix(c01, c11, c21, c31, cx);
	float4 x2 = CubicMix(c02, c12, c22, c32, cx);
	float4 x3 = CubicMix(c03, c13, c23, c33, cx);

	return CubicMix(x0, x1, x2, x3, cy);
}

/***** Sharp Bilinear Filtering *****/

// Based on https://github.com/libretro/slang-shaders/blob/master/interpolation/shaders/sharp-bilinear.slang
// by Themaister, Public Domain license
// Does a bilinear stretch, with a preapplied Nx nearest-neighbor scale,
// giving a sharper image than plain bilinear.
float4 SharpBilinearSample(float3 uvw, float gamma)
{
	float2 source_size = GetResolution();
	float2 inverted_source_size = GetInvResolution();
	float2 target_size = GetWindowResolution();
	float2 texel = uvw.xy * source_size;
	float2 texel_floored = floor(texel);
	float2 s = fract(texel);
	float scale = max(floor(max(target_size.x * inverted_source_size.x, target_size.y * inverted_source_size.y)), 1.0);
	float region_range = 0.5 - (0.5 / scale);

	// Figure out where in the texel to sample to get correct pre-scaled bilinear.

	float2 center_dist = s - 0.5;
	float2 f = ((center_dist - clamp(center_dist, -region_range, region_range)) * scale) + 0.5;

	float2 mod_texel = texel_floored + f;

	uvw.xy = mod_texel * inverted_source_size;
	return BilinearSample(uvw, gamma);
}

/***** Area Sampling *****/

// By Sam Belliveau and Filippo Tarpini. Public Domain license.
// Effectively a more accurate sharp bilinear filter when upscaling,
// that also works as a mathematically perfect downscale filter.
// https://entropymine.com/imageworsener/pixelmixing/
// https://github.com/obsproject/obs-studio/pull/1715
// https://legacy.imagemagick.org/Usage/filter/
float4 AreaSampling(float3 uvw, float gamma)
{
	// Determine the sizes of the source and target images.
	float2 source_size = GetResolution();
	float2 target_size = GetWindowResolution();
	float2 inverted_target_size = GetInvWindowResolution();

	// Compute the top-left and bottom-right corners of the target pixel box.
	float2 t_beg = floor(uvw.xy * target_size);
	float2 t_end = t_beg + float2(1.0, 1.0);

	// Convert the target pixel box to source pixel box.
	float2 beg = t_beg * inverted_target_size * source_size;
	float2 end = t_end * inverted_target_size * source_size;

	// Compute the top-left and bottom-right corners of the pixel box.
	float2 f_beg = floor(beg);
	float2 f_end = floor(end);

	// Compute how much of the start and end pixels are covered horizontally & vertically.
	float area_w = 1.0 - fract(beg.x);
	float area_n = 1.0 - fract(beg.y);
	float area_e = fract(end.x);
	float area_s = fract(end.y);

	// Compute the areas of the corner pixels in the pixel box.
	float area_nw = area_n * area_w;
	float area_ne = area_n * area_e;
	float area_sw = area_s * area_w;
	float area_se = area_s * area_e;

	// Initialize the color accumulator.
	float4 avg_color = float4(0.0, 0.0, 0.0, 0.0);
	float avg_luminance = 0.0;
	float4 temp_color;

	float luminance_gamma = gamma; // For now, default to the user selected gamma (alternatively we could fix it to anything between 2.2 and 3.0)
	float luminance_inv_gamma = 1.0 / luminance_gamma;

	// Prevents rounding errors due to the coordinates flooring above
	const float2 offset = float2(0.5, 0.5);

	// Accumulate corner pixels.
	temp_color = QuickSampleByPixel(float2(f_beg.x, f_beg.y) + offset, uvw.z, gamma);
	avg_color += area_nw * temp_color;
	avg_luminance += area_nw * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
	temp_color = QuickSampleByPixel(float2(f_end.x, f_beg.y) + offset, uvw.z, gamma);
	avg_color += area_ne * temp_color;
	avg_luminance += area_ne * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
	temp_color = QuickSampleByPixel(float2(f_beg.x, f_end.y) + offset, uvw.z, gamma);
	avg_color += area_sw * temp_color;
	avg_luminance += area_sw * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
	temp_color = QuickSampleByPixel(float2(f_end.x, f_end.y) + offset, uvw.z, gamma);
	avg_color += area_se * temp_color;
	avg_luminance += area_se * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);

	// Determine the size of the pixel box.
	int x_range = int(f_end.x - f_beg.x - 0.5);
	int y_range = int(f_end.y - f_beg.y - 0.5);

	// Workaround to compile the shader with DX11/12.
	// If this isn't done, it will complain that the loop could have too many iterations.
	// This number should be enough to guarantee downscaling from very high to very small resolutions.
	// Note that this number might be referenced in the UI.
	const int max_iterations = 16;

	// Fix up the average calculations in case we reached the upper limit
	x_range = min(x_range, max_iterations);
	y_range = min(y_range, max_iterations);

	// Accumulate top and bottom edge pixels.
	for (int ix = 0; ix < max_iterations; ++ix)
	{
		if (ix < x_range)
		{
			float x = f_beg.x + 1.0 + float(ix);
			temp_color = QuickSampleByPixel(float2(x, f_beg.y) + offset, uvw.z, gamma);
			avg_color += area_n * temp_color;
			avg_luminance += area_n * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
			temp_color = QuickSampleByPixel(float2(x, f_end.y) + offset, uvw.z, gamma);
			avg_color += area_s * temp_color;
			avg_luminance += area_s * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
		}
	}

	// Accumulate left and right edge pixels and all the pixels in between.
	for (int iy = 0; iy < max_iterations; ++iy)
	{
		if (iy < y_range)
		{
			float y = f_beg.y + 1.0 + float(iy);
			
			temp_color = QuickSampleByPixel(float2(f_beg.x, y) + offset, uvw.z, gamma);
			avg_color += area_w * temp_color;
			avg_luminance += area_w * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
			temp_color = QuickSampleByPixel(float2(f_end.x, y) + offset, uvw.z, gamma);
			avg_color += area_e * temp_color;
			avg_luminance += area_e * pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);

			for (int ix = 0; ix < max_iterations; ++ix)
			{
				if (ix < x_range)
				{
					float x = f_beg.x + 1.0 + float(ix);
					temp_color = QuickSampleByPixel(float2(x, y) + offset, uvw.z, gamma);
					avg_color += temp_color;
					avg_luminance += pow(Luminance(temp_color.rgb, true), luminance_inv_gamma);
				}
			}
		}
	}

	// Compute the area of the pixel box that was sampled.
	float area_corners = area_nw + area_ne + area_sw + area_se;
	float area_edges = float(x_range) * (area_n + area_s) + float(y_range) * (area_w + area_e);
	float area_center = float(x_range) * float(y_range);
	
	float4 nrm_color = avg_color / (area_corners + area_edges + area_center);
	float target_nrm_color_luminance = avg_luminance / (area_corners + area_edges + area_center);

	// Restore the averaged "gamma" space luminance, for better gamma correction.
	// This retains the best feature of gamma correct sampling (no hue shifts),
	// while also maintaining the perceptual "brightness" level of blending two colors with an alpha
	// (in linear space a 0.5 alpha won't produce a color that has a perceptual brightness in the middle point of the two source colors).
	float nrm_color_luminance = Luminance(nrm_color.rgb, true);
	if (nrm_color_luminance != 0.0)
	{
		nrm_color.rgb *= pow(target_nrm_color_luminance, luminance_gamma) / nrm_color_luminance;
	}

	// Return the normalized average color.
	return nrm_color;
}

/***** Magic Kernel Sharp *****/

// John Costella's Magic Kernel Sharp 2021: https://johncostella.com/magic/
// PostProcessing.cpp runs it as up to three passes (see OutputResampling.h) and compiles a
// copy of this shader for each kind of pass, with these defined as constants:
//   RESAMPLING_PASS          1 = table, 2 = sharpen, 3 = enlarge
//   RESAMPLING_AXES          table and sharpen: 0 = horizontal, 1 = vertical
//                            enlarge: bit 0 = horizontal, bit 1 = vertical
//   RESAMPLING_DECODE_INPUT  1 for the first pass, which reads the gamma-encoded source
//   RESAMPLING_FINAL_PASS    1 for the last pass, which goes on to color correction and encoding
#ifndef RESAMPLING_PASS
#define RESAMPLING_PASS 0
#endif

#if RESAMPLING_PASS != 0

#if RESAMPLING_DECODE_INPUT
#define ResamplingFetch(texel) LinearizeSample(texelFetch(samp1, texel, 0), game_gamma)
#else
#define ResamplingFetch(texel) texelFetch(samp1, texel, 0)
#endif

// The first texel of the source rectangle, and the source layer.
int3 ResamplingOrigin()
{
	// Match ordinary array sampling when only one stereo source layer is available.
	int layer = clamp(int(v_tex0.z), 0, textureSize(samp1, 0).z - 1);
	return int3(int2(floor(src_rect.xy * GetResolution() + 0.5)), layer);
}

// The nearest value a half-float target can hold. Some GPUs round towards zero when they
// write one, which would darken every pass a little and turn 1.0 into 0.9995.
float4 RoundToHalfFloat(float4 color)
{
	// Half-floats have 11 significant bits down to 2^-14, and steps of 2^-24 below that.
	float4 magnitude = max(abs(color), float4(6.103515625e-05, 6.103515625e-05, 6.103515625e-05, 6.103515625e-05));
	float4 step_size = exp2(floor(log2(magnitude)) - 10.0);
	return round(color / step_size) * step_size;
}

#endif

#if RESAMPLING_PASS == 1

// Taps i + k of the table row, in groups: the GPU overlaps the fetches of one group,
// but waits for each turn of a loop.
#define RESAMPLING_TAP(k) color += texelFetch(samp2, int3(i + (k), index, 0), 0).r * ResamplingFetch(tap + texel_step * (k));
#define RESAMPLING_TAPS_2(k) RESAMPLING_TAP(k) RESAMPLING_TAP((k) + 1)
#define RESAMPLING_TAPS_4(k) RESAMPLING_TAPS_2(k) RESAMPLING_TAPS_2((k) + 2)
#define RESAMPLING_TAPS_8(k) RESAMPLING_TAPS_4(k) RESAMPLING_TAPS_4((k) + 4)

// One axis, point taps. Row i of the table in samp2 is for output pixel i along the axis:
// the first source texel, then resampling_taps weights (Magic Kernel reduction, with or
// without Sharp 2021 on the reduced grid). The table never reads outside the source rectangle.
float4 ResamplingTablePass()
{
	int3 origin = ResamplingOrigin();
	// The other axis is unchanged in this pass, so v_tex0 is at a texel center there.
	int2 texel = int2(floor(v_tex0.xy * GetResolution()));
	// Along the filtered axis, v_tex0 crosses the source rectangle over the target's pixels.
	float2 target_pixel = (v_tex0.xy - src_rect.xy) / src_rect.zw * GetWindowResolution();
#if RESAMPLING_AXES == 0
	int index = int(floor(target_pixel.x));
	int3 texel_step = int3(1, 0, 0);
	int3 tap = int3(origin.x, texel.y, origin.z);
#else
	int index = int(floor(target_pixel.y));
	int3 texel_step = int3(0, 1, 0);
	int3 tap = int3(texel.x, origin.y, origin.z);
#endif
	tap += texel_step * int(texelFetch(samp2, int3(0, index, 0), 0).r);

	float4 color = float4(0.0, 0.0, 0.0, 0.0);
	int i = 1;
	while (i + 7 <= resampling_taps)
	{
		RESAMPLING_TAPS_8(0)
		tap += texel_step * 8;
		i += 8;
	}
	if (i + 3 <= resampling_taps)
	{
		RESAMPLING_TAPS_4(0)
		tap += texel_step * 4;
		i += 4;
	}
	if (i + 1 <= resampling_taps)
	{
		RESAMPLING_TAPS_2(0)
		tap += texel_step * 2;
		i += 2;
	}
	if (i <= resampling_taps)
	{
		RESAMPLING_TAP(0)
	}
	return color;
}

#elif RESAMPLING_PASS == 2

// One axis of Sharp 2021, [-1, 6, -35, 204, -35, 6, -1] / 144, on the grid of its source.
// resampling_sharp_weights holds the center weight and the three others, already blended
// with the identity by the sharpness setting.
float4 ResamplingSharpPass()
{
	int3 origin = ResamplingOrigin();
	int2 first_texel = origin.xy;
	int2 last_texel = first_texel + int2(floor(src_rect.zw * GetResolution() + 0.5)) - int2(1, 1);
	int2 texel = int2(floor(v_tex0.xy * GetResolution()));
#if RESAMPLING_AXES == 0
	int2 texel_step = int2(1, 0);
#else
	int2 texel_step = int2(0, 1);
#endif

	float4 color = float4(0.0, 0.0, 0.0, 0.0);
	for (int i = -3; i <= 3; ++i)
		color += resampling_sharp_weights[abs(i)] * ResamplingFetch(int3(clamp(texel + texel_step * i, first_texel, last_texel), origin.z));
	return color;
}

#elif RESAMPLING_PASS == 3

// Magic Kernel enlargement of one or both axes: the three nearest texels of each.
float4 ResamplingEnlargePass()
{
	int3 origin = ResamplingOrigin();
	int2 first_texel = origin.xy;
	int2 last_texel = first_texel + int2(floor(src_rect.zw * GetResolution() + 0.5)) - int2(1, 1);
	// Position in texels, with texel centers on the integers, and the nearest texel.
	// An unchanged axis is at a texel center and takes that texel alone.
	float2 position = v_tex0.xy * GetResolution() - 0.5;
	float2 nearest = floor(position + 0.5);
	float2 offset = position - nearest;
	// Quadratic B-spline weights of the texels before, at and after the nearest one.
	float2 before = 0.5 * (0.5 - offset) * (0.5 - offset);
	float2 center = 0.75 - offset * offset;
	float2 after = 0.5 * (0.5 + offset) * (0.5 + offset);
	int2 texel = clamp(int2(nearest), first_texel, last_texel);
	int2 texel_before = max(texel - int2(1, 1), first_texel);
	int2 texel_after = min(texel + int2(1, 1), last_texel);

#if RESAMPLING_AXES == 1
	return before.x * ResamplingFetch(int3(texel_before.x, texel.y, origin.z)) +
	       center.x * ResamplingFetch(int3(texel.x, texel.y, origin.z)) +
	       after.x * ResamplingFetch(int3(texel_after.x, texel.y, origin.z));
#elif RESAMPLING_AXES == 2
	return before.y * ResamplingFetch(int3(texel.x, texel_before.y, origin.z)) +
	       center.y * ResamplingFetch(int3(texel.x, texel.y, origin.z)) +
	       after.y * ResamplingFetch(int3(texel.x, texel_after.y, origin.z));
#else
#define RESAMPLING_ROW(y) (before.x * ResamplingFetch(int3(texel_before.x, y, origin.z)) + \
                           center.x * ResamplingFetch(int3(texel.x, y, origin.z)) + \
                           after.x * ResamplingFetch(int3(texel_after.x, y, origin.z)))
	return before.y * RESAMPLING_ROW(texel_before.y) + center.y * RESAMPLING_ROW(texel.y) +
	       after.y * RESAMPLING_ROW(texel_after.y);
#endif
}

#endif

/***** Main Functions *****/

// Returns an accurate (gamma corrected) sample of a gamma space space texture.
// Outputs in linear space for simplicity.
float4 LinearGammaCorrectedSample(float gamma)
{
	float3 uvw = v_tex0;
	float4 color = float4(0, 0, 0, 1);

	if (resampling_method <= 1) // Bilinear
	{
		color = BilinearSample(uvw, gamma);
	}
	else if (resampling_method == 2) // Bicubic: B-Spline
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(1.0, 0.0));
	}
	else if (resampling_method == 3) // Bicubic: Mitchell-Netravali
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(1.0 / 3.0, 1.0 / 3.0));
	}
	else if (resampling_method == 4) // Bicubic: Catmull-Rom
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(0.0, 0.5));
	}
	else if (resampling_method == 5) // Sharp Bilinear
	{
		color = SharpBilinearSample(uvw, gamma);
	}
	else if (resampling_method == 6) // Area Sampling
	{
		color = AreaSampling(uvw, gamma);
	}
	else if (resampling_method == 7) // Nearest Neighbor
	{
		color = QuickSample(uvw, gamma);
	}
	else if (resampling_method == 8) // Bicubic: Hermite
	{
		color = BicubicSample(uvw, gamma, CUBIC_COEFF_GEN(0.0, 0.0));
	}
	else if (resampling_method == 9) // Magic Kernel Sharp: fallback if the extra render target failed
	{
		// No extra pass is needed for a 1:1 copy, even when the texture has stride padding.
		float2 source_extent = floor(src_rect.zw * GetResolution() + 0.5);
		if (source_extent == GetWindowResolution())
			color = QuickSample(uvw, gamma);
		else
			color = AreaSampling(uvw, gamma);
	}

	return color;
}

void main()
{
	// This tries to fall back on GPU HW sampling if it can (it won't be gamma corrected).
	bool raw_resampling = resampling_method <= 0;
	bool needs_rescaling = GetResolution() != GetWindowResolution();

	bool needs_resampling = needs_rescaling && (OptionEnabled(hdr_output) || OptionEnabled(correct_gamma) || !raw_resampling);

	float4 color;

#if RESAMPLING_PASS != 0
#if RESAMPLING_PASS == 1
	color = ResamplingTablePass();
#elif RESAMPLING_PASS == 2
	color = ResamplingSharpPass();
#else
	color = ResamplingEnlargePass();
#endif
#if !RESAMPLING_FINAL_PASS
	// Signed linear light goes from pass to pass.
	SetOutput(RoundToHalfFloat(color));
	return;
#endif
	// Negative filter lobes can undershoot black; pow() requires nonnegative input.
	color.rgb = max(color.rgb, float3(0.0, 0.0, 0.0));
	color.a = clamp(color.a, 0.0, 1.0);
#else
	if (needs_resampling)
	{
		// Doing linear sampling in "gamma space" on linear texture formats isn't correct.
		// If the source and target resolutions don't match, the GPU will return a color
		// that is the average of 4 gamma space colors, but gamma space colors can't be blended together,
		// gamma needs to be de-applied first. This makes a big difference if colors change
		// drastically between two pixels.

		color = LinearGammaCorrectedSample(game_gamma);
	}
	else
	{
		// Default GPU HW sampling. Bilinear is identical to Nearest Neighbor if the input and output resolutions match.
		if (needs_rescaling)
			color = texture(samp0, v_tex0);
		else
			color = texture(samp1, v_tex0);

		// Convert to linear before doing any other of follow up operations.
		color.rgb = pow(color.rgb, float3(game_gamma));
	}
#endif

	if (OptionEnabled(correct_color_space))
	{
		if (game_color_space == 0)
			color.rgb = color.rgb * from_NTSCM;
		else if (game_color_space == 1)
			color.rgb = color.rgb * from_NTSCJ;
		else if (game_color_space == 2)
			color.rgb = color.rgb * from_PAL;
	}

	if (OptionEnabled(hdr_output))
	{
		float hdr_paper_white = hdr_paper_white_nits / hdr_sdr_white_nits;
		color.rgb *= hdr_paper_white;
	}

	if (OptionEnabled(linear_space_output))
	{
		// Nothing to do here
	}
	// Correct the SDR gamma for sRGB (PC/Monitor) or ~2.2 (Common TV gamma)
	else if (OptionEnabled(correct_gamma))
	{
		if (OptionEnabled(sdr_display_gamma_sRGB))
			color.rgb = LinearTosRGBGamma(color.rgb);
		else
			color.rgb = pow(color.rgb, float3(1.0 / sdr_display_custom_gamma));
	}
	// Restore the original gamma without changes
	else
	{
		color.rgb = pow(color.rgb, float3(1.0 / game_gamma));
	}

	SetOutput(color);
}
