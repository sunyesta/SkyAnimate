"""The Draw tool's pens: the shelf's presets, saving and putting them back, scripted strokes, welding, F and Shift F.

The shelf, the header's dropdowns and the popup need the window: checked live on a private copy.

Run with:  OpenAnimate --background --python tests/test_draw_presets.py
"""

import math
import os
import tempfile

import oanim

wm = oanim.wm
ops = oanim.ops


def close(a, b, tolerance=1e-4):
    return abs(a - b) < tolerance


def stroke_text(path, count=80, seconds=0.5, pressure=0.8):
    """draw.stroke's samples along path(u)."""
    rows = []
    for i in range(count):
        u = i / (count - 1)
        x, y = path(u)
        rows.append("%f,%f,%f,%f" % (x, y, pressure, u * seconds))
    return ";".join(rows)


# --- The built-in pens -----------------------------------------------------------------------------------------------
presets = {p["name"]: p for p in wm.draw_presets()}
for name in ("Ink", "Pencil", "Marker", "Tapered", "Mouse", "Bezier Pen"):
    assert name in presets and presets[name]["builtin"] and presets[name]["category"] == "Pens", name
assert presets["Bezier Pen"]["mode"] == "PEN" and presets["Ink"]["mode"] == "FREEHAND"
assert not presets["Mouse"]["use_pressure"] and presets["Mouse"]["speed_width"] > 0.0
assert presets["Tapered"]["taper_end"] > 0.0
assert not presets["Marker"]["use_pressure"]

# Ink is what the tool starts with.
settings = wm.draw_settings()
assert settings["preset"] == "Ink" and close(settings["line_width"], presets["Ink"]["line_width"])
assert settings["stabilizer"] == presets["Ink"]["stabilizer"] and settings["min_size"] == presets["Ink"]["min_size"]

# Picking one makes Draw the tool, and takes its settings.
ops.wm.tool_select()
ops.draw.preset_load(name="Tapered")
assert wm.tool() == "DRAW", wm.tool()
settings = wm.draw_settings()
assert settings["preset"] == "Tapered" and settings["mode"] == "FREEHAND"
assert settings["taper_end"] == presets["Tapered"]["taper_end"] and close(settings["line_width"], presets["Tapered"]["line_width"])
assert ops.draw.preset_load(name="No Such Pen") == {"CANCELLED"}

# The Bezier Pen is the tool's other kind: coming back, the stroke pen is as it was.
ops.draw.preset_load(name="Bezier Pen")
assert wm.draw_settings()["mode"] == "PEN" and wm.draw_settings()["preset"] == "Tapered"
ops.wm.tool_draw(mode="FREEHAND")
assert wm.draw_settings()["preset"] == "Tapered"

# A pen keeps what it was changed to, and its saved settings are there to go back to.
wm.set_draw(taper_end=33)
ops.draw.preset_load(name="Ink")
assert wm.draw_settings()["taper_end"] == 0.0, "Ink has no taper"
ops.draw.preset_load(name="Tapered")
assert wm.draw_settings()["taper_end"] == 33.0, "brought back"
ops.draw.preset_reset(name="Tapered")
assert wm.draw_settings()["taper_end"] == presets["Tapered"]["taper_end"]
ops.draw.preset_load(name="Ink")
ops.draw.preset_load(name="Tapered")
assert wm.draw_settings()["taper_end"] == presets["Tapered"]["taper_end"], "put back for good"
# A pen not being used can be put back too.
wm.set_draw(taper_end=33)
ops.draw.preset_load(name="Ink")
ops.draw.preset_reset(name="Tapered")
ops.draw.preset_load(name="Tapered")
assert wm.draw_settings()["taper_end"] == presets["Tapered"]["taper_end"]

# --- Saved pens ------------------------------------------------------------------------------------------------------
library = tempfile.mkdtemp(prefix="oanim_draw_")
ops.preferences.asset_library_set(directory=library)
ops.draw.preset_load(name="Ink")
wm.set_draw(stabilizer="PULLED_STRING", string_length=31, min_size=0.3, taper_start=12, pressure_curve=(0.1, 0.5, 0.4, 0.9),
            use_pressure=False, line_width=0.03)
ops.draw.preset_save(name="My Pen")
assert wm.draw_settings()["preset"] == "My Pen"
assert os.path.exists(os.path.join(library, "Draw Presets", "My Pen.oadraw"))
saved = {p["name"]: p for p in wm.draw_presets()}["My Pen"]
assert saved["category"] == "Mine" and not saved["builtin"] and saved["stabilizer"] == "PULLED_STRING"
assert saved["string_length"] == 31.0 and close(saved["line_width"], 0.03) and not saved["use_pressure"]
assert close(saved["pressure_curve"][1], 0.5) and close(saved["min_size"], 0.3)

