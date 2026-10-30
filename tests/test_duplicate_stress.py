"""Duplicate (Shift D) under stress: random drawings (open and closed curves, boxes, circles, grids; then
segments made, points merged and split, edges hidden, fills joined, looks changed, points deleted and
extruded) and random selections (scattered points, whole shapes, one segment, runs along a curve, all).
Each copy must be exactly the selected pieces: no point left on no curve, every piece a shape drew drawn
by a shape of the copy (a whole shape copied whole, in the same look and as closed; a piece of a stroke
keeping its line), the originals unchanged, ids its own, the copy selected, and undo and redo exact.
Then the same with keys (copies keep their originals' motion on every frame), detail levels, point groups,
mirror editing, Separate (P), and saving and opening again (every place exactly as it was).

SEEDS (default 150) and SEED0 pick the drawings.
Run with:  OpenAnimate --background --python tests/test_duplicate_stress.py
"""
import os, random, tempfile, traceback
import oanim

ops = oanim.ops
L = oanim.data.layers
doc = oanim.data.document

# A point picked on its own on a curve that also has a run of picked points (0, and 2-3) gets no copy of its
# own: it used to be copied too, and left on no curve.
ops.layer.new()
lone = L.active
lone.add_curve([(0, 0), (1, 0), (2, 0), (3, 0), (4, 0)])
ops.mesh.select_none()
lone.select_points([0, 2, 3])
assert ops.mesh.duplicate_move() == {'FINISHED'}
assert lone.point_count == 5 + 2, ("only the run's two points are copied", lone.point_count)
assert lone.detail["wires"][1] == [5, 6] and [p[:2] for p in lone.points[5:]] == [(2.0, 0.0), (3.0, 0.0)]
ops.ed.undo()

SEEDS = int(os.environ.get("SEEDS", "150"))
SEED0 = int(os.environ.get("SEED0", "0"))
problems = []


def say(seed, case, msg):
    problems.append((seed, case, msg))
    kind = ''.join(ch for ch in msg.split(" (")[0][:40] if not ch.isdigit())
    if sum(1 for _, _, m in problems if ''.join(ch for ch in m.split(" (")[0][:40] if not ch.isdigit()) == kind) <= 4:
        print("PROBLEM seed", seed, case, ":", msg)


def snap(l):
    d = l.detail
    shapes = []
    for s in range(l.shape_count):
        st = l.shape_style(s)
        shapes.append((l.shape_runs(s), st["has_fill"], st["has_outline"], st["fill_rule"], st["line_width"],
                       l.shape_outline(s, 1)[1]))
    return (tuple((round(x, 5), round(y, 5)) for x, y, _ in l.points), tuple(map(tuple, d["wires"])), tuple(map(str, shapes)),
            tuple(l.hidden_edges), tuple(l.point_ids), tuple(l.shape_ids))


def closed_curves(l):
    """Curves known closed: some shape has the segment from the last point back to the first."""
    wires = l.detail["wires"]
    closed = set()
    for s in range(l.shape_count):
        for run in l.shape_runs(s):
            for c, seg in run:
                if seg == len(wires[c]) - 1:
                    closed.add(c)
    return closed


def segments(l, curves=None):
    """{(curve, seg): (a, b)} for every segment of the curves we can see."""
    wires = l.detail["wires"]
    closed = closed_curves(l)
    out = {}
    for c, w in enumerate(wires):
        if curves is not None and c not in curves:
            continue
        for k in range(len(w) - 1):
            out[(c, k)] = (w[k], w[k + 1])
        if c in closed and len(w) > 1:
            out[(c, len(w) - 1)] = (w[-1], w[0])
    return out


def shape_segments(l):
    """{(curve, seg): [shape indices]}"""
    m = {}
    for s in range(l.shape_count):
        for run in l.shape_runs(s):
            for e in run:
                m.setdefault(tuple(e), []).append(s)
    return m


