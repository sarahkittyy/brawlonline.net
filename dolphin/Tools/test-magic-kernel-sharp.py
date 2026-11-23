#!/usr/bin/env python3
# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Render the shipped shader and compare it with an independent double-precision reference.

Requires numpy, moderngl, and an EGL OpenGL implementation (Mesa software rendering works):
    python -m pip install numpy moderngl
    python dolphin/Tools/test-magic-kernel-sharp.py
"""

import json
import re
import struct
import unittest
from pathlib import Path

import moderngl
import numpy as np

DOLPHIN = Path(__file__).resolve().parents[1]
CPP = (DOLPHIN / "Source/Core/VideoCommon/PostProcessing.cpp").read_text()
# Read the actual builtin uniform declarations and sampling helpers, so the test does not
# maintain a second version of the shader's interface.
UNIFORM_CODE = CPP.split("std::string PostProcessing::GetUniformBufferHeader(", 1)[1]
UNIFORM_CODE = UNIFORM_CODE.split("if (user_post_process)", 1)[0]
UNIFORMS = (
    "".join(
        json.loads('"' + s + '"') for s in re.findall(r'ss << "((?:[^"\\]|\\.)*)"', UNIFORM_CODE)
    )
    + "};\n"
)
HELPERS = (
    CPP.split("std::string PostProcessing::GetHeader(", 1)[1].split('R"(', 1)[1].split(')"', 1)[0]
)
PREFIX = """#version 430
#define float2 vec2
#define float3 vec3
#define float4 vec4
#define int2 ivec2
#define int3 ivec3
#define lerp mix
#define UBO_BINDING(format, index) layout(format, binding = index)
"""
VERTEX = (
    PREFIX
    + UNIFORMS
    + """
out vec3 v_tex0;
void main() {
    vec2 uv = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
    v_tex0 = vec3(src_rect.xy + src_rect.zw * uv, float(src_layer));
}
"""
)


def fragment(operation=None, decode=False, final=True):
    """The default shader, or the copy PostProcessing.cpp compiles for one kind of pass."""
    defines = ""
    if operation:
        defines = (
            f"#define RESAMPLING_PASS {operation['type']}\n"
            f"#define RESAMPLING_AXES {operation['axes']}\n"
            f"#define RESAMPLING_DECODE_INPUT {int(decode)}\n"
            f"#define RESAMPLING_FINAL_PASS {int(final)}\n"
        )
    return (
        PREFIX
        + defines
        + UNIFORMS
        + """
