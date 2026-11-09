"""Mutation checks, batched until a milestone: each proves a test fails without the fix it guards.

Each feature adds an entry to PENDING for every fix it makes: the file, a piece of its code (found exactly once), what
to put there instead to undo the fix, a name, and the test that should then fail. They are run together when the user
says "run milestone checks":

    python tools/mutation_checks.py              # every pending check
    python tools/mutation_checks.py 3 7          # only those
    python tools/mutation_checks.py --lanes 6    # six at once (this folder and five lane folders)
    python tools/mutation_checks.py --lanes 6 --prepare   # only make and build the lane folders
    python tools/mutation_checks.py --lanes 6 --not-here  # the five lane folders only: this one stays free to edit
    python tools/mutation_checks.py --remove-lanes

Lanes: a check edits the source and rebuilds, so checks run side by side only in folders of their own. --lanes N uses
this folder and N-1 lane worktrees beside it (<this folder's name>-mut1, ...), made on first use (a configure and full
build, about ten minutes, reusing this build's fetched sources) and kept for the next run; each run brings them to this
folder's commit plus its uncommitted changes. A lane takes the next check on the file it last changed when there is
one, so each rebuild compiles one file. --remove-lanes deletes them.

Run it from the worktree's root, with nothing else building or editing the source there: it changes one file at a
time, rebuilds (for C++), runs the test and puts the file back. A check the test still passes is "NOT CAUGHT": the
test is strengthened until it fails. Once every check is caught and committed, PENDING is emptied.

Two chats can both add entries here; on a merge conflict keep both lists.
"""

import os
import shutil
import subprocess
import sys
import tempfile
import threading

CMAKE = (r"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/"
         r"CMake/CMake/bin/cmake.exe")
EXE = os.path.join("build", "bin", "RelWithDebInfo", "OpenAnimate.exe")