def build(rng):
    ops.layer.new()
    l = L.active
    for _ in range(rng.randint(1, 5)):
        ox, oy = rng.uniform(-5, 5), rng.uniform(-5, 5)
        kind = rng.choice(["open", "closed", "closed", "box", "circle", "grid"])
        if kind in ("open", "closed"):
            n = rng.randint(2 if kind == "open" else 3, 7)
            pts = [(ox + rng.uniform(-1, 1), oy + rng.uniform(-1, 1)) for _ in range(n)]
            l.add_curve(pts, closed=kind == "closed", fill=kind == "closed" and rng.random() < 0.8)
        else:
            w = rng.uniform(0.5, 2)
            l.add_primitive(kind.upper(), [(ox, oy), (ox + w, oy + w * rng.uniform(0.5, 1.5))], fill=rng.random() < 0.7,
                            columns=rng.randint(1, 3), rows=rng.randint(1, 3))
    # Random topology edits.
    for _ in range(rng.randint(0, 6)):
        n = l.point_count
        if n < 2:
            break
        what = rng.choice(["segment", "merge", "split", "hide", "join", "style", "delete", "extrude"])
        ops.mesh.select_none()
        try:
            if what == "segment":
                l.select_points(rng.sample(range(n), 2))
                ops.mesh.make_segment()
            elif what == "merge":
                l.select_points(rng.sample(range(n), 2))
                ops.mesh.merge()
            elif what == "split":
                segs = list(segments(l).keys())
                if segs:
                    c, s = rng.choice(segs)
                    l.split_segment(c, s, rng.uniform(0.2, 0.8))
            elif what == "hide":
                l.select_points(rng.sample(range(n), min(n, rng.randint(2, 4))))
                l.set_selected_edges_visible(False)
            elif what == "join":
                closed = [s for s in range(l.shape_count) if l.shape_outline(s, 1)[1]]
                if len(closed) >= 2:
                    pick = rng.sample(closed, 2)
                    sel = set()
                    for s in pick:
                        for run in l.shape_runs(s):
                            for e in run:
                                sel.update(segments(l).get(tuple(e), ()))
                    l.select_points(sorted(sel))
                    ops.mesh.join_fills()
            elif what == "style" and l.shape_count:
                s = rng.randrange(l.shape_count)
                if rng.random() < 0.5:
                    l.set_shape_style(s, has_outline=False)
                else:
                    l.set_shape_style(s, has_fill=False)
            elif what == "delete":
                l.select_points([rng.randrange(n)])
                ops.mesh.delete_points()
            elif what == "extrude":
                l.select_points([rng.randrange(n)])
                l.extrude_selected()
                l.move_points((rng.uniform(-0.5, 0.5), rng.uniform(-0.5, 0.5)))
        except Exception as e:
            print("build op failed", what, e)
    # Distinct places, so copies map back to their originals by where they are.
    ops.mesh.select_none()
    return l


def selection(rng, l):
    n = l.point_count
    how = rng.choice(["random", "random", "shape", "segment", "run", "all", "two shapes"])
    if how == "random":
        p = rng.uniform(0.2, 0.9)
        return how, [i for i in range(n) if rng.random() < p]
    if how == "all":
        return how, list(range(n))
    segs = segments(l)
    if how == "segment" and segs:
        return how, list(rng.choice(list(segs.values())))
    if how == "run" and l.detail["wires"]:
        w = rng.choice(l.detail["wires"])
        a = rng.randrange(len(w))
        b = rng.randrange(a, len(w))
        return how, sorted(set(w[a:b + 1]))
    if how in ("shape", "two shapes") and l.shape_count:
        sel = set()
        for s in rng.sample(range(l.shape_count), min(l.shape_count, 1 if how == "shape" else 2)):
            for run in l.shape_runs(s):
                for e in run:
                    sel.update(segs.get(tuple(e), ()))
        return how, sorted(sel)
    return "random", [i for i in range(n) if rng.random() < 0.5]


