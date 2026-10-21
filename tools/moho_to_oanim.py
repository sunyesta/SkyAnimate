"""Converts a Moho project (.moho, Moho 13.5 and later: a zip holding Project.mohoproj) into a
Open Animate file (.oanim).

    python tools/moho_to_oanim.py "mouth rig.moho" [out.oanim]

What comes across: the document (size, frame rate, frame range, background), the layer tree (bone,
group and vector layers, bottom-to-top order turned into Open Animate's top-first lists), each layer's
transform, opacity and visibility (not masking: set masks up after converting), vector drawings (points with widths, curves with Moho's
curvature, weights and offsets, hidden edges, shapes with their fill, line colour, line width and
brush), bones (place, angle, length, scale, strength, parents; pin bones get the Pin shape; bone tags become bone collections), point binding (a point bound to one
bone, not bound, or flexi-bound), layers bound to a bone, reference layers (linked to their
originals, with their differences kept as the reference's own changes), and every channel's keys
in the mainline and in each action (a smart bone's action plays from its bone's angle in
Open Animate as in Moho). Moho features Open Animate doesn't have (3D, physics, layer effects,
switch layers' extras...) are left out and listed at the end.

Only Python's standard library is needed.
"""

import json
import math
import pathlib
import sys
import zipfile

# Moho's interpolation numbers (MOHO.INTERP_*) to Open Animate's.
INTERP = {0: "LINEAR", 1: "SMOOTH", 2: "EASE_IN_OUT", 3: "STEP", 4: "NOISY", 5: "CYCLE", 6: "POSE",
          7: "EASE_IN", 8: "EASE_OUT", 9: "BEZIER", 10: "BOUNCE", 11: "ELASTIC"}

skipped = set()


def value_of(v, kind):
    """A Moho channel value as Open Animate saves it."""
    if kind == "Vec2":
        return [v["x"], v["y"]]
    if kind == "Vec3":
        return [v["x"], v["y"]]
    if kind == "Color":
        return [v["r"], v["g"], v["b"], v["a"]]
    if kind == "Bool":
        return 1.0 if v else 0.0
    return v


def keys_of(ch, convert=lambda v: v):
    """A Moho channel's keys (when, val, interp) as Open Animate's key list."""
    kind = ch.get("type")
    keys = []
    for when, val, interp in zip(ch.get("when", []), ch.get("val", []), ch.get("interp", [])):
        key = {"frame": int(when), "value": convert(value_of(val, kind)),
               "interp": INTERP.get(interp.get("im", 1), "SMOOTH")}
        if interp.get("h", 0):
            key["hold"] = int(interp["h"])
        if interp.get("in", 1) > 1:
            key["interval"] = int(interp["in"])
        keys.append(key)
    return keys


def channel(ch, convert=lambda v: v):
    """(the value on frame 0, its mainline keys or None, its actions' keys by name)."""
    kind = ch.get("type")
    vals = ch.get("val", [])
    plain = convert(value_of(vals[0], kind)) if vals else None
    keys = keys_of(ch, convert)
    mainline = keys if len(keys) > 1 or (keys and keys[0]["frame"] != 0) else None
    actions = {}
    for a in ch.get("actions", []) or []:
        if a.get("name") and a.get("pose"):
            actions[a["name"]] = keys_of(a["pose"], convert)
    return plain, mainline, actions


def put_channel(j, name, ch, convert=lambda v: v):
    """Sets j[name] (the plain value) and its keys and action keys; returns the plain value."""
    plain, mainline, actions = channel(ch, convert)
    if mainline:
        j[name + "_keys"] = mainline
    if actions:
        j[name + "_keys_actions"] = actions
    return plain


def action_names(o, found):
    """Every action name used in a channel anywhere below `o`."""
    if isinstance(o, dict):
        for a in o.get("actions", []) if isinstance(o.get("actions"), list) else []:
            if isinstance(a, dict) and a.get("name") and "pose" in a:
                found.add(a["name"])
        for v in o.values():
            action_names(v, found)
    elif isinstance(o, list):
        for v in o:
            action_names(v, found)