# (file, old, new, name, test)
PENDING = [
    ('src/wm/context.hh', 'return tool == Tool::TransformLayer || (tool == Tool::MotionPath && path_edit_layer);',
     'return tool == Tool::TransformLayer || (false && path_edit_layer);',
     'with the Motion Path tool\'s Edit Layer, G R S and Snap move the layer, not its points', 'motion_path_edit_layer'),
    ('scripts/startup/oanim_keymap_default.py',
     '    ("Timeline", "anim.timeline_scale_spacing_modal", "S", {"alt": True}),\n', '',
     'Alt S over the timeline scales the timing around the ticks and keys picked', 'motion_path_edit_speed'),
    ('src/anim/motion_path.cc', '''    ch.keys[i - 1].interval = 1;
  }
  stretch_timing_keep(ch, i, was, from);
''', '''    ch.keys[i - 1].interval = 1;
  }
''', 'Convert to Control Point keeps every dot on its frame', 'motion_path_key_to_control'),
    ('src/anim/motion_path.cc', '''  stretch_timing_keep(*ch, k + 1, was, from);
''', '', 'Convert to Key keeps the dots after the new key on their frames', 'motion_path_key_to_control'),
    ('src/anim/motion_path.cc', 'if (target.point && layer && layer->key_whole_drawing) {', 'if (false) {',
     "a Key Whole Drawing point's key is not made a control point", 'motion_path_key_to_control'),
    ('src/anim/channel.cc', '      h.aligned = k.path_handles != PathHandles::Broken;', '      h.aligned = false;',
     'the key made a control point keeps its handles in line', 'motion_path_key_to_control'),
    ('src/anim/motion_path.cc', '''  if (stepped) {
    ch.keys[i - 1].interval = 1;
  }''', '', 'a stretch on 2s made one with the key gone keeps its steps', 'motion_path_key_to_control'),
    ('src/anim/channel.cc', '''  key_aligned_again(b, b_broken);
  keys.erase(keys.begin() + long(i));''', '''  key_aligned_again(b, b_broken);''', 'Convert to Control Point takes the key out', 'motion_path_key_to_control'),
    ('src/editors/timeline.cc', '  if (f.mode != 2 || !f.picked) {', '  if (f.mode != 2) {',
     "the timing chart is hidden with nothing picked (the playhead's teal is not a pick)", 'timing_chart_rows'),
    ('src/anim/animation.cc',
     '  return layer.type == LayerType::Group || layer.type == LayerType::Bone || layer.type == LayerType::Vector;',
     '  return layer.type == LayerType::Group || layer.type == LayerType::Bone;',
     'a vector layer alone keys together', 'key_together_fold'),
    ('src/editors/timeline.cc', '    row.open = open_all || !l.key_together;', '    row.open = open_all || row.open;',
     "a layer's row is folded exactly while it keys together", 'key_together_fold'),
    ('src/editors/timeline.cc', '      row.filter.kinds = 0;', '',
     "an open layer's row is a heading with no keys", 'key_together_fold'),
    ('src/editors/timeline.cc', '    add("Summary", all, layer_keys, 0, "", false, false, 0);',
     '    add("Summary", all, layer_keys, 0, "summary", true, false, 0);',
     "the Summary is its own row and never folds", 'timeline_summary_merge'),
    ('src/editors/timeline.cc',
     '    add(l.name, under, layer_keys, depth, fold, fold_default, red || (l.key_together && pick), l.id, false);',
     '    add(l.name, under, layer_keys, depth, fold, fold_default, red, l.id, false);',
     "a folded layer's row over a pick is red", 'timeline_pick_open'),
    ('src/editors/timeline.cc', '  if (rows[row].together) {', '  if (false) {',
     "folding a layer's row turns Key Together on", 'key_together_fold'),
    ('src/anim/motion_path.cc',
     '    return layer->name + (key_together_allowed(*layer) && layer->key_together ? " together" :',
     '    return layer->name + (false ? " together" :',
     'a vector layer keying together has a "<layer> together" timing row', 'key_together_fold'),
    ('src/anim/animation.cc', '    fillers_hold_around(ch, frame);', '',
     'keyed between poses while keying together, the poses either side hold', 'key_together_fold'),
    ('src/anim/animation.cc', '      k->filler = false;', '      k->filler = true;',
     'the held poses either side of an edit stay when Key Together is off', 'key_together_fold'),
    ('src/anim/animation.cc', '          hold_poses_around(keyed, poses, frame);', '',
     'Auto-Freeze Keys holds a channel keyed at its layer\'s poses either side', 'auto_key_options'),
    ('src/anim/animation.cc', 'if (at_after != INT32_MIN && !ch.key_at(at_after)) {', 'if (false) {',
     'Auto-Freeze Keys holds the pose after the edit too, not only the one before', 'auto_key_options'),
    ('src/data/document.hh', '  bool auto_freeze_keys = true;', '  bool auto_freeze_keys = false;',
     'Auto-Freeze Keys is on for new files', 'auto_key_options'),
    ('src/io/oanim_file.cc', 'get_bool(j, "auto_freeze_keys", false)', 'get_bool(j, "auto_freeze_keys", true)',
     'a file saved without Auto-Freeze Keys opens with it off', 'auto_key_options'),
    ('src/anim/animation.cc', '    k.filler = false;\n    k.filler_inside = false;\n    if (!plan.all.empty()) {', '    if (!plan.all.empty()) {',
     "an edit on a filler's frame right after opening is the channel's own", 'key_together_sweep'),
    ('src/anim/animation.cc', '        group->key_together = false;\n        group->key_together_settled = false;\n        key_together_release(doc, *group);', '        group->key_together = false;\n        group->key_together_settled = false;',
     'a group made a switch lets its Key Together fillers go', 'key_together_sweep'),
    ('src/anim/animation.cc', '      if (!fillers) {\n        /* A key of its own', '      if (false) {\n        /* A key of its own',
     "Key Whole Drawing's keys in a Key Together group hold the fillers either side", 'key_together_sweep'),
    ('src/anim/animation.cc', '  key_settings_apply(k, arriving);\n  k.hold = std::max(start - frame, 0);', '  k.hold = std::max(start - frame, 0);',
     'an Auto-Freeze hold key plays as the stretch it splits', 'key_together_sweep'),
    ('src/anim/animation.cc', '    fillers_hold_around(ch, frame);\n    hold_apply(ch, plan);', '    fillers_hold_around(ch, frame);\n    plan = hold_plan(ch, poses(), frame);\n    hold_apply(ch, plan);',
     "Auto-Freeze reads the poses before Key Together's filler there is taken", 'key_together_sweep'),
    ('src/anim/switch.cc', '    auto_freeze_hold(doc, sw, ch, frame);\n', '',
     'a switch pick with Auto-Freeze holds the poses either side', 'key_together_sweep'),
    ('src/anim/animation.cc', '    if (!plan.all.empty()) {\n      hold_reach(ch, plan, frame);', '    if (false) {\n      hold_reach(ch, plan, frame);',
     "with Smooth keys Auto-Freeze holds every pose the edit's bend reaches", 'key_together_sweep'),
    ('src/anim/animation.cc', '        if (f.filler && f.frame != frame) {', '        if (false) {',
     "Key Together alone: a Smooth edit doesn't bend the curve past its fillers' poses", 'key_together_sweep'),
    ('src/anim/animation.cc', '  const std::vector<Layer *> layers = together_picks_widen(doc, layers_picked);', '  const std::vector<Layer *> layers = layers_picked;',
     'a key moved alone in a Key Together group takes its frame on every layer along', 'key_together_sweep'),
    ('src/anim/animation.cc', '            seen.insert(&dial->angle_keys);', '            (void)dial;',
     "Key Together leaves a controller's linked dials alone (folding a rig deletes no keys)", 'controller_edges'),
    ('src/anim/animation.cc', '      if (std::none_of(inside.begin(), inside.end(), [&](const Layer *l) { return keyed_layers.count(l) > 0; })) {', '      if (false) {',
     'entering an action and leaving with Key Together on changes nothing', 'key_together_sweep'),
    ('src/anim/keep_poses.cc', '  Document look = doc;\n  for (DrawingsShownAll::Action &a', '  Document &look = doc;\n  for (DrawingsShownAll::Action &a',
     'a Keep Animation tool that changes nothing keeps an edit not keyed', 'weight_keep_animation'),
    ('src/anim/motion_path.cc', '    return key_together_layers(d, *layer);\n  }\n  return {layer};', '    return {layer};\n  }\n  return {layer};',
     "a folded group's path finds its keys on the layers inside it (Alt click in its row)", 'key_together_path_pick'),
    ('src/anim/motion_path.cc', '    add(motion_path_timing_owner(doc, t), f);', '    add(t, f);',
     "a dot on a path in a folded group picks the group's stretch", 'key_together_path_pick'),
    ('src/ops/motion_path_ops.cc', '        const MotionPathTarget owner = motion_path_timing_owner(doc, t);', '        const MotionPathTarget owner = t;',
     "a key on a path in a folded group selects every layer's key there", 'key_together_path_pick'),
    ('src/anim/motion_path.cc', '    if (t.same(target) || (owned && motion_path_timing_owner(doc, t).same(owner))) {', '    if (t.same(target)) {',
     "the group's stretch is lit on every path in it", 'key_together_path_pick'),
    ('src/anim/motion_path.cc', '  const MotionPathTarget t = motion_path_timing_owner(doc, target);\n  const void *own = nullptr;', '  const MotionPathTarget t = target;\n  const void *own = nullptr;',
     'a timing tool on a path in a folded group retimes the whole group', 'key_together_path_pick'),
    ('src/anim/motion_path.cc', '  const MotionPathTarget target = motion_path_timing_owner(doc, target_in);', '  const MotionPathTarget target = target_in;',
     "a breakdown made on a path in a folded group is the group's", 'key_together_path_pick'),
    ('src/anim/motion_path.cc', '        owned.push_back(o.same(t) ? t : o);', '        owned.push_back(t);',
     "with Edit Layer the timing shown for a folded group is the group's", 'key_together_path_pick'),
    ('src/ops/motion_path_ops.cc', ' || (t.drawing && key_together_group(*C.document, t.layer))) {', ') {',
     "with Edit Layer a key picked on a path in a folded group picks the group's stretch", 'key_together_path_pick'),
    ('src/editors/timeline.cc', '      if (open && l.is_switch()) {', '      if (false) {',
     "an open switch's row shows only its own switch keys", 'timeline_switch_row'),
    ('src/editors/timeline.cc', '''      else if (!open) {
        rows.resize(at + 1);
      }''', '',
     "a layer's properties are under its folded Layer row", 'timeline_layer_props_row'),
    ('src/anim/animation.cc', '''  if (path == "switch") {
    return KEY_SWITCH;
  }''', '',
     "a switch's keys stay out of its Layer row", 'timeline_layer_props_row'),
    ('src/anim/animation.cc', 'std::set<int> &owner_frames = fillers ? owner.key_together_frames : owner.key_whole_frames;',
     'std::set<int> &owner_frames = owner.key_whole_frames;',
     'a folded drawing keeps its Key Together frames apart from Key Whole Drawing\'s, so a point edit on a new frame '
     'stays', 'mirror'),
    ('src/anim/animation.cc', '''  if (group.key_together_members_of != group.id) {
    group.key_together_members.clear();
    return {0, 0};
  }
''', '', 'a duplicated folded layer or group leaves its source\'s fillers alone', 'key_together_copy'),
    ('src/anim/animation.cc', '''  walk(walk, group);
  return out;''', '''  for (const Layer *l : document_layers_flat(doc)) {
    for (Layer *a = layer_find(doc, l->id); a; a = layer_parent(doc, a->id)) {
      if (a == &group) {
        out.push_back(layer_find(doc, l->id));
        break;
      }
    }
  }
  return out;''', 'a frame change stays fast with hundreds of folded layers', 'drivers_rig'),
    ('src/data/document_layers.cc', '  layer.key_together = type == LayerType::Vector;', '  layer.key_together = false;',
     'a new vector layer starts folded in the timeline', 'timeline_tree'),
    ('src/io/oanim_file.cc', '''  layer.key_together_action = get_string(j, "key_together_action");
''', '',
     'a file opens keying together as it was saved: a layer folded in an action fills its mainline only once keyed there',
     'key_together_reopen'),
    ('src/anim/animation.cc', 'if (!fillers && was_animated && b != ch.keys.begin()', 'if (was_animated && b != ch.keys.begin()',
     "a folded layer's filler inside a Custom or controlled stretch moves nothing", 'key_together_shaped_fillers'),
    ('src/anim/animation.cc', '''  if (together_went || std::any_of(keys_went.begin(), keys_went.end(), [](char w) { return w != 0; })) {
    animation_evaluate(doc, frame);
  }
''', '', "a key deleted on one channel of a folded layer: the frame shown is without the others' keys at once",
     'key_together_deletes'),
    ('src/anim/animation.cc', '        !group->key_together_frames.count(timeline))', '        true)',
     'in an action entered again, a key deleted on one channel of a folded layer goes from them all',
     'key_together_deletes'),
    ('src/io/oanim_file.cc', '''        layer.key_together_members_of = layer.id;
''', '',
     "right after opening a file, a layer moved out of a folded group lets go of the group's fillers",
     'key_together_deletes'),
    ('src/anim/animation.cc', '''      k.filler_edited = true;
''', '''''',
     'opening a folded row after a timing edit leaves a layer that never moves with no keys', 'key_together_retimed'),
    ('src/anim/animation.cc', '''  if (without.keys.empty()) {
    return false;
  }''', '''  if (!without.keys.empty()) {
    return true;
  }''',
     'opening a folded row keeps a retimed filler that shapes a motion', 'key_together_retimed'),
    ('src/io/oanim_file.cc', '''    key.filler_edited = !key.filler && get_bool(k, "filler_edited", false);
''', '''''',
     'a retimed filler is still let go when the row is opened after saving and opening', 'key_together_retimed'),
    ('src/anim/motion_path.cc', 'if (!kept || !f_kept.made', 'if (true || !kept || !f_kept.made',
     "a frame change reuses the paths' frames worked out on copies", 'motion_path_kept'),
    ('src/ops/undo.hh', '''    mark_changed();
    selection_changes_++;
''', '''    mark_changed();
''', 'picking keys on the paths keeps what they are worked out from', 'motion_path_kept'),
    ('src/anim/motion_path.cc', 'serial_ != serial || keys_ != keys)', 'serial_ != serial)',
     'keys changed with no change counted work the paths out again', 'motion_path_kept'),
    ('src/anim/motion_path.cc', 'MotionPathPlacesKept *kept = kind != PLACES_UNKEPT ? t_places_kept : nullptr;',
     'MotionPathPlacesKept *kept = nullptr;',
     'the places handles and control points come from are kept while the document stays the same', 'motion_path_kept'),
    ('src/anim/motion_path.cc', '    if (d.target.layer == t.layer ||', '    if (false ||',
     "a driven layer's own values are worked out, not taken from its keys", 'motion_path_from_keys'),
    ('src/anim/motion_path.cc', '''        if (pool) {
          own_values_set(copy, t, was);
        }''', '''        if (false) {
          own_values_set(copy, t, was);
        }''', "a kept copy of the document is put back after each path's places", 'motion_path_from_keys'),
    ('src/anim/motion_path.cc', '''      if (pool) {
        own_values_set(mine, t, was);
        mine.frame_current = doc->frame_current;
      }''', '''      if (false) {
        own_values_set(mine, t, was);
        mine.frame_current = doc->frame_current;
      }''', 'a kept copy of the document is put back after the handles and control points are placed',
     'motion_path_from_keys'),
    ('src/anim/motion_path.cc', '  const Channel<float2> &ch = position ? *position : layer->position_keys;',
     '  const Channel<float2> &ch = layer->position_keys; (void)position;',
     "the handles and control points take their own values from the keys changed", 'motion_path_from_keys'),
    ('src/anim/deform.cc', 'return layer_point_skins_bones_some(doc, layer, {int(i)})[0].apply(stored);',
     'return stored;', "a point placed by itself goes where its bones' skin takes it", 'motion_path_edit_fast'),
    ('src/anim/deform.cc', '        return shown.points[i].pos;', '        return stored;',
     'a bent point placed by itself goes where the deforms show it', 'motion_path_edit_fast'),
    ('src/anim/motion_path.cc', 'const bool in_place = !target.bone &&', 'const bool in_place = false && !target.bone &&',
     'a dot moved is placed on the document held as shown, not another copy', 'motion_path_edit_fast'),
    ('src/anim/timing_curve.cc', '    if (f.speeds == speeds) {', '    if (false) {',
     "a drawing keyed whole fits its timing once for all its channels", 'motion_path_edit_fast'),
    ('src/anim/motion_path.cc', '    if (ch && base) {', '    if (false) {',
     "handles and control points start from the channel given, not the document's", 'motion_path_edit_fast'),
    ('src/anim/motion_path.cc', 'if (!g_own_from_keys || t.drawing) {', 'if (!g_own_from_keys || t.bone || t.drawing) {',
     "a bone's own values come from its keys, with no copy worked out", 'motion_path_edit_fast'),
    ('src/anim/motion_path.cc', '!b.angle_keys.actions.empty() ||', 'false ||',
     "a bone an action turns is worked out on copies", 'motion_path_edit_fast'),
    ('src/anim/motion_path.cc', 'if (frame == 0 && bone_is_controller(b) && !ch.animated()) {', 'if (false) {',
     "a knob with no keys is at rest on frame 0", 'motion_path_edit_fast'),
    ('src/anim/motion_path.cc', 'at(b.scale_keys, b.scale)', 'b.scale',
     "a bone's size comes from its keys", 'motion_path_edit_fast'),
    ('src/anim/motion_path.cc', 'const Channel<float2> &ch = position ? *position : b.position_keys;',
     'const Channel<float2> &ch = b.position_keys;',
     "a bone's handles and control points take its keys changed", 'motion_path_edit_fast'),
]