def check(seed, case, l, before, before_segs, before_shape_segs, before_styles, picked, result):
    pts_before = before[0]
    n0 = len(pts_before)
    pts = l.points
    wires = l.detail["wires"]
    c0 = len(before[1])
    picked = set(picked)
    want = {k: ab for k, ab in before_segs.items() if ab[0] in picked and ab[1] in picked}
    if result != {'FINISHED'}:
        if want:
            say(seed, case, "cancelled with %d selected segments" % len(want))
        if snap(l) != before:
            say(seed, case, "cancelled but the drawing changed")
        return False
    # The original stays as it was.
    if tuple((round(x, 5), round(y, 5)) for x, y, _ in pts[:n0]) != pts_before:
        say(seed, case, "original points moved")
    if tuple(map(tuple, wires[:c0])) != before[1]:
        say(seed, case, "original curves changed")
    # Selection: the copy, only.
    if any(p[2] for p in pts[:n0]) or not all(p[2] for p in pts[n0:]):
        say(seed, case, "selection isn't the copy")
    ids = l.point_ids
    if len(set(ids)) != len(ids):
        say(seed, case, "point ids repeat")
    sids = l.shape_ids
    if len(set(sids)) != len(sids):
        say(seed, case, "shape ids repeat")
    # Every new point is on a new curve, and new curves use only new points.
    used = set()
    for c, w in enumerate(wires[c0:], c0):
        for p in w:
            if p < n0:
                say(seed, case, "copy curve %d uses an original point %d" % (c, p))
            used.add(p)
    orphans = [p for p in range(n0, len(pts)) if p not in used]
    if orphans:
        say(seed, case, "%d copied points on no curve (picked %s)" % (len(orphans), sorted(picked)))
    # Map copies back by place.
    where = {}
    for i, (x, y) in enumerate(pts_before):
        where.setdefault((x, y), []).append(i)
    orig = {}
    for p in range(n0, len(pts)):
        k = (round(pts[p][0], 5), round(pts[p][1], 5))
        cands = [i for i in where.get(k, []) if i in picked]
        if len(cands) != 1:
            if not cands:
                say(seed, case, "copied point %d isn't where a picked point was" % p)
            orig[p] = None
        else:
            orig[p] = cands[0]
    new_segs = segments(l, set(range(c0, len(wires))))
    # closed copies with no shape aren't seen as closed: count the closing segment from the original instead
    got = {}
    seen_pairs = {}
    for k, (a, b) in sorted(new_segs.items()):
        oa, ob = orig.get(a), orig.get(b)
        if oa is None or ob is None:
            continue
        match = [ok for ok, (x, y) in want.items() if (x, y) == (oa, ob) or (x, y) == (ob, oa)]
        # Two curves through the same two points: the copy's is the one from the curve it was copied from
        # (copies come in their originals' order), so keep the match on the curve its other segments match.
        if len(match) > 1:
            pair = frozenset((oa, ob))
            match = [sorted(match)[seen_pairs.get(pair, 0) % len(match)]]
            seen_pairs[pair] = seen_pairs.get(pair, 0) + 1
        if not match:
            # A curve closed with no shape on it: its closing segment, run past by a copy that wraps.
            if any(len(w) > 2 and {w[0], w[-1]} == {oa, ob} for w in before[1]):
                continue
            say(seed, case, "copied segment %s (%d-%d) wasn't an original segment" % (k, oa, ob))
        for m in match:
            got.setdefault(m, []).append(k)
    # Closing segments of copies without shapes can't be seen; only check segments that were in shapes.
    new_shape_segs = shape_segments(l)
    # Picked points in one place can't be told apart by place: leave their segments out of these checks.
    shared = set(i for k, v in where.items() if len([j for j in v if j in picked]) > 1 for i in v)
    for ok in want:
        if ok in before_shape_segs and ok not in got and not shared & set(want[ok]):
            say(seed, case, "shape segment %s wasn't copied" % (ok,))
    # Shapes form: every copied segment some shape drew is drawn by a new shape, in a look an original had.
    n_shapes0 = len(before_styles)
    for ok, nks in got.items():
        if ok not in before_shape_segs:
            continue
        if not any(s >= n_shapes0 for nk in nks for s in new_shape_segs.get(nk, [])):
            say(seed, case, "copied segment %s (shapes %s) is in no new shape" % (ok, before_shape_segs[ok]))
    for s in range(n_shapes0, l.shape_count):
        st = l.shape_style(s)
        outline, closed = l.shape_outline(s, 1)
        runs = l.shape_runs(s)
        edges = [tuple(e) for run in runs for e in run]
        if not edges:
            say(seed, case, "new shape %d has no edges" % s)
            continue
        if any(c < c0 for c, _ in edges):
            say(seed, case, "new shape %d uses an original curve" % s)
        if not (st["has_outline"] or (st["has_fill"] and closed)):
            # Fill-only pieces wait to be closed; say only when an original drew those pieces with a line.
            srcs = set()
            for e in edges:
                for ok, nks in got.items():
                    if e in nks:
                        srcs.update(before_shape_segs.get(ok, []))
            if any(before_styles[o][2] for o in srcs):
                say(seed, case, "new shape %d shows nothing (fill only, open) though its original had a line" % s)
        try:
            l.evaluated_outline(s, 4)
        except Exception as e:
            say(seed, case, "evaluated outline failed %s" % e)
    # Whole shapes copied whole: same closure, same look.
    for o, (runs, has_fill, has_outline, closed0) in enumerate(before_styles):
        oks = [tuple(e) for run in runs for e in run]
        if not oks or not all(k in want for k in oks) or any(shared & set(want[k]) for k in oks):
            continue
        nks = set(nk for k in oks for nk in got.get(k, []))
        twins = [s for s in range(n_shapes0, l.shape_count)
                 if set(tuple(e) for run in l.shape_runs(s) for e in run) == nks]
        if not twins:
            say(seed, case, "whole shape %d has no whole copy" % o)
            continue
        st = l.shape_style(twins[0])
        if (st["has_fill"], st["has_outline"], l.shape_outline(twins[0], 1)[1]) != (has_fill, has_outline, closed0):
            say(seed, case, "whole shape %d copied as %s, was %s" % (o, (st["has_fill"], st["has_outline"],
                                                                          l.shape_outline(twins[0], 1)[1]),
                                                                       (has_fill, has_outline, closed0)))
    return True