# Read back from the file, as after opening the app again.
ops.preferences.asset_library_set(directory=library)
again = {p["name"]: p for p in wm.draw_presets()}["My Pen"]
assert again == saved, (again, saved)

ops.draw.preset_save(name="My Pen")
assert "My Pen.001" in {p["name"] for p in wm.draw_presets()}, "a taken name gets a number"
ops.draw.preset_remove(name="My Pen.001")
assert "My Pen.001" not in {p["name"] for p in wm.draw_presets()}
assert ops.draw.preset_remove(name="Ink") == {"CANCELLED"}, "built-in"
ops.draw.preset_load(name="Ink")

# --- A stroke through the same code as the tool ------------------------------------------------------------------------
ops.wm.read_homefile() if hasattr(ops.wm, "read_homefile") else None
ops.layer.new()
layer = oanim.data.layers.active
doc = oanim.data.document
doc.frame_current = 0
ops.draw.preset_load(name="Ink")
pixel = 2.0 / doc.height

straight = stroke_text(lambda u: (-0.5 + u, 0.2))
assert ops.draw.stroke(samples=straight, pixel=pixel) == {"FINISHED"}
assert layer.curve_count == 1 and layer.point_count == 2 and layer.shape_count == 1, (layer.curve_count, layer.point_count)
ops.ed.undo()
assert layer.curve_count == 0, "one undo step"
ops.ed.redo()
assert layer.curve_count == 1

# The fit's handles are stored: not worked out afterward from the neighbours.
curve_text = stroke_text(lambda u: (-0.5 + u, 0.5 * math.sin(u * 3.0)), count=160, seconds=1.0)
ops.draw.stroke(samples=curve_text, pixel=pixel)
assert layer.curve_count == 2
handles = layer.curve_handles(1)
assert 3 <= len(handles) <= 16, len(handles)
assert all(t == "ALIGNED" for t, _, _ in handles), handles

# A pen's width comes through: Marker has no pressure, Ink does.
ops.draw.preset_load(name="Marker")
ops.draw.stroke(samples=stroke_text(lambda u: (-0.5 + u, -0.2), pressure=0.2), pixel=pixel)
assert layer.shape_style(layer.shape_count - 1)["line_width"] == presets["Marker"]["line_width"] or close(
    layer.shape_style(layer.shape_count - 1)["line_width"], presets["Marker"]["line_width"])
ops.draw.preset_load(name="Ink")

# Close and Fill: a stroke ending where it started closes, and fills.
wm.set_draw(fill=True)
circle = lambda u: (0.3 * math.cos(u * 2.0 * math.pi), 0.3 * math.sin(u * 2.0 * math.pi))
ops.draw.stroke(samples=stroke_text(circle, count=120, seconds=0.8), pixel=pixel)
wm.set_draw(fill=False)
assert layer.shape_style(layer.shape_count - 1)["has_fill"], layer.shape_style(layer.shape_count - 1)

# Weld Ends: a stroke finishing on a point joins it, and off it, doesn't.
ops.layer.new()
layer = oanim.data.layers.active
ops.draw.stroke(samples=stroke_text(lambda u: (-0.5 + 0.5 * u, 0.0)), pixel=pixel)
assert layer.point_count == 2
ops.draw.stroke(samples=stroke_text(lambda u: (0.002, 0.5 * u)), pixel=pixel)
assert layer.point_count == 4, "off, it doesn't"
wm.set_draw(weld=True)
ops.draw.stroke(samples=stroke_text(lambda u: (0.3 * (1.0 - u) + 0.002 * u, 0.3 * (1.0 - u))), pixel=pixel)
assert layer.point_count == 5, ("the last end joined the first stroke's end", layer.point_count)
ops.draw.stroke(samples=stroke_text(lambda u: (0.8, 0.8 - u * 0.1)), pixel=pixel)
assert layer.point_count == 7, "nothing near: new points"
wm.set_draw(weld=False)

# On the setup frame only.
doc.frame_current = 5
assert ops.draw.stroke(samples=straight, pixel=pixel) == {"CANCELLED"}
doc.frame_current = 0

# --- Keys ------------------------------------------------------------------------------------------------------------
ops.wm.tool_draw(mode="FREEHAND")
assert wm.event_simulate("F", area="Viewport"), "F sizes the line"
assert wm.event_simulate("F", shift=True, area="Viewport"), "Shift F its opacity"
assert wm.event_simulate("SPACE", shift=True, area="Viewport"), "Shift Space shows the pens"
ops.wm.tool_draw(mode="PEN")
ops.wm.tool_select()
assert not wm.event_simulate("SPACE", shift=True, area="Viewport") or not oanim.context.is_playing

print("draw presets: ok")
