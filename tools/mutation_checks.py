"""Mutation checks, batched until a milestone: each proves a test fails without the fix it guards.

Each feature adds an entry to PENDING for every fix it makes: the file, a piece of its code (found exactly once), what
to put there instead to undo the fix, a name, and the test that should then fail. They are run together when the user
says "run milestone checks":

    python tools/mutation_checks.py          # every pending check
    python tools/mutation_checks.py 3 7      # only those

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

CMAKE = (r"C:/Program Files (x86)/Microsoft Visual Studio/18/BuildTools/Common7/IDE/CommonExtensions/Microsoft/"
         r"CMake/CMake/bin/cmake.exe")
EXE = os.path.join("build", "bin", "RelWithDebInfo", "OpenAnimate.exe")

# (file, old, new, name, test)
PENDING = [
    ("src/anim/animation.cc", "if (!before.empty() && ch.synced) {",
     "if (!before.empty()) {", "eraser cut ends say nothing about deleted keys", "tests/test_key_whole_drawing.py"),
    ("src/anim/animation.cc", "drawing_key_whole(layer, !layer.key_whole_settled, action_key);",
     "drawing_key_whole(layer, !layer.key_whole_settled, doc.action);", "same-named actions told apart",
     "tests/test_key_whole_drawing.py"),
    ("src/anim/animation.cc", "      together_on.push_back(group);\n",
     "      key_together_sync(doc, *group, !group->key_together_settled);\n      group->key_together_settled = true;\n",
     "groups turned off let go before others sync", "tests/test_key_together.py"),
    ("src/anim/animation.cc",
     "      std::erase_if(keys, [&](const auto &k) { return k.filler && !own_away.count({unit, it->first, k.frame}); });\n",
     "", "fillers in actions let go", "tests/test_key_together.py"),
    ("src/ops/anim_ops.cc", "  KeySettings s = r_settings;", "  KeySettings &s = r_settings;",
     "a refused argument changes no key", "tests/test_motion_path_key_menu.py"),
    ("src/anim/motion_path.cc", "          k->speeds = speeds;", "          (void)speeds;",
     "whole drawing slide: every point alike", "tests/test_key_whole_drawing.py"),
    ("src/anim/motion_path.cc", "  if (!target.bone && !target.point) {\n    /* A layer's path:",
     "  if (false) {\n    /* A layer's path:", "a layer's diamonds only its path keys",
     "tests/test_motion_path_key_menu.py"),
    ("src/anim/animation.cc", "      k.filler_inside = fillers && was_animated && frame < own_last;",
     "      k.filler_inside = false;", "a filler inside motion shapes nothing", "tests/test_key_together.py"),
    ("src/anim/channel.cc", "  return k.filler && k.frame != 0 && (k.filler_inside || !own_after);",
     "  return k.filler && k.frame != 0 && k.filler_inside;", "a filler after the last key shapes nothing",
     "tests/test_key_together.py"),
    ("src/anim/channel.cc", "  return k.filler && k.frame != 0 && (k.filler_inside || !own_after);",
     "  return k.filler && k.frame != 0;", "a filler before a new key still holds", "tests/test_key_together.py"),
    ('src/anim/channel.cc', '  if ((a.type == KeyType::Breakdown || b.type == KeyType::Breakdown) && b.interp != Interp::Custom) {',
     '  if (false) {', 'a span across breakdowns eased as one', "tests/test_breakdown_keys.py"),
    ('src/anim/motion_path.cc', '        key.frame = sp.start + to;',
     '        key.frame = b < count ? original[sp.m0 + 1 + b].frame : k.frame;', "timing tools move the breakdown to its pose's frame", "tests/test_breakdown_keys.py"),
    ('src/anim/motion_path.cc', '        st.breakdowns = flat.breakdown_frames(*sp);',
     '        (void)sp;', "the timing chart marks a span's breakdowns", "tests/test_breakdown_keys.py"),
    ('src/anim/animation.cc', '              k.frame = std::clamp(k.frame, lo, hi);',
     '              (void)hi;', 'a dragged breakdown stops short of its neighbours', "tests/test_breakdown_keys.py"),
    ('src/ops/motion_path_ops.cc', '      motion_path_key_type_set(doc, targets.front(), frame, type);',
     '      (void)type;', 'Alt click makes a breakdown', "tests/test_breakdown_keys.py"),
    ('src/ops/motion_path_ops.cc', '      motion_path_key_type_set(doc, targets.front(), doc.frame_current, KeyType::Breakdown);\n',
     '', 'Alt Shift makes a breakdown', "tests/test_breakdown_keys.py"),
    ('src/ops/anim_ops.cc', '                                                                      s.type = type;\n',
     '', 'Paste Interpolation keeps the key type', "tests/test_breakdown_keys.py"),
    ('src/ops/anim_ops.cc', '                      new_key_settings().settings.type = type;\n',
     '', 'Use for New Keys keeps the new-key type', "tests/test_breakdown_keys.py"),
    ('src/anim/channel.cc', '  key.type = frame == 0 ? KeyType::Key : defaults.settings.type;',
     '  key.type = defaults.settings.type;', 'the setup frame is never a breakdown', "tests/test_breakdown_keys.py"),
    ('src/io/oanim_file.cc', '      key["type"] = key_type_identifier(k.type);',
     '      (void)k.type;', 'breakdowns saved', "tests/test_breakdown_keys.py"),
    ('src/anim/animation.cc', '    if (!before.empty() && (fillers ? ch.together_synced : ch.synced)) {',
     '    if (!before.empty() && ch.synced) {', 'a layer new to a group deletes none of its keys', "tests/test_key_together.py"),
    ('src/anim/animation.cc', '    const auto [removed, kept] = key_together_leavers_release(doc, *group);',
     '    const auto [removed, kept] = std::pair<int, int>{0, 0};', 'a layer moved out lets go of its fillers', "tests/test_key_together.py"),
    ('src/anim/animation.cc', '  visit([&](auto &ch, auto & /*value*/, const void * /*unit*/) { ch.together_synced = false; });\n',
     '', 'a layer moved between groups is new to the second', "tests/test_key_together.py"),
    ('src/anim/animation.cc', '      if (group->key_together) {\n        /* On inside one turned on: handed over.',
     '      if (false) {\n        /* On inside one turned on: handed over.', 'an inner group handed over keeps its fillers',
     "tests/test_key_together.py"),
    ('src/anim/animation.cc', '          ch.together_synced = false;\n        });\n        continue;',
     '        });\n        continue;', 'a group handed over is new to the outer one', "tests/test_key_together.py"),
    ('src/io/oanim_file.cc', '  layer.key_whole_settled = layer.key_whole_drawing;\n', '',
     "opening keeps a whole drawing's timing", "tests/test_key_whole_drawing.py"),
    ('src/anim/animation.cc', '  return ch.keys.size() >= 2 && ch.key_at(st.lo) && ch.key_at(st.hi);',
     '  return ch.keys.size() >= 2 && ch.key_at(st.lo);', 'Time Together links only channels keyed on both ends', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '      if (b.selected) {\n        any_add(out, b.position_keys);',
     '      if (true) {\n        any_add(out, b.position_keys);', 'Time Together links only selected bones', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '  layers_walk(doc.layers, [&](Layer &l) {\n    if (!l.selected) {\n      return;\n    }\n    any_add(out, l.position_keys);',
     '  layers_walk(doc.layers, [&](Layer &l) {\n    any_add(out, l.position_keys);', 'Time Together links only selected layers', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '        return b == a ? float(nb) : float(na) + float(nb - na) * float(f - a) / float(b - a);',
     '        return float(f);', 'linked keys go through the same stretch', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '    if (mapped[i] && to[i] <= to[i - 1]) {\n      to[i] = to[i - 1] + 1;\n    }',
     '    (void)mapped;', 'linked keys kept a frame apart', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '            if (!key.selected && key.frame > last) {\n              key.frame += shift;',
     '            if (false) {\n              key.frame += shift;', 'Ripple: the keys after go along', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '  if (together && doc.time_together) {\n    sts = selected_stretches(layers, to, ripple);',
     '  if (together) {\n    sts = selected_stretches(layers, to, ripple);', 'Time Together off: just the one', "tests/test_time_together.py"),
    ('src/io/oanim_file.cc', '           {"time_together", doc.time_together},\n',
     '', 'Time Together saved', "tests/test_time_together.py"),
    ('src/ops/operator.cc', '      keys_moved_report(time_together_timing_follow(*C.document, timing_before));',
     '      (void)timing_before;', 'timing edits reach the linked channels', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '[&](const auto &o) { return o.frame > lo && o.frame < hi && o.frame != e.frame; }))',
     '[&](const auto &o) { return o.frame < 0; }))', 'timing goes only to the same stretches', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '          hi = std::min(hi, keys[i + 1].frame - 1 - keys[i].frame);',
     '          (void)hi;', 'a slid stretch stops a frame short', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '        if (b - a + amount < 1) {\n          return;',
     '        if (false) {\n          return;', '- never takes a stretch below one frame', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '  if (together && doc.time_together && !sts.empty()) {\n    for (const AnyChannel &c : together_candidates(doc)) {\n      if (done.count(c.id())) {',
     '  if (together && doc.time_together && sts.empty()) {\n    for (const AnyChannel &c : together_candidates(doc)) {\n      if (done.count(c.id())) {', '+ and - reach the linked channels', "tests/test_time_together.py"),
    ('src/anim/animation.cc', '      if (ch.keys.size() == to.size()) {',
     '      if (false) {', 'Match Timing keeps every key when the counts match', "tests/test_time_together.py"),
    ('src/editors/timeline.cc', '        own_rows(&l, depth + 1);\n        self(self, l.children, depth + 1);',
     '        own_rows(&l, depth + 1);\n        self(self, l.children, depth);', 'layers nest under their group', 'tests/test_timeline_tree.py'),
    ('src/editors/timeline.cc', '  if (keyed(all, KeyFilter{}) || !pinned.empty()) {',
     '  if (false) {', 'the Summary row comes first', 'tests/test_timeline_tree.py'),
    ('src/editors/timeline.cc', 'if (add("Points", {l}, points, depth, "pts:" + id, true, false, 0) && keyed({l}, sel)) {',
     'if (add("Points", {l}, points, depth, "pts:" + id, true, false, 0) && false) {', "the selected points' keys under Points", 'tests/test_timeline_selected_points.py'),
    ('src/editors/timeline.cc', 'depth, "layer:" + std::to_string(l.id), picked(l), false, l.id)',
     'depth, "layer:" + std::to_string(l.id), false, false, l.id)', 'the picked layers start open', 'tests/test_timeline_tree.py'),
    ('src/editors/timeline.cc', 'add(channel_label(*l, ref.path, owner || other), {l}, f, depth',
     'add(channel_label(*l, ref.path, owner), {l}, f, depth', "a linked knob's row named with its bone", 'tests/test_controller.py'),
    ('src/editors/timeline.cc', 'if (!under.empty() && keyed(under, KeyFilter{})) {',
     'if (false) {', 'the groups above a picked layer, dimmed', 'tests/test_timeline_only_selected.py'),
    ('src/editors/timeline.cc', 'const bool only_picked = doc.timeline_only_selected && any_picked;',
     'const bool only_picked = false;', 'only the picked bones with Only Selected', 'tests/test_timeline_only_selected.py'),
    ('src/ops/anim_ops.cc', 'if (C.document && !C.document->timeline_only_selected && !C.document->timeline_lock) {',
     'if (false) {', 'Only Selected off shows every animated layer', 'tests/test_timeline_only_selected.py'),
    ('src/ops/anim_ops.cc', '  if (C.document && C.document->timeline_lock) {\n    /* Locked:',
     '  if (false) {\n    /* Locked:', 'Lock keeps the layers shown', 'tests/test_timeline_only_selected.py'),
    ('src/io/oanim_file.cc', '  doc.timeline_lock = get_bool(j, "timeline_lock", false);',
     '  doc.timeline_lock = false;', 'Lock is saved', 'tests/test_timeline_only_selected.py'),
    ('src/editors/timeline.cc', '    args["extend"] = "1";\n    args["deselect"] = "1";',
     '    args["extend"] = "1";\n    args["deselect"] = "0";', 'Ctrl-click takes a key out', 'tests/test_timeline_mouse.py'),
    ('src/editors/timeline.cc', '  if (double_click && !ctrl && !shift) {',
     '  if (false) {', "a double-click picks the row's every key", 'tests/test_timeline_mouse.py'),
    ('src/editors/timeline.cc', '  if (keys > 0 && selected == keys) {\n    *r_reselect = true;',
     '  if (false) {\n    *r_reselect = true;', 'pressing a selected key keeps the selection for a drag', 'tests/test_timeline_mouse.py'),
    ('src/editors/timeline.cc', '  return num.any() ? int(std::lround(num.value(0, float(mouse_offset)))) : mouse_offset;',
     '  return mouse_offset;', 'frames typed set the move', 'tests/test_timeline_mouse.py'),
    ('src/editors/timeline.cc', '      keep[i] = (kinds & kind) && (doc.timeline_show',
     '      keep[i] = true && (doc.timeline_show', 'the kinds filter', 'tests/test_timeline_filters.py'),
    ('src/editors/timeline.cc', '(doc.timeline_show != TIMELINE_SHOW_CHANGED || row_channel_changes(row))',
     'true', 'Changed leaves out keys holding one value', 'tests/test_timeline_filters.py'),
    ('src/editors/timeline.cc', 'stack.push_back({full[i].depth, found_above[i] || (folder && !search.empty() && fuzzy_match(full[i].name, search))});',
     'stack.push_back({full[i].depth, false});', "a folder's name finds what it holds", 'tests/test_timeline_filters.py'),
    ('src/editors/timeline.cc', '    if (at == name.size()) {\n      return false;',
     '    if (false) {\n      return false;', 'the search needs its letters in order', 'tests/test_timeline_filters.py'),
    ('src/editors/timeline.cc', 'if ((!animated && !show_all) ||',
     'if ((!animated) ||', 'All lists every channel', 'tests/test_timeline_filters.py'),
    ('src/editors/timeline.cc', 'row.open = open_all || !search.empty() || row_open(',
     'row.open = open_all || row_open(', 'searching opens folded rows', 'tests/test_timeline_filters.py'),
    ('src/io/oanim_file.cc', '  doc.timeline_search = get_string(j, "timeline_search", "");',
     '  doc.timeline_search.clear();', 'the filters are saved', 'tests/test_timeline_filters.py'),
    ('src/anim/animation.cc', '      s.fillers += key.filler ? 1 : 0;',
     '      s.fillers += 0;', 'filler keys counted for their grey hollow look', 'tests/test_timeline_key_fills.py'),
    ('src/editors/timeline.cc', '    start = std::min(start + frames, end - MIN_SPAN);',
     '    start = start + frames;', 'the scroll bar never zooms closer than 10 frames', 'tests/test_timeline_navigation.py'),
    ('src/editors/timeline.cc', '    const int by = std::max(frames, -from_start);',
     '    const int by = frames;', 'the range never moves before frame 0', 'tests/test_timeline_navigation.py'),
    ('src/editors/timeline.cc', '    start = std::clamp(from_start + frames, 0, std::max(0, from_end - 1));',
     '    start = from_start + frames;', 'Start stays before the End', 'tests/test_timeline_navigation.py'),
    ('src/editors/timeline.cc', 'add("Drawing", {l}, points, depth, "", false, keyed({l}, sel), 0);',
     'add("Drawing", {l}, points, depth, "", false, false, 0);', 'the Drawing row red with points picked',
     'tests/test_timeline_selected_points.py'),
    ('src/editors/timeline.cc', '  return holds_pick ? !g_pick_folded.contains(key) : row_open(area, key, fallback);',
     '  return row_open(area, key, fallback);', 'rows holding the pick open', 'tests/test_timeline_pick_open.py'),
    ('src/editors/timeline.cc', 'picked(l), picks_in(l), l.id,', 'picked(l), false, l.id,',
     'the layer holding the pick is red', 'tests/test_timeline_pick_open.py'),
    ('src/editors/timeline.cc', '      const bool pick = picks_under(picks_under, l);', '      const bool pick = picks_in(l);',
     'the groups holding the pick open', 'tests/test_timeline_pick_open.py'),
    ('src/editors/timeline.cc', '"bones:" + id, any_picked, false, 0, any_picked)', '"bones:" + id, any_picked, false, 0, false)',
     'the Bones folder opens for a picked bone', 'tests/test_timeline_pick_open.py'),
    ('src/editors/timeline.cc', '      g_pick_folded.insert(key);', '      (void)key;', 'folded while picked, it stays folded', 'tests/test_timeline_pick_open.py'),
    ('src/editors/timeline.cc', 'return r.holds_pick && r.fold == key; });', 'return key.empty(); });',
     'folded only while that pick lasts', 'tests/test_timeline_pick_open.py'),
    ('src/ops/anim_ops.cc', '    const bool picked = flat->selected || (flat->id == doc.active_layer && doc.active_unpicked != flat->id);',
     '    const bool picked = flat->selected || flat->id == doc.active_layer;', 'the unpicked active layer shows nothing', 'tests/test_timeline_nothing_selected.py'),
    ('src/ops/layer_ops.cc', '                   doc.active_unpicked = doc.active_layer;', '                   doc.active_unpicked = 0;',
     'Deselect All Layers unpicks the active layer', 'tests/test_timeline_nothing_selected.py'),
    ('src/ops/anim_ops.cc', '[](const MeshPoint &p) { return p.selected; })) ||', '[](const MeshPoint &p) { return p.selected && false; })) ||',
     'picked points show their layer', 'tests/test_timeline_nothing_selected.py'),
    ('src/ops/anim_ops.cc', '    const bool edits = edited->contains(flat->id) || flat->id == doc.active_layer;',
     '    const bool edits = edited->contains(flat->id);', 'picked bones show their layer', 'tests/test_timeline_nothing_selected.py'),
    ('src/ops/anim_ops.cc', 'picked.empty() &&', 'false &&',
     'nothing picked: no Camera or Simulation rows', 'tests/test_timeline_nothing_selected.py'),
    ('src/python/py_runtime.cc', '    current_context().document->active_unpicked = 0;', '',
     'made active is picked again', 'tests/test_timeline_nothing_selected.py'),
    ('src/anim/animation.cc', 'if (filter.picked_bones_only && ref.path.rfind("bones[\\"", 0) == 0 && !channel_thing_selected(layer, ref.path, filter)) {',
     'if (false) {', "an unpicked bone's keys aren't the layer row's", 'tests/test_timeline_picked_layer.py'),
    ('src/editors/timeline.cc', '  layer_keys.picked_bones_only = sel_only;',
     '  layer_keys.picked_bones_only = false;', "the layer row shows its own keys and its picked bones'", 'tests/test_timeline_picked_layer.py'),
    ('src/editors/timeline.cc', '!keyed(under, layer_keys) && !named(l)) {',
     '!keyed(under, layer_keys)) {', "a picked layer's name row with no keys", 'tests/test_timeline_picked_layer.py'),
    ('src/editors/timeline.cc', 'if (!(sel_only && !any_picked) && ((show_all',
     'if (((show_all', 'no Bones folder with no bone picked', 'tests/test_timeline_picked_layer.py'),
    ('src/ops/anim_ops.cc', '(!sel_only && l->is_bone_layer())',
     'l->is_bone_layer()', "a bone layer's layers only when picked", 'tests/test_timeline_picked_layer.py'),
    ('src/editors/timeline.cc', '          {"picked_bones", filter.picked_bones_only ? "1" : "0"},\n',
     '', "the row's keys picked as it shows them", 'tests/test_timeline_picked_layer.py'),
    ('src/data/document.hh', 'point == o.point && head == o.head && drawing', 'point == o.point && drawing',
     "a bone's head and tip are two paths", 'tests/test_motion_paths.py'),
    ('src/anim/motion_path.cc', '          if (!b.connected) {', '          if (true) {',
     "no head path for a connected bone", 'tests/test_motion_paths.py'),
    ('src/anim/motion_path.cc', 'return !b.angle_keys.keys.empty() || b.connected || b.position_keys.keys.empty();',
     'return true;', "a tip that only moves goes by its position keys", 'tests/test_motion_paths.py'),
    ('src/anim/motion_path.cc', '  t.head = target.head;', '',
     "a head path's style is its own", 'tests/test_motion_paths.py'),
    ('src/ops/motion_path_ops.cc', '    t.head = op_arg_bool(C, "head");', '',
     "head=1 names a bone's head path", 'tests/test_motion_path_tool.py'),
    ('src/editors/viewport.cc', ' || ts.tool == Tool::MotionPath) &&', ') &&',
     "Motion Path stays on through a layer change", 'tests/test_motion_path_tool.py'),
    ('src/ops/motion_path_ops.cc', 'const int frame = std::max(0, op_arg_int(C, "frame", doc.frame_current));',
     'const int frame = std::max(1, op_arg_int(C, "frame", doc.frame_current));', "frame 0's key can be picked",
     'tests/test_motion_path_setup_handle.py'),
    ('src/anim/animation.cc', '          ch.action_setup[from] = k;', '',
     "an action's frame 0 handle kept as it is put away", 'tests/test_motion_path_setup_handle.py'),
    ('src/anim/animation.cc', '        k.path_handles = it->second.path_handles;', '',
     "an action's frame 0 handle brought back", 'tests/test_motion_path_setup_handle.py'),
    ('src/anim/animation.cc', '      ch.action_setup[to] = std::move(key);', '',
     "a renamed action keeps its frame 0 handle", 'tests/test_motion_path_setup_handle.py'),
    ('src/io/oanim_file.cc', '    j[std::string(name) + "_action_setup"] = std::move(setup);', '',
     "an action's frame 0 handle saved", 'tests/test_motion_path_setup_handle.py'),
    ('src/anim/motion_path.cc', 'const int start = doc.frame_start <= 1 ? 0 : doc.frame_start,',
     'const int start = std::max(1, doc.frame_start),', "paths start at frame 0, the setup", 'tests/test_motion_paths.py'),
    ('src/anim/motion_path.cc', '    add(b.scale_keys);',
     '    add(b.scale_keys); for (const int f : layer_key_frames(*layer)) { if (f > 0) { keys.insert(f); } }', "a bone's row: not its parent's or other bones' keys", 'tests/test_motion_path_own_keys.py'),
    ('src/anim/motion_path.cc', '      add(layer->drawing.points[size_t(i)].pos_keys);',
     '      add(layer->drawing.points[size_t(i)].pos_keys); for (const Layer *l = layer_parent(const_cast<Document &>(doc), layer->id); l; l = layer_parent(const_cast<Document &>(doc), l->id)) { for (const int f : layer_key_frames(*l)) { if (f > 0) { keys.insert(f); } } }', "a point's row: not the keys of the bones and layers above", 'tests/test_motion_path_own_keys.py'),
    ('src/anim/motion_path.cc', '    for (const ChannelRef &ref : layer_filtered_channels(*const_cast<Layer *>(layer), filter)) {',
     '    for (const Layer *l = layer_parent(const_cast<Document &>(doc), layer->id); l; l = layer_parent(const_cast<Document &>(doc), l->id)) { for (const int f : layer_key_frames(*l)) { if (f > 0) { keys.insert(f); } } } for (const ChannelRef &ref : layer_filtered_channels(*const_cast<Layer *>(layer), filter)) {', "a layer's row: not its group's keys", 'tests/test_motion_path_own_keys.py'),
    ('src/anim/motion_path.cc', '        add(knob->position_keys);',
     '', "a bone a controller plays keeps the knob's keys", 'tests/test_motion_path_own_keys.py'),
    ('src/anim/motion_path.cc', '  std::erase_if(out, [&](const MotionPathTarget &t) { return motion_path_keys(doc, t).empty(); });',
     '', 'no timing row with nothing of its own', 'tests/test_motion_path_own_keys.py'),
    ('src/anim/motion_path.cc', '  if (!doc.path_own_motion || target.drawing) {',
     '  if (true) {', 'a picked path shows its own motion', 'tests/test_motion_path_own_motion.py'),
    ('src/anim/motion_path.cc', '        p.points.push_back(own);',
     '        p.points.push_back(at);', 'the held path, not the real one', 'tests/test_motion_path_own_motion.py'),
    ('src/anim/motion_path.cc', '  if (g_shown && motion_path_own_motion(doc, target)) {',
     '  if (false) {', 'a dot dragged lands where the held path is drawn', 'tests/test_motion_path_own_motion.py'),
    ('src/anim/motion_path.cc', '  if (!motion_path_own_motion(copy, t)) {',
     '  if (true) {', 'handles worked out on the held path', 'tests/test_motion_path_own_motion.py'),
    ('src/anim/motion_path.cc', '  if (std::any_of(g_dots_picked.begin(), g_dots_picked.end(), [&](const MotionPathTarget &t) { return t.same(target); }))',
     '  if (false)', 'a picked dot edits its path too', 'tests/test_motion_path_own_motion.py'),
    ('src/ops/motion_path_ops.cc', '      PathDotsSync sync{C};',
     '', 'picking dots tells Own Motion', 'tests/test_motion_path_own_motion.py'),
    ('src/io/oanim_file.cc', '           {"path_own_motion", doc.path_own_motion},',
     '', 'Own Motion saved', 'tests/test_motion_path_own_motion.py'),
    ('src/anim/motion_path.cc', '    g_shown = held.get();',
     '', "on another frame, the frame shown is what's held", 'tests/test_motion_path_own_motion.py'),
    ('src/anim/motion_path.cc', '      p.ghost.clear();',
     '', 'no ghost when nothing else moves it', 'tests/test_motion_path_own_motion.py'),
    ('src/editors/overlay_contrast.cc', '  if (c >= (previous == ContrastPick::Base ? ENTER : LEAVE)) {',
     '  if (c >= ENTER) {', 'an alternative held until clearly readable', 'tests/test_overlay_colors.py'),
    ('src/editors/overlay_contrast.cc', '  return under_luma <= base_luma ? ContrastPick::Lighter : ContrastPick::Darker;',
     '  return ContrastPick::Lighter;', 'darker over something lighter', 'tests/test_overlay_colors.py'),
    ('src/editors/overlay_contrast.cc', '  const int x = int(std::floor(px.x)), y = int(std::floor(px.y));',
     '  const int x = 0, y = int(std::floor(px.y));', 'each piece by what is under it', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '      else if (!selected && (under || fades)) {',
     '      else if (false) {', 'wires pick by what is under them', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '                         : under_at(p.pos, c) ? contrast_u32(wire.of(contrast_pick(c, wire.base)), 1.0f)',
     '                         : false ? contrast_u32(wire.of(contrast_pick(c, wire.base)), 1.0f)', 'points pick by what is under them', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '      const float thickness = selected ? 1.6f : 1.5f;',
     '      const float thickness = selected ? 1.6f : 1.0f;', 'wires 1.5 px', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '  return p.color == path_colors().base ? path_colors() : contrast_colors_of(p.color);',
     '  return path_colors();', "a path's own colour keeps itself", 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '      contrast_pieces(run, piece, under_at, colors.base, pieces);',
     '      contrast_pieces(run, piece, [](float2, Color &) { return false; }, colors.base, pieces);', 'path pieces pick by what is under them', 'tests/test_overlay_colors.py'),
    ('src/anim/motion_path.cc', '  if (index % 6 == 0) {',
     '  if (false) {', "the first path the Preferences' colour", 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '      const bool fades = picked_a != picked_b;',
     '      const bool fades = false;', 'a half-picked segment fades', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '          const float f0 = picked_a ? t0 : 1.0f - t0, f1 = picked_a ? t1 : 1.0f - t1;',
     '          const float f0 = picked_b ? t0 : 1.0f - t0, f1 = picked_b ? t1 : 1.0f - t1;', 'orange at the picked end', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '        g.marks.push_back({h, tip, 0u, handle_size * 0.5f, 1.0f});',
     '        g.marks.push_back({h, tip, IM_COL32(0, 0, 0, 220), handle_size * 0.5f, 1.0f});', 'handle tips not outlined', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '        const bool at_point = point_selected, at_tip = point_selected || tip_picked;',
     '        const bool at_point = false, at_tip = point_selected || tip_picked;', 'a picked point lights its handles all along', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '        const bool at_point = point_selected, at_tip = point_selected || tip_picked;',
     '        const bool at_point = point_selected, at_tip = tip_picked;', "a picked point lights its handles' tips", 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '        const ContrastColors &tip_colors = at_tip ? handle_tip_picked : handle_own;',
     '        const ContrastColors &tip_colors = at_tip ? handle_picked : handle_own;', 'picked handle tips orange', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '    out.push_back({from, to, u32_mix(own, picked, (from + to) * 0.5f)});',
     '    out.push_back({from, to, u32_mix(own, picked, 0.0f)});', 'a dragged path handle fades to orange', 'tests/test_overlay_colors.py'),
    ('src/editors/overlay_contrast.cc', '    mix(w);',
     '', 'the canvas fingerprint sees every pixel', 'tests/test_overlay_colors.py'),
    ('src/editors/overlay_contrast.cc', '                        document_to_pixels.tx, document_to_pixels.ty, background.r, background.g, background.b})',
     '                        document_to_pixels.tx, document_to_pixels.ty})', 'the canvas fingerprint sees the background', 'tests/test_overlay_colors.py'),
    ('src/ops/vector_ops.cc', '        C.document->motion_paths = true;',
     '', 'choosing the Motion Path tool shows paths', 'tests/test_motion_path_own_motion.py'),
    ('src/editors/viewport.cc', '      g.marks.push_back({p.pos, fill, p.selected ? 0u : IM_COL32(0x10, 0x10, 0x10, 170), r, 1.0f});',
     '      g.marks.push_back({p.pos, fill, p.selected ? 0u : IM_COL32(0x10, 0x10, 0x10, 170), r, 0.0f});', 'points round', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '      const ImU32 fill = contrast_u32(colors.of(pick), 1.0f);',
     '      const ImU32 fill = key ? IM_COL32(240, 240, 240, 255) : contrast_u32(colors.of(pick), 1.0f);', "outside the tool, keys the path's colour", 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '      const ContrastPick pick = under && under->at(p.points[i], c) ? contrast_pick(c, colors.base)',
     '      const ContrastPick pick = false ? contrast_pick(c, colors.base)', 'quiet dots and keys readable on anything', 'tests/test_overlay_colors.py'),
    ('src/anim/motion_path.cc', '  if (!doc.path_own_motion || target.drawing || !g_path_tool) {',
     '  if (!doc.path_own_motion || target.drawing) {', 'Own Motion only in the tool', 'tests/test_motion_path_own_motion.py'),
    ('src/data/vector.cc', '        if (point) {',
     '        if (false) {', 'a picked point turns both handles', 'tests/test_handles.py'),
    ('src/data/vector.cc', '  if (length(back) < 1e-4f) {',
     '  if (false) {', 'folded handles line up along the curve', 'tests/test_handles.py'),
    ('src/editors/viewport.cc', '            pick_point = contrast_pick(seen, near_point.base, pick_point);',
     '', 'handle lines pick by what is under them', 'tests/test_overlay_colors.py'),
    ('src/editors/viewport.cc', '            under_at(h, seen) ? tip_colors.of(contrast_pick(seen, tip_colors.base)) : tip_colors.base, 1.0f);',
     '            tip_colors.base, 1.0f);', 'handle tips pick by what is under them', 'tests/test_overlay_colors.py'),
]


def build():
    return subprocess.run([CMAKE, "--build", "--preset", "release"], capture_output=True, text=True).returncode == 0


def main():
    only = set(sys.argv[1:])
    rebuilt = False
    caught = missed = 0
    for i, (path, old, new, name, test) in enumerate(PENDING):
        if only and str(i) not in only:
            continue
        with open(path, encoding="utf-8") as f:
            src = f.read()
        if src.count(old) != 1:
            print(f"{i} {name}: its code is found {src.count(old)} times in {path}; fix the entry", flush=True)
            missed += 1
            continue
        backup = os.path.join(tempfile.gettempdir(), f"oanim_mutation_{os.getpid()}_{os.path.basename(path)}")
        shutil.copyfile(path, backup)
        try:
            with open(path, "w", encoding="utf-8", newline="") as f:
                f.write(src.replace(old, new))
            native = not path.endswith(".py")
            if native:
                rebuilt = True
                if not build():
                    print(f"{i} {name}: BUILD FAILED with it undone; fix the entry", flush=True)
                    missed += 1
                    continue
            r = subprocess.run([EXE, "--background", "--python", test], capture_output=True, text=True)
            why = [l for l in (r.stdout + r.stderr).splitlines() if "Error" in l or "assert" in l][-1:]
            if r.returncode:
                caught += 1
                print(f"{i} {name}: caught {why}", flush=True)
            else:
                missed += 1
                print(f"{i} {name}: NOT CAUGHT by {test}", flush=True)
        finally:
            shutil.copyfile(backup, path)
            os.remove(backup)
    if rebuilt and not build():
        print("The build with every file put back failed: look before going on", flush=True)
        sys.exit(2)
    print(f"{caught} caught, {missed} not", flush=True)
    sys.exit(1 if missed else 0)


if __name__ == "__main__":
    main()