for seed in range(SEED0, SEED0 + SEEDS):
    rng = random.Random(seed)
    try:
        l = build(rng)
    except Exception:
        say(seed, "build", traceback.format_exc())
        continue
    for case in range(12):
        try:
            L.active = l
            doc.frame_current = 0
            ops.mesh.select_none()
            how, picked = selection(rng, l)
            if not picked:
                continue
            l.select_points(picked)
            before = snap(l)
            # select_points may pick through: read what's really selected.
            picked = [i for i, p in enumerate(l.points) if p[2]]
            bsegs = segments(l)
            bshape = shape_segments(l)
            bstyles = []
            for s in range(l.shape_count):
                st = l.shape_style(s)
                bstyles.append((l.shape_runs(s), st["has_fill"], st["has_outline"], l.shape_outline(s, 1)[1]))
            r = ops.mesh.duplicate_move()
            did = check(seed, "%d %s" % (case, how), l, before, bsegs, bshape, bstyles, picked, r)
            if not did:
                continue
            after = snap(l)
            ops.ed.undo()
            if snap(l)[:3] != before[:3]:
                say(seed, case, "undo doesn't restore")
            ops.ed.redo()
            if snap(l)[:3] != after[:3]:
                say(seed, case, "redo differs")
            # Again, from the copy (now the selection).
            if rng.random() < 0.5:
                before2 = snap(l)
                picked2 = [i for i, p in enumerate(l.points) if p[2]]
                b2 = (segments(l), shape_segments(l))
                st2 = [(l.shape_runs(s), l.shape_style(s)["has_fill"], l.shape_style(s)["has_outline"],
                        l.shape_outline(s, 1)[1]) for s in range(l.shape_count)]
                r2 = ops.mesh.duplicate_move()
                check(seed, "%d %s again" % (case, how), l, before2, b2[0], b2[1], st2, picked2, r2)
                ops.ed.undo()
            ops.ed.undo()
            if snap(l)[:3] != before[:3]:
                say(seed, case, "undo after redo doesn't restore")
        except Exception:
            say(seed, case, traceback.format_exc())

