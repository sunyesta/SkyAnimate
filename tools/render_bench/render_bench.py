"""Renderer benchmarks: what renders cost, from the render statistics (oanim.wm.set_render_stats) of private copies.

A configuration is a build and, optionally, Viewport Shading settings, as with render_check:
    python tools/render_bench/render_bench.py --config EXE:gpu_geometry=0 --config EXE:gpu_geometry=1 \\
        --redraw "FILE.oanim:1" --bend FILE.oanim --zoom FILE.oanim
Each runs in a private copy (its own LOCALAPPDATA, never the user's running app), stopped by its own PID.

Scenarios:
  redraw FILE[:FRAME]  the frame rendered to a file at one size again and again (everything made once and then kept):
                       what drawing alone costs, the GPU's time by kind of draw.
  bend FILE            every bone of every bone layer swung by a driver (its angle, from the setup pose, a sine of the
                       frame, each bone out of step), then played for --seconds in the viewport: every weighted point
                       moves every frame, so drawings are made again on every one.
  zoom FILE            the top layers' scale swung by drivers between about 0.9 and 2.1, played: each frame's drawings
                       are at a new size (the zoom steps are 2^(1/8) apart), so they are made again.

Renders are timed by the build itself: its CPU time (and of it preparing, bones and modifiers, and making drawings,
summed over the threads: flattening outlines, cutting fills, fans, strokes) and the GPU's, by kind of draw (each render
waits for the GPU while statistics are on). Printed as medians per scenario and configuration, the playing ones with how
many frames rendered; all renders to --out/bench.json.
"""
import argparse
import json
import os
import statistics as st
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "render_check"))
import render_check as rc  # noqa: E402

FIELDS = ["cpu_ms", "prepare_ms", "evaluate_ms", "make_ms", "mask_make_ms", "outline_ms", "earcut_ms", "fan_ms", "stroke_ms",
          "drawings_made", "gpu_ms"]

BEND = r'''
import oanim
n = 0
for L in [l for l in oanim.data.layers.all() if l.type == "BONE"]:
    for k, b in enumerate(L.bones):
        oanim.ops.anim.driver_add(layer=str(L.id), path='bones["%s"].angle' % b["name"],
                                  expression="%r * sin(frame * 0.25 + %d)" % (AMPLITUDE, k))
        n += 1
print(n)
'''

ZOOM = r'''
import oanim
n = 0
for L in oanim.data.layers:
    for c in ("0", "1"):
        oanim.ops.anim.driver_add(layer=str(L.id), path="scale", component=c,
                                  expression="1.5 + 0.6 * sin(frame * 0.2)")
    n += 1
print(n)
'''


def summarize(renders):
    out = {"renders": len(renders)}
    for f in FIELDS:
        vals = [r[f] for r in renders]
        out[f] = round(st.median(vals), 2) if vals else 0
    for field in ("gpu", "cpu", "calls"):
        kinds = {}
        for r in renders:
            for k, v in r.get(field, {}).items():
                kinds.setdefault(k, []).append(v)
        out[field] = {k: round(st.median(v + [0] * (len(renders) - len(v))), 2) for k, v in kinds.items()}
    # The render's CPU time outside preparing and the device's draw calls: the renderer's own drawing logic.
    other = [r["cpu_ms"] - r["prepare_ms"] - sum(r.get("cpu", {}).values()) for r in renders]
    out["renderer_ms"] = round(st.median(other), 2) if other else 0
    return out


def show(name, s):
    parts = " ".join("%s %s" % (f.replace("_ms", ""), s[f]) for f in FIELDS)
    print("%-34s renders %4d  %s  renderer %s" % (name, s["renders"], parts, s["renderer_ms"]))
    for field, unit in (("gpu", "gpu ms"), ("cpu", "cpu ms in calls"), ("calls", "calls")):
        kinds = sorted(s[field].items(), key=lambda kv: -kv[1])
        print("%-34s %s by kind: %s" % ("", unit, ", ".join("%s %g" % kv for kv in kinds if kv[1] >= 0.01)))
    sys.stdout.flush()


def viewport_renders(stats):
    """The viewport's renders: not to a file, and the main view's size (the largest; backdrops and previews are smaller)."""
    views = [r for r in stats if not r["final"]]
    if not views:
        return []
    size = max((r["width"] * r["height"]) for r in views)
    return [r for r in views if r["width"] * r["height"] == size]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--config", action="append", required=True, help="EXE[:setting=0/1,...]")
    ap.add_argument("--redraw", action="append", default=[], help="FILE.oanim[:FRAME]")
    ap.add_argument("--bend", action="append", default=[], help="FILE.oanim")
    ap.add_argument("--zoom", action="append", default=[], help="FILE.oanim")
    ap.add_argument("--seconds", type=float, default=6.0)
    ap.add_argument("--amplitude", type=float, default=0.35, help="bend: the bones' swing, radians")
    ap.add_argument("--size", default="1920x1080", help="redraw: the render's size")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "render_bench"))
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    width, height = (int(v) for v in args.size.split("x"))
    results = []
    for ci, spec in enumerate(args.config):
        exe, settings = rc.parse_config(spec)
        label = os.path.basename(os.path.dirname(exe)) + (":" + ",".join("%s=%d" % kv for kv in settings.items())
                                                          if settings else "")
        copy = rc.Copy(exe, os.path.join(args.out, "work_%d" % ci), settings)
        try:
            copy.run("import oanim\noanim.wm.set_render_stats(True)")
            for item in args.redraw:
                path, _, frame = item.rpartition(":") if item.rpartition(":")[2].isdigit() else (item, "", "1")
                copy.open(path, int(frame))
                png = os.path.join(args.out, "redraw.png")
                for _ in range(2):
                    copy.render(png, width, height)
                copy.run("import oanim\noanim.wm.render_stats()")
                for _ in range(8):
                    copy.render(png, width, height)
                stats = json.loads(copy.run("import oanim, json\nprint(json.dumps(oanim.wm.render_stats()))").strip())
                renders = [r for r in stats if r["final"] and r["width"] == width and r["height"] == height]
                s = summarize(renders)
                show("redraw %s (%s)" % (os.path.basename(path), label), s)
                results.append({"scenario": "redraw", "file": path, "config": spec, "summary": s, "renders": renders})
            for kind, items, code in (("bend", args.bend, BEND), ("zoom", args.zoom, ZOOM)):
                for path in items:
                    copy.open(path, 1)
                    made = copy.run(code.replace("AMPLITUDE", repr(args.amplitude))).strip()
                    copy.pump(10)
                    time.sleep(0.5)
                    copy.run("import oanim\noanim.wm.render_stats()")
                    copy.run("import oanim\noanim.ops.screen.animation_play()")
                    time.sleep(args.seconds)
                    copy.run("import oanim\noanim.ops.screen.animation_play()")
                    stats = json.loads(copy.run("import oanim, json\nprint(json.dumps(oanim.wm.render_stats()))").strip())
                    renders = viewport_renders(stats)
                    s = summarize(renders)
                    s["frames_per_second"] = round(len(renders) / args.seconds, 1)
                    s["driven"] = made
                    show("%s %s (%s), %s driven" % (kind, os.path.basename(path), label, made), s)
                    results.append({"scenario": kind, "file": path, "config": spec, "summary": s, "renders": renders})
        finally:
            copy.stop()
    json.dump(results, open(os.path.join(args.out, "bench.json"), "w"), indent=1)


if __name__ == "__main__":
    main()