def style_of(shape, doc_styles):
    """A shape's look: its own style, with what a named library style defines laid over it."""
    own = shape.get("style", {})
    style = {"fill": value_of(own["fill_color"]["val"][0], "Color") if "fill_color" in own else [1, 1, 1, 1],
             "line": value_of(own["line_color"]["val"][0], "Color") if "line_color" in own else [0, 0, 0, 1],
             "line_width": own.get("line_width", 0.008)}
    brush = own.get("brush_name", "")
    for name in (shape.get("inherited_style_name", ""), shape.get("inherited_style2_name", "")):
        lib = doc_styles.get(name)
        if not lib:
            continue
        if lib.get("define_fill_color"):
            style["fill"] = value_of(lib["fill_color"]["val"][0], "Color")
        if lib.get("define_line_col"):
            style["line"] = value_of(lib["line_color"]["val"][0], "Color")
        if lib.get("define_line_width"):
            style["line_width"] = lib["line_width"]
        if lib.get("brush_name"):
            brush = lib["brush_name"]
            own = lib
    if brush:
        style["brush"] = {"name": pathlib.Path(brush).stem, "align": own.get("brush_align", False),
                          "jitter": own.get("brush_jitter", 0.0), "spacing": own.get("brush_spacing", 0.25),
                          "angleDrift": own.get("brush_angle_drift", 0.0), "randomize": own.get("brush_randomize", False),
                          "randomOrder": own.get("brush_rand_order", True), "mergedAlpha": own.get("brush_merged_alpha", False),
                          "sizeVariationAmp": own.get("brush_size_amp", 0.0),
                          "sizeVariationScale": own.get("brush_size_scale", 0.3),
                          "randomInterval": own.get("brush_random_interval", 1), "tint": own.get("brush_tint", False),
                          "hueDrift": own.get("brush_hue_drift", 0.0), "satDrift": own.get("brush_sat_drift", 0.0),
                          "valDrift": own.get("brush_val_drift", 0.0)}
    return style


def drawing_of(mesh, bone_ids, flexi_moves, doc_styles):
    """A Moho mesh as a Open Animate drawing. Point ids are their index + 1, shape ids Moho's."""
    first_bone = next(iter(bone_ids.values()), 0)
    points = []
    for i, p in enumerate(mesh.get("points", [])):
        point = {"id": i + 1}
        point["pos"] = put_channel(point, "pos", p["position"])
        width = put_channel(point, "radius", p["width"], lambda v: max(0.0, min(1.0, v)))
        if width != 1.0:
            point["radius"] = width
        parent = p.get("parent", -1)
        if parent >= 0 and parent in bone_ids:
            point["weights"] = [[bone_ids[parent], 1.0]]
        elif parent == -1 or (parent == -2 and not flexi_moves):
            # Not bound (or flexi-bound to bones with no strength): it stays put.
            if first_bone:
                point["weights"] = [[first_bone, 0.0]]
        # Flexi-bound with bones that have strength: Open Animate's automatic weights.
        points.append(point)
    curves = []
    for c in mesh.get("curves", []):
        cps = []
        for cp in c.get("points", []):
            point = {"point": cp["point"]}
            point["curvature"] = put_channel(point, "curvature", cp["smoothness"])
            for name in ("weight_in", "weight_out", "offset_in", "offset_out"):
                if name in cp:
                    point[name] = channel(cp[name])[0]
            if not cp.get("segments_on", True):
                point["edge_hidden"] = True
            cps.append(point)
        curves.append({"closed": bool(c.get("closed")), "points": cps})
    shapes = []
    next_id = len(points) + 1
    for s in mesh.get("shapes", []):
        e = s.get("edges", {})
        edges = [[c, seg] for c, seg in zip(e.get("curve", []), e.get("segment", []))]
        sid = int(s.get("id", 0)) + len(points) + 1
        next_id = max(next_id, sid + 1)
        shapes.append({"id": sid, "name": s.get("name", ""), "fill": bool(s.get("has_fill", True)),
                       "outline": bool(s.get("has_outline", True)), "style": style_of(s, doc_styles),
                       "edges": edges})
    if mesh.get("groups"):
        skipped.add("point groups")
    return {"points": points, "curves": curves, "shapes": shapes, "next_id": next_id}


def looks_differ(ref_shape, src_shape):
    """A reference's shape against its original's: its own look, per setting."""
    look = {}
    rs, ss = ref_shape["style"], src_shape["style"]
    if rs["fill"] != ss["fill"]:
        look["fill"] = rs["fill"]
    if rs["line"] != ss["line"]:
        look["line"] = rs["line"]
    if abs(rs["line_width"] - ss["line_width"]) > 1e-9:
        look["line_width"] = rs["line_width"]
    if rs.get("brush") != ss.get("brush") and rs.get("brush"):
        look["brush"] = rs["brush"]
    if ref_shape["fill"] != src_shape["fill"]:
        look["has_fill"] = ref_shape["fill"]
    if ref_shape["outline"] != src_shape["outline"]:
        look["has_outline"] = ref_shape["outline"]
    return look