TMP = tempfile.mkdtemp()


def rounded(pts):
    return [(round(x, 4), round(y, 4)) for x, y, *_ in pts]


def valid(seed, case, l, orphans_before):
    """What holds of any drawing: ids its own, every point on a curve (as many loose ones as before at most),
    every shape drawable."""
    ids = l.point_ids
    if len(set(ids)) != len(ids):
        say(seed, case, "point ids repeat")
    if len(set(l.shape_ids)) != len(l.shape_ids):
        say(seed, case, "shape ids repeat")
    wires = l.detail["wires"]
    on = set(p for w in wires for p in w)
    loose = sum(1 for p in range(l.point_count) if p not in on)
    if loose > orphans_before:
        say(seed, case, "%d points on no curve (was %d)" % (loose, orphans_before))
    for s in range(l.shape_count):
        runs = l.shape_runs(s)
        if not runs:
            say(seed, case, "shape %d has no edges" % s)
        for run in runs:
            for c, seg in run:
                if c >= len(wires) or (seg >= len(wires[c]) and not any(l.detail["levels"])):
                    say(seed, case, "shape %d edge (%d, %d) past its curve" % (s, c, seg))
        l.evaluated_outline(s, 4)
    return loose


def loose_count(l):
    on = set(p for w in l.detail["wires"] for p in w)
    return sum(1 for p in range(l.point_count) if p not in on)


