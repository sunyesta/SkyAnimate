#include "anim/keep_poses.hh"

#include <algorithm>
#include <cmath>
#include <set>
#include <string>

#include "anim/animation.hh"
#include "anim/controller.hh"
#include "anim/deform.hh"
#include "data/bones.hh"
#include "data/document.hh"

#include <set>

namespace oanim {

SetupSnapshot setup_snapshot(const Document &doc, const std::vector<uint64_t> &layers)
{
  SetupSnapshot snap;
  for (const uint64_t id : layers) {
    const Layer *layer = layer_find(doc, id);
    if (!layer || layer->is_reference()) {
      continue;
    }
    SetupSnapshot::LayerSetup s;
    s.layer = id;
    for (const MeshPoint &p : layer->drawing.points) {
      /* Detail points' keys hold offsets along the level below, not positions: they stay as they are. */
      if (p.detail.level == 0) {
        s.points[p.id] = p.setup_pos;
      }
    }
    for (const Curve &curve : layer->drawing.curves) {
      std::vector<std::pair<float2, float2>> handles;
      for (const CurvePoint &cp : curve.points) {
        handles.emplace_back(handle_setup(cp, true), handle_setup(cp, false));
      }
      s.handles.push_back(std::move(handles));
    }
    for (const Bone &b : layer->bones) {
      s.bones[b.id] = {b.setup_position, b.setup_angle, b.length};
    }
    snap.layers.push_back(std::move(s));
  }
  return snap;
}

/* `f(key value)` for every key on another frame than the setup's, in the mainline and every action
 * but the action bone ones (`action_bone_actions`): an action bone action's keys are added on top of the pose as its
 * bone turns,
 * an offset that stays the same whatever the setup is, so taking the move off them as well would take
 * it off twice where it plays. `keep_action_bones`: the action bone action whose own keys these are taken in too (a
 * action bone's angle in its action, which picks the frame from its turn). */
template<typename T, typename F>
static int each_posed_key(Channel<T> &ch, const std::set<std::string> &action_bone_actions, F &&f,
                          const std::string &keep_action_bones = {})
{
  int n = 0;
  const auto run = [&](std::vector<Key<T>> &keys) {
    for (Key<T> &k : keys) {
      if (k.frame != 0) {
        f(k.value);
        n++;
      }
    }
  };
  run(ch.keys);
  for (auto &[name, keys] : ch.actions) {
    if (!action_bone_actions.count(name) || name == keep_action_bones) {
      run(keys);
    }
  }
  return n;
}

int keep_poses(Document &doc, const SetupSnapshot &before)
{
  int changed = 0;
  std::set<std::string> action_bone_actions;
  for (const Layer *l : document_layers_flat(doc)) {
    for (const std::string &name : l->actions) {
      if (l->is_bone_layer() && action_adds(*l, name)) {
        action_bone_actions.insert(name);
      }
    }
  }
  for (const SetupSnapshot::LayerSetup &s : before.layers) {
    Layer *layer = layer_find(doc, s.layer);
    if (!layer) {
      continue;
    }
    /* Points: a key holds the change from the setup position, so the setup's move comes off it. */
    for (MeshPoint &p : layer->drawing.points) {
      const auto it = s.points.find(p.id);
      if (it == s.points.end()) {
        continue;
      }
      const float2 moved = p.setup_pos - it->second;
      if (length(moved) > 1e-9f) {
        changed += each_posed_key(p.pos_keys, action_bone_actions, [&](float2 &v) { v -= moved; });
      }
    }
    /* Handles: a key holds the turn and the length added to the setup handle; the handle it made is
     * worked out against the new setup handle. */
    VectorDrawing &d = layer->drawing;
    for (size_t c = 0; c < d.curves.size() && c < s.handles.size(); c++) {
      Curve &curve = d.curves[c];
      for (size_t k = 0; k < curve.points.size() && k < s.handles[c].size(); k++) {
        CurvePoint &cp = curve.points[k];
        for (const bool in : {true, false}) {
          const float2 was = in ? s.handles[c][k].first : s.handles[c][k].second;
          const float2 now = handle_setup(cp, in);
          if (length(now - was) <= 1e-9f) {
            continue;
          }
          changed += each_posed_key(in ? cp.handle_in_keys : cp.handle_out_keys, action_bone_actions, [&](float2 &v) {
            v = handle_change(now, handle_from_change(was, v), v.x);
          });
        }
      }
    }
    /* Bones: a key holds the position along the setup's axes and the angle added to the setup's. */
    for (Bone &b : layer->bones) {
      const auto it = s.bones.find(b.id);
      /* A controller's keys are its knob's frames in its box, wherever the box is put. */
      if (it == s.bones.end() || bone_is_controller(b)) {
        continue;
      }
      const SetupSnapshot::BoneSetup &was = it->second;
      const bool moved = length(b.setup_position - was.position) > 1e-9f || std::fabs(b.setup_angle - was.angle) > 1e-9f;
      /* An action bone's angle keys in its own action say which turn plays which frame: they keep
       * the turn's meaning as its mainline keys do. */
      std::string own_action_bones;
      for (const std::string &name : action_bone_actions) {
        if (action_bone_of(*layer, name) == &b) {
          own_action_bones = name;
        }
      }
      if (moved) {
        changed += each_posed_key(b.position_keys, action_bone_actions, [&](float2 &t) {
          const float2 where = was.position + rotate(t, was.angle);
          t = rotate(where - b.setup_position, -b.setup_angle);
        });
        changed += each_posed_key(b.angle_keys, action_bone_actions, [&](float &u) { u += was.angle - b.setup_angle; },
                                  own_action_bones);
      }
      /* Lengthened on the setup frame: its keyed scale along it shrinks to keep its posed length. */
      if (std::fabs(b.length - was.length) > 1e-9f && b.length > 1e-9f) {
        const float k = was.length / b.length;
        changed += each_posed_key(b.scale_keys, action_bone_actions, [&](float2 &v) { v.y *= k; });
      }
    }
  }
  return changed;
}

static bool same_skin(const Transform2D &a, const Transform2D &b)
{
  const float eps = 1e-6f;
  return std::fabs(a.a - b.a) < eps && std::fabs(a.b - b.b) < eps && std::fabs(a.c - b.c) < eps &&
         std::fabs(a.d - b.d) < eps && std::fabs(a.tx - b.tx) < eps && std::fabs(a.ty - b.ty) < eps;
}

/* Where a layer's points (and handles) show now: through each point's skin. */
static DrawingsShown::Layer layer_shown(const Document &doc, const Layer &layer)
{
  DrawingsShown::Layer st;
  st.id = layer.id;
  st.skins = layer_point_skins(doc, layer);
  const VectorDrawing &d = layer.drawing;
  for (size_t i = 0; i < d.points.size() && i < st.skins.size(); i++) {
    st.points.push_back(st.skins[i].apply(d.points[i].pos));
  }
  for (const Curve &curve : d.curves) {
    for (const CurvePoint &cp : curve.points) {
      const Transform2D skin = size_t(cp.point) < st.skins.size() ? st.skins[size_t(cp.point)] : Transform2D();
      st.handles.push_back({skin.apply_vector(cp.handle_in), skin.apply_vector(cp.handle_out)});
    }
  }
  return st;
}

DrawingsShown drawings_shown_capture(Document &doc, const std::vector<uint64_t> &layers)
{
  DrawingsShown out;
  std::set<int> frames;
  const auto add = [&](const auto &ch) {
    for (const auto &k : ch.keys) {
      if (k.frame != 0) {
        frames.insert(k.frame);
      }
    }
  };
  std::vector<uint64_t> kept;
  for (const uint64_t id : layers) {
    const Layer *layer = layer_find(doc, id);
    const Layer *armature = layer ? layer_armature(doc, id) : nullptr;
    if (!layer || !armature || !layer->has_drawing() || layer->is_reference()) {
      continue;
    }
    kept.push_back(id);
    for (const MeshPoint &p : layer->drawing.points) {
      add(p.pos_keys);
    }
    for (const Curve &curve : layer->drawing.curves) {
      for (const CurvePoint &cp : curve.points) {
        add(cp.handle_in_keys);
        add(cp.handle_out_keys);
      }
    }
    for (const Bone &b : armature->bones) {
      add(b.position_keys);
      add(b.angle_keys);
      add(b.scale_keys);
    }
    for (const uint64_t up : {id, armature->id}) {
      for (const Layer *l = layer_find(doc, up); l; l = layer_parent(doc, l->id)) {
        add(l->position_keys);
        add(l->rotation_keys);
        add(l->scale_keys);
      }
    }
  }
  if (kept.empty() || frames.empty()) {
    return out;
  }
  const int was = doc.frame_current;
  animation_record(doc);
  for (const int f : frames) {
    doc.frame_current = f;
    animation_sync(doc);
    std::vector<DrawingsShown::Layer> per;
    for (const uint64_t id : kept) {
      per.push_back(layer_shown(doc, *layer_find(doc, id)));
    }
    out.frames.push_back(f);
    out.shown.push_back(std::move(per));
  }
  doc.frame_current = was;
  animation_sync(doc);
  return out;
}

DrawingsShown drawings_shown_capture_frames(Document &doc, const std::vector<uint64_t> &layers,
                                            const std::vector<int> &frames)
{
  DrawingsShown out;
  std::vector<uint64_t> kept;
  for (const uint64_t id : layers) {
    const Layer *layer = layer_find(doc, id);
    if (layer && layer_armature(doc, id) && layer->has_drawing() && !layer->is_reference()) {
      kept.push_back(id);
    }
  }
  if (kept.empty() || frames.empty()) {
    return out;
  }
  const int was = doc.frame_current;
  animation_record(doc);
  for (const int f : frames) {
    doc.frame_current = f;
    animation_sync(doc);
    std::vector<DrawingsShown::Layer> per;
    for (const uint64_t id : kept) {
      per.push_back(layer_shown(doc, *layer_find(doc, id)));
    }
    out.frames.push_back(f);
    out.shown.push_back(std::move(per));
  }
  doc.frame_current = was;
  animation_sync(doc);
  return out;
}

int drawings_shown_keep(Document &doc, const DrawingsShown &before)
{
  if (before.empty()) {
    return 0;
  }
  const int was = doc.frame_current;
  const bool auto_key = doc.auto_key;
  /* The points put back become keys on their frame, whatever the record button says. */
  doc.auto_key = true;
  animation_record(doc);
  int keyed = 0;
  for (size_t fi = 0; fi < before.frames.size(); fi++) {
    doc.frame_current = before.frames[fi];
    animation_sync(doc);
    for (const DrawingsShown::Layer &st : before.shown[fi]) {
      Layer *layer = layer_find(doc, st.id);
      if (!layer) {
        continue;
      }
      const std::vector<Transform2D> skins = layer_point_skins(doc, *layer);
      VectorDrawing &d = layer->drawing;
      for (size_t i = 0; i < d.points.size() && i < skins.size() && i < st.skins.size(); i++) {
        /* Points the change doesn't bend differently stay as they are. A detail point is bent by its own
         * bones too, so it is put back as any point is (recording takes its offsets from there). */
        if (same_skin(skins[i], st.skins[i]) || length(skins[i].apply(d.points[i].pos) - st.points[i]) < 1e-6f) {
          continue;
        }
        d.points[i].pos = skins[i].inverted().apply(st.points[i]);
        keyed++;
      }
      size_t h = 0;
      for (Curve &curve : d.curves) {
        for (CurvePoint &cp : curve.points) {
          const size_t pi = size_t(cp.point);
          if (cp.handles_set && pi < skins.size() && pi < st.skins.size() && h < st.handles.size() &&
              !same_skin(skins[pi], st.skins[pi]) &&
              (length(skins[pi].apply_vector(cp.handle_in) - st.handles[h].first) >= 1e-6f ||
               length(skins[pi].apply_vector(cp.handle_out) - st.handles[h].second) >= 1e-6f))
          {
            const Transform2D undo = skins[pi].inverted();
            cp.handle_in = undo.apply_vector(st.handles[h].first);
            cp.handle_out = undo.apply_vector(st.handles[h].second);
          }
          h++;
        }
      }
    }
    animation_record(doc);
  }
  doc.auto_key = auto_key;
  doc.frame_current = was;
  animation_sync(doc);
  return keyed;
}

/* Edits action `name` of `owner`, the mainline for "". */
static void use_action(Document &doc, uint64_t owner, const std::string &name)
{
  if (doc.action != name) {
    document_set_action(doc, name.empty() ? 0 : owner, name);
  }
}

bool DrawingsShownAll::empty() const
{
  for (const Action &a : actions) {
    if (!a.shown.empty()) {
      return false;
    }
  }
  return true;
}

DrawingsShownAll drawings_shown_capture_all(Document &doc, const std::vector<uint64_t> &layers)
{
  DrawingsShownAll out;
  out.actions.push_back({0, "", {}});
  for (const uint64_t id : layers) {
    if (const Layer *owner = action_owner(doc, id)) {
      for (const std::string &name : owner->actions) {
        if (std::none_of(out.actions.begin(), out.actions.end(), [&](const DrawingsShownAll::Action &a) {
              return a.owner == owner->id && a.name == name;
            }))
        {
          out.actions.push_back({owner->id, name, {}});
        }
      }
    }
  }
  const std::string was = doc.action;
  uint64_t was_owner = 0;
  for (DrawingsShownAll::Action &a : out.actions) {
    if (a.name == was) {
      was_owner = a.owner;
    }
    use_action(doc, a.owner, a.name);
    a.shown = drawings_shown_capture(doc, layers);
  }
  use_action(doc, was_owner, was);
  return out;
}

/* The way a frame's X axis points, and an angle brought within half a turn. */
static float frame_angle(const Transform2D &t)
{
  return std::atan2(t.b, t.a);
}

static float within_half_turn(float a)
{
  return std::remainder(a, 2.0f * 3.14159265f);
}

static BonesShownAll::Shown bone_shown(const BonePose &p)
{
  return {frame_angle(p.pose), {length(p.pose.apply_vector({1.0f, 0.0f})), length(p.pose.apply_vector({0.0f, 1.0f}))}};
}

/* The frames of the current timeline keyed on `bones` or the bones above them. */
static std::set<int> bones_keyed_frames(const Layer &rig, const std::vector<uint64_t> &bones)
{
  std::set<int> frames;
  const auto add = [&](const auto &ch) {
    for (const auto &k : ch.keys) {
      if (k.frame != 0) {
        frames.insert(k.frame);
      }
    }
  };
  for (const uint64_t id : bones) {
    for (const Bone *b = bone_find(rig, id); b; b = b->parent != b->id ? bone_find(rig, b->parent) : nullptr) {
      add(b->position_keys);
      add(b->angle_keys);
      add(b->scale_keys);
    }
  }
  return frames;
}

BonesShownAll bones_shown_capture_all(Document &doc, uint64_t armature, const std::vector<uint64_t> &bones)
{
  BonesShownAll out;
  out.armature = armature;
  out.bones = bones;
  const Layer *rig = layer_find(doc, armature);
  if (!rig) {
    return out;
  }
  std::vector<std::string> names{""};
  names.insert(names.end(), rig->actions.begin(), rig->actions.end());
  const std::string was = doc.action;
  const int was_frame = doc.frame_current;
  animation_record(doc);
  for (const std::string &name : names) {
    use_action(doc, armature, name);
    BonesShownAll::Action a;
    a.name = name;
    const Layer *now = layer_find(doc, armature);
    for (const int f : bones_keyed_frames(*now, bones)) {
      doc.frame_current = f;
      animation_sync(doc);
      const std::vector<BonePose> poses = armature_pose(*layer_find(doc, armature), false);
      BonesShownAll::Frame frame;
      frame.frame = f;
      for (const uint64_t id : bones) {
        const BonePose *p = bone_pose_find(poses, id);
        frame.bones.push_back(p ? bone_shown(*p) : BonesShownAll::Shown{});
      }
      a.frames.push_back(std::move(frame));
    }
    out.actions.push_back(std::move(a));
  }
  use_action(doc, armature, was);
  doc.frame_current = was_frame;
  animation_sync(doc);
  return out;
}

int bones_shown_keep_all(Document &doc, const BonesShownAll &before)
{
  const std::string was = doc.action;
  const int was_frame = doc.frame_current;
  int keyed = 0;
  for (auto it = before.actions.rbegin(); it != before.actions.rend(); ++it) {
    if (it->frames.empty()) {
      continue;
    }
    use_action(doc, before.armature, it->name);
    for (const BonesShownAll::Frame &frame : it->frames) {
      doc.frame_current = frame.frame;
      animation_sync(doc);
      Layer *rig = layer_find(doc, before.armature);
      if (!rig) {
        break;
      }
      /* Parents first: a child is put right on its parent as the parent shows again. */
      std::vector<size_t> order(before.bones.size());
      for (size_t i = 0; i < order.size(); i++) {
        order[i] = i;
      }
      const auto depth = [&](uint64_t id) {
        int d = 0;
        for (const Bone *b = bone_find(*rig, id); b && d < 256; b = bone_find(*rig, b->parent)) {
          d++;
        }
        return d;
      };
      std::stable_sort(order.begin(), order.end(),
                       [&](size_t a, size_t b) { return depth(before.bones[a]) < depth(before.bones[b]); });
      for (const size_t i : order) {
        Bone *bone = bone_find(*rig, before.bones[i]);
        if (!bone) {
          continue;
        }
        const BonesShownAll::Shown was_shown = frame.bones[i];
        /* A stretched parent skews its children (Inherit Scale Full), so one correction can fall short:
         * measured again until it shows as it did. */
        bool turned = false, scaled = false;
        for (int pass = 0; pass < 8; pass++) {
          const std::vector<BonePose> poses = armature_pose(*rig, false);
          const BonePose *p = bone_pose_find(poses, bone->id);
          if (!p) {
            break;
          }
          const BonesShownAll::Shown now = bone_shown(*p);
          const float turn = within_half_turn(was_shown.angle - now.angle);
          const bool off_x = std::abs(now.size.x - was_shown.size.x) > 1e-6f && now.size.x > 1e-9f;
          const bool off_y = std::abs(now.size.y - was_shown.size.y) > 1e-6f && now.size.y > 1e-9f;
          if (std::abs(turn) <= 1e-6f && !off_x && !off_y) {
            break;
          }
          if (std::abs(turn) > 1e-6f) {
            bone->angle += turn;
            /* The key holds the turn from the setup pose. */
            bone_changes_from_values(*bone);
            turned = true;
          }
          /* Along the bone is its Y scale, across it its X. */
          if (off_x) {
            bone->scale.y *= was_shown.size.x / now.size.x;
            scaled = true;
          }
          if (off_y) {
            bone->scale.x *= was_shown.size.y / now.size.y;
            scaled = true;
          }
        }
        const std::string path = "bones[\"" + bone->name + "\"].";
        if (turned) {
          keyed += layer_insert_filtered_keys(*rig, KeyFilter{KEY_BONE_ANGLE, false, path + "angle", false}, frame.frame);
        }
        if (scaled) {
          keyed += layer_insert_filtered_keys(*rig, KeyFilter{KEY_BONE_TRANSFORM, false, path + "scale", false}, frame.frame);
        }
      }
    }
  }
  use_action(doc, before.armature, was);
  doc.frame_current = was_frame;
  animation_sync(doc);
  return keyed;
}

int drawings_shown_keep_all(Document &doc, const DrawingsShownAll &before)
{
  const std::string was = doc.action;
  uint64_t was_owner = 0;
  int keyed = 0;
  for (auto it = before.actions.rbegin(); it != before.actions.rend(); ++it) {
    if (it->name == was) {
      was_owner = it->owner;
    }
    if (!it->shown.empty()) {
      use_action(doc, it->owner, it->name);
      keyed += drawings_shown_keep(doc, it->shown);
    }
  }
  use_action(doc, was_owner, was);
  return keyed;
}

}  // namespace oanim