class Converter:
    def __init__(self, project):
        self.project = project
        self.next_id = 1
        self.by_uuid = {}       # Moho layer uuid -> Open Animate layer id
        self.drawings = {}      # Open Animate layer id -> its drawing (for references)
        self.references = []    # (layer json, Moho layer, source uuid)
        self.doc_styles = {s.get("name"): s for s in project.get("styles", [])}

    def layer(self, l, bone_ids, flexi_moves):
        lid = self.next_id
        self.next_id += 1
        self.by_uuid[l.get("uuid")] = lid
        kind = l.get("type")
        j = {"id": lid, "name": l.get("name", "Layer"), "visible": bool(l.get("visible", True)),
             "expanded": bool(l.get("expanded", True))}
        t = l.get("transforms", {})
        if "translation" in t:
            j["position"] = put_channel(j, "position", t["translation"])
        if "rotation_z" in t:
            j["rotation"] = put_channel(j, "rotation", t["rotation_z"], math.degrees)
        if "scale" in t:
            j["scale"] = put_channel(j, "scale", t["scale"])
        for unsupported in ("rotation_x", "rotation_y", "shear"):
            ch = t.get(unsupported)
            if ch and any(abs(v) > 1e-9 for v in ch.get("val", []) if isinstance(v, (int, float))):
                skipped.add(f"layer {unsupported.replace('_', ' ')}")
        alpha = l.get("layer_effects", {}).get("alpha")
        if alpha:
            j["opacity"] = put_channel(j, "opacity", alpha)
        origin = l.get("origin", {"x": 0, "y": 0})
        if abs(origin.get("x", 0)) > 1e-9 or abs(origin.get("y", 0)) > 1e-9:
            skipped.add("layer origins (layers turn about their own 0, 0)")
        # Masking isn't converted: Open Animate's masks name their layers (set them up after converting).
        parent_bone = l.get("parent_bone", -1)
        if parent_bone >= 0 and parent_bone in bone_ids:
            j["parent_bone"] = bone_ids[parent_bone]

        if kind == "BoneLayer":
            j["type"] = "BONE"
            bones, own_ids, moves = self.bones(l.get("skeleton", {}))
            j["bones"] = bones
            j["next_bone_id"] = len(bones) + 1
            # Moho's bone tags (a bitmask per bone, unnamed) become bone collections "Tag 1", "Tag 2"...
            bits = sorted({bit for b in bones for bit in b.pop("_tags", [])})
            if bits:
                j["bone_collections"] = [{"id": n + 1, "name": f"Tag {bit + 1}", "visible": True, "solo": False}
                                         for n, bit in enumerate(bits)]
                j["active_collection"] = 1
                j["next_collection_id"] = len(bits) + 1
                by_bit = {bit: n + 1 for n, bit in enumerate(bits)}
                for b, src in zip(bones, l.get("skeleton", {}).get("bones", [])):
                    tags = int(src.get("bone_tags", 0) or 0)
                    ids = [by_bit[bit] for bit in bits if tags & (1 << bit)]
                    if ids:
                        b["collections"] = ids
            names = set()
            action_names(l, names)
            j["actions"] = [a.get("name") for a in l.get("actions", []) if a.get("name")] or sorted(names)
            j["children"] = [self.layer(c, own_ids, moves) for c in reversed(l.get("layers", []))]
        elif kind in ("GroupLayer", "SwitchLayer"):
            j["type"] = "GROUP"
            if kind == "SwitchLayer":
                skipped.add("switch layers (made groups)")
            j["children"] = [self.layer(c, bone_ids, flexi_moves) for c in reversed(l.get("layers", []))]
        elif kind == "MeshLayer":
            j["type"] = "VECTOR"
            drawing = drawing_of(l.get("mesh", {}), bone_ids, flexi_moves, self.doc_styles)
            self.drawings[lid] = drawing
            if l.get("layer_ref_uuid") and l.get("layer_ref_same_doc"):
                self.references.append((j, drawing, l["layer_ref_uuid"]))
            else:
                j["drawing"] = drawing
        else:
            j["type"] = "GROUP" if l.get("layers") else "VECTOR"
            skipped.add(f"{kind} layers")
            if l.get("layers"):
                j["children"] = [self.layer(c, bone_ids, flexi_moves) for c in reversed(l["layers"])]
        return j

    def bones(self, skeleton):
        moho = skeleton.get("bones", [])
        ids = {i: i + 1 for i in range(len(moho))}
        # Flexi-bound points move only with bones that have strength (Moho's rule).
        moves = any(b.get("strength", 0) > 0 for b in moho)
        out = []
        for i, b in enumerate(moho):
            j = {"id": i + 1, "name": b.get("name", f"B{i + 1}"),
                 "parent": ids.get(b.get("parent", -1), 0) if b.get("parent", -1) >= 0 else 0}
            j["position"] = put_channel(j, "position", b["anim_pos"])
            j["angle"] = put_channel(j, "angle", b["anim_angle"])
            j["scale"] = put_channel(j, "scale", b["anim_scale"], lambda s: [s, s])
            j["length"] = b.get("length", 0.0)
            j["_tags"] = [bit for bit in range(32) if int(b.get("bone_tags", 0) or 0) & (1 << bit)]
            # Moho's pin bones are the bones with no length: drawn as pins in Open Animate too.
            if j["length"] <= 0.0:
                j["shape"] = "PIN"
            j["strength"] = b.get("strength", 0.0) if b.get("strength", 0.0) > 0 else 1.0
            if b.get("ignored_by_ik"):
                j["ik_ignore"] = True
            if b.get("constraints"):
                j["constraints"] = [{"type": "LIMIT_ANGLE", "name": "Limit Angle", "enabled": True,
                                     "min_angle": b.get("min_constraint", -math.pi),
                                     "max_angle": b.get("max_constraint", math.pi)}]
            if b.get("angle_control_parent", -1) >= 0 or b.get("pos_control_parent", -1) >= 0:
                skipped.add("bone controls (angle/position/scale control bones)")
            if b.get("angle_dynamics") or b.get("pos_dynamics") or b.get("scale_dynamics"):
                skipped.add("bone dynamics")
            out.append(j)
        return out, ids, moves

    def link_references(self):
        """Reference layers point at their originals; how they differ becomes their own changes."""
        for j, drawing, source_uuid in self.references:
            source = self.by_uuid.get(source_uuid)
            if source is None or source not in self.drawings:
                j["drawing"] = drawing  # Its original isn't here: an ordinary layer.
                skipped.add("references to layers in other files (made ordinary layers)")
                continue
            src = self.drawings[source]
            ref = {"edit_points": False, "follow_animation": True, "match_original": True,
                   "points": [], "looks": [], "swaps": [], "weights": []}
            same = (len(src["points"]) == len(drawing["points"]) and len(src["shapes"]) == len(drawing["shapes"]))
            if same:
                for rp, sp in zip(drawing["points"], src["points"]):
                    dx, dy = rp["pos"][0] - sp["pos"][0], rp["pos"][1] - sp["pos"][1]
                    if abs(dx) > 1e-6 or abs(dy) > 1e-6:
                        ref["points"].append({"id": sp["id"], "offset": [dx, dy]})
                for rs, ss in zip(drawing["shapes"], src["shapes"]):
                    look = looks_differ(rs, ss)
                    if look:
                        look["id"] = ss["id"]
                        ref["looks"].append(look)
            else:
                skipped.add("references whose drawing differs in points or shapes (they show their originals)")
            if ref["points"]:
                ref["edit_points"] = True
            j["reference_source"] = source
            j["reference"] = ref

    def document(self):
        pd = self.project.get("project_data", {})
        bg = pd.get("back_color", {"r": 0.92, "g": 0.92, "b": 0.92, "a": 1.0})
        layers = [self.layer(l, {}, False) for l in reversed(self.project.get("layers", []))]
        self.link_references()
        first = layers[0]["id"] if layers else 0
        return {"format": "OpenAnimate", "version": 1,
                "document": {"name": "", "frame_start": int(pd.get("start_frame", 1)),
                             "frame_end": int(pd.get("end_frame", 72)), "frame_current": 0,
                             "fps": float(pd.get("fps", 24)), "width": int(pd.get("width", 1920)),
                             "height": int(pd.get("height", 1080)),
                             "background": [bg.get("r", 1) if isinstance(bg.get("r"), float) else bg["r"] / 255.0,
                                            bg.get("g", 1) if isinstance(bg.get("g"), float) else bg["g"] / 255.0,
                                            bg.get("b", 1) if isinstance(bg.get("b"), float) else bg["b"] / 255.0, 1.0],
                             "active_layer": first, "next_layer_id": self.next_id, "layers": layers, "texts": []}}


def convert(src, dst):
    with zipfile.ZipFile(src) as z:
        name = next((n for n in z.namelist() if n.endswith(".mohoproj")), None)
        if not name:
            raise SystemExit(f"{src}: no .mohoproj inside (only Moho 13.5 and later files can be read)")
        project = json.loads(z.read(name).decode("utf-8"))
    doc = Converter(project).document()
    with zipfile.ZipFile(dst, "w", zipfile.ZIP_DEFLATED) as z:
        z.writestr("document.json", json.dumps(doc, indent=1))
    return doc


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    src = pathlib.Path(sys.argv[1])
    dst = pathlib.Path(sys.argv[2]) if len(sys.argv) > 2 else src.with_suffix(".oanim")
    convert(src, dst)
    print(f"Wrote {dst}")
    if skipped:
        print("Left out (Open Animate doesn't have them):")
        for s in sorted(skipped):
            print("  -", s)


if __name__ == "__main__":
    main()