def build(root):
    return subprocess.run([CMAKE, "--build", "--preset", "release"], cwd=root, capture_output=True,
                          text=True).returncode == 0


def git(root, *args, data=None):
    r = subprocess.run(["git", *args], cwd=root, input=data, capture_output=True)
    if r.returncode:
        raise RuntimeError(f"git {' '.join(args)} in {root}: {r.stderr.decode(errors='replace')}")
    return r.stdout


def lane_roots(root, count):
    parent, name = os.path.split(os.path.abspath(root))
    return [os.path.join(parent, f"{name}-mut{k}") for k in range(1, count)]


def lane_ready(root, lane):
    """Bring a lane worktree to this folder's commit and uncommitted changes, made and built the first time."""
    sha = git(root, "rev-parse", "HEAD").decode().strip()
    if not os.path.isdir(lane):
        git(root, "worktree", "add", "--detach", lane, sha)
        deps = os.path.join(root, "build", "_deps")
        fetched = [f"-DFETCHCONTENT_SOURCE_DIR_{d[:-4].upper()}={os.path.join(deps, d)}"
                   for d in (os.listdir(deps) if os.path.isdir(deps) else []) if d.endswith("-src")]
        if subprocess.run([CMAKE, "--preset", "windows", *fetched], cwd=lane, capture_output=True).returncode:
            raise RuntimeError(f"configure failed in {lane}")
    else:
        git(lane, "checkout", "-f", "--detach", sha)
        git(lane, "clean", "-fdq", "--", "src", "tests", "scripts", "tools")
    diff = git(root, "diff", "--binary", "HEAD")
    if diff:
        git(lane, "apply", "--whitespace=nowarn", "-", data=diff)
    for path in git(root, "ls-files", "--others", "--exclude-standard").decode().splitlines():
        os.makedirs(os.path.dirname(os.path.join(lane, path)) or lane, exist_ok=True)
        shutil.copyfile(os.path.join(root, path), os.path.join(lane, path))
    if not build(lane):
        raise RuntimeError(f"build failed in {lane}")


