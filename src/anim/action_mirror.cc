#include "anim/action_mirror.hh"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <set>

#include "anim/animation.hh"
#include "anim/channel.hh"
#include "anim/draw_order.hh"
#include "data/bones.hh"
#include "data/layer_mirror.hh"
#include "data/vector.hh"

namespace oanim {

namespace {

const char *const FLIPPED = " Flipped";

/* One handle of a curve point, and the point next along the curve that it reaches for. */
struct Handle {
  CurvePoint *curve_point = nullptr;
  bool out = false;
  int next = -1;
  float2 setup() const
  {
    return handle_setup(*curve_point, !out);
  }
  Channel<float2> &keys() const
  {
    return out ? curve_point->handle_out_keys : curve_point->handle_in_keys;
  }
};

std::vector<Handle> handles_of(VectorDrawing &drawing, int point)
{
  std::vector<Handle> handles;
  for (Curve &curve : drawing.curves) {
    const int n = int(curve.points.size());
    for (int i = 0; i < n; i++) {
      if (curve.points[i].point != point) {
        continue;
      }
      if (i > 0 || curve.closed) {
        handles.push_back({&curve.points[i], false, curve.points[(i + n - 1) % n].point});
      }
      if (i + 1 < n || curve.closed) {
        handles.push_back({&curve.points[i], true, curve.points[(i + 1) % n].point});
      }
    }
  }
  return handles;
}

/* `to`'s keys in `dst` are `from`'s in `src`, mapped. The same channel can be both. */
template<typename T, typename F>
void mirror_keys(const Channel<T> &src, Channel<T> &dst, const std::string &from, const std::string &to, F &&map)
{
  const auto it = src.actions.find(from);
  if (it == src.actions.end()) {
    return;
  }
  std::vector<Key<T>> keys = it->second;
  for (Key<T> &k : keys) {
    const T was = k.value;
    k.value = map(was);
    if constexpr (std::is_same_v<T, float2>) {
      /* Path handles are offsets from the value: mirrored as the places they reach. */
      if (k.path_handles != PathHandles::Auto) {
        k.path_in = map(was + k.path_in) - k.value;
        k.path_out = map(was + k.path_out) - k.value;
      }
    }
    k.selected = false;
  }
  dst.actions[to] = std::move(keys);
  /* Frame 0's path handles (its value is the setup's): offsets, mirrored by how the map moves them. */
  dst.action_setup.erase(to);
  if (const auto s = src.action_setup.find(from); s != src.action_setup.end()) {
    Key<T> k = s->second;
    if constexpr (std::is_same_v<T, float2>) {
      k.path_in = map(k.path_in) - map(T{});
      k.path_out = map(k.path_out) - map(T{});
    }
    dst.action_setup[to] = std::move(k);
  }
}

/* A channel's change from the setup on `frame` of action `name`. */
template<typename T> T sample(const Channel<T> &ch, const std::string &name, const T &setup, int frame)
{
  const auto it = ch.actions.find(name);
  if (it == ch.actions.end()) {
    return setup;
  }
  const ChannelKeysQuiet scratch;
  Channel<T> c;
  c.keys = it->second;
  c.set_key(0, setup);
  return c.evaluate(frame);
}

/* A point of the target with no partner, between two that have them, on a line the partners run straight along. */
struct LineSpot {
  int point = -1;
  Handle before, after;
  /* The target's handles of its neighbours facing it, and the source's that the line is mirrored from. */
  Handle a, b, source_a, source_b;
  int source_point_a = -1, source_point_b = -1;
};

/* What makes a layer's keys: `source`'s go into `target`, through `to` (reflecting one's setup onto the other's). */
struct Link {
  Layer *target = nullptr;
  Layer *source = nullptr;
  Transform2D to;
  /* By point: its partner in the other layer, or -1. */
  std::vector<int> source_of, target_of;
};

Link link_layers(Document &doc, Layer &target, Layer &source, bool same_points)
{
  Link link;
  link.target = &target;
  link.source = &source;
  link.to = layer_mirror_to_partner(doc, source.id, target.id);
  const int nt = int(target.drawing.points.size()), ns = int(source.drawing.points.size());
  link.source_of.assign(size_t(nt), -1);
  link.target_of.assign(size_t(ns), -1);
  if (same_points) {
    for (int i = 0; i < std::min(nt, ns); i++) {
      link.source_of[size_t(i)] = i;
      link.target_of[size_t(i)] = i;
    }
    return link;
  }
  std::vector<int> all_t(static_cast<size_t>(nt)), all_s(static_cast<size_t>(ns));
  for (int i = 0; i < nt; i++) {
    all_t[size_t(i)] = i;
  }
  for (int i = 0; i < ns; i++) {
    all_s[size_t(i)] = i;
  }
  const std::vector<int> to_target = layer_mirror_images(doc, source, target, all_s);
  const std::vector<int> to_source = layer_mirror_images(doc, target, source, all_t);
  for (int t = 0; t < nt; t++) {
    const int s = to_source[size_t(t)];
    if (s >= 0 && to_target[size_t(s)] == t) {
      link.source_of[size_t(t)] = s;
      link.target_of[size_t(s)] = t;
    }
  }
  return link;
}

/* Where a target point without a partner sits on a mirrored line, or point -1 if it can't be said. */
LineSpot line_spot(Link &link, int point)
{
  LineSpot spot;
  VectorDrawing &target = link.target->drawing, &source = link.source->drawing;
  const std::vector<Handle> mine = handles_of(target, point);
  for (const Handle &before : mine) {
    for (const Handle &after : mine) {
      if (before.curve_point != after.curve_point || before.out || !after.out) {
        continue;
      }
      const int a = before.next, b = after.next;
      const int sa = link.source_of[size_t(a)], sb = link.source_of[size_t(b)];
      if (sa < 0 || sb < 0) {
        continue;
      }
      const auto facing = [](const std::vector<Handle> &handles, int next) {
        for (const Handle &h : handles) {
          if (h.next == next) {
            return h;
          }
        }
        return Handle();
      };
      LineSpot found;
      found.before = before;
      found.after = after;
      found.a = facing(handles_of(target, a), point);
      found.b = facing(handles_of(target, b), point);
      found.source_a = facing(handles_of(source, sa), sb);
      found.source_b = facing(handles_of(source, sb), sa);
      found.source_point_a = sa;
      found.source_point_b = sb;
      if (found.a.curve_point && found.b.curve_point && found.source_a.curve_point && found.source_b.curve_point) {
        found.point = point;
        return found;
      }
    }
  }
  return spot;
}

/* Keys the point and the handles either side so the line through it is the mirrored line, at the same place along it.
 * All as changes from the line at rest, so the frame-0 pose stays the setup's. */
void key_on_line(Link &link, const LineSpot &spot, const std::string &from, const std::string &to)
{
  VectorDrawing &source = link.source->drawing;
  const MeshPoint &pa = source.points[size_t(spot.source_point_a)], &pb = source.points[size_t(spot.source_point_b)];
  const auto control = [&](int frame, float2 r_ctrl[4]) {
    const float2 a = point_setup_pos(pa) + sample(pa.pos_keys, from, float2(), frame);
    const float2 b = point_setup_pos(pb) + sample(pb.pos_keys, from, float2(), frame);
    const float2 ha = handle_from_change(spot.source_a.setup(), sample(spot.source_a.keys(), from, float2(), frame));
    const float2 hb = handle_from_change(spot.source_b.setup(), sample(spot.source_b.keys(), from, float2(), frame));
    r_ctrl[0] = link.to.apply(a);
    r_ctrl[1] = link.to.apply(a + ha);
    r_ctrl[2] = link.to.apply(b + hb);
    r_ctrl[3] = link.to.apply(b);
  };
  float2 rest[4];
  control(0, rest);
  const float2 place = point_setup_pos(link.target->drawing.points[size_t(spot.point)]);
  float t = 0.5f, nearest = std::numeric_limits<float>::max();
  for (int i = 1; i < 256; i++) {
    const float at = float(i) / 256.0f;
    const float d = distance(bezier_eval(rest, at), place);
    if (d < nearest) {
      nearest = d;
      t = at;
    }
  }
  struct Split {
    float2 pos, in, out, a, b;
  };
  const auto split_at = [&](int frame) {
    float2 c[4];
    control(frame, c);
    const auto mix = [&](float2 p, float2 q) { return p + (q - p) * t; };
    const float2 q0 = mix(c[0], c[1]), q1 = mix(c[1], c[2]), q2 = mix(c[2], c[3]);
    const float2 r0 = mix(q0, q1), r1 = mix(q1, q2), s = mix(r0, r1);
    return Split{s, r0 - s, r1 - s, q0 - c[0], q2 - c[3]};
  };
  std::set<int> frames;
  Key<float2> like;
  like.interp = Interp::Linear;
  bool have_like = false;
  const std::vector<const Channel<float2> *> driving = {&pa.pos_keys, &pb.pos_keys, &spot.source_a.keys(), &spot.source_b.keys()};
  for (const Channel<float2> *ch : driving) {
    const auto it = ch->actions.find(from);
    if (it != ch->actions.end()) {
      for (const Key<float2> &k : it->second) {
        frames.insert(k.frame);
        if (!have_like) {
          like = k;
          have_like = true;
        }
      }
    }
  }
  frames.erase(0);
  if (frames.empty()) {
    return;
  }
  const Split at_rest = split_at(0);
  std::vector<Key<float2>> pos, in, out, a, b;
  const auto add = [&](std::vector<Key<float2>> &keys, int frame, float2 value) {
    Key<float2> k = like;
    k.frame = frame;
    k.value = value;
    k.selected = false;
    keys.push_back(k);
  };
  float2 last_in, last_out, last_a, last_b;
  for (const int f : frames) {
    const Split s = split_at(f);
    add(pos, f, s.pos - at_rest.pos);
    last_in = handle_change(at_rest.in, s.in, last_in.x);
    last_out = handle_change(at_rest.out, s.out, last_out.x);
    last_a = handle_change(at_rest.a, s.a, last_a.x);
    last_b = handle_change(at_rest.b, s.b, last_b.x);
    add(in, f, last_in);
    add(out, f, last_out);
    add(a, f, last_a);
    add(b, f, last_b);
  }
  link.target->drawing.points[size_t(spot.point)].pos_keys.actions[to] = std::move(pos);
  spot.before.keys().actions[to] = std::move(in);
  spot.after.keys().actions[to] = std::move(out);
  spot.a.keys().actions[to] = std::move(a);
  spot.b.keys().actions[to] = std::move(b);
}

bool has_drawing_keys(const Layer &layer, const std::string &name)
{
  for (const MeshPoint &p : layer.drawing.points) {
    if (p.pos_keys.actions.contains(name) || p.radius_keys.actions.contains(name)) {
      return true;
    }
  }
  for (const Curve &curve : layer.drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      if (cp.handle_in_keys.actions.contains(name) || cp.handle_out_keys.actions.contains(name)) {
        return true;
      }
    }
  }
  return false;
}

/* The target points that neither have a partner nor sit on a mirrored line. */
std::vector<int> points_left_out(Link &link)
{
  std::vector<int> left;
  for (int t = 0; t < int(link.source_of.size()); t++) {
    if (link.source_of[size_t(t)] < 0 && line_spot(link, t).point < 0) {
      left.push_back(t);
    }
  }
  return left;
}

void mirror_drawing_keys(Link &link, const std::string &from, const std::string &to)
{
  VectorDrawing &target = link.target->drawing, &source = link.source->drawing;
  std::vector<LineSpot> spots;
  for (int t = 0; t < int(target.points.size()); t++) {
    const int s = link.source_of[size_t(t)];
    if (s < 0) {
      if (const LineSpot spot = line_spot(link, t); spot.point >= 0) {
        spots.push_back(spot);
      }
      continue;
    }
    const MeshPoint &sp = source.points[size_t(s)];
    MeshPoint &tp = target.points[size_t(t)];
    /* A detail point's keys are along and across its segment, not in the layer's space. */
    if (sp.detail.level > 0 || tp.detail.level > 0) {
      mirror_keys(sp.pos_keys, tp.pos_keys, from, to, [](float2 v) { return float2(v.x, -v.y); });
    }
    else {
      mirror_keys(sp.pos_keys, tp.pos_keys, from, to, [&](float2 v) { return link.to.apply_vector(v); });
    }
    mirror_keys(sp.radius_keys, tp.radius_keys, from, to, [](float v) { return v; });
    /* Handles by what they reach for: the partner of the point it reaches, else (a point with no partner there)
     * whichever is left, the nearest. */
    const std::vector<Handle> mine = handles_of(target, t), theirs = handles_of(source, s);
    std::vector<bool> used(theirs.size(), false);
    std::vector<int> pick(mine.size(), -1);
    for (size_t m = 0; m < mine.size(); m++) {
      for (size_t n = 0; n < theirs.size(); n++) {
        if (!used[n] && link.target_of[size_t(theirs[n].next)] == mine[m].next) {
          pick[m] = int(n);
          used[n] = true;
          break;
        }
      }
    }
    for (size_t m = 0; m < mine.size(); m++) {
      if (pick[m] >= 0) {
        continue;
      }
      float nearest = std::numeric_limits<float>::max();
      int best = -1;
      for (size_t n = 0; n < theirs.size(); n++) {
        const float d = distance(link.to.apply(point_setup_pos(source.points[size_t(theirs[n].next)])),
                                 point_setup_pos(target.points[size_t(mine[m].next)]));
        if (!used[n] && d < nearest) {
          nearest = d;
          best = int(n);
        }
      }
      if (best >= 0) {
        pick[m] = best;
        used[size_t(best)] = true;
      }
    }
    for (size_t m = 0; m < mine.size(); m++) {
      if (pick[m] < 0) {
        continue;
      }
      const Handle &from_handle = theirs[size_t(pick[m])];
      /* Facing a point with no partner, its line runs on past that, and is one segment here: it reaches as far. */
      float scale = 1.0f;
      if (link.target_of[size_t(from_handle.next)] < 0) {
        const float part = distance(point_setup_pos(source.points[size_t(s)]), point_setup_pos(source.points[size_t(from_handle.next)]));
        const float whole = distance(point_setup_pos(target.points[size_t(t)]), point_setup_pos(target.points[size_t(mine[m].next)]));
        scale = part > 1e-9f ? whole / part : 1.0f;
      }
      /* The handle itself goes across, then becomes a change from this one's setup: the two setups can differ a lot
       * (one a stub), so a change alone wouldn't come out the same shape. */
      if (const auto it = from_handle.keys().actions.find(from); it != from_handle.keys().actions.end()) {
        std::vector<Key<float2>> keys = it->second;
        float near_turn = 0.0f;
        for (Key<float2> &k : keys) {
          k.selected = false;
          if (k.frame == 0) {
            k.value = float2();
            continue;
          }
          const float2 shown = handle_from_change(from_handle.setup(), k.value) * scale;
          k.value = handle_change(mine[m].setup(), link.to.apply_vector(shown), near_turn);
          near_turn = k.value.x;
        }
        mine[m].keys().actions[to] = std::move(keys);
      }
      mirror_keys(from_handle.curve_point->curvature_keys, mine[m].curve_point->curvature_keys, from, to,
                  [](float v) { return v; });
    }
  }
  for (const LineSpot &spot : spots) {
    key_on_line(link, spot, from, to);
  }
}

/* What every layer has: `partner` names the layers Draw Order and switch keys point at. */
void mirror_layer_keys(const Layer &source, Layer &target, const std::map<uint64_t, uint64_t> &partner,
                       const std::string &from, const std::string &to)
{
  const auto copy = [](float v) { return v; };
  const auto other = [&](uint64_t id) {
    const auto it = partner.find(id);
    return it == partner.end() ? id : it->second;
  };
  for (const CustomChannel &c : source.custom_channels) {
    for (CustomChannel &d : target.custom_channels) {
      if (d.path == c.path) {
        mirror_keys(c.keys, d.keys, from, to, copy);
      }
    }
  }
  mirror_keys(source.visible_keys, target.visible_keys, from, to, copy);
  mirror_keys(source.draw_order_keys, target.draw_order_keys, from, to, [&](float v) {
    DrawOrderRule rule = draw_order_rule(v);
    rule.target = other(rule.target);
    return draw_order_value(rule);
  });
  if (source.is_switch() && target.is_switch()) {
    mirror_keys(source.switch_keys, target.switch_keys, from, to,
                [&](float v) { return float(other(uint64_t(v + 0.5f))); });
  }
  mirror_keys(source.position_keys, target.position_keys, from, to, [&](float2 v) {
    const float2 change = v - source.position;
    return target.position + float2(-change.x, change.y);
  });
  mirror_keys(source.rotation_keys, target.rotation_keys, from, to,
              [&](float v) { return target.rotation - (v - source.rotation); });
  mirror_keys(source.scale_keys, target.scale_keys, from, to,
              [&](float2 v) { return target.scale + (v - source.scale); });
  mirror_keys(source.shear_keys, target.shear_keys, from, to,
              [&](float2 v) { return target.shear - (v - source.shear); });
  mirror_keys(source.opacity_keys, target.opacity_keys, from, to, copy);
  if (source.masks.size() == target.masks.size()) {
    for (size_t i = 0; i < source.masks.size(); i++) {
      mirror_keys(source.masks[i].line_reach_keys, target.masks[i].line_reach_keys, from, to, copy);
    }
  }
  if (source.modifiers.size() == target.modifiers.size()) {
    for (size_t i = 0; i < source.modifiers.size(); i++) {
      const Modifier &s = source.modifiers[i];
      Modifier &t = target.modifiers[i];
      if (s.type == t.type) {
        mirror_keys(s.merge_smoothing_keys, t.merge_smoothing_keys, from, to, copy);
        mirror_keys(s.bevel_amount_keys, t.bevel_amount_keys, from, to, copy);
        mirror_keys(s.deform_strength_keys, t.deform_strength_keys, from, to, copy);
        mirror_keys(s.deform_shift_keys, t.deform_shift_keys, from, to, [](float2 v) { return float2(-v.x, v.y); });
        mirror_keys(s.deform_shift2_keys, t.deform_shift2_keys, from, to, [](float2 v) { return float2(-v.x, v.y); });
      }
    }
  }
}

std::vector<Channel<float> *> constraint_channels(BoneConstraint &c)
{
  std::vector<Channel<float> *> channels = {&c.influence_keys, &c.weight_keys, &c.wind_catch_keys, &c.gravity_keys,
                                            &c.wind_keys, &c.wind_direction_keys, &c.wind_gusts_keys,
                                            &c.wind_gust_speed_keys, &c.wind_turbulence_keys};
  for (BoneSpring *s : {&c.angle_spring, &c.position_spring, &c.scale_spring}) {
    for (Channel<float> *ch : {&s->settle_speed_keys, &s->bounce_keys, &s->inertia_keys, &s->stretch_keys, &s->limit_keys,
                               &s->squash_limit_keys})
    {
      channels.push_back(ch);
    }
  }
  return channels;
}

/* The bone of `source` that `bone` of `target` takes its keys from: its other side in the same bone layer, and in a
 * partner bone layer its other side there, else the one of its own name. */
uint64_t bone_mirror_source(const Layer &target, const Bone &bone, const Layer &source)
{
  if (target.id == source.id) {
    return bone_mirror_id(target, bone.id);
  }
  const Bone *other = bone_find_by_name(source, bone_mirror_name(bone.name));
  other = other ? other : bone_find_by_name(source, bone.name);
  return other ? other->id : 0;
}

/* `target`'s bones take the keys of `source`'s, `flip` reflecting the one bone layer's space into the other's; the two are
 * one for a bone layer with no side. A bone's Transform is along its own rest axes, so it goes through both bones' setup
 * frames. */
void mirror_bone_keys(Layer &target, const Layer &source_layer, const Transform2D &flip, const std::string &from,
                      const std::string &to)
{
  const std::vector<BonePose> from_poses = armature_pose(source_layer), to_poses = armature_pose(target);
  const std::vector<Bone> sources = source_layer.bones;
  for (Bone &bone : target.bones) {
    const uint64_t other = bone_mirror_source(target, bone, source_layer);
    const auto source = std::find_if(sources.begin(), sources.end(), [&](const Bone &b) { return b.id == other; });
    const BonePose *from_pose = bone_pose_find(from_poses, other), *to_pose = bone_pose_find(to_poses, bone.id);
    if (source == sources.end() || !from_pose || !to_pose) {
      continue;
    }
    const Transform2D across = to_pose->setup.inverted() * flip * from_pose->setup;
    mirror_keys(source->position_keys, bone.position_keys, from, to, [&](float2 v) { return across.apply_vector(v); });
    mirror_keys(source->angle_keys, bone.angle_keys, from, to, [](float v) { return -v; });
    mirror_keys(source->scale_keys, bone.scale_keys, from, to, [](float2 v) { return v; });
    mirror_keys(source->depth_keys, bone.depth_keys, from, to, [](float v) { return v; });
    if (source->constraints.size() == bone.constraints.size()) {
      for (size_t i = 0; i < bone.constraints.size(); i++) {
        BoneConstraint mine = source->constraints[i];
        mirror_keys(mine.follow_shift_keys, bone.constraints[i].follow_shift_keys, from, to,
                    [](float2 v) { return float2(-v.x, v.y); });
        mirror_keys(mine.follow_shift2_keys, bone.constraints[i].follow_shift2_keys, from, to,
                    [](float2 v) { return float2(-v.x, v.y); });
        mirror_keys(mine.follow_shift3_keys, bone.constraints[i].follow_shift3_keys, from, to,
                    [](float2 v) { return float2(-v.x, v.y); });
        const std::vector<Channel<float> *> a = constraint_channels(mine), b = constraint_channels(bone.constraints[i]);
        for (size_t k = 0; k < a.size(); k++) {
          mirror_keys(*a[k], *b[k], from, to, [](float v) { return v; });
        }
      }
    }
  }
}

/* The flipped copy of a drawing in a switch, beside it: its setup is `layer`'s mirror image, point for point. Remade if
 * it is there, keeping its id so what names it still does. Returns its id. */
uint64_t flipped_twin(Document &doc, uint64_t owner, uint64_t layer_id)
{
  Layer &layer = *layer_find(doc, layer_id);
  const std::string name = layer.name + FLIPPED;
  int index = 0;
  std::vector<Layer> *siblings = layer_container(doc, layer_id, &index);
  Layer twin = layer_copy_with_new_ids(doc, layer);
  layer_strip_keys(twin, false, false);
  twin.name = name;
  const Transform2D flip = layer_mirror_to_partner(doc, layer_id, layer_id);
  const Layer &armature = *layer_find(doc, owner);
  for (MeshPoint &p : twin.drawing.points) {
    p.pos = flip.apply(p.pos);
    p.setup_pos = flip.apply(p.setup_pos);
    for (BoneWeight &w : p.weights) {
      w.bone = bone_mirror_id(armature, w.bone);
    }
  }
  for (Curve &curve : twin.drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.handle_in = flip.apply_vector(cp.handle_in);
      cp.handle_out = flip.apply_vector(cp.handle_out);
      cp.setup_in = flip.apply_vector(cp.setup_in);
      cp.setup_out = flip.apply_vector(cp.setup_out);
    }
  }
  for (Layer &sibling : *siblings) {
    if (sibling.name == name && sibling.id != layer_id) {
      twin.id = sibling.id;
      sibling = std::move(twin);
      return sibling.id;
    }
  }
  const uint64_t id = twin.id;
  siblings->insert(siblings->begin() + index, std::move(twin));
  return id;
}

