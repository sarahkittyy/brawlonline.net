#!/usr/bin/env python3
# Copyright 2026 Dolphin Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

"""Measure resident output-resampling pipelines with OpenGL GPU elapsed-time queries.

Requires numpy, moderngl, Pillow and EGL. Example:
    python dolphin/Tools/benchmark-output-resampling.py \
        --source-dir <captures> --output <file>.json

The captures are raw XFB screenshots (Frame Dumps Resolution Type: raw) named
*-source-4x.png; --pattern and --outputs select other names and output sizes.

Sources and targets are allocated once. Timed batches include filter draws and the
transitions between their targets, excluding compilation, uploads, readback and PNG I/O.
These are isolated stage timings, not total game frame times or an FPS prediction.
"""

import argparse
import hashlib
import json
import platform
import random
import runpy
import statistics
import time
from datetime import UTC, datetime
from pathlib import Path

import moderngl
from PIL import Image


class Pipeline:
    def __init__(self, ctx, helpers, vertex_array, method, source, target, gamma):
        """vertex_array(operation, decode, final) gives the program for one pass (None: no pass)."""
        self.ctx = ctx
        self.method = method
        self.target = target
        self.linear = ctx.sampler(
            filter=(moderngl.LINEAR, moderngl.LINEAR), repeat_x=False, repeat_y=False
        )
        self.point = ctx.sampler(
            filter=(moderngl.NEAREST, moderngl.NEAREST), repeat_x=False, repeat_y=False
        )
        self.table = ctx.sampler(
            filter=(moderngl.NEAREST, moderngl.NEAREST), repeat_x=False, repeat_y=False
        )
        self.stages = []
        self.names = []
        if method == "mks2021":
            operations = helpers["plan"](source.size[:2], target)
        else:
            operations = [None]
        for index, operation in enumerate(operations):
            final = index + 1 == len(operations)
            width, height, _ = source.size
            size = operation["size"] if operation else target
            table = None
            taps = 0
            if operation and operation["type"] == helpers["TABLE"]:
                data = helpers["table_data"](
                    helpers["pass_matrices"](operation, None)[operation["axes"]]
                )
                taps = data.shape[1] - 1
                table = ctx.texture_array(
                    (data.shape[1], data.shape[0], 1), 1, data.tobytes(), dtype="f4"
                )
            if operation:
                kind = {helpers["TABLE"]: "table", helpers["SHARP"]: "sharp"}.get(
                    operation["type"], "enlarge"
                )
                if operation.get("sharpen"):
                    kind += "+sharp"
                axes = operation["axes"]
                if kind == "enlarge":
                    axis = "xy" if axes == 3 else "x" if axes == 1 else "y"
                else:
                    axis = "xy"[axes]
                self.names.append(f"{index + 1}_{kind}_{axis}" + (f"_{taps}_taps" if taps else ""))
            values = {
                "resolution": (width, height, 1 / width, 1 / height),
                "target_resolution": (*size, 1 / size[0], 1 / size[1]),
                "window_resolution": (*size, 1 / size[0], 1 / size[1]),
                "src_rect": (0, 0, 1, 1),
                "stereo_views": (1, 1),
                "src_layer": 0,
                "resampling_method": {"default": 0, "bilinear": 1, "area": 6, "mks2021": 9}[method],
                "game_gamma": gamma,
                "resampling_taps": taps,
                "resampling_sharp_weights": tuple(helpers["sharp_weights"](1.0)[3:]),
            }
            output = ctx.texture_array((*size, 1), 4, dtype="f1" if final else "f2")
            framebuffer = ctx.framebuffer([output])
            uniforms = ctx.buffer(helpers["uniform_bytes"](values))
            vao = vertex_array(operation, index == 0, final)
            self.stages.append((source, output, framebuffer, uniforms, size, table, vao))
            source = output

    def bind(self, stage):
        source, _, framebuffer, uniforms, size, table, _ = stage
        framebuffer.use()
        self.ctx.viewport = (0, 0, *size)
        uniforms.bind_to_uniform_block(1)
        self.linear.texture = source
        self.point.texture = source
        self.linear.use(0)
        self.point.use(1)
        if table:
            self.table.texture = table
            self.table.use(2)

    def draw(self):
        for stage in self.stages:
            self.bind(stage)
            stage[6].render(mode=moderngl.TRIANGLES, vertices=3)

    def batch(self, frames):
        # Single-pass modes need no intervening target or texture changes within a batch.
        if len(self.stages) == 1:
            self.bind(self.stages[0])
            query = self.ctx.query(time=True)
            with query:
                for _ in range(frames):
                    self.stages[0][6].render(mode=moderngl.TRIANGLES, vertices=3)
        else:
            query = self.ctx.query(time=True)
            with query:
                for _ in range(frames):
                    self.draw()
        self.ctx.finish()
        return query.elapsed / frames / 1_000_000

    def profile_stages(self, frames):
        samples = [[] for _ in self.stages]
        queries = []
        for _ in range(frames):
            frame = []
            for stage in self.stages:
                self.bind(stage)
                query = self.ctx.query(time=True)
                with query:
                    stage[6].render(mode=moderngl.TRIANGLES, vertices=3)
                frame.append(query)
            queries.append(frame)
        self.ctx.finish()
        for frame in queries:
            for index, query in enumerate(frame):
                samples[index].append(query.elapsed / 1_000_000)
        return [statistics.median(stage) for stage in samples]

    def release(self):
        for _, output, framebuffer, uniforms, _, table, _ in reversed(self.stages):
            framebuffer.release()
            uniforms.release()
            output.release()
            if table:
                table.release()
        self.linear.release()
        self.point.release()
        self.table.release()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--source-dir", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument("--rounds", type=int, default=7)
    parser.add_argument("--frames", type=int, default=48)
    parser.add_argument("--warmup", type=int, default=24)
    parser.add_argument("--gamma", type=float, default=2.35)
    parser.add_argument("--seed", type=int, default=20261118)
    parser.add_argument("--pattern", default="*-source-4x.png", help="source captures to load")
    parser.add_argument(
        "--outputs", default="1280x720,1920x1080", help="output sizes, as WIDTHxHEIGHT,..."
    )
    args = parser.parse_args()
    if args.rounds < 1 or args.frames < 1 or args.warmup < 0:
        parser.error("rounds and frames must be positive; warmup must be nonnegative")
    outputs = [tuple(int(n) for n in size.split("x")) for size in args.outputs.split(",")]
    helpers = runpy.run_path(str(Path(__file__).with_name("test-magic-kernel-sharp.py")))
    ctx = moderngl.create_standalone_context(backend="egl", require=430)
    fragment = helpers["FRAGMENT"]
    # One program per kind of Magic Kernel Sharp pass, as in PostProcessing.cpp, next to
    # the default shader for the other filters and the empty shader for Default.
    programs = {}

    def vertex_array(source):
        if source not in programs:
            program = helpers["make_program"](ctx, source)
            programs[source] = (program, ctx.vertex_array(program, []))
        return programs[source][1]

    def filter_vertex_array(operation, decode, final):
        return vertex_array(helpers["fragment"](operation, decode, final))

    def default_vertex_array(operation, decode, final):
        return vertex_array(
            fragment.rsplit("\nvoid main()", 1)[0] + "\nvoid main() { SetOutput(Sample()); }\n"
        )

    result = {
        "utc_time": datetime.now(UTC).isoformat(),
        "platform": platform.platform(),
        "renderer": ctx.info["GL_RENDERER"],
        "driver": ctx.info["GL_VERSION"],
        "timer": "GL_TIME_ELAPSED (GPU), milliseconds per frame",
        "scope": "Resident textures/targets/UBOs; filter draws and target transitions only; no compilation, uploads, readback or PNG I/O; no game rendering",
        "shader_sha256": hashlib.sha256(fragment.encode()).hexdigest(),
        "rounds": args.rounds,
        "frames_per_batch": args.frames,
        "warmup_frames_per_condition": args.warmup,
        "gamma": args.gamma,
        "strength": 1.0,
        "seed": args.seed,
        "sources": [],
        "results": [],
    }
    sources = {}
    for path in sorted(args.source_dir.glob(args.pattern)):
        image = Image.open(path).convert("RGBA")
        sources[path.stem.removesuffix("-source-4x")] = ctx.texture_array(
            (*image.size, 1), 4, image.tobytes(), dtype="f1"
        )
        result["sources"].append(
            {
                "path": str(path),
                "size": image.size,
                "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
            }
        )
    if not sources:
        raise SystemExit(f"No {args.pattern} captures found")
    methods = ["default", "bilinear", "area", "mks2021"]
    pipelines = {}
    for scene, source in sources.items():
        for target in outputs:
            for method in methods:
                pipelines[(scene, target, method)] = Pipeline(
                    ctx,
                    helpers,
                    default_vertex_array if method == "default" else filter_vertex_array,
                    method,
                    source,
                    target,
                    args.gamma,
                )
    started = time.perf_counter()
    print("Renderer:", result["renderer"], flush=True)
    print("Warming", len(pipelines), "conditions", flush=True)
    for pipeline in pipelines.values():
        for _ in range(args.warmup):
            pipeline.draw()
    ctx.finish()
    rng = random.Random(args.seed)
    keys = list(pipelines)
    samples = {key: [] for key in keys}
    for round_number in range(args.rounds):
        rng.shuffle(keys)
        for key in keys:
            samples[key].append(pipelines[key].batch(args.frames))
        print(f"Round {round_number + 1}/{args.rounds} complete", flush=True)
    for key, pipeline in pipelines.items():
        scene, target, method = key
        batches = samples[key]
        record = {
            "scene": scene,
            "output": target,
            "method": method,
            "batch_ms_per_frame": batches,
            "median_ms": statistics.median(batches),
            "min_ms": min(batches),
            "max_ms": max(batches),
        }
        if method == "mks2021":
            record["stage_median_ms"] = dict(zip(pipeline.names, pipeline.profile_stages(64)))
            record["intermediate_bytes"] = sum(
                stage[4][0] * stage[4][1] * 8 for stage in pipeline.stages[:-1]
            )
        result["results"].append(record)
    result["wall_seconds"] = time.perf_counter() - started
    for output in outputs:
        values = {}
        for method in methods:
            values[method] = statistics.median(
                record["median_ms"]
                for record in result["results"]
                if tuple(record["output"]) == output and record["method"] == method
            )
        print("x".join(str(n) for n in output), json.dumps(values), flush=True)
        print("MKS/default ratio:", values["mks2021"] / values["default"], flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2))
    print("Saved", args.output, flush=True)
    for pipeline in pipelines.values():
        pipeline.release()
    for source in sources.values():
        source.release()
    for program, array in programs.values():
        array.release()
        program.release()
    ctx.release()


if __name__ == "__main__":
    main()