class Lane:
    def __init__(self, root, number):
        self.root, self.number = root, number
        self.rebuilt = False  # its build holds a native entry's change
        self.last = None      # the file it last changed


def check(lane, i, entry, say):
    """Run one check in a lane: True when its test fails with the fix undone."""
    path, old, new, name, test = entry
    full = os.path.join(lane.root, path)
    if not os.path.exists(os.path.join(lane.root, test)):
        # Older entries name the test without its folder and prefix.
        test = os.path.join("tests", f"test_{test}.py")
    if not os.path.exists(os.path.join(lane.root, test)):
        say(f"{i} {name}: no test {test}; fix the entry")
        return False
    with open(full, encoding="utf-8") as f:
        src = f.read()
    if src.count(old) != 1:
        say(f"{i} {name}: its code is found {src.count(old)} times in {path}; fix the entry")
        return False
    backup = os.path.join(tempfile.gettempdir(), f"oanim_mutation_{os.getpid()}_{lane.number}_{os.path.basename(path)}")
    shutil.copyfile(full, backup)
    # On disk before the source changes: a power cut mid-check once left a source file zeroed with no copy to go back to.
    with open(backup, "rb+") as f:
        os.fsync(f.fileno())
    try:
        with open(full, "w", encoding="utf-8", newline="") as f:
            f.write(src.replace(old, new))
        lane.last = path
        native = not path.endswith(".py")
        if not native and lane.rebuilt:
            # The build still holds the last native entry's change: a script-only entry runs on a clean one.
            shutil.copyfile(backup, full)
            if not build(lane.root):
                raise RuntimeError(f"the build in {lane.root} with every file put back failed: look before going on")
            lane.rebuilt = False
            with open(full, "w", encoding="utf-8", newline="") as f:
                f.write(src.replace(old, new))
        if native:
            lane.rebuilt = True
            if not build(lane.root):
                say(f"{i} {name}: BUILD FAILED with it undone; fix the entry")
                return False
        r = subprocess.run([os.path.join(lane.root, EXE), "--background", "--python", test], cwd=lane.root,
                           capture_output=True, text=True)
        why = [l for l in (r.stdout + r.stderr).splitlines() if "Error" in l or "assert" in l][-1:]
        if r.returncode:
            say(f"{i} {name}: caught {why}")
            return True
        say(f"{i} {name}: NOT CAUGHT by {test}")
        return False
    finally:
        shutil.copyfile(backup, full)
        os.remove(backup)