/* Bone-layer members in the Layers editor's order, parents first. */
std::vector<uint64_t> layers_inside(const Document &doc, uint64_t owner)
{
  std::vector<uint64_t> ids;
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->id != owner && layer_inside(doc, l->id, owner)) {
      ids.push_back(l->id);
    }
  }
  return ids;
}

/* A layer's partner: its other side's name and kind, under its parent's partner if it can be. Layers with no side are
 * their own. */
std::map<uint64_t, uint64_t> layer_partners(Document &doc, const std::vector<uint64_t> &ids, std::vector<std::string> &notes)
{
  std::map<uint64_t, uint64_t> partner;
  for (const uint64_t id : ids) {
    const Layer *layer = layer_find(doc, id);
    const std::string other = bone_mirror_name(layer->name);
    if (other == layer->name) {
      partner[id] = id;
      continue;
    }
    const Layer *parent = layer_parent(doc, id);
    const auto parent_partner = parent ? partner.find(parent->id) : partner.end();
    uint64_t best = 0;
    for (const uint64_t candidate : ids) {
      const Layer *c = layer_find(doc, candidate);
      if (c->name != other || c->type != layer->type) {
        continue;
      }
      const Layer *cp = layer_parent(doc, candidate);
      if (best == 0 || (cp && parent_partner != partner.end() && cp->id == parent_partner->second)) {
        best = candidate;
      }
    }
    if (best) {
      partner[id] = best;
    }
    else {
      notes.push_back("\"" + layer->name + "\" has no \"" + other + "\" to go to");
    }
  }
  return partner;
}

}  // namespace