uniform sampler2DArray samp0;
uniform sampler2DArray samp1;
uniform sampler2DArray samp2;
in vec3 v_tex0;
layout(location = 0) out vec4 ocol0;
"""
        + HELPERS
        + (DOLPHIN / "Data/Sys/Shaders/default_pre_post_process.glsl").read_text()
    )


FRAGMENT = fragment()


def make_program(ctx, source):
    program = ctx.program(vertex_shader=VERTEX, fragment_shader=source)
    for unit in range(3):
        if f"samp{unit}" in program:
            program[f"samp{unit}"].value = unit
    return program


def uniform_bytes(values):
    """Pack the shared GLSL std140 block, including its vec2/vec4 alignment."""
    result = bytearray(256)
    offset = 0
    for kind, name in re.findall(r"\b(float4|int2|float|int|uint) (\w+);", UNIFORMS):
        count = {"float4": 4, "int2": 2}.get(kind, 1)
        alignment = count * 4
        offset = (offset + alignment - 1) // alignment * alignment
        value = values.get(name, (0,) * count if count > 1 else 0)
        if count == 1:
            value = (value,)
        code = "f" if kind.startswith("float") else "I" if kind == "uint" else "i"
        struct.pack_into("=" + code * count, result, offset, *value)
        offset += count * 4
    return result[: (offset + 15) // 16 * 16]


SHARP_2021 = np.array([-1, 6, -35, 204, -35, 6, -1], dtype=np.float64) / 144
TABLE, SHARP, ENLARGE = 1, 2, 3
HORIZONTAL, VERTICAL = 0, 1


def reference_weights(source, target):
    """Dense (target, source) Magic Kernel matrix; the kernel only widens when reducing."""
    if source == target:
        return np.eye(source)
    scale = max(source / target, 1.0)
    weights = np.zeros((target, source))

    def quadratic(x):
        x = abs(x)
        return 0.75 - x * x if x < 0.5 else 0.5 * (x - 1.5) ** 2 if x < 1.5 else 0.0

    for out in range(target):
        center = (out + 0.5) * source / target - 0.5
        for tap in range(
            int(np.ceil(center - 1.5 * scale)), int(np.floor(center + 1.5 * scale)) + 1
        ):
            x = (tap - center) / scale
            weights[out, min(max(tap, 0), source - 1)] += quadratic(x)
        weights[out] /= weights[out].sum()
    return weights


def sharp_weights(strength):
    """Sharp 2021 blended with the identity, on one axis."""
    weights = strength * SHARP_2021
    weights[3] += 1 - strength
    return weights


def sharp_matrix(length, strength):
    matrix = np.zeros((length, length))
    for index in range(length):
        for offset, weight in zip(range(-3, 4), sharp_weights(strength)):
            matrix[index, min(max(index + offset, 0), length - 1)] += weight
    return matrix


def plan(source, target):
    """The passes CreateOutputResamplingPlan (OutputResampling.h) makes, as dictionaries."""
    size = list(source)
    result = []

    def table(axis, sharpen):
        lengths = (source[axis], target[axis])
        size[axis] = target[axis]
        result.append(
            {
                "type": TABLE,
                "size": tuple(size),
                "axes": axis,
                "sharpen": sharpen,
                "lengths": lengths,
            }
        )

    def sharp(axis):
        result.append({"type": SHARP, "size": tuple(size), "axes": axis})

    shrinks = [target[axis] < source[axis] for axis in (0, 1)]
    grow_axes = sum(1 << axis for axis in (0, 1) if target[axis] > source[axis])
    if all(shrinks):
        first = VERTICAL if source[0] * target[1] <= target[0] * source[1] else HORIZONTAL
        table(first, False)
        table(first ^ 1, True)
        sharp(first)
    elif any(shrinks):
        axis = HORIZONTAL if shrinks[0] else VERTICAL
        if grow_axes:
            sharp(axis ^ 1)
            table(axis, True)
            result.append({"type": ENLARGE, "size": tuple(target), "axes": grow_axes})
        else:
            table(axis, False)
            sharp(axis)
    elif grow_axes:
        for axis in (0, 1):
            if grow_axes & (1 << axis):
                sharp(axis)
        result.append({"type": ENLARGE, "size": tuple(target), "axes": grow_axes})
    return result


def pass_matrices(operation, size, strength=1.0):
    """What one pass does to an image of `size`, as a (horizontal, vertical) pair of matrices."""
    matrices = [None, None]
    if operation["type"] == TABLE:
        axis = operation["axes"]
        source, target = operation["lengths"]
        matrix = reference_weights(source, target)
        if operation["sharpen"]:
            matrix = sharp_matrix(target, strength) @ matrix
        matrices[axis] = matrix
    elif operation["type"] == SHARP:
        axis = operation["axes"]
        matrices[axis] = sharp_matrix(size[axis], strength)
    else:
        for axis in (0, 1):
            if operation["axes"] & (1 << axis):
                matrices[axis] = reference_weights(size[axis], operation["size"][axis])
    return matrices


def table_data(matrix):
    """The table a Table pass reads: per row, the first texel and then the weights from there."""
    spans = []
    for row in matrix:
        used = np.nonzero(np.abs(row) > 1e-9)[0]
        spans.append((used[0], used[-1] + 1))
    taps = max(last - first for first, last in spans)
    data = np.zeros((len(matrix), taps + 1), dtype="f4")
    for index, (first, last) in enumerate(spans):
        first = min(first, matrix.shape[1] - taps)
        data[index, 0] = first
        data[index, 1:] = matrix[index, first : first + taps]
    return data


def apply_matrices(image, matrices):
    if matrices[0] is not None:
        image = np.einsum("xs,ysc->yxc", matrices[0], image)
    if matrices[1] is not None:
        image = np.einsum("ys,sxc->yxc", matrices[1], image)
    return image


def reference(image, size, gamma=2.2, linear=False, strength=1.0):
    result = image.astype(np.float64).copy()
    result[..., :3] **= gamma
    operations = plan((image.shape[1], image.shape[0]), size)
    for index, operation in enumerate(operations):
        matrices = pass_matrices(operation, (result.shape[1], result.shape[0]), strength)
        result = apply_matrices(result, matrices)
        # Every pass but the last rounds to the nearest half-float, as the shader does itself.
        if index + 1 < len(operations):
            result = result.astype(np.float16).astype(np.float64)
    result[..., :3] = np.maximum(result[..., :3], 0)
    result[..., 3] = np.clip(result[..., 3], 0, 1)
    if not linear:
        result[..., :3] **= 1 / gamma
    return result


class MagicKernelSharpTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.ctx = moderngl.create_standalone_context(backend="egl", require=430)
        cls.programs = {}
        print("Rendering on:", cls.ctx.info["GL_RENDERER"], flush=True)

    @classmethod
    def tearDownClass(cls):
        for program, vao in cls.programs.values():
            vao.release()
            program.release()
        cls.ctx.release()

    def vertex_array(self, operation, decode, final):
        key = (operation["type"], operation["axes"], decode, final) if operation else None
        if key not in self.programs:
            program = make_program(self.ctx, fragment(operation, decode, final))
            self.programs[key] = (program, self.ctx.vertex_array(program, []))
        return self.programs[key][1]

    def render_pass(
        self,
        image,
        size,
        operation,
        crop=None,
        layer=0,
        gamma=2.2,
        linear=False,
        dtype=None,
        decode=False,
        final=True,
        strength=1.0,
        options=None,
    ):
        """Draw one pass. `operation` is None for no filter pass, or a dictionary from plan()."""
        if image.ndim == 3:
            image = image[np.newaxis]
        layers, height, width, _ = image.shape
        x, y, crop_width, crop_height = crop or (0, 0, width, height)
        values = {
            "resolution": (width, height, 1 / width, 1 / height),
            "target_resolution": (*size, 1 / size[0], 1 / size[1]),
            "window_resolution": (*size, 1 / size[0], 1 / size[1]),
            "src_rect": (x / width, y / height, crop_width / width, crop_height / height),
            "src_layer": layer,
            "resampling_method": 9,
            "game_gamma": gamma,
            "linear_space_output": int(linear),
            "resampling_sharp_weights": tuple(sharp_weights(strength)[3:]),
        }
        table = None
        if operation and operation["type"] == TABLE:
            data = table_data(pass_matrices(operation, None, strength)[operation["axes"]])
            values["resampling_taps"] = data.shape[1] - 1
            table = self.ctx.texture_array(
                (data.shape[1], data.shape[0], 1), 1, data.tobytes(), dtype="f4"
            )
            table.filter = (moderngl.NEAREST, moderngl.NEAREST)
            table.use(2)
        values.update(options or {})
        texture = self.ctx.texture_array(
            (width, height, layers), 4, image.astype("f4").tobytes(), dtype="f4"
        )
        texture.filter = (moderngl.NEAREST, moderngl.NEAREST)
        texture.repeat_x = texture.repeat_y = False
        texture.use(0)
        texture.use(1)
        sampler = self.ctx.sampler(
            texture=texture,
            filter=(moderngl.LINEAR, moderngl.LINEAR),
            repeat_x=False,
            repeat_y=False,
        )
        sampler.use(0)
        dtype = dtype or ("f4" if final else "f2")
        output = self.ctx.texture(size, 4, dtype=dtype)
        framebuffer = self.ctx.framebuffer([output])
        uniforms = self.ctx.buffer(uniform_bytes(values))
        uniforms.bind_to_uniform_block(1)
        framebuffer.use()
        self.ctx.viewport = (0, 0, *size)
        self.vertex_array(operation, decode, final).render(mode=moderngl.TRIANGLES, vertices=3)
        result = np.frombuffer(output.read(), dtype=dtype).reshape(size[1], size[0], 4).copy()
        uniforms.release()
        framebuffer.release()
        output.release()
        sampler.release()
        texture.release()
        if table:
            table.release()
        return result

    def assert_nearest_half_float(self, actual, expected, slack=4e-6):
        """An intermediate pass: `expected` rounded to the nearest half-float."""
        np.testing.assert_array_equal(actual, actual.astype(np.float16).astype(actual.dtype))
        step = np.spacing(np.abs(expected).astype(np.float16)).astype(np.float64)
        error = np.abs(actual.astype(np.float64) - expected)
        self.assertTrue((error <= step / 2 + slack).all(), f"largest error {error.max()}")

    def render(
        self,
        image,
        size,
        crop=None,
        layer=0,
        gamma=2.2,
        linear=False,
        strength=1.0,
        options=None,
    ):
        source = (crop[2], crop[3]) if crop else (image.shape[-2], image.shape[-3])
        operations = plan(source, size)
        if not operations:
            return self.render_pass(
                image,
                size,
                None,
                crop=crop,
                layer=layer,
                gamma=gamma,
                linear=linear,
                options=options,
            )
        for index, operation in enumerate(operations):
            final = index + 1 == len(operations)
            image = self.render_pass(
                image,
                operation["size"],
                operation,
                crop=crop if index == 0 else None,
                layer=layer if index == 0 else 0,
                gamma=gamma,
                linear=linear,
                decode=index == 0,
                final=final,
                strength=strength,
                options=options,
            )
        return image

    def test_plan_matches_the_documented_cases(self):
        def kinds(source, target):
            return [
                (p["type"], p["axes"], p.get("sharpen", False), p["size"])
                for p in plan(source, target)
            ]

        self.assertEqual(
            kinds((2560, 1920), (1920, 1080)),
            [
                (TABLE, VERTICAL, False, (2560, 1080)),
                (TABLE, HORIZONTAL, True, (1920, 1080)),
                (SHARP, VERTICAL, False, (1920, 1080)),
            ],
        )
        self.assertEqual(
            kinds((640, 528), (1920, 1080)),
            [
                (SHARP, HORIZONTAL, False, (640, 528)),
                (SHARP, VERTICAL, False, (640, 528)),
                (ENLARGE, 3, False, (1920, 1080)),
            ],
        )
        self.assertEqual(
            kinds((1920, 1584), (2560, 1440)),
            [
                (SHARP, HORIZONTAL, False, (1920, 1584)),
                (TABLE, VERTICAL, True, (1920, 1440)),
                (ENLARGE, 1, False, (2560, 1440)),
            ],
        )
        self.assertEqual(kinds((23, 19), (23, 19)), [])

    def test_reference_at_integer_fractional_and_extreme_scales(self):
        rng = np.random.default_rng(123)
        for source, target in [
            ((64, 48), (16, 12)),
            ((39, 27), (17, 11)),
            ((513, 257), (7, 3)),
            ((23, 19), (31, 9)),
            ((23, 19), (11, 29)),
            ((1, 19), (7, 5)),
            ((23, 1), (9, 4)),
            ((23, 19), (1, 1)),
            ((19, 23), (57, 46)),
        ]:
            with self.subTest(source=source, target=target):
                image = rng.uniform(0.1, 0.9, (source[1], source[0], 4)).astype("f4")
                # Every tap is a point fetch, so what is left is an intermediate value on the
                # boundary between two half-floats that rounds the other way.
                # Compare in linear light; gamma encoding amplifies errors close to black.
                np.testing.assert_allclose(
                    self.render(image, target, linear=True),
                    reference(image, target, linear=True),
                    atol=1.5e-3,
                    rtol=0,
                )

    def test_reductions_do_not_depend_on_the_sampler(self):
        # Reductions only use point taps. What is left is an intermediate value that sits on
        # the boundary between two half-floats and rounds the other way.
        rng = np.random.default_rng(77)
        for source, target in [((64, 48), (16, 12)), ((39, 27), (17, 11)), ((40, 27), (40, 11))]:
            with self.subTest(source=source, target=target):
                image = rng.uniform(0.1, 0.9, (source[1], source[0], 4)).astype("f4")
                actual = self.render(image, target, linear=True)
                expected = reference(image, target, linear=True)
                np.testing.assert_allclose(actual, expected, atol=1.5e-3, rtol=0)
                self.assertLess(np.mean(np.abs(actual - expected) > 2e-5), 0.02)

    def test_unchanged_axes_and_identity(self):
        image = np.random.default_rng(23).uniform(0.1, 0.9, (19, 23, 4)).astype("f4")
        for target in [(23, 19), (23, 7), (9, 19), (23, 41), (50, 19)]:
            with self.subTest(target=target):
                np.testing.assert_allclose(
                    self.render(image, target, linear=True),
                    reference(image, target, linear=True),
                    atol=1.5e-3,
                    rtol=0,
                )

    def test_constant_color_and_crop_exclude_padding(self):
        image = np.ones((17, 43, 4), dtype="f4")
        image[3:14, 7:26] = [0.2, 0.4, 0.6, 0.8]
        for target in [(7, 5), (31, 23), (7, 23), (31, 5)]:
            with self.subTest(target=target):
                actual = self.render(image, target, crop=(7, 3, 19, 11))
                np.testing.assert_allclose(
                    actual, np.broadcast_to([0.2, 0.4, 0.6, 0.8], actual.shape), atol=1e-3, rtol=0
                )
        image[3:14, 7:26] = np.random.default_rng(51).uniform(0.1, 0.9, (11, 19, 4))
        for target in [(7, 5), (31, 23), (7, 23), (31, 5)]:
            with self.subTest(target=target):
                np.testing.assert_allclose(
                    self.render(image, target, crop=(7, 3, 19, 11), linear=True),
                    reference(image[3:14, 7:26], target, linear=True),
                    atol=1.5e-3,
                    rtol=0,
                )

    def test_gamma_correct_antialiasing(self):
        image = np.ones((128, 128, 4), dtype="f4")
        image[..., :3] = (np.indices((128, 128)).sum(axis=0) % 2)[..., np.newaxis]
        actual = self.render(image, (11, 9))
        self.assertLess(np.max(np.abs(actual[2:-2, 2:-2, :3] - 0.5 ** (1 / 2.2))), 4e-4)
        np.testing.assert_allclose(actual, reference(image, (11, 9)), atol=9e-4, rtol=0)

    def test_no_extra_pass_for_one_to_one_copy_with_padding(self):
        image = np.random.default_rng(37).uniform(0.1, 0.9, (17, 43, 4)).astype("f4")
        np.testing.assert_allclose(
            self.render_pass(image, (19, 11), None, crop=(7, 3, 19, 11)),
            image[3:14, 7:26],
            atol=1e-6,
            rtol=0,
        )

    def test_negative_lobes_survive_first_pass_and_finish_finite(self):
        image = np.ones((24, 32, 4), dtype="f4")
        image[:, :16, :3] = 0
        sharp = self.render_pass(
            image, (32, 24), {"type": SHARP, "axes": HORIZONTAL}, decode=True, final=False
        )
        self.assertLess(sharp[..., :3].min(), -0.01)
        self.assertGreater(sharp[..., :3].max(), 1.01)
        for linear in [False, True]:
            actual = self.render(image, (23, 17), linear=linear)
            self.assertTrue(np.isfinite(actual).all())
            self.assertGreaterEqual(actual[..., :3].min(), 0)
            self.assertGreater(actual[..., :3].max(), 1)
            np.testing.assert_allclose(
                actual, reference(image, (23, 17), linear=linear), atol=1.5e-3, rtol=0
            )

    def test_strength_and_gamma_for_every_scale_direction(self):
        image = np.random.default_rng(17).uniform(0.2, 0.8, (19, 23, 4)).astype("f4")
        for target in [
            (11, 9),
            (23, 9),
            (31, 9),
            (11, 19),
            (23, 19),
            (31, 19),
            (11, 29),
            (23, 29),
            (31, 29),
        ]:
            for strength in [0.0, 0.35, 1.0]:
                with self.subTest(target=target, strength=strength):
                    actual = self.render(image, target, gamma=2.8, strength=strength)
                    actual[..., :3] **= 2.8
                    np.testing.assert_allclose(
                        actual,
                        reference(image, target, gamma=2.8, strength=strength, linear=True),
                        atol=1.5e-3,
                        rtol=0,
                    )

    def test_table_passes_against_exact_reference(self):
        image = np.random.default_rng(28).uniform(0.1, 0.9, (48, 64, 4)).astype("f4")
        light = image.astype(np.float64)
        light[..., :3] **= 2.8
        for axis, target, lengths in [
            (HORIZONTAL, (17, 48), (64, 17)),
            (VERTICAL, (64, 19), (48, 19)),
        ]:
            for sharpen in [False, True]:
                for strength in [0.35, 1.0]:
                    with self.subTest(axis=axis, sharpen=sharpen, strength=strength):
                        operation = {
                            "type": TABLE,
                            "size": target,
                            "axes": axis,
                            "sharpen": sharpen,
                            "lengths": lengths,
                        }
                        actual = self.render_pass(
                            image,
                            target,
                            operation,
                            decode=True,
                            final=False,
                            dtype="f4",
                            gamma=2.8,
                            strength=strength,
                        )
                        expected = apply_matrices(light, pass_matrices(operation, None, strength))
                        self.assert_nearest_half_float(actual, expected)

    def test_sharp_coefficients_and_strength(self):
        image = np.random.default_rng(31).uniform(-0.1, 1.1, (19, 23, 4)).astype("f4")
        for axis in [HORIZONTAL, VERTICAL]:
            for strength in [0.0, 0.35, 1.0]:
                with self.subTest(axis=axis, strength=strength):
                    operation = {"type": SHARP, "axes": axis}
                    actual = self.render_pass(
                        image, (23, 19), operation, final=False, dtype="f4", strength=strength
                    )
                    weights = strength * SHARP_2021
                    weights[3] += 1 - strength
                    length = image.shape[1 - axis]
                    expected = sum(
                        weight
                        * np.take(
                            image.astype(float),
                            np.clip(np.arange(length) + offset, 0, length - 1),
                            axis=1 - axis,
                        )
                        for offset, weight in zip(range(-3, 4), weights)
                    )
                    self.assert_nearest_half_float(actual, expected)

    def test_intermediate_passes_round_to_nearest_half_float(self):
        # Values a hair under a half-float, which a GPU that rounds towards zero would lower.
        image = np.ones((8, 16, 4), dtype="f4")
        image[..., 1] = 0.5
        image[..., 2] = 0.2001953125
        for target in [(7, 5), (40, 31)]:
            with self.subTest(target=target):
                operations = plan((16, 8), target)
                source = image
                for index, operation in enumerate(operations[:-1]):
                    source = self.render_pass(
                        source, operation["size"], operation, decode=False, final=False
                    ).astype("f4")
                    expected = np.broadcast_to([1.0, 0.5, 0.2001953125, 1.0], source.shape)
                    np.testing.assert_array_equal(source, expected)

    def test_enlargement_against_exact_reference(self):
        image = np.random.default_rng(63).uniform(0.1, 0.9, (19, 23, 4)).astype("f4")
        for target, axes in [((61, 47), 3), ((61, 19), 1), ((23, 47), 2)]:
            with self.subTest(target=target):
                operation = {"type": ENLARGE, "size": target, "axes": axes}
                actual = self.render_pass(image, target, operation, final=False, dtype="f4")
                expected = apply_matrices(
                    image.astype(np.float64), pass_matrices(operation, (23, 19))
                )
                self.assert_nearest_half_float(actual, expected, slack=2e-5)
        # A gradient stays one.
        ramp = np.zeros((8, 16, 4), dtype="f4")
        ramp[...] = (np.arange(16) / 15)[np.newaxis, :, np.newaxis]
        operation = {"type": ENLARGE, "size": (64, 8), "axes": 1}
        actual = self.render_pass(ramp, (64, 8), operation, final=False, dtype="f4")
        self.assertTrue((np.diff(actual[..., 0], axis=1) >= -1e-6).all())

    def test_source_layers_stay_separate(self):
        image = np.ones((2, 19, 23, 4), dtype="f4")
        image[0, ..., :3] = [0.8, 0.2, 0.1]
        image[1, ..., :3] = [0.1, 0.3, 0.9]
        for target in [(9, 7), (40, 31)]:
            for layer in [0, 1, 2]:
                with self.subTest(target=target, layer=layer):
                    np.testing.assert_allclose(
                        self.render(image, target, layer=layer),
                        reference(image[min(layer, 1)], target),
                        atol=9e-4,
                        rtol=0,
                    )

    def test_color_correction_and_hdr_apply_only_after_filtering(self):
        image = np.random.default_rng(41).uniform(0.2, 0.8, (25, 31, 4)).astype("f4")
        for target in [(13, 11), (43, 11), (13, 37), (43, 37)]:
            baseline = self.render(image, target, linear=True)
            actual = self.render(
                image,
                target,
                linear=True,
                options={
                    "correct_color_space": 1,
                    "game_color_space": 0,
                    "hdr_output": 1,
                    "hdr_paper_white_nits": 160.0,
                    "hdr_sdr_white_nits": 80.0,
                },
            )
            matrix = np.array(
                [
                    [0.939497225737661, 0.0502268452914346, 0.0102759289709032],
                    [0.0177558637510127, 0.965824605885027, 0.0164195303639603],
                    [-0.00162163209967010, -0.00437400622653655, 1.00599563832621],
                ]
            )
            expected = baseline.copy()
            expected[..., :3] = 2 * (baseline[..., :3] @ matrix)
            np.testing.assert_allclose(actual, expected, atol=2e-6, rtol=0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