def main():
    root = os.getcwd()
    args = sys.argv[1:]
    if "--remove-lanes" in args:
        parent, name = os.path.split(root)
        for lane in sorted(d for d in os.listdir(parent) if d.startswith(f"{name}-mut")):
            git(root, "worktree", "remove", "--force", os.path.join(parent, lane))
            print(f"removed {lane}", flush=True)
        return
    count = 1
    if "--lanes" in args:
        at = args.index("--lanes")
        count = max(1, int(args[at + 1]))
        del args[at:at + 2]
    only = set(args) - {"--prepare", "--not-here"}
    todo = [i for i in range(len(PENDING)) if not only or str(i) in only]
    # --not-here: only the lane folders, so this one stays free for editing while they run.
    here = "--not-here" not in args
    lanes = [Lane(root, 0)] if here else []
    extra = lane_roots(root, min(count, len(todo) + (not here)))
    if extra:
        print(f"getting {len(extra)} lane folder(s) ready", flush=True)
        errors = []

        def ready(lane):
            try:
                lane_ready(root, lane)
            except RuntimeError as e:
                errors.append(str(e))

        threads = [threading.Thread(target=ready, args=(lane,)) for lane in extra]
        [t.start() for t in threads]
        [t.join() for t in threads]
        if errors:
            print("\n".join(errors), flush=True)
            sys.exit(2)
        lanes += [Lane(lane, k + 1) for k, lane in enumerate(extra)]
    if "--prepare" in args:
        # Lanes made and built ahead of a run (it brings them up to date again).
        return
    lock = threading.Lock()
    tally = {"caught": 0, "missed": 0}
    broken = []

    def say(line):
        with lock:
            print(line, flush=True)

    def next_for(lane):
        with lock:
            if not todo or broken:
                return None
            # The next check on the file this lane last changed: its rebuild then compiles that one file.
            k = next((k for k, i in enumerate(todo) if PENDING[i][0] == lane.last), 0)
            return todo.pop(k)

    def work(lane):
        try:
            while (i := next_for(lane)) is not None:
                ok = check(lane, i, PENDING[i], say)
                with lock:
                    tally["caught" if ok else "missed"] += 1
            if lane.rebuilt and not build(lane.root):
                raise RuntimeError(f"the build in {lane.root} with every file put back failed: look before going on")
        except RuntimeError as e:
            with lock:
                broken.append(str(e))

    threads = [threading.Thread(target=work, args=(lane,)) for lane in lanes]
    [t.start() for t in threads]
    [t.join() for t in threads]
    if broken:
        print("\n".join(broken), flush=True)
        sys.exit(2)
    print(f"{tally['caught']} caught, {tally['missed']} not", flush=True)
    sys.exit(1 if tally["missed"] else 0)


if __name__ == "__main__":
    main()