std::vector<std::string> document_action_mirror(Document &doc, uint64_t owner_id, const std::string &from, const std::string &to)
{
  std::vector<std::string> notes;
  Layer *owner = layer_find(doc, owner_id);
  if (!owner || from == to) {
    return notes;
  }
  document_action_delete(doc, owner_id, to);

  /* Drawings in a switch that can't pair up get a flipped copy first, as it moves layers about. */
  std::map<uint64_t, uint64_t> twin_of;
  for (const uint64_t id : layers_inside(doc, owner_id)) {
    Layer *layer = layer_find(doc, id);
    Layer *parent = layer_parent(doc, id);
    if (!layer->has_drawing() || !parent || !parent->is_switch() || bone_mirror_name(layer->name) != layer->name) {
      continue;
    }
    const std::string name = layer->name;
    const size_t suffix = std::string(FLIPPED).size();
    if (name.size() > suffix && name.compare(name.size() - suffix, suffix, FLIPPED) == 0) {
      /* Going back the other way: the drawing it was flipped from is its partner, and stays as it is. */
      if (has_drawing_keys(*layer, from)) {
        const std::string base = name.substr(0, name.size() - suffix);
        for (const Layer &sibling : parent->children) {
          if (sibling.name == base && sibling.id != id) {
            twin_of[id] = sibling.id;
          }
        }
      }
      continue;
    }
    Link self = link_layers(doc, *layer, *layer, false);
    if (has_drawing_keys(*layer, from) && !points_left_out(self).empty()) {
      const uint64_t twin = flipped_twin(doc, owner_id, id);
      twin_of[id] = twin;
      notes.push_back("\"" + name + "\" can't turn round by its keys, so it has a flipped copy");
    }
  }
  owner = layer_find(doc, owner_id);
  owner->actions.push_back(to);
  /* The other side loops as this one does (muting stays each side's own). */
  if (const ActionPlay *play = action_play_find(*owner, from); play && play->loop != ActionLoop::Auto) {
    const ActionLoop loop = play->loop;
    action_play_ensure(*owner, to).loop = loop;
  }

  const std::vector<uint64_t> ids = layers_inside(doc, owner_id);
  std::map<uint64_t, uint64_t> partner = layer_partners(doc, ids, notes);
  std::set<uint64_t> no_keys;
  for (const auto &[id, twin] : twin_of) {
    partner[id] = twin;
    /* The original keeps nothing of the mirrored action: only the copy shows in it. */
    no_keys.insert(id);
  }
  for (const uint64_t id : ids) {
    if (no_keys.count(id)) {
      continue;
    }
    Layer &target = *layer_find(doc, id);
    uint64_t source_id = id;
    bool same_points = false;
    for (const auto &[from_id, twin] : twin_of) {
      if (twin == id) {
        source_id = from_id;
        same_points = true;
      }
    }
    if (!same_points) {
      const auto it = std::find_if(partner.begin(), partner.end(), [&](const auto &p) { return p.second == id; });
      if (it == partner.end()) {
        continue;
      }
      source_id = it->first;
    }
    Layer &source = *layer_find(doc, source_id);
    mirror_layer_keys(source, target, partner, from, to);
    if (source.is_bone_layer() && target.is_bone_layer()) {
      mirror_bone_keys(target, source, layer_mirror_to_partner(doc, source.id, target.id), from, to);
    }
    if (!source.has_drawing() || !target.has_drawing() || source.is_reference() || target.is_reference()) {
      continue;
    }
    Link link = link_layers(doc, target, source, same_points);
    mirror_drawing_keys(link, from, to);
    if (has_drawing_keys(source, from)) {
      if (const std::vector<int> left = points_left_out(link); !left.empty()) {
        notes.push_back("\"" + target.name + "\": " + std::to_string(left.size()) + (left.size() == 1 ? " point has" : " points have") +
                        " no partner, and stay as they are");
      }
    }
  }
  Layer &mirrored = *layer_find(doc, owner_id);
  mirror_bone_keys(mirrored, mirrored, Transform2D{-1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f}, from, to);
  animation_evaluate(doc, doc.frame_current);
  return notes;
}

}  // namespace oanim