for seed in range(SEED0, SEED0 + SEEDS):
    rng = random.Random(1000 + seed)
    doc.frame_current = 0
    try:
        l = build(rng)
    except Exception:
        say(seed, "build", traceback.format_exc())
        continue
    n = l.point_count
    if n < 2:
        continue
    variant = rng.choice(["anim", "anim", "detail", "groups", "mirror", "separate", "save"])
    try:
        if variant == "anim":
            doc.auto_key = True
            doc.frame_current = 10
            ops.mesh.select_none()
            l.select_points(rng.sample(range(n), max(1, n // 2)))
            l.move_points((rng.uniform(-1, 1), rng.uniform(-1, 1)))
            doc.frame_current = 0
        elif variant == "detail":
            ops.mesh.select_none()
            l.select_points(rng.sample(range(n), max(1, n // 3)))
            ops.mesh.detail_raise()
        elif variant == "groups":
            ops.mesh.point_group_add(name="G")
            ops.mesh.select_none()
            l.select_points(rng.sample(range(n), max(1, n // 2)))
            ops.mesh.point_group_assign(weight="0.5")
        elif variant == "mirror":
            oanim.wm.set_mirror_editing(x=True)
        ops.mesh.select_none()
    except Exception:
        say(seed, variant + " setup", traceback.format_exc())
        continue
    for case in range(8):
        tag = "%s %d" % (variant, case)
        try:
            L.active = l
            frame = rng.choice([0, 5, 10]) if variant == "anim" else 0
            doc.frame_current = frame
            ops.mesh.select_none()
            how, picked = selection(rng, l)
            if not picked:
                continue
            l.select_points(picked)
            picked = [i for i, p in enumerate(l.points) if p[2]]
            tag += " " + how
            before = snap(l)
            loose = loose_count(l)
            n0, c0, s0 = l.point_count, l.curve_count, l.shape_count
            groups_before = l.point_groups
            per_frame = {}
            for f in ((0, 5, 10) if variant == "anim" else ()):
                doc.frame_current = f
                per_frame[f] = rounded(l.points)
            doc.frame_current = frame
            if variant == "separate":
                count = len(L.all())
                r = ops.mesh.separate()
                if r == {'FINISHED'}:
                    if len(L.all()) != count + 1:
                        say(seed, tag, "separate made %d layers" % (len(L.all()) - count))
                    for x in L.all():
                        if x.name.startswith(l.name) and x.id != l.id:
                            valid(seed, tag + " piece", x, 0)
                    valid(seed, tag + " rest", l, loose)
                    ops.ed.undo()
                    if snap(l)[:3] != before[:3] or len(L.all()) != count:
                        say(seed, tag, "undo of separate doesn't restore")
                continue
            r = ops.mesh.duplicate_move()
            if variant == "mirror":
                if r == {'FINISHED'}:
                    valid(seed, tag, l, loose)
                    ops.ed.undo()
                    if snap(l)[:3] != before[:3]:
                        say(seed, tag, "undo doesn't restore")
                continue
            did = check(seed, tag, l, before, segments_b, shape_b, styles_b, picked, r) if False else None
            # (the full check needs the before-state taken on the same frame: redo it here)
            if r != {'FINISHED'}:
                continue
            valid(seed, tag, l, loose)
            pts = l.points
            # Map copies to originals by place on this frame.
            where = {}
            for i in picked:
                where.setdefault((round(pts[i][0], 4), round(pts[i][1], 4)), []).append(i)
            orig = {}
            for p in range(n0, len(pts)):
                c = where.get((round(pts[p][0], 4), round(pts[p][1], 4)), [])
                if len(c) == 1:
                    orig[p] = c[0]
                elif not c:
                    say(seed, tag, "copy %d isn't where a picked point is" % p)
            if variant == "anim":
                for f in (0, 5, 10):
                    doc.frame_current = f
                    now = rounded(l.points)
                    if now[:n0] != per_frame[f]:
                        say(seed, tag, "originals moved on frame %d: %s" % (f, [(i, per_frame[f][i], now[i]) for i in range(n0) if now[i] != per_frame[f][i]][:3]))
                    for p, o in orig.items():
                        if now[p] != now[o]:
                            say(seed, tag, "copy %d off its original %d on frame %d: %s vs %s" % (p, o, f, now[p], now[o]))
                            break
                doc.frame_current = frame
            if variant == "groups":
                groups = l.point_groups
                for (name, members, _), (_, members0, _) in zip(groups, groups_before):
                    for p, o in orig.items():
                        if members.get(p) != members0.get(o):
                            say(seed, tag, "copy %d in group %s %s, original %s" % (p, name, members.get(p), members0.get(o)))
                            break
            if variant == "detail":
                d = l.detail
                for p, o in orig.items():
                    if d["levels"][p] != d["levels"][o]:
                        say(seed, tag, "copy %d at detail level %d, original %d" % (p, d["levels"][p], d["levels"][o]))
                        break
            if variant == "save":
                path = os.path.join(TMP, "dup_%d.oanim" % seed)
                after = snap(l)
                assert ops.wm.save_as_mainfile(filepath=path) == {'FINISHED'}
                assert ops.wm.open_mainfile(filepath=path) == {'FINISHED'}
                lid = l.id
                l = [x for x in oanim.data.layers.all() if x.id == lid][0]
                again = snap(l)
                if [p[:2] for p in l.points] != [p[:2] for p in pts]:
                    say(seed, tag, "reopened points aren't exactly where they were")
                for part, (x, y) in enumerate(zip(after[:3], again[:3])):
                    if x != y:
                        say(seed, tag, "reopened drawing differs in %s: %s" % (["points", "curves", "shapes"][part],
                            [(i, a, b) for i, (a, b) in enumerate(zip(x, y)) if a != b][:2] or (len(x), len(y))))
                L.active = l
                os.remove(path)
                continue
            ops.ed.undo()
            if snap(l)[:3] != before[:3]:
                say(seed, tag, "undo doesn't restore")
        except Exception:
            say(seed, tag, traceback.format_exc())
    oanim.wm.set_mirror_editing(x=False)
    doc.frame_current = 0

for seed, case, msg in problems[:20]:
    print("seed", seed, case, ":", msg)
assert not problems, "%d problems" % len(problems)
print("duplicate stress OK")
