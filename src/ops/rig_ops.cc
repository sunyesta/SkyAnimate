/* Rigging and modifier operators: bones, binding, symmetrize, and the modifier stack.
 *
 * As in Moho, changing the rig (adding or deleting bones, binding) only happens on the setup
 * frame, 0; posing (moving and turning bones) works on any frame and keys like other edits. */

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

#include "anim/switch.hh"
#include "anim/bone_family.hh"
#include "anim/animation.hh"
#include "anim/deform.hh"
#include "anim/bevel.hh"
#include "anim/subdivide.hh"
#include "anim/drivers.hh"
#include "anim/dynamics.hh"
#include "anim/keep_poses.hh"
#include "anim/merge.hh"
#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

/* How a Parent Bone moving a layer's points (Deform: Points) moves them now: the same for each point
 * (each is weighted all to it), identity when it doesn't. */
static Transform2D points_bone_move(const Document &doc, const Layer &layer)
{
  if (!layer.points_follow_bone()) {
    return Transform2D();
  }
  const std::vector<Transform2D> skins = layer_point_skins(doc, layer);
  return skins.empty() ? Transform2D() : skins.front();
}

Layer *active_armature(Context &C)
{
  if (!C.document) {
    return nullptr;
  }
  Document &doc = *C.document;
  Layer *active = document_active_layer(doc);
  if (!active) {
    return nullptr;
  }
  if (active->is_bone_layer()) {
    return active;
  }
  const Layer *armature = layer_armature(doc, active->id);
  return armature ? layer_find(doc, armature->id) : nullptr;
}

static bool on_setup_frame(Context &C)
{
  if (C.document->frame_current == 0) {
    return true;
  }
  report_banner(ReportType::Warning, "Switch to Setup (frame 0) to change the rig");
  return false;
}

static void rig_operator(const char *idname, const char *name, const char *description,
                         std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec,
                         std::vector<OpProperty> props = {})
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot.poll = std::move(poll);
  ot.exec = std::move(exec);
  ot.props = std::move(props);
  operators().add(std::move(ot));
}

/* "|"-separated names or ids of `drawing`'s shapes as ids; false (and a report) for one not there. */
static bool shape_ids_from(const VectorDrawing &drawing, const std::string &list, const std::string &layer_name,
                           std::vector<uint64_t> *r_ids)
{
  r_ids->clear();
  size_t at = 0;
  while (at <= list.size()) {
    const size_t bar = std::min(list.find('|', at), list.size());
    const std::string part = list.substr(at, bar - at);
    at = bar + 1;
    if (part.empty()) {
      continue;
    }
    const auto it = std::find_if(drawing.shapes.begin(), drawing.shapes.end(), [&](const Shape &s) {
      return s.name == part || std::to_string(s.id) == part;
    });
    if (it == drawing.shapes.end()) {
      report(ReportType::Warning, layer_name + " has no shape \"" + part + "\"");
      return false;
    }
    if (std::find(r_ids->begin(), r_ids->end(), it->id) == r_ids->end()) {
      r_ids->push_back(it->id);
    }
  }
  return true;
}

/* A Boolean modifier's settings from the operator's arguments (see object.modifier_boolean_set). */
static bool merge_settings(Context &C, const Layer &layer, Modifier &m)
{
  Document &doc = *C.document;
  if (op_has_arg(C, "targets")) {
    std::vector<MergeTarget> targets;
    const std::string list = op_arg(C, "targets");
    size_t at = 0;
    while (at <= list.size()) {
      const size_t bar = std::min(list.find('|', at), list.size());
      const std::string part = list.substr(at, bar - at);
      at = bar + 1;
      if (part.empty()) {
        continue;
      }
      const Layer *t = layer_by_name_or_id(doc, part);
      if (!t || t->type != LayerType::Vector || t->id == layer.id) {
        report(ReportType::Warning, t && t->id == layer.id ? "A layer merges its own shapes already: name others"
                                                           : "There's no drawing layer \"" + part + "\" to merge");
        return false;
      }
      if (std::none_of(targets.begin(), targets.end(), [&](const MergeTarget &x) { return x.layer == t->id; })) {
        /* Kept: the shapes it already took from there. */
        const auto old = std::find_if(m.merge_targets.begin(), m.merge_targets.end(),
                                      [&](const MergeTarget &x) { return x.layer == t->id; });
        targets.push_back(old != m.merge_targets.end() ? *old : MergeTarget{t->id, {}});
      }
    }
    m.merge_targets = std::move(targets);
  }
  if (op_has_arg(C, "operation")) {
    const std::string op = op_arg(C, "operation");
    if (op == "UNION") {
      m.merge_operation = MergeOperation::Union;
    }
    else if (op == "SUBTRACT") {
      m.merge_operation = MergeOperation::Subtract;
    }
    else if (op == "INTERSECT") {
      m.merge_operation = MergeOperation::Intersect;
    }
    else {
      report(ReportType::Error, "operation is UNION, SUBTRACT or INTERSECT");
      return false;
    }
  }
  if (op_has_arg(C, "smoothing")) {
    m.merge_smoothing = std::max(0.0f, op_arg_float(C, "smoothing", 0.0f));
  }
  if (op_has_arg(C, "corners_only")) {
    m.merge_corners_only = op_arg_bool(C, "corners_only");
  }
  if (op_has_arg(C, "shapes") && !shape_ids_from(layer.drawing, op_arg(C, "shapes"), layer.name, &m.merge_shapes)) {
    return false;
  }
  if (op_has_arg(C, "pick")) {
    /* The eyedropper's click: a shape and its layer, in place of layer `replace` (or added). The
     * shape joins that layer's picks; a layer taking all its shapes takes just the one clicked. */
    const Layer *t = layer_by_name_or_id(doc, op_arg(C, "pick"));
    if (!t || t->type != LayerType::Vector || t->id == layer.id) {
      report(ReportType::Warning, t && t->id == layer.id ? "That's this layer's own shape: click another layer's"
                                                         : "There's no drawing layer \"" + op_arg(C, "pick") + "\"");
      return false;
    }
    std::vector<uint64_t> shape;
    if (!shape_ids_from(t->drawing, op_arg(C, "pick_shape"), t->name, &shape) || shape.size() != 1) {
      report(ReportType::Warning, "Name the shape picked (pick_shape=)");
      return false;
    }
    const auto it = std::find_if(t->drawing.shapes.begin(), t->drawing.shapes.end(),
                                 [&](const Shape &s) { return s.id == shape.front(); });
    if (!merge_takes_shape(t->drawing, *it, {})) {
      report(ReportType::Warning, "\"" + it->name + "\" doesn't go all the way round, so it can't merge");
      return false;
    }
    const uint64_t replace = op_has_arg(C, "replace") ? uint64_t(std::stoull("0" + op_arg(C, "replace"))) : 0;
    std::vector<MergeTarget> targets;
    for (const MergeTarget &x : m.merge_targets) {
      if (x.layer == t->id && x.layer != replace) {
        /* Already in. */
        continue;
      }
      targets.push_back(x.layer == replace ? MergeTarget{t->id, {}} : x);
    }
    const auto old = std::find_if(m.merge_targets.begin(), m.merge_targets.end(),
                                  [&](const MergeTarget &x) { return x.layer == t->id && x.layer != replace; });
    MergeTarget picked{t->id, shape};
    if (old != m.merge_targets.end()) {
      picked.shapes = old->shapes.empty() ? shape : old->shapes;
      if (std::find(picked.shapes.begin(), picked.shapes.end(), shape.front()) == picked.shapes.end()) {
        picked.shapes.push_back(shape.front());
      }
    }
    const auto at = std::find_if(targets.begin(), targets.end(), [&](const MergeTarget &x) { return x.layer == t->id; });
    if (at != targets.end()) {
      *at = picked;
    }
    else if (old != m.merge_targets.end()) {
      /* Kept where it was. */
      targets.insert(targets.begin() + (old - m.merge_targets.begin()), picked);
    }
    else {
      targets.push_back(picked);
    }
    m.merge_targets = std::move(targets);
  }
  if (op_has_arg(C, "target")) {
    const Layer *t = layer_by_name_or_id(doc, op_arg(C, "target"));
    const auto it = t ? std::find_if(m.merge_targets.begin(), m.merge_targets.end(),
                                     [&](const MergeTarget &x) { return x.layer == t->id; })
                      : m.merge_targets.end();
    if (it == m.merge_targets.end()) {
      report(ReportType::Warning, "\"" + op_arg(C, "target") + "\" isn't one of the layers merged");
      return false;
    }
    if (!shape_ids_from(t->drawing, op_arg(C, "target_shapes"), t->name, &it->shapes)) {
      return false;
    }
  }
  return true;
}

static bool armature_poll(Context &C)
{
  return active_armature(C) != nullptr;
}

/* A bone's frame from where it runs in bone layer space, given its parent's frame. */
static void bone_place(Bone &bone, const Transform2D &parent, float2 head, float2 tail)
{
  const Transform2D to_parent = parent.inverted();
  const float2 h = to_parent.apply(head), t = to_parent.apply(tail);
  bone.position = h;
  bone.angle = std::atan2(t.y - h.y, t.x - h.x);
  bone.length = distance(h, t);
  bone.scale = {1.0f, 1.0f};
}

std::string armature_unique_bone_name(const Layer &armature, const std::string &base)
{
  if (!bone_find_by_name(armature, base)) {
    return base;
  }
  for (int i = 1;; i++) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%03d", i);
    if (!bone_find_by_name(armature, base + suffix)) {
      return base + suffix;
    }
  }
}

Bone *armature_add_bone(Layer &armature, const std::string &name, float2 head, float2 tail, uint64_t parent)
{
  const std::vector<BonePose> poses = armature_pose(armature);
  const BonePose *pp = bone_pose_find(poses, parent);
  Bone bone;
  bone.id = armature.next_bone_id++;
  bone.name = armature_unique_bone_name(armature, name.empty() ? "Bone" : name);
  bone.parent = pp ? parent : 0;
  bone_place(bone, pp ? pp->setup : Transform2D(), head, tail);
  /* A new bone joins the active collection, as in Blender. */
  if (bone_collection_find(armature, armature.active_collection)) {
    bone.collections.push_back(armature.active_collection);
  }
  armature_deselect_all(armature);
  bone_select(bone, true);
  armature.active_bone = bone.id;
  armature.bones.push_back(std::move(bone));
  return &armature.bones.back();
}

/* Every layer inside the bone layer, any depth. */
static void layers_below(Layer &layer, std::vector<Layer *> &r)
{
  for (Layer &child : layer.children) {
    r.push_back(&child);
    layers_below(child, r);
  }
}

/* The layers being edited that weights apply to: drawings inside a bone layer with bones. (Giving
 * one weights takes it off its parent bone.) */
static std::vector<Layer *> weight_layers(Context &C)
{
  std::vector<Layer *> r;
  for (Layer *layer : editable_layers(C)) {
    const Layer *armature = layer_armature(*C.document, layer->id);
    if (armature && !armature->bones.empty()) {
      r.push_back(layer);
    }
  }
  return r;
}

/* Keep Animation (keep_animation=1) around a weight change: the drawings stay where they show on every
 * keyed frame of every action they play in, their points keyed back there (anim/keep_poses.hh). From
 * whatever frame and action it is done on. */
static DrawingsShownAll weight_keep_begin(Context &C)
{
  if (!op_arg_bool(C, "keep_animation")) {
    return {};
  }
  std::vector<uint64_t> ids;
  for (Layer *layer : weight_layers(C)) {
    ids.push_back(layer->id);
  }
  return drawings_shown_capture_all(*C.document, ids);
}

static OpStatus weight_keep_end(Context &C, const DrawingsShownAll &kept, bool changed)
{
  if (!changed) {
    return OpStatus::Cancelled;
  }
  drawings_shown_keep_all(*C.document, kept);
  return OpStatus::Finished;
}

static bool weight_poll(Context &C)
{
  return C.document && !weight_layers(C).empty();
}

static bool any_point_selected(const Layer &layer)
{
  return std::any_of(layer.drawing.points.begin(), layer.drawing.points.end(),
                     [](const MeshPoint &p) { return p.selected; });
}

/* The collection an operator names (`collection`, by name), or the active one; null for All (and for
 * none of that name). */
static BoneCollection *collection_arg(Context &C, Layer &armature)
{
  const std::string name = op_arg(C, "collection");
  return name.empty() ? bone_collection_find(armature, armature.active_collection)
                      : bone_collection_find_by_name(armature, name);
}

/* Whether an operator means All: by name, or, named nothing, with All active. */
static bool all_arg(Context &C, const Layer &armature)
{
  const std::string name = op_arg(C, "collection");
  return name == BONE_COLLECTION_ALL || (name.empty() && !bone_collection_find(armature, armature.active_collection));
}

static bool in_collection(const std::vector<uint64_t> &ids, uint64_t id)
{
  return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/* The settings of a constraint of `type` worth keeping in step with another's (Copy Constraints with
 * Drivers): Enabled and Influence, and those its type uses (the bone it aims at can't be driven). */
static std::vector<std::string> bone_constraint_driven_fields(BoneConstraintType type)
{
  std::vector<std::string> fields = {"enabled", "influence"};
  switch (type) {
    case BoneConstraintType::LimitAngle:
      fields.insert(fields.end(), {"min_angle", "max_angle"});
      break;
    case BoneConstraintType::MaintainVolume:
      fields.push_back("amount");
      break;
    case BoneConstraintType::CopyAngle:
    case BoneConstraintType::CopyPosition:
    case BoneConstraintType::CopyScale:
      fields.insert(fields.end(), {"factor", "delay"});
      break;
    case BoneConstraintType::IK:
      fields.insert(fields.end(), {"chain", "max_stretch"});
      break;
    case BoneConstraintType::LimitShape:
      fields.insert(fields.end(), {"overlap", "softness"});
      break;
    case BoneConstraintType::LookAt:
      fields.insert(fields.end(), {"squash", "divergence"});
      break;
    case BoneConstraintType::Dynamics:
      fields.insert(fields.end(), {"weight", "wind_catch", "own_gravity", "own_wind", "gravity", "wind",
                                   "wind_direction", "wind_gusts", "wind_gust_speed", "wind_turbulence"});
      for (const char *spring : {"angle_spring.", "position_spring.", "scale_spring."}) {
        for (const char *f : {"enabled", "use_limit", "settle_speed", "bounce", "inertia", "stretch", "limit"}) {
          fields.push_back(std::string(spring) + f);
        }
      }
      for (const char *f : {"use_squash_limit", "squash_limit"}) {
        fields.push_back(std::string("scale_spring.") + f);
      }
      break;
    case BoneConstraintType::IndependentAngle:
    case BoneConstraintType::TrackTo:
    case BoneConstraintType::StretchTo:
      break;
  }
  return fields;
}

/* Parent Bone's Keep Animation: the frames worth keying when a layer's bone changes, so it shows where
 * it did. Every keyed frame (bar the setup) of what places it: its own and its groups' transforms, the
 * bone layer's and its groups', and every bone of the bone layer (they pose the old and the new bone). */
static std::vector<int> parent_change_frames(Document &doc, const Layer &layer, const Layer &armature)
{
  std::set<int> frames;
  const auto add = [&](const auto &ch) {
    for (const auto &k : ch.keys) {
      if (k.frame != 0) {
        frames.insert(k.frame);
      }
    }
  };
  for (const uint64_t id : {layer.id, armature.id}) {
    for (const Layer *l = layer_find(doc, id); l; l = layer_parent(doc, l->id)) {
      add(l->position_keys);
      add(l->rotation_keys);
      add(l->scale_keys);
    }
  }
  for (const Bone &b : armature.bones) {
    add(b.position_keys);
    add(b.angle_keys);
    add(b.scale_keys);
  }
  return {frames.begin(), frames.end()};
}

/* A layer's position, rotation (degrees, the turn nearest `near`) and scale from its local transform
 * (translate * rotate * scale; a negative X scale kept as it was). */
static void local_from_transform(const Transform2D &local, float2 old_scale, float near, float2 *r_position,
                                 float *r_rotation, float2 *r_scale)
{
  const float sign = old_scale.x < 0.0f ? -1.0f : 1.0f;
  const float sx = std::sqrt(local.a * local.a + local.b * local.b) * sign;
  *r_position = {local.tx, local.ty};
  float rotation = std::atan2(local.b * sign, local.a * sign) * 180.0f / PI;
  rotation += 360.0f * std::round((near - rotation) / 360.0f);
  *r_rotation = rotation;
  *r_scale = {sx, std::abs(sx) > 1e-9f ? (local.a * local.d - local.b * local.c) / sx : old_scale.y};
}

void rig_operators_register()
{
  rig_operator("layer.new_bone", "New Bone Layer",
               "Add a bone layer: a group whose bones move the layers inside it", nullptr, [](Context &C) {
                 document_add_layer(*C.document, LayerType::Bone, "");
                 return OpStatus::Finished;
               });

  rig_operator("layer.convert_to_bone", "Convert to Bone Group",
               "Turn the active group into a bone group, whose bones move the layers inside it",
               [](Context &C) {
                 const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
                 return l && l->type == LayerType::Group;
               },
               [](Context &C) {
                 document_active_layer(*C.document)->type = LayerType::Bone;
                 document_active_layer(*C.document)->edit_descendants = false;
                 return OpStatus::Finished;
               });
  rig_operator("layer.convert_to_group", "Convert to Group",
               "Turn the active bone group into a plain group; its bones are removed and the layers inside "
               "stop following them",
               [](Context &C) {
                 const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
                 return l && l->is_bone_layer();
               },
               [](Context &C) {
                 Layer &layer = *document_active_layer(*C.document);
                 layer.type = LayerType::Group;
                 layer.edit_descendants = true;
                 layer.bones.clear();
                 layer.active_bone = 0;
                 return OpStatus::Finished;
               });

  const auto bone_layer_active = [](Context &C) {
    const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
    return l && l->is_bone_layer();
  };
  rig_operator("armature.bone_swap_set", "Set Bone Swap",
               "Make a bone (bone=, else the active one) of the active bone layer stand in for bone target= of bone "
               "layer layer= (another child of the same switch): which bone is which across the switch's "
               "drawings. Without target= it stands in for none",
               bone_layer_active, [](Context &C) {
                 Document &doc = *C.document;
                 Layer &armature = *active_armature(C);
                 Bone *bone = op_has_arg(C, "bone") ? const_cast<Bone *>(bone_find_by_name(armature, op_arg(C, "bone")))
                                                    : bone_find(armature, armature.active_bone);
                 if (!bone) {
                   report(ReportType::Warning, "No such bone");
                   return OpStatus::Cancelled;
                 }
                 if (op_arg(C, "target").empty()) {
                   bone->swap_layer = bone->swap_bone = 0;
                   C.undo_name = bone->name + ": No Swap";
                   return OpStatus::Finished;
                 }
                 const Layer *other = layer_by_name_or_id(doc, op_arg(C, "layer"));
                 const Bone *target = other && other->is_bone_layer() ? bone_find_by_name(*other, op_arg(C, "target")) : nullptr;
                 if (!target || other->id == armature.id) {
                   report(ReportType::Warning, "Name a bone layer (layer=) and one of its bones (target=)");
                   return OpStatus::Cancelled;
                 }
                 if (bone_swap_root(doc, other->id, target->id) == std::pair<uint64_t, uint64_t>{armature.id, bone->id}) {
                   report(ReportType::Warning, "\"" + other->name + ": " + target->name + "\" already stands in for \"" +
                                                   armature.name + ": " + bone->name + "\": that would go round in a loop");
                   return OpStatus::Cancelled;
                 }
                 bone->swap_layer = other->id;
                 bone->swap_bone = target->id;
                 C.undo_name = bone->name + " Stands In for " + other->name + ": " + target->name;
                 return OpStatus::Finished;
               });
  rig_operator("armature.bone_link_all", "Link All Channels",
               "Link a bone (bone=, else the active one) of a switch's child two-way with the bone it stands in for: "
               "its angle, position and scale share that bone's keys at factor 1, editable from either (for bones "
               "that really are the same bone across drawings)",
               bone_layer_active, [](Context &C) {
                 Document &doc = *C.document;
                 Layer &armature = *active_armature(C);
                 const Bone *bone = op_has_arg(C, "bone") ? bone_find_by_name(armature, op_arg(C, "bone"))
                                                          : bone_find(armature, armature.active_bone);
                 const Layer *other = bone && bone->swap_layer ? layer_find(doc, bone->swap_layer) : nullptr;
                 const Bone *partner = other ? bone_find(*other, bone->swap_bone) : nullptr;
                 if (!partner) {
                   report(ReportType::Warning, "Give the bone a swap first (Stands In For, or Auto-link by Name)");
                   return OpStatus::Cancelled;
                 }
                 animation_record(doc);
                 const std::string own = "bones[\"" + bone->name + "\"].", theirs = "bones[\"" + partner->name + "\"].";
                 const uint64_t own_layer = armature.id, their_layer = other->id;
                 int linked = 0;
                 for (const auto &[part, component] : {std::pair{"angle", -1}, std::pair{"position", 0}, std::pair{"position", 1},
                                                       std::pair{"scale", 0}, std::pair{"scale", 1}})
                 {
                   const DriverRef target{own_layer, own + part, component};
                   std::erase_if(doc.drivers, [&](const Driver &d) { return d.target == target; });
                   Driver d;
                   d.target = target;
                   d.vars.push_back({"var", DriverRef{their_layer, theirs + part, component}});
                   doc.drivers.push_back(d);
                   std::string why;
                   int dropped = 0;
                   if (!driver_two_way_link(doc, doc.drivers.back(), false, &why, &dropped)) {
                     doc.drivers.pop_back();
                     report(ReportType::Warning, why);
                     continue;
                   }
                   linked++;
                 }
                 if (linked == 0) {
                   return OpStatus::Cancelled;
                 }
                 drivers_evaluate(doc);
                 report(ReportType::Info, "\"" + bone->name + "\" shares " + std::to_string(linked) + " channels with \"" +
                                              other->name + ": " + partner->name + "\"");
                 C.undo_name = "Link All Channels";
                 return OpStatus::Finished;
               });
  rig_operator("armature.swaps_auto_link", "Auto-link by Name",
               "In the switch holding the active layer (or layer=), each bone of its children's bone layers stands in "
               "for the bone of the same name in the first child with bones, unless it has a swap already",
               nullptr, [](Context &C) {
                 Document &doc = *C.document;
                 const Layer *named = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : nullptr;
                 Layer *sw = switch_of(doc, named ? named->id : doc.active_layer);
                 if (!sw) {
                   report(ReportType::Warning, "Not a switch layer, nor inside one");
                   return OpStatus::Cancelled;
                 }
                 const int made = switch_swaps_auto_link(doc, *sw);
                 report(ReportType::Info, std::to_string(made) + (made == 1 ? " bone" : " bones") + " of \"" + sw->name +
                                              (made == 1 ? "\" now stands in by name" : "\" now stand in by name"));
                 if (made == 0) {
                   return OpStatus::Cancelled;
                 }
                 C.undo_name = sw->name + ": Auto-link Swaps";
                 return OpStatus::Finished;
               });
  rig_operator("armature.match_pose_previous", "Match Pose from Previous Drawing",
               "Pose the active bone layer's bones (a child of a switch, or inside one) as their counterparts in the "
               "drawing the switch showed before this hold lie on this frame, and key them: no pop at the swap",
               bone_layer_active, [](Context &C) {
                 Document &doc = *C.document;
                 if (doc.frame_current == 0) {
                   report_banner(ReportType::Warning, "Match Pose keys a pose: go to a frame after the setup (frame 0)");
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *active_armature(C);
                 Layer *sw = switch_of(doc, armature.id);
                 const Layer *child = sw ? switch_child_holding(doc, *sw, armature.id) : nullptr;
                 if (!child) {
                   report(ReportType::Warning, "\"" + armature.name + "\" isn't inside a switch");
                   return OpStatus::Cancelled;
                 }
                 /* The hold this frame is in starts at the switch key at or before it; before it, the
                  * switch's keys showed the previous drawing. */
                 const SwitchLink *link = switch_link_of(doc, *sw);
                 const Layer *keyed = link && layer_find(doc, link->lead) ? layer_find(doc, link->lead) : sw;
                 int start = 0;
                 for (const Key<float> &k : keyed->switch_keys.keys) {
                   if (k.frame <= doc.frame_current) {
                     start = k.frame;
                   }
                 }
                 Layer *previous = start > 0 ? const_cast<Layer *>(switch_child(*sw, float(switch_child_at(doc, *sw, start - 1))))
                                             : nullptr;
                 const Layer *from = previous ? switch_child_armature(*previous) : nullptr;
                 if (start == 0 || !from || previous->id == child->id) {
                   report(ReportType::Warning, "\"" + sw->name + "\" shows no other drawing with bones before this hold");
                   return OpStatus::Cancelled;
                 }
                 animation_record(doc);
                 const int count = armature_match_pose(doc, armature, *from);
                 if (count == 0) {
                   report(ReportType::Warning, "No bone of \"" + armature.name + "\" stands in for one of \"" + from->name +
                                                   "\" (Bone panel: Stands In For, or Auto-link by Name)");
                   return OpStatus::Cancelled;
                 }
                 /* Keyed whatever Auto Keying says: matching is for this frame. */
                 const bool auto_key = doc.auto_key;
                 doc.auto_key = true;
                 animation_record(doc);
                 doc.auto_key = auto_key;
                 report(ReportType::Info, "Posed " + std::to_string(count) + (count == 1 ? " bone" : " bones") + " of \"" +
                                              armature.name + "\" as \"" + from->name + "\"'s");
                 C.undo_name = "Match Pose from " + from->name;
                 return OpStatus::Finished;
               });
  rig_operator("armature.select_used_bones", "Select Bones This Drawing Uses",
               "Select the bones the active drawing follows (its points' weights and its Parent Bone), in its bone "
               "layer, which becomes the active layer",
               [](Context &C) {
                 const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
                 return l && l->has_drawing() && layer_armature(*C.document, l->id);
               },
               [](Context &C) {
                 Document &doc = *C.document;
                 const Layer &drawing = *document_active_layer(doc);
                 Layer &armature = *layer_find(doc, layer_armature(doc, drawing.id)->id);
                 std::set<uint64_t> used;
                 if (drawing.parent_bone) {
                   used.insert(drawing.parent_bone);
                 }
                 for (const MeshPoint &p : drawing.drawing.points) {
                   for (const BoneWeight &w : p.weights) {
                     if (w.weight > 0.0f) {
                       used.insert(w.bone);
                     }
                   }
                 }
                 int count = 0;
                 for (Bone &b : armature.bones) {
                   const bool on = used.contains(b.id) && bone_shown(armature, b);
                   bone_select(b, on);
                   if (on) {
                     armature.active_bone = count++ == 0 ? b.id : armature.active_bone;
                   }
                 }
                 doc.active_layer = armature.id;
                 report(ReportType::Info, "\"" + drawing.name + "\" uses " + std::to_string(count) +
                                              (count == 1 ? " bone" : " bones") + " of \"" + armature.name + "\"");
                 return OpStatus::Finished;
               });
  rig_operator("armature.select_all", "Select All Bones", "Select every bone of the bone layer (A)",
               bone_layer_active, [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 for (Bone &b : armature.bones) {
                   bone_select(b, bone_shown(armature, b));
                 }
                 if (!bone_find(armature, armature.active_bone) && !armature.bones.empty()) {
                   armature.active_bone = armature.bones.front().id;
                 }
                 return OpStatus::Finished;
               });
  rig_operator("armature.select_invert", "Invert Bone Selection",
               "Select the bones that aren't selected and deselect those that are (Ctrl I), as Blender's Invert; "
               "hidden bones stay unselected",
               bone_layer_active, [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 for (Bone &b : armature.bones) {
                   /* A bone with only one end picked counts as picked, as it shows. */
                   const bool picked = b.selected || b.select_head || b.select_tail;
                   bone_select(b, !picked && bone_shown(armature, b));
                 }
                 const Bone *active = bone_find(armature, armature.active_bone);
                 if (!active || !active->selected) {
                   for (const Bone &b : armature.bones) {
                     if (b.selected) {
                       armature.active_bone = b.id;
                       break;
                     }
                   }
                 }
                 return OpStatus::Finished;
               });
  rig_operator("armature.select_none", "Deselect All Bones", "Deselect every bone of the bone layer (Alt A)",
               bone_layer_active, [](Context &C) {
                 armature_deselect_all(*document_active_layer(*C.document));
                 return OpStatus::Finished;
               });

  rig_operator("armature.bone_add", "Add Bone",
               "Add a bone to the bone layer from (head_x, head_y) to (tail_x, tail_y), in the bone layer's "
               "space, as a child of the active bone",
               armature_poll, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *active_armature(C);
                 const float2 head(op_arg_float(C, "head_x", 0.0f), op_arg_float(C, "head_y", 0.0f));
                 const float2 tail(op_arg_float(C, "tail_x", 0.5f), op_arg_float(C, "tail_y", 0.0f));
                 if (distance(head, tail) < 1e-5f) {
                   report(ReportType::Warning, "A bone needs some length");
                   return OpStatus::Cancelled;
                 }
                 const std::string parent_name = op_arg(C, "parent", "\x01");
                 /* By default the new bone hangs from the selected (active) bone, if any. */
                 const Bone *active = bone_find(armature, armature.active_bone);
                 uint64_t parent = active && active->selected ? active->id : 0;
                 if (parent_name != "\x01") {
                   const Bone *p = bone_find_by_name(armature, parent_name);
                   parent = p ? p->id : 0;
                 }
                 armature_add_bone(armature, op_arg(C, "name"), head, tail, parent);
                 return OpStatus::Finished;
               });

  rig_operator("armature.bone_delete", "Delete Bones",
               "Delete the selected bones; their children move up to the deleted bone's parent",
               [](Context &C) {
                 /* With no bone picked, X goes on to delete points (of layers marked Editable in Bone
                  * Group). */
                 const Layer *active = C.document ? document_active_layer(*C.document) : nullptr;
                 return active && active->is_bone_layer() && armature_any_picked(*active);
               },
               [](Context &C) {
                 Layer &armature = *active_armature(C);
                 std::vector<uint64_t> doomed;
                 /* A bone picked by its body or either ball goes. */
                 for (const Bone &b : armature.bones) {
                   if (b.selected || b.select_head || b.select_tail) {
                     doomed.push_back(b.id);
                   }
                 }
                 if (doomed.empty()) {
                   report(ReportType::Warning, "Select bones to delete");
                   return OpStatus::Cancelled;
                 }
                 if (C.wm->tool_settings.mirror_x) {
                   for (size_t i = 0, count = doomed.size(); i < count; i++) {
                     const uint64_t other = bone_mirror_id(armature, doomed[i]);
                     if (std::find(doomed.begin(), doomed.end(), other) == doomed.end()) {
                       doomed.push_back(other);
                     }
                   }
                 }
                 const std::vector<BonePose> poses = armature_pose(armature);
                 const auto gone = [&](uint64_t id) { return std::find(doomed.begin(), doomed.end(), id) != doomed.end(); };
                 for (Bone &b : armature.bones) {
                   if (gone(b.id) || !gone(b.parent)) {
                     continue;
                   }
                   /* Keep the child where it is, under the nearest surviving ancestor. */
                   uint64_t parent = b.parent;
                   while (parent != 0 && gone(parent)) {
                     const Bone *p = bone_find(armature, parent);
                     parent = p ? p->parent : 0;
                   }
                   const BonePose *self = bone_pose_find(poses, b.id);
                   const BonePose *pp = bone_pose_find(poses, parent);
                   const float length = b.length;
                   bone_place(b, pp ? pp->setup : Transform2D(), self->setup_head, self->setup_tail);
                   b.length = length;
                   b.parent = parent;
                   b.connected = false;
                 }
                 std::erase_if(armature.bones, [&](const Bone &b) { return gone(b.id); });
                 /* Points lose the deleted bones' weights; one that followed only them stays put. */
                 std::vector<Layer *> below;
                 layers_below(armature, below);
                 for (Layer *layer : below) {
                   for (MeshPoint &p : layer->drawing.points) {
                     const bool had = !p.weights.empty();
                     std::erase_if(p.weights, [&](const BoneWeight &w) { return gone(w.bone); });
                     if (had && p.weights.empty()) {
                       p.weights.push_back({0, 0.0f});
                     }
                   }
                   if (gone(layer->parent_bone)) {
                     layer->parent_bone = 0;
                   }
                 }
                 if (gone(armature.active_bone)) {
                   armature.active_bone = 0;
                 }
                 return OpStatus::Finished;
               });

  rig_operator("armature.bone_parent_set", "Set Parent",
               "Give bone `bone` (default: the active bone) a new parent bone `parent` (\"\" for none), keeping "
               "it where it is",
               armature_poll, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *active_armature(C);
                 const std::string bone_name = op_arg(C, "bone");
                 Bone *bone = bone_name.empty() ? bone_find(armature, armature.active_bone)
                                                : const_cast<Bone *>(bone_find_by_name(armature, bone_name));
                 if (!bone) {
                   report(ReportType::Warning, "No such bone");
                   return OpStatus::Cancelled;
                 }
                 const std::string parent_name = op_arg(C, "parent");
                 const Bone *parent = parent_name.empty() ? nullptr : bone_find_by_name(armature, parent_name);
                 if (!parent_name.empty() && !parent) {
                   report(ReportType::Warning, "No bone named " + parent_name);
                   return OpStatus::Cancelled;
                 }
                 const uint64_t parent_id = parent ? parent->id : 0;
                 if (parent_id != 0 && bone_is_descendant(armature, parent_id, bone->id)) {
                   report_banner(ReportType::Warning, "A bone can't hang from itself or one of its children");
                   return OpStatus::Cancelled;
                 }
                 if (parent_id == bone->parent) {
                   return OpStatus::Cancelled;
                 }
                 /* Keep the bone where it is in the setup pose, now measured from the new parent. */
                 const std::vector<BonePose> poses = armature_pose(armature);
                 const BonePose *self = bone_pose_find(poses, bone->id);
                 const BonePose *pp = bone_pose_find(poses, parent_id);
                 const float length = bone->length;
                 const float2 scale = bone->scale;
                 bone_place(*bone, pp ? pp->setup : Transform2D(), self->setup_head, self->setup_tail);
                 bone->length = length;
                 bone->scale = scale;
                 bone->parent = parent_id;
                 bone->connected = false;
                 return OpStatus::Finished;
               });

  /* --- Weights (Blender's vertex groups) and Parent to Bone -------------------------------- */
  const OpProperty keep_animation_property = op_bool_property(
      "keep_animation", "Keep Animation", "The drawings show where they did on every keyed frame (their points keyed back)",
      "0");
  const OpProperty weight_property = op_float_property("weight", "Weight", "The active bone's weight on the selected points",
                                                       "1", 0.0f, 1.0f, 0.01f);
  rig_operator("weight.automatic", "Automatic Weights",
               "Give the selected points (all points when none are selected) of the layers being edited "
               "weights from every bone: nearer bones pull harder, as Blender's Automatic Weights. "
               "bones=SELECTED uses only the selected bones",
               weight_poll, [](Context &C) {
                 const DrawingsShownAll kept = weight_keep_begin(C);
                 /* Every bone by default: the weight tools keep the bone being painted selected, so going
                  * by the selection would hand that one bone everything. */
                 const bool selected_bones = op_arg(C, "bones", "ALL") == "SELECTED";
                 int changed = 0;
                 for (Layer *layer : weight_layers(C)) {
                   const Layer &armature = *layer_armature(*C.document, layer->id);
                   std::vector<uint64_t> bones;
                   for (const Bone &b : armature.bones) {
                     if (selected_bones && (b.selected || b.select_head || b.select_tail)) {
                       bones.push_back(b.id);
                     }
                   }
                   if (C.wm->tool_settings.mirror_x) {
                     for (size_t i = 0, n = bones.size(); i < n; i++) {
                       const uint64_t other = bone_mirror_id(armature, bones[i]);
                       if (std::find(bones.begin(), bones.end(), other) == bones.end()) {
                         bones.push_back(other);
                       }
                     }
                   }
                   changed += layer_automatic_weights(*C.document, *layer, bones, any_point_selected(*layer));
                 }
                 return weight_keep_end(C, kept, changed > 0);
               },
               {keep_animation_property});

  rig_operator("weight.assign", "Assign",
               "Set the active bone's weight on the selected points to `weight` (default: the tool's Weight), as "
               "Blender's Assign; with Normalize the other bones share what is left",
               weight_poll, [](Context &C) {
                 const DrawingsShownAll kept = weight_keep_begin(C);
                 const ToolSettings &ts = C.wm->tool_settings;
                 const float value = std::clamp(op_arg_float(C, "weight", ts.weight_value), 0.0f, 1.0f);
                 /* The panel shows the weight it used (the tool's, when none was given). */
                 op_arg_used(C, "weight", std::to_string(value));
                 int changed = 0;
                 for (Layer *layer : weight_layers(C)) {
                   const uint64_t bone = layer_armature(*C.document, layer->id)->active_bone;
                   if (bone == 0 || !any_point_selected(*layer)) {
                     continue;
                   }
                   if (layer->parent_bone != 0) {
                     /* Weighted points follow their weights, not a parent bone. */
                     layer->parent_bone = 0;
                     report(ReportType::Info,
                            "\"" + layer->name + "\" now follows its weights instead of its parent bone");
                   }
                   for (MeshPoint &p : layer->drawing.points) {
                     if (!p.selected) {
                       continue;
                     }
                     std::erase_if(p.weights, [](const BoneWeight &w) { return w.bone == 0; });
                     auto it = std::find_if(p.weights.begin(), p.weights.end(),
                                            [&](const BoneWeight &w) { return w.bone == bone; });
                     if (it == p.weights.end()) {
                       p.weights.push_back({bone, 0.0f});
                       it = std::prev(p.weights.end());
                     }
                     it->weight = value;
                     if (ts.weight_normalize) {
                       float others = 0.0f;
                       for (const BoneWeight &w : p.weights) {
                         others += w.bone == bone ? 0.0f : w.weight;
                       }
                       for (BoneWeight &w : p.weights) {
                         if (w.bone != bone) {
                           w.weight = others > 1e-6f ? w.weight / others * (1.0f - value) : 0.0f;
                         }
                       }
                       std::erase_if(p.weights, [&](const BoneWeight &w) { return w.bone != bone && w.weight <= 1e-5f; });
                     }
                     changed++;
                   }
                 }
                 if (changed == 0) {
                   report(ReportType::Warning, "Select points and a bone to assign");
                   return OpStatus::Cancelled;
                 }
                 return weight_keep_end(C, kept, true);
               },
               {weight_property, keep_animation_property});

  rig_operator("weight.remove", "Remove",
               "Take the selected points out of the active bone (as Blender's Remove); a point left with no bones "
               "stays where it is",
               weight_poll, [](Context &C) {
                 const DrawingsShownAll kept = weight_keep_begin(C);
                 int changed = 0;
                 for (Layer *layer : weight_layers(C)) {
                   const uint64_t bone = layer_armature(*C.document, layer->id)->active_bone;
                   for (MeshPoint &p : layer->drawing.points) {
                     if (!p.selected) {
                       continue;
                     }
                     const size_t before = p.weights.size();
                     std::erase_if(p.weights, [&](const BoneWeight &w) { return w.bone == bone; });
                     if (p.weights.size() != before) {
                       changed++;
                     }
                     if (p.weights.empty()) {
                       p.weights.push_back({0, 0.0f});
                     }
                   }
                 }
                 return weight_keep_end(C, kept, changed > 0);
               },
               {keep_animation_property});

  rig_operator("weight.normalize", "Normalize",
               "Make each selected point's weights (all points' when none are selected) add up to 1",
               weight_poll, [](Context &C) {
                 const DrawingsShownAll kept = weight_keep_begin(C);
                 int changed = 0;
                 for (Layer *layer : weight_layers(C)) {
                   const bool selected_only = any_point_selected(*layer);
                   for (MeshPoint &p : layer->drawing.points) {
                     if (selected_only && !p.selected) {
                       continue;
                     }
                     float total = 0.0f;
                     for (const BoneWeight &w : p.weights) {
                       total += w.weight;
                     }
                     if (total > 1e-6f && std::abs(total - 1.0f) > 1e-6f) {
                       for (BoneWeight &w : p.weights) {
                         w.weight /= total;
                       }
                       changed++;
                     }
                   }
                 }
                 return weight_keep_end(C, kept, changed > 0);
               },
               {keep_animation_property});

  rig_operator("weight.remove_all", "Remove All Weights",
               "Take the selected points out of every bone: they follow no bone and stay where they are (with "
               "Keep Animation, where they show on every keyed frame too)",
               weight_poll,
               [](Context &C) {
                 const DrawingsShownAll kept = weight_keep_begin(C);
                 int changed = 0;
                 bool any = false;
                 for (Layer *layer : weight_layers(C)) {
                   for (MeshPoint &p : layer->drawing.points) {
                     if (!p.selected) {
                       continue;
                     }
                     any = true;
                     const bool weighted = std::any_of(p.weights.begin(), p.weights.end(),
                                                       [](const BoneWeight &w) { return w.weight > 0.0f; });
                     /* An empty entry: weighted to nothing (new points aren't given automatic weights). */
                     p.weights = {{0, 0.0f}};
                     changed += weighted ? 1 : 0;
                   }
                 }
                 if (!any) {
                   report(ReportType::Warning, "Select points first");
                   return OpStatus::Cancelled;
                 }
                 return weight_keep_end(C, kept, changed > 0);
               },
               {keep_animation_property});

  rig_operator("weight.clean", "Clean",
               "Take the selected points (all points when none are selected) out of the bones that move them less "
               "than `threshold` (0.02: 2%), as Blender's Clean; the bones left share what the removed ones had, "
               "and a point moved only that weakly keeps its strongest bone",
               weight_poll,
               [](Context &C) {
                 const DrawingsShownAll kept = weight_keep_begin(C);
                 const float threshold = std::clamp(op_arg_float(C, "threshold", 0.02f), 0.0f, 1.0f);
                 int changed = 0;
                 for (Layer *layer : weight_layers(C)) {
                   const bool selected_only = any_point_selected(*layer);
                   for (MeshPoint &p : layer->drawing.points) {
                     if (selected_only && !p.selected) {
                       continue;
                     }
                     float total = 0.0f, strongest = 0.0f;
                     for (const BoneWeight &w : p.weights) {
                       total += w.weight;
                       strongest = std::max(strongest, w.weight);
                     }
                     const auto weak = [&](const BoneWeight &w) {
                       return w.weight > 0.0f && w.weight < threshold && w.weight < strongest;
                     };
                     if (std::none_of(p.weights.begin(), p.weights.end(), weak)) {
                       continue;
                     }
                     std::erase_if(p.weights, weak);
                     float left = 0.0f;
                     for (const BoneWeight &w : p.weights) {
                       left += w.weight;
                     }
                     if (left > 1e-6f) {
                       for (BoneWeight &w : p.weights) {
                         w.weight *= total / left;
                       }
                     }
                     changed++;
                   }
                 }
                 if (changed > 0) {
                   report(ReportType::Info, "Cleaned " + std::to_string(changed) + (changed == 1 ? " point" : " points"));
                 }
                 return weight_keep_end(C, kept, changed > 0);
               },
               {op_float_property("threshold", "Limit", "Bones moving a point less than this let go of it", "0.02",
                                  0.0f, 1.0f, 0.005f),
                keep_animation_property});

  rig_operator("layer.parent_bone_set", "Parent to Bone",
               "Make the active and selected layers inside a bone layer follow bone `bone` as a whole, as "
               "parenting to a bone in Blender (\"\" frees them); `deform`=LAYER moves the layer (a bone's "
               "squash and stretch squashes its lines too), POINTS its points (lines keep their width). "
               "Either alone, or both. keep_animation=1: the layer shows where it did on every keyed frame "
               "(its transform keyed again there), as Keep Poses does for setup moves. Setup frame only",
               [](Context &C) {
                 const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
                 return l && !l->is_bone_layer() && layer_armature(*C.document, l->id);
               },
               [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 const bool set_bone = op_has_arg(C, "bone");
                 const std::string name = op_arg(C, "bone");
                 const std::string deform = op_arg(C, "deform");
                 if (!deform.empty() && deform != "LAYER" && deform != "POINTS") {
                   report(ReportType::Error, "deform is LAYER or POINTS");
                   return OpStatus::Cancelled;
                 }
                 int changed = 0, same_as_original = 0;
                 /* Keep Animation: where each layer showed on the frames that matter, before its bone
                  * changes (worked out frame by frame, then the frame put back). */
                 Document &doc = *C.document;
                 const bool keep = op_arg_bool(C, "keep_animation") && set_bone;
                 struct Kept {
                   uint64_t layer = 0;
                   std::vector<int> frames;
                   std::vector<Transform2D> world, local;
                 };
                 /* Every action the layers play in (the mainline and their bone layers' actions), each
                  * with its own keyed frames. */
                 struct ActionKept {
                   uint64_t owner = 0;
                   std::string name;
                   std::vector<Kept> kept;
                 };
                 std::vector<ActionKept> actions;
                 const auto use_action = [&doc](uint64_t owner, const std::string &action) {
                   if (doc.action != action) {
                     document_set_action(doc, action.empty() ? 0 : owner, action);
                   }
                 };
                 const std::string was_action = doc.action;
                 uint64_t was_owner = 0;
                 if (keep) {
                   animation_record(doc);
                   actions.push_back({0, "", {}});
                   for (Layer *layer : keyed_layers(C)) {
                     if (const Layer *owner = action_owner(doc, layer->id)) {
                       for (const std::string &action : owner->actions) {
                         if (std::none_of(actions.begin(), actions.end(), [&](const ActionKept &a) {
                               return a.owner == owner->id && a.name == action;
                             }))
                         {
                           actions.push_back({owner->id, action, {}});
                         }
                       }
                     }
                   }
                   std::vector<uint64_t> ids;
                   for (Layer *layer : keyed_layers(C)) {
                     const Layer *armature = layer_armature(doc, layer->id);
                     if (!armature || layer->is_bone_layer() || (layer->points_follow_bone() && !set_bone) ||
                         (layer->is_reference() && layer->reference.match_original))
                     {
                       continue;
                     }
                     ids.push_back(layer->id);
                   }
                   for (ActionKept &a : actions) {
                     if (a.name == was_action) {
                       was_owner = a.owner;
                     }
                     use_action(a.owner, a.name);
                     for (const uint64_t id : ids) {
                       const Layer *layer = layer_find(doc, id);
                       a.kept.push_back({id, parent_change_frames(doc, *layer, *layer_armature(doc, id)), {}, {}});
                     }
                     for (Kept &k : a.kept) {
                       for (const int f : k.frames) {
                         doc.frame_current = f;
                         animation_sync(doc);
                         const Layer *l = layer_find(doc, k.layer);
                         k.world.push_back(layer_world_transform(doc, k.layer) * points_bone_move(doc, *l));
                         k.local.push_back(l->local_transform());
                       }
                     }
                     doc.frame_current = 0;
                     animation_sync(doc);
                   }
                   use_action(was_owner, was_action);
                 }
                 for (Layer *layer : keyed_layers(C)) {
                   const Layer *armature = layer_armature(*C.document, layer->id);
                   if (!armature || layer->is_bone_layer()) {
                     continue;
                   }
                   /* A copy posed Same as Original moves with its original's bones. */
                   if (layer->is_reference() && layer->reference.match_original) {
                     same_as_original++;
                     continue;
                   }
                   const Bone *bone = name.empty() ? nullptr : bone_find_by_name(*armature, name);
                   if (!name.empty() && !bone) {
                     report(ReportType::Warning, "No bone named " + name);
                     return OpStatus::Cancelled;
                   }
                   const uint64_t id = bone ? bone->id : 0;
                   if (set_bone && layer->parent_bone != id) {
                     layer->parent_bone = id;
                     changed++;
                   }
                   if (!deform.empty() && layer->parent_bone_points != (deform == "POINTS")) {
                     layer->parent_bone_points = deform == "POINTS";
                     changed++;
                   }
                 }
                 /* The actions first, the mainline last: a smart action's keys add to the mainline, so the
                  * mainline is put right with them already in place. */
                 for (auto action = actions.rbegin(); keep && changed > 0 && action != actions.rend(); ++action) {
                   use_action(action->owner, action->name);
                   /* The new bone's: what places the layer on each frame, before its own transform; the
                    * transform that puts it where it was, keyed there. */
                   std::vector<std::tuple<uint64_t, int, Transform2D>> keys;
                   for (const Kept &k : action->kept) {
                     for (size_t i = 0; i < k.frames.size(); i++) {
                       doc.frame_current = k.frames[i];
                       animation_sync(doc);
                       const Layer *l = layer_find(doc, k.layer);
                       const Transform2D world = layer_world_transform(doc, k.layer);
                       const Transform2D above = world * k.local[i].inverted();
                       /* Points that follow the bone are moved by it after the layer places them (its
                        * move in document space, the same whatever the layer's own transform): the
                        * layer's transform is what takes them back to where they showed. */
                       const Transform2D bone = world * points_bone_move(doc, *l) * world.inverted();
                       keys.push_back({k.layer, k.frames[i], above.inverted() * bone.inverted() * k.world[i]});
                     }
                   }
                   doc.frame_current = 0;
                   animation_sync(doc);
                   for (const auto &[id, frame, local] : keys) {
                     Layer *l = layer_find(doc, id);
                     float2 position, scale;
                     float rotation = 0.0f;
                     local_from_transform(local, l->scale, l->rotation_keys.animated() ? l->rotation_keys.evaluate(frame) : l->rotation,
                                          &position, &rotation, &scale);
                     /* Unkeyed channels get their setup key too, so the other frames stay as they were. */
                     if (!l->position_keys.animated()) {
                       l->position_keys.set_key(0, l->position);
                     }
                     if (!l->rotation_keys.animated()) {
                       l->rotation_keys.set_key(0, l->rotation);
                     }
                     if (!l->scale_keys.animated()) {
                       l->scale_keys.set_key(0, l->scale);
                     }
                     l->position_keys.set_key(frame, position);
                     l->rotation_keys.set_key(frame, rotation);
                     l->scale_keys.set_key(frame, scale);
                   }
                   animation_sync(doc);
                   C.undo_name = "Parent to Bone, Keeping Animation";
                 }
                 if (keep) {
                   use_action(was_owner, was_action);
                 }
                 if (changed == 0 && same_as_original > 0) {
                   report(ReportType::Warning, "Its pose is Same as Original: choose Own Bones in the Reference tab "
                                               "to give it a parent bone");
                 }
                 return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
               });

  rig_operator("armature.copy_bone_color_to_selected", "Copy Bone Color to Selected",
               "Give the other selected bones the active bone's colour set (and its custom colours), as "
               "Blender's",
               [](Context &C) {
                 const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
                 return l && l->is_bone_layer() && bone_find(*l, l->active_bone);
               },
               [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 const Bone from = *bone_find(armature, armature.active_bone);
                 int changed = 0;
                 for (Bone &b : armature.bones) {
                   const bool picked = b.selected || b.select_head || b.select_tail;
                   if (!picked || b.id == from.id) {
                     continue;
                   }
                   b.color_set = from.color_set;
                   b.custom_colors = from.custom_colors;
                   changed++;
                 }
                 if (changed == 0) {
                   report(ReportType::Warning, "Select other bones to copy this bone's colour to");
                   return OpStatus::Cancelled;
                 }
                 return OpStatus::Finished;
               });

  /* --- Bone constraints ------------------------------------------------------------------- */
  /* The bone named `bone` (default: the active one) of the active bone layer. */
  const auto constraint_bone = [](Context &C) -> Bone * {
    Layer *armature = C.document ? document_active_layer(*C.document) : nullptr;
    if (!armature || !armature->is_bone_layer()) {
      return nullptr;
    }
    const std::string name = op_arg(C, "bone");
    return name.empty() ? bone_find(*armature, armature->active_bone)
                        : const_cast<Bone *>(bone_find_by_name(*armature, name));
  };
  rig_operator("armature.constraint_add", "Add Bone Constraint",
               "Add a constraint (type=LIMIT_ANGLE, INDEPENDENT_ANGLE, MAINTAIN_VOLUME, COPY_ANGLE, COPY_POSITION, "
               "COPY_SCALE, TRACK_TO, STRETCH_TO, IK or DYNAMICS) to the bone `bone` (default: the active bone)",
               bone_layer_active, [constraint_bone](Context &C) {
                 Bone *bone = constraint_bone(C);
                 BoneConstraintType type;
                 if (!bone) {
                   report(ReportType::Warning, "Select a bone to add a constraint to");
                   return OpStatus::Cancelled;
                 }
                 if (!bone_constraint_from_identifier(op_arg(C, "type", "LIMIT_ANGLE"), &type)) {
                   report(ReportType::Error, "Unknown constraint type " + op_arg(C, "type"));
                   return OpStatus::Cancelled;
                 }
                 BoneConstraint c;
                 c.type = type;
                 c.name = bone_constraint_name(type);
                 const auto dynamics = std::find_if(bone->constraints.begin(), bone->constraints.end(), [](const auto &x) {
                   return x.type == BoneConstraintType::Dynamics;
                 });
                 if (type == BoneConstraintType::Dynamics) {
                   if (dynamics != bone->constraints.end()) {
                     report(ReportType::Warning, "A bone has one Dynamics constraint");
                     return OpStatus::Cancelled;
                   }
                   c.angle_spring.enabled = true;
                   bone->constraints.push_back(c);
                 }
                 else {
                   /* Dynamics stays last. */
                   bone->constraints.insert(dynamics, c);
                 }
                 return OpStatus::Finished;
               });
  rig_operator("armature.constraints_copy", "Copy Constraints to Selected Bones",
               "Ctrl L > Copy Constraints: every other selected bone's constraints (and their drivers) replaced "
               "by the active bone's (a constraint aimed at the bone it would go on is left out). drivers=1 "
               "(Copy Constraints with Drivers): each copy's settings are driven by the original's, so changing "
               "the original changes the copies",
               bone_layer_active, [](Context &C) {
                 const bool drive = op_arg_bool(C, "drivers");
                 Layer &armature = *document_active_layer(*C.document);
                 const Bone *active = bone_find(armature, armature.active_bone);
                 if (!active) {
                   report(ReportType::Warning, "Click a bone to make it the active one, then select the bones to "
                                               "give its constraints");
                   return OpStatus::Cancelled;
                 }
                 if (active->constraints.empty()) {
                   report(ReportType::Info, "The active bone \"" + active->name + "\" has no constraints");
                   return OpStatus::Cancelled;
                 }
                 const std::vector<BoneConstraint> copied = active->constraints;
                 const uint64_t from = active->id;
                 const std::string from_name = active->name;
                 /* Where each copy went, for its drivers: the bone, the original's place and the copy's. */
                 struct Placed {
                   std::string bone;
                   size_t from_index = 0;
                 };
                 constexpr uint64_t COPY_MARK = uint64_t(1) << 63;
                 std::vector<Placed> placed;
                 int bones = 0;
                 for (Bone &b : armature.bones) {
                   if (b.id == from || !(b.selected || b.select_head || b.select_tail)) {
                     continue;
                   }
                   /* Its own constraints go first, with the drivers on them: the copy is the active bone's
                    * stack, nothing mixed in (Dynamics stays last as it is there). */
                   b.constraints.clear();
                   const std::string own = "bones[\"" + b.name + "\"].constraints[";
                   std::erase_if(C.document->drivers, [&](const Driver &d) {
                     return d.target.layer == armature.id && d.target.path.rfind(own, 0) == 0;
                   });
                   for (size_t j = 0; j < copied.size(); j++) {
                     BoneConstraint c = copied[j];
                     if (c.target == b.id) {
                       continue;
                     }
                     /* A marker for finding the copy once all are in (cleared after). */
                     c.uid = COPY_MARK | uint64_t(placed.size());
                     b.constraints.push_back(c);
                     placed.push_back({b.name, j});
                   }
                   bones++;
                 }
                 if (bones == 0) {
                   report(ReportType::Info, "Select the bones to give the active bone's constraints");
                   return OpStatus::Cancelled;
                 }
                 int driven = 0;
                 if (drive) {
                   /* Each copy's settings read the original's: the copy's place in its stack found again
                    * once every copy is in (a later insert may have moved an earlier one along). */
                   Document &doc = *C.document;
                   for (size_t n = 0; n < placed.size(); n++) {
                     const Placed &p = placed[n];
                     const Bone *to = bone_find_by_name(armature, p.bone);
                     const BoneConstraint &original = copied[p.from_index];
                     int k = -1;
                     for (int i = 0; to && i < int(to->constraints.size()); i++) {
                       if (to->constraints[size_t(i)].uid == (COPY_MARK | uint64_t(n))) {
                         k = i;
                       }
                     }
                     if (k < 0) {
                       continue;
                     }
                     const std::string src = "bones[\"" + from_name + "\"].constraints[" + std::to_string(p.from_index) + "].";
                     const std::string dst = "bones[\"" + p.bone + "\"].constraints[" + std::to_string(k) + "].";
                     for (const std::string &field : bone_constraint_driven_fields(original.type)) {
                       if (driver_ref_size(doc, armature.id, dst + field) == 0) {
                         continue;
                       }
                       Driver d = driver_copying(DriverRef{armature.id, src + field, -1});
                       d.target = DriverRef{armature.id, dst + field, -1};
                       std::erase_if(doc.drivers, [&](const Driver &x) { return x.target == d.target; });
                       doc.drivers.push_back(std::move(d));
                       driven++;
                     }
                   }
                   C.undo_name = "Copy Constraints with Drivers";
                 }
                 for (Bone &b : armature.bones) {
                   for (BoneConstraint &c : b.constraints) {
                     if (c.uid & COPY_MARK) {
                       c.uid = 0;
                     }
                   }
                 }
                 if (drive) {
                   drivers_evaluate(*C.document);
                 }
                 report(ReportType::Info, "Copied " + from_name + "'s constraints to " + std::to_string(bones) +
                                              (bones == 1 ? " bone" : " bones") +
                                              (drive ? ", each setting driven by the original's (" +
                                                           std::to_string(driven) + " drivers)"
                                                     : std::string()));
                 return OpStatus::Finished;
               });
  rig_operator("armature.constraint_remove", "Remove Bone Constraint",
               "Remove constraint `index` of the bone `bone` (default: the active bone)", bone_layer_active,
               [constraint_bone](Context &C) {
                 Bone *bone = constraint_bone(C);
                 const int index = op_arg_int(C, "index", -1);
                 if (!bone || index < 0 || index >= int(bone->constraints.size())) {
                   return OpStatus::Cancelled;
                 }
                 bone->constraints.erase(bone->constraints.begin() + index);
                 return OpStatus::Finished;
               });
  rig_operator("armature.bone_relations_set", "Bone Relations",
               "Set how bone `bone` (default: the active bone) hangs from its parent: connected (1 or 0, setup frame "
               "only: its head onto the parent's tail), inherit_rotation (1 or 0), "
               "inherit_scale (FULL, UNIFORM or NONE), local_location (1 or 0: its Transform reads along it as it rests, "
               "or along its parent). keep_animation=1: every keyed frame of every action still shows the bone as it "
               "did (its angle and scale keyed back) after an Inherit change",
               bone_layer_active, [](Context &C) {
                 Document &doc = *C.document;
                 Layer &rig = *document_active_layer(doc);
                 const std::string bone_name = op_arg(C, "bone");
                 Bone *bone = bone_name.empty() ? bone_find(rig, rig.active_bone)
                                                : const_cast<Bone *>(bone_find_by_name(rig, bone_name));
                 if (!bone) {
                   report(ReportType::Warning, "No such bone");
                   return OpStatus::Cancelled;
                 }
                 if (op_has_arg(C, "connected")) {
                   /* A rig change, as Blender's edit mode: on the setup frame. Its head goes onto the
                    * parent's tail; its tail stays. */
                   const bool connect = op_arg_bool(C, "connected");
                   if (connect != bone->connected) {
                     if (!on_setup_frame(C)) {
                       return OpStatus::Cancelled;
                     }
                     if (connect && !bone_find(rig, bone->parent)) {
                       report(ReportType::Warning, "A bone needs a parent to connect to");
                       return OpStatus::Cancelled;
                     }
                     if (connect) {
                       const std::vector<BonePose> poses = armature_pose(rig);
                       const BonePose *self = bone_pose_find(poses, bone->id);
                       const BonePose *up = bone_pose_find(poses, bone->parent);
                       bone->connected = true;
                       armature_set_joints(rig, {{bone->id, {up->tail, self->tail}}});
                       bone = bone_find(rig, self->id);
                     }
                     else {
                       bone->connected = false;
                     }
                     if (!op_has_arg(C, "inherit_rotation") && !op_has_arg(C, "inherit_scale") &&
                         !op_has_arg(C, "local_location"))
                     {
                       return OpStatus::Finished;
                     }
                   }
                 }
                 bool inherit_rotation = bone->inherit_rotation, local_location = bone->local_location;
                 InheritScale inherit_scale = bone->inherit_scale;
                 if (op_has_arg(C, "inherit_rotation")) {
                   inherit_rotation = op_arg_bool(C, "inherit_rotation");
                 }
                 if (op_has_arg(C, "local_location")) {
                   local_location = op_arg_bool(C, "local_location");
                 }
                 if (op_has_arg(C, "inherit_scale")) {
                   const std::string s = op_arg(C, "inherit_scale");
                   if (s != "FULL" && s != "UNIFORM" && s != "NONE") {
                     report(ReportType::Warning, "inherit_scale is FULL, UNIFORM or NONE");
                     return OpStatus::Cancelled;
                   }
                   inherit_scale = s == "UNIFORM" ? InheritScale::Uniform : s == "NONE" ? InheritScale::None : InheritScale::Full;
                 }
                 const bool inherits = inherit_rotation != bone->inherit_rotation || inherit_scale != bone->inherit_scale;
                 if (!inherits && local_location == bone->local_location) {
                   return OpStatus::Cancelled;
                 }
                 const uint64_t rig_id = rig.id, bone_id = bone->id;
                 BonesShownAll kept;
                 if (inherits && op_arg_bool(C, "keep_animation")) {
                   kept = bones_shown_capture_all(doc, rig_id, {bone_id});
                 }
                 bone = bone_find(*layer_find(doc, rig_id), bone_id);
                 bone->inherit_rotation = inherit_rotation;
                 bone->inherit_scale = inherit_scale;
                 bone->local_location = local_location;
                 if (!kept.actions.empty()) {
                   const int keys = bones_shown_keep_all(doc, kept);
                   if (keys > 0) {
                     report(ReportType::Info, "Kept the animation: " + std::to_string(keys) + (keys == 1 ? " key" : " keys"));
                   }
                 }
                 animation_evaluate(doc, doc.frame_current);
                 return OpStatus::Finished;
               });
  rig_operator("armature.limit_angle_set_current", "Set to Current Angle",
               "Set the Min (end=MIN) or Max (end=MAX) of Limit Angle constraint `index` of the bone `bone` "
               "(default: the active bone) to the bone's turn now, as posed (before its constraints): its Angle "
               "in the Bone panel",
               bone_layer_active, [constraint_bone](Context &C) {
                 Bone *bone = constraint_bone(C);
                 const int index = op_arg_int(C, "index", -1);
                 if (!bone || index < 0 || index >= int(bone->constraints.size()) ||
                     bone->constraints[index].type != BoneConstraintType::LimitAngle)
                 {
                   return OpStatus::Cancelled;
                 }
                 BoneConstraint &c = bone->constraints[index];
                 (op_arg(C, "end", "MIN") == "MAX" ? c.max_angle : c.min_angle) = bone->angle - bone->setup_angle;
                 return OpStatus::Finished;
               });
  rig_operator("armature.constraint_move", "Move Bone Constraint",
               "Move constraint `index` up (direction=-1) or down (1) the stack; Dynamics stays last",
               bone_layer_active, [constraint_bone](Context &C) {
                 Bone *bone = constraint_bone(C);
                 const int index = op_arg_int(C, "index", -1);
                 const int to = index + (op_arg_int(C, "direction", 1) < 0 ? -1 : 1);
                 if (!bone || index < 0 || to < 0 || index >= int(bone->constraints.size()) ||
                     to >= int(bone->constraints.size()) ||
                     bone->constraints[index].type == BoneConstraintType::Dynamics ||
                     bone->constraints[to].type == BoneConstraintType::Dynamics)
                 {
                   return OpStatus::Cancelled;
                 }
                 std::swap(bone->constraints[index], bone->constraints[to]);
                 return OpStatus::Finished;
               });
  rig_operator("armature.constraint_move_to", "Move Bone Constraint",
               "Move constraint `index` to place `to` in the stack (dragging its box); Dynamics stays last",
               bone_layer_active, [constraint_bone](Context &C) {
                 Bone *bone = constraint_bone(C);
                 const int index = op_arg_int(C, "index", -1);
                 int to = op_arg_int(C, "to", -1);
                 if (!bone || index < 0 || index >= int(bone->constraints.size()) ||
                     bone->constraints[index].type == BoneConstraintType::Dynamics)
                 {
                   return OpStatus::Cancelled;
                 }
                 /* Nothing goes below Dynamics. */
                 int last = int(bone->constraints.size()) - 1;
                 if (bone->constraints.back().type == BoneConstraintType::Dynamics) {
                   last--;
                 }
                 to = std::clamp(to, 0, last);
                 if (to == index) {
                   return OpStatus::Cancelled;
                 }
                 BoneConstraint moved = std::move(bone->constraints[index]);
                 bone->constraints.erase(bone->constraints.begin() + index);
                 bone->constraints.insert(bone->constraints.begin() + to, std::move(moved));
                 return OpStatus::Finished;
               });
  rig_operator("armature.dynamics_bake", "Bake Dynamics",
               "Turn the bone dynamics into keys, from the simulation start to the end of the frame range, and switch "
               "the Dynamics constraints off (all=1: every bone layer)",
               [](Context &C) { return C.document != nullptr; },
               [](Context &C) {
                 Layer *active = document_active_layer(*C.document);
                 Layer *only = op_arg_bool(C, "all") || !active || !active->is_bone_layer() ? nullptr : active;
                 const int keys = dynamics_bake(*C.document, only);
                 if (keys == 0) {
                   report(ReportType::Warning, "No bone has dynamics to bake");
                   return OpStatus::Cancelled;
                 }
                 report(ReportType::Info, "Baked " + std::to_string(keys) + " keys");
                 /* Show the frame from its new keys. */
                 animation_evaluate(*C.document, C.document->frame_current);
                 return OpStatus::Finished;
               });
  {
    OperatorType ot;
    ot.idname = "armature.dynamics_clear_cache";
    ot.name = "Clear Dynamics Cache";
    ot.description = "Simulate the bone dynamics again from the start";
    ot.flag = OPTYPE_REGISTER;
    ot.exec = [](Context &C) {
      dynamics_clear_cache();
      animation_evaluate(*C.document, C.document->frame_current);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  /* Only with the bone layer itself active: on a drawing inside it, Shift A is the drawing's Add menu. */
  rig_operator("armature.bone_primitive_add", "Add Bone",
               "Add a new bone at the active bone layer's origin, pointing up (Shift A, as in Blender)",
               bone_layer_active, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 armature_add_bone(*document_active_layer(*C.document), op_arg(C, "name"), {0.0f, 0.0f}, {0.0f, 0.5f}, 0);
                 return OpStatus::Finished;
               });

  rig_operator("armature.extrude", "Extrude Bones",
               "Grow a new bone from each selected tip, connected to it, then move its tip (E, as in Blender). "
               "A selected loose base grows a bone that shares its bone's parent",
               bone_layer_active, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *document_active_layer(*C.document);
                 const std::vector<BonePose> poses = armature_pose(armature);
                 struct Grow {
                   uint64_t from;
                   bool from_tail;
                   float2 at;
                 };
                 std::vector<Grow> grows;
                 for (size_t i = 0; i < armature.bones.size(); i++) {
                   const Bone &b = armature.bones[i];
                   if (bone_tail_selected(armature, b)) {
                     grows.push_back({b.id, true, poses[i].tail});
                   }
                   else if (bone_head_selected(armature, b) && !(b.connected && bone_find(armature, b.parent))) {
                     grows.push_back({b.id, false, poses[i].head});
                   }
                 }
                 if (grows.empty()) {
                   report(ReportType::Warning, "Select a bone or a bone end to extrude from");
                   return OpStatus::Cancelled;
                 }
                 std::vector<uint64_t> grown;
                 for (const Grow &g : grows) {
                   const Bone *from = bone_find(armature, g.from);
                   const uint64_t parent = g.from_tail ? from->id : from->parent;
                   const std::string name = from->name;
                   const BonePose *pp = bone_pose_find(poses, parent);
                   /* It starts with no length; the move that follows gives it one. It points the
                    * parent's way, for a sensible angle if the move is cancelled. */
                   const float2 dir = pp ? normalize(pp->setup_tail - pp->setup_head) : float2(0.0f, 1.0f);
                   Bone *bone = armature_add_bone(armature, name, g.at, g.at + dir * 1e-3f, parent);
                   bone->length = 0.0f;
                   bone->connected = g.from_tail;
                   grown.push_back(bone->id);
                   /* X mirror: the other side grows a bone too, named to match; the move that
                    * follows mirrors it. */
                   const uint64_t other = bone_mirror_id(armature, g.from);
                   const bool other_grows = std::any_of(grows.begin(), grows.end(),
                                                        [&](const Grow &x) { return x.from == other; });
                   if (C.wm->tool_settings.mirror_x && other != g.from && !other_grows) {
                     const std::string new_name = bone->name;
                     const bool connected = bone->connected;
                     const Bone *o = bone_find(armature, other);
                     const BonePose *op = bone_pose_find(poses, other);
                     const float2 at = g.from_tail ? op->setup_tail : op->setup_head;
                     const uint64_t o_parent = g.from_tail ? o->id : o->parent;
                     const float2 o_dir = float2(-dir.x, dir.y);
                     Bone *twin = armature_add_bone(armature, bone_mirror_name(new_name), at, at + o_dir * 1e-3f, o_parent);
                     twin->length = 0.0f;
                     twin->connected = connected;
                   }
                 }
                 /* Only the new tips are selected, so the move drags them out. */
                 armature_deselect_all(armature);
                 for (const uint64_t id : grown) {
                   bone_find(armature, id)->select_tail = true;
                 }
                 C.wm->transform_request = 'G';
                 return OpStatus::Finished;
               });

  rig_operator("armature.subdivide", "Subdivide",
               "Split each selected bone into a chain of number_cuts + 1 connected bones, as in Blender "
               "(right-click a bone)",
               bone_layer_active, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *document_active_layer(*C.document);
                 const int pieces = std::clamp(op_arg_int(C, "number_cuts", 1), 1, 100) + 1;
                 std::vector<uint64_t> picked;
                 for (const Bone &b : armature.bones) {
                   if (b.selected || b.select_head || b.select_tail) {
                     picked.push_back(b.id);
                   }
                 }
                 if (picked.empty()) {
                   report(ReportType::Warning, "Select bones to subdivide");
                   return OpStatus::Cancelled;
                 }
                 if (C.wm->tool_settings.mirror_x) {
                   for (size_t i = 0, count = picked.size(); i < count; i++) {
                     const uint64_t other = bone_mirror_id(armature, picked[i]);
                     if (std::find(picked.begin(), picked.end(), other) == picked.end()) {
                       picked.push_back(other);
                     }
                   }
                 }
                 for (const uint64_t id : picked) {
                   Bone &first = *bone_find(armature, id);
                   const float piece = first.length / float(pieces);
                   const std::string name = first.name;
                   first.length = piece;
                   std::vector<uint64_t> children;
                   for (const Bone &child : armature.bones) {
                     if (child.parent == id) {
                       children.push_back(child.id);
                     }
                   }
                   /* The pieces run on along the bone, each connected to the one before. */
                   uint64_t previous = id;
                   for (int k = 1; k < pieces; k++) {
                     Bone bone;
                     bone.id = armature.next_bone_id++;
                     bone.name = armature_unique_bone_name(armature, name);
                     bone.parent = previous;
                     bone.connected = true;
                     bone.position = {piece, 0.0f};
                     bone.length = piece;
                     bone.strength = bone_find(armature, id)->strength;
                     bone.color_set = bone_find(armature, id)->color_set;
                     bone.custom_colors = bone_find(armature, id)->custom_colors;
                     bone.collections = bone_find(armature, id)->collections;
                     bone_select(bone, true);
                     previous = bone.id;
                     armature.bones.push_back(std::move(bone));
                   }
                   /* The old bone's children now hang from the last piece, where they were. */
                   for (Bone &child : armature.bones) {
                     if (std::find(children.begin(), children.end(), child.id) != children.end()) {
                       child.position.x -= piece * float(pieces - 1);
                       child.parent = previous;
                     }
                   }
                   bone_select(*bone_find(armature, id), true);
                 }
                 return OpStatus::Finished;
               });

  rig_operator("armature.duplicate_move", "Duplicate Bones",
               "Copy the selected bones, then move the copies (Shift D, as in Blender)", bone_layer_active,
               [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *document_active_layer(*C.document);
                 std::map<uint64_t, uint64_t> copies;
                 for (const Bone &b : armature.bones) {
                   if (b.selected) {
                     copies[b.id] = 0;
                   }
                 }
                 if (copies.empty()) {
                   report(ReportType::Warning, "Select bones to duplicate");
                   return OpStatus::Cancelled;
                 }
                 for (auto &[from, to] : copies) {
                   to = armature.next_bone_id++;
                 }
                 const uint64_t active = armature.active_bone;
                 const size_t count = armature.bones.size();
                 for (size_t i = 0; i < count; i++) {
                   if (!armature.bones[i].selected) {
                     continue;
                   }
                   Bone copy = armature.bones[i];
                   copy.id = copies[copy.id];
                   copy.name = armature_unique_bone_name(armature, copy.name);
                   /* A copied child hangs from its parent's copy, as in Blender. */
                   if (auto it = copies.find(copy.parent); it != copies.end()) {
                     copy.parent = it->second;
                   }
                   bone_select(armature.bones[i], false);
                   armature.bones.push_back(std::move(copy));
                 }
                 if (auto it = copies.find(active); it != copies.end()) {
                   armature.active_bone = it->second;
                 }
                 C.wm->transform_request = 'G';
                 return OpStatus::Finished;
               });

  rig_operator("armature.parent_set", "Make Parent",
               "Parent the selected bones to the active bone (Ctrl P). type=CONNECTED puts each one's base on "
               "the active bone's tip; type=OFFSET keeps them where they are",
               bone_layer_active, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *document_active_layer(*C.document);
                 const Bone *parent = bone_find(armature, armature.active_bone);
                 if (!parent) {
                   report(ReportType::Warning, "Make a bone active to parent to");
                   return OpStatus::Cancelled;
                 }
                 const bool connect = op_arg(C, "type", "OFFSET") == "CONNECTED";
                 const uint64_t parent_id = parent->id;
                 const std::vector<BonePose> poses = armature_pose(armature);
                 const BonePose *pp = bone_pose_find(poses, parent_id);
                 std::map<uint64_t, std::pair<float2, float2>> targets;
                 for (Bone &b : armature.bones) {
                   if (!b.selected || b.id == parent_id) {
                     continue;
                   }
                   if (bone_is_descendant(armature, parent_id, b.id)) {
                     report_banner(ReportType::Warning, "A bone can't hang from one of its children: " + b.name);
                     continue;
                   }
                   const BonePose *self = bone_pose_find(poses, b.id);
                   const float length = b.length;
                   const float2 scale = b.scale;
                   bone_place(b, pp->setup, self->setup_head, self->setup_tail);
                   b.length = length;
                   b.scale = scale;
                   b.parent = parent_id;
                   b.connected = false;
                   if (connect) {
                     /* The child slides, keeping its direction and length, until its base is on the tip. */
                     const float2 shift = pp->setup_tail - self->setup_head;
                     targets[b.id] = {self->setup_head + shift, self->setup_tail + shift};
                   }
                   else {
                     targets[b.id] = {self->setup_head, self->setup_tail};
                   }
                 }
                 if (targets.empty()) {
                   report(ReportType::Warning, "Select the bones to parent, then the parent last");
                   return OpStatus::Cancelled;
                 }
                 armature_set_joints(armature, targets);
                 if (connect) {
                   for (auto &[id, joints] : targets) {
                     bone_find(armature, id)->connected = true;
                   }
                 }
                 return OpStatus::Finished;
               });

  rig_operator("armature.parent_clear", "Clear Parent",
               "Alt P: type=CLEAR frees the selected bones from their parents; type=DISCONNECT keeps the parent "
               "but lets the base move on its own",
               bone_layer_active, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *document_active_layer(*C.document);
                 const bool clear = op_arg(C, "type", "CLEAR") == "CLEAR";
                 const std::vector<BonePose> poses = armature_pose(armature);
                 int done = 0;
                 for (Bone &b : armature.bones) {
                   if (!b.selected || (clear ? b.parent == 0 : !b.connected)) {
                     continue;
                   }
                   b.connected = false;
                   if (clear) {
                     const BonePose *self = bone_pose_find(poses, b.id);
                     const float length = b.length;
                   const float2 scale = b.scale;
                     bone_place(b, Transform2D(), self->setup_head, self->setup_tail);
                     b.length = length;
                     b.scale = scale;
                     b.parent = 0;
                   }
                   done++;
                 }
                 return done ? OpStatus::Finished : OpStatus::Cancelled;
               });

  /* Ctrl P and Alt P open a menu of the choices, as in Blender. */
  for (const auto &[idname, name, menu] :
       {std::tuple{"armature.parent_set_menu", "Make Parent", "bone_parent_set"},
        std::tuple{"armature.parent_clear_menu", "Clear Parent", "bone_parent_clear"}})
  {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = "Choose how to change the selected bones' parents";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = bone_layer_active;
    const std::string menu_name = menu;
    ot.exec = [menu_name](Context &C) {
      C.wm->menu_request = menu_name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  /* Blender's Apply Pose as Rest Pose and Apply Selected as Rest Pose (Ctrl A): the bones' pose on
   * the frame shown, in the action shown, becomes the setup pose (frame 0). Their position and angle
   * do (a scale stays a pose: the setup has none); what smart actions add stays out, as they still
   * add it. Unlike Blender's, the drawings come along (the setup drawing is bent to the new rest),
   * and every keyed pose, in the mainline and every action, shows as it did: the bones' keys are
   * worked out again against the new rest (keep_poses), and with Keep Animation the points the new
   * rest bends differently are keyed back where they showed. A smart bone keeps its rest, so its
   * actions play from the same turns; other bones left out keep where they rest. */
  const OpProperty keep_rest_property = op_bool_property(
      "keep_animation", "Keep Animation",
      "Every keyed frame of every action shows as it did: points the new rest bends differently are keyed back", "1");
  for (const auto &[idname, name, selected_only] :
       {std::tuple{"armature.apply_pose_as_rest", "Apply Pose as Rest Pose", false},
        std::tuple{"armature.apply_selected_as_rest", "Apply Selected as Rest Pose", true}})
  {
    const bool only_selected = selected_only;
    rig_operator(
        idname, name,
        only_selected ? "Make the selected bones' pose on this frame their setup pose (frame 0), keeping the drawings "
                        "and every action's animation as they show"
                      : "Make the bones' pose on this frame the setup pose (frame 0), keeping the drawings and every "
                        "action's animation as they show",
        bone_layer_active,
        [only_selected](Context &C) {
          Document &doc = *C.document;
          Layer *armature = document_active_layer(doc);
          if (doc.frame_current == 0 && doc.action.empty()) {
            report(ReportType::Warning, "Frame 0 is the rest pose already: pose the bones on another frame");
            return OpStatus::Cancelled;
          }
          animation_record(doc);
          std::set<uint64_t> smart;
          for (const std::string &action : armature->actions) {
            if (const Bone *b = smart_action_bone(*armature, action)) {
              smart.insert(b->id);
            }
          }
          /* The new rest of the bones applied (in their parents' space): the pose shown, less what smart
           * actions add. */
          std::map<uint64_t, std::pair<float2, float>> rest;
          int left_smart = 0;
          for (const Bone &b : armature->bones) {
            if (only_selected && !b.selected) {
              continue;
            }
            if (smart.count(b.id)) {
              left_smart++;
              continue;
            }
            const float2 position = b.position - rotate(b.position_keys.smart, b.setup_angle);
            const float angle = b.angle - b.angle_keys.smart;
            if (length(position - b.setup_position) > 1e-7f || std::fabs(angle - b.setup_angle) > 1e-7f) {
              rest[b.id] = {position, angle};
            }
          }
          if (left_smart > 0) {
            report(ReportType::Info, std::to_string(left_smart) + (left_smart == 1 ? " smart bone keeps" : " smart bones keep") +
                                         " its rest, so its actions play from the same turns");
          }
          if (rest.empty()) {
            report(ReportType::Warning, only_selected ? "The selected bones are at rest already" : "The bones are at rest already");
            return OpStatus::Cancelled;
          }
          const uint64_t armature_id = armature->id;
          std::vector<uint64_t> drawings;
          for (const Layer *l : document_layers_flat(doc)) {
            const Layer *above = layer_armature(doc, l->id);
            if (above && above->id == armature_id && !l->is_bone_layer() && !l->is_reference()) {
              drawings.push_back(l->id);
            }
          }
          /* On unless turned off (the property's default). */
          const std::string keep_arg = op_arg(C, "keep_animation", "1");
          const bool keep = keep_arg == "1" || keep_arg == "True" || keep_arg == "true";
          const DrawingsShownAll kept = keep ? drawings_shown_capture_all(doc, drawings) : DrawingsShownAll();
          const SetupSnapshot before = setup_snapshot(doc, {armature_id});
          /* Rig changes are made on the setup frame of the mainline. */
          const std::string was_action = doc.action;
          const uint64_t was_owner = was_action.empty() ? 0 : action_owner(doc, armature_id) ? action_owner(doc, armature_id)->id : 0;
          const int was_frame = doc.frame_current;
          if (!was_action.empty()) {
            document_set_action(doc, 0, "");
          }
          doc.frame_current = 0;
          animation_sync(doc);
          armature = layer_find(doc, armature_id);
          const std::vector<BonePose> old_poses = armature_pose(*armature, false);
          std::map<uint64_t, std::pair<float2, float>> old_rest;
          for (const Bone &b : armature->bones) {
            old_rest[b.id] = {b.setup_position, b.setup_angle};
          }
          const auto depth_of = [&](const Bone &b) {
            int depth = 0;
            for (const Bone *p = bone_find(*armature, b.parent); p; p = bone_find(*armature, p->parent)) {
              depth++;
            }
            return depth;
          };
          int deepest = 0;
          for (const Bone &b : armature->bones) {
            deepest = std::max(deepest, depth_of(b));
          }
          /* Parents first: an applied bone takes its new rest; a bone left out (not a smart one) whose
           * parent's rest moved keeps where it rests, its rest worked out again in its parent's space. */
          std::set<uint64_t> moved;
          for (int depth = 0; depth <= deepest; depth++) {
            const std::vector<BonePose> now = armature_pose(*armature, false);
            for (Bone &b : armature->bones) {
              if (depth_of(b) != depth) {
                continue;
              }
              const auto it = rest.find(b.id);
              if (it != rest.end()) {
                b.setup_position = it->second.first;
                b.setup_angle = it->second.second;
              }
              else if (moved.count(b.parent) && !smart.count(b.id)) {
                const Transform2D local = bone_pose_find(now, b.parent)->setup.inverted() *
                                          bone_pose_find(old_poses, b.id)->setup;
                b.setup_position = {local.tx, local.ty};
                b.setup_angle = std::atan2(local.b, local.a);
              }
              else if (!moved.count(b.parent)) {
                continue;
              }
              moved.insert(b.id);
              /* Limit Angle holds turns from the rest: they keep the same angles in the parent's space. */
              for (BoneConstraint &c : b.constraints) {
                if (c.type == BoneConstraintType::LimitAngle) {
                  c.min_angle += old_rest[b.id].second - b.setup_angle;
                  c.max_angle += old_rest[b.id].second - b.setup_angle;
                }
              }
              b.position = b.setup_position;
              b.angle = b.setup_angle;
              bone_changes_from_values(b);
            }
          }
          /* Each bone's move from its old rest to its new one, to bend the setup drawings with. */
          std::vector<BonePose> change = armature_pose(*armature, false);
          for (BonePose &p : change) {
            p.pose = p.setup;
            p.setup = bone_pose_find(old_poses, p.id)->setup;
            p.skin = p.pose * p.setup.inverted();
          }
          const Transform2D armature_world = layer_world_transform(doc, armature_id);
          for (const uint64_t id : drawings) {
            Layer *layer = layer_find(doc, id);
            if (const uint64_t bone = layer->followed_bone()) {
              /* Moved by its bone as a whole: its transform takes the bone's move, on every key. */
              const BonePose *p = bone_pose_find(change, bone);
              if (!p) {
                continue;
              }
              const Transform2D above = layer_world_transform(doc, id) * layer->local_transform().inverted();
              const Transform2D m = above.inverted() * armature_world * p->skin * armature_world.inverted() * above;
              const float turn = std::atan2(m.b, m.a) * 180.0f / PI;
              layer->position = m.apply(layer->position);
              layer->rotation += turn;
              const auto each = [](auto &ch, auto &&f) {
                for (auto &k : ch.keys) {
                  f(k.value);
                }
                for (auto &[action, keys] : ch.actions) {
                  for (auto &k : keys) {
                    f(k.value);
                  }
                }
              };
              each(layer->position_keys, [&](float2 &v) { v = m.apply(v); });
              each(layer->rotation_keys, [&](float &v) { v += turn; });
              continue;
            }
            const std::vector<Transform2D> skins = layer_point_skins_posed(doc, *layer, change);
            VectorDrawing &d = layer->drawing;
            for (size_t i = 0; i < d.points.size() && i < skins.size(); i++) {
              /* Detail points sit along the level below and follow it. */
              if (d.points[i].detail.level == 0) {
                d.points[i].pos = skins[i].apply(d.points[i].pos);
              }
            }
            for (Curve &curve : d.curves) {
              for (CurvePoint &cp : curve.points) {
                if (cp.handles_set && size_t(cp.point) < skins.size()) {
                  cp.handle_in = skins[size_t(cp.point)].apply_vector(cp.handle_in);
                  cp.handle_out = skins[size_t(cp.point)].apply_vector(cp.handle_out);
                }
              }
            }
          }
          animation_record(doc);
          keep_poses(doc, before);
          /* A timeline without keys for a bone shows it at rest: where the rest moved, it gets one key
           * (frame 1 on) holding the old rest, so it shows as it did. */
          for (Bone &b : armature->bones) {
            if (!moved.count(b.id)) {
              continue;
            }
            const auto &[was_position, was_angle] = old_rest[b.id];
            const float turn = was_angle - b.setup_angle;
            const float2 transform = rotate(was_position - b.setup_position, -b.setup_angle);
            const auto posed = [](const auto &keys) {
              return std::any_of(keys.begin(), keys.end(), [](const auto &k) { return k.frame != 0; });
            };
            const auto hold = [&](auto &ch, auto value, bool needed) {
              if (!needed) {
                return;
              }
              if (!posed(ch.keys)) {
                ch.set_key(0, decltype(value){});
                ch.set_key(1, value);
              }
              for (const std::string &action : armature->actions) {
                if (action_adds(*armature, action)) {
                  continue;
                }
                auto &keys = ch.actions[action];
                if (!posed(keys)) {
                  keys.clear();
                  keys.emplace_back();
                  keys.back().frame = 1;
                  keys.back().value = value;
                }
              }
            };
            hold(b.angle_keys, turn, std::fabs(turn) > 1e-7f);
            hold(b.position_keys, transform, length(transform) > 1e-7f);
          }
          animation_sync(doc);
          if (!was_action.empty() && was_owner) {
            document_set_action(doc, was_owner, was_action);
          }
          doc.frame_current = was_frame;
          animation_sync(doc);
          if (keep) {
            drawings_shown_keep_all(doc, kept);
          }
          return OpStatus::Finished;
        },
        {keep_rest_property});
  }
  /* Bone Follows Selection (the Layer tab's Bones panel): on every frame the selected points (all of
   * the layer's when none are) are keyed in the timeline being edited, the layer's parent bone is
   * keyed where the selection shows, with Move Only Bones (the drawings it bends are keyed back where
   * they showed). For a bone that follows a feature through a smart action, as an eye through a head
   * turn. Keep Offset moves it by the selection's move from frame 0 (off: its head onto the
   * selection's middle); Fit also keys the selection's turn, or its turn and size (a best fit). */
  const OpProperty follow_offset_property = op_bool_property(
      "keep_offset", "Keep Offset", "Move the bone by the selection's move from frame 0 (off: its head onto the selection's middle)",
      "1");
  OpProperty follow_fit_property;
  follow_fit_property.name = "fit";
  follow_fit_property.label = "Fit";
  follow_fit_property.description = "What of the selection's motion the bone takes: its move, its move and turn, or its move, "
                                    "turn and size";
  follow_fit_property.kind = OpProperty::Kind::Enum;
  follow_fit_property.items = {{"MOVE", "Move"}, {"TURN", "Move & Turn"}, {"SCALE", "Move, Turn & Scale"}};
  follow_fit_property.fallback = "MOVE";
  const OpProperty follow_children_property = op_bool_property(
      "move_children", "Move Children",
      "The bone's children come along (their drawings kept where they showed); off, they are keyed to stay where they "
      "were, so their keys hold their move from it",
      "1");
  rig_operator(
      "layer.bone_follow_selection", "Bone Follows Selection",
      "Key the active layer's parent bone (a group's: the bone it follows) where its selected points (a group's: "
      "those of the layers inside it; all, when none are) show, on every frame they are keyed in this timeline, "
      "keeping the drawings where they are (Move Only Bones)",
      [](Context &C) {
        const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
        return l && (l->has_drawing() || l->is_group()) && !l->is_bone_layer() && layer_armature(*C.document, l->id);
      },
      [](Context &C) {
        Document &doc = *C.document;
        Layer *layer = document_active_layer(doc);
        const uint64_t layer_id = layer->id;
        const Layer *binding = layer_binding_group(doc, layer_id);
        const uint64_t bone_id = binding ? binding->parent_bone : layer->parent_bone;
        Layer *armature = const_cast<Layer *>(layer_armature(doc, layer_id));
        if (!bone_find(*armature, bone_id)) {
          report(ReportType::Warning, std::string("Give the ") + (layer->is_group() ? "group" : "layer") +
                                          " a Parent Bone first");
          return OpStatus::Cancelled;
        }
        if (doc.frame_current == 0 && doc.action.empty()) {
          report(ReportType::Warning, "Frame 0 is the setup pose: use it in an action or on a posed frame");
          return OpStatus::Cancelled;
        }
        animation_record(doc);
        /* The selection: the layer's selected points, or a group's, those of the drawings inside it; all of
         * them when none are selected. */
        std::vector<uint64_t> sources;
        const auto inside = [&](uint64_t id) {
          for (const Layer *up = layer_parent(doc, id); up; up = layer_parent(doc, up->id)) {
            if (up->id == layer_id) {
              return true;
            }
          }
          return false;
        };
        if (layer->is_group()) {
          for (const Layer *l : document_layers_flat(doc)) {
            if (l->has_drawing() && !l->is_reference() && inside(l->id)) {
              sources.push_back(l->id);
            }
          }
        }
        else {
          sources.push_back(layer_id);
        }
        std::vector<std::pair<uint64_t, std::vector<size_t>>> picked;
        bool any_selected = false;
        for (const uint64_t id : sources) {
          const VectorDrawing &d = layer_find(doc, id)->drawing;
          for (const MeshPoint &p : d.points) {
            any_selected = any_selected || p.selected;
          }
        }
        size_t count = 0;
        for (const uint64_t id : sources) {
          const VectorDrawing &d = layer_find(doc, id)->drawing;
          std::vector<size_t> points;
          for (size_t i = 0; i < d.points.size(); i++) {
            if (d.points[i].selected || !any_selected) {
              points.push_back(i);
            }
          }
          if (!points.empty()) {
            count += points.size();
            picked.push_back({id, std::move(points)});
          }
        }
        /* The frames the selection is keyed on here, and how its keys move on from each. */
        std::map<int, Interp> frames;
        const auto add = [&](const auto &ch) {
          for (const auto &k : ch.keys) {
            if (k.frame != 0 && !frames.count(k.frame)) {
              frames[k.frame] = k.interp;
            }
          }
        };
        for (const auto &[id, points] : picked) {
          const VectorDrawing &d = layer_find(doc, id)->drawing;
          std::set<int> picked_points;
          for (const size_t i : points) {
            add(d.points[i].pos_keys);
            picked_points.insert(int(i));
          }
          for (const Curve &curve : d.curves) {
            for (const CurvePoint &cp : curve.points) {
              if (picked_points.count(cp.point)) {
                add(cp.handle_in_keys);
                add(cp.handle_out_keys);
              }
            }
          }
        }
        if (frames.empty() || count == 0) {
          report(ReportType::Warning, "The selection has no keys in this timeline");
          return OpStatus::Cancelled;
        }
        /* The selection's own frames: a key added on another frame moves as the selection does there. */
        const std::map<int, Interp> selection_frames = frames;
        const std::string fit = op_arg(C, "fit", "MOVE");
        const bool turn = fit == "TURN" || fit == "SCALE", grow = fit == "SCALE";
        /* The bone's own keys here too: it follows the selection from now on, so a key it had on
         * another frame (an Auto-Freeze key from before it moved, say) would pull it away there. */
        {
          const Bone &b = *bone_find(*armature, bone_id);
          add(b.position_keys);
          if (turn) {
            add(b.angle_keys);
          }
          if (grow) {
            add(b.scale_keys);
          }
        }
        const std::string offset_arg = op_arg(C, "keep_offset", "1");
        const bool keep_offset = offset_arg == "1" || offset_arg == "True" || offset_arg == "true";
        const uint64_t armature_id = armature->id;
        /* What the bone moves: drawings it bends point by point, and layers (groups) it moves as a whole. */
        std::vector<uint64_t> drawings, wholes;
        for (const Layer *l : document_layers_flat(doc)) {
          const Layer *above = layer_armature(doc, l->id);
          if (!above || above->id != armature_id || l->is_bone_layer() || l->is_reference()) {
            continue;
          }
          drawings.push_back(l->id);
          if (l->followed_bone() != 0 && !layer_binding_group(doc, l->id)) {
            wholes.push_back(l->id);
          }
        }
        const std::string children_arg = op_arg(C, "move_children", "1");
        const bool move_children = children_arg == "1" || children_arg == "True" || children_arg == "true";
        std::vector<uint64_t> children;
        if (!move_children) {
          for (const Bone &c : armature->bones) {
            if (c.parent == bone_id) {
              children.push_back(c.id);
            }
          }
        }
        /* A layer moved as a whole that holds the selection (a group the bone follows): the bone takes
         * its motion, so its drawings' points are keyed back where they showed, their keys left with
         * what the bone doesn't do; its own transform is left alone. */
        const auto under = [&](uint64_t id, uint64_t ancestor) {
          for (const Layer *up = layer_find(doc, id); up; up = layer_parent(doc, up->id)) {
            if (up->id == ancestor) {
              return true;
            }
          }
          return false;
        };
        /* Only where the layers between turn and size the same on every frame: the points sit inside
         * their transforms, so under a keyed turn or size the bone's straight move would bend there
         * between keys. Otherwise the layer's transform is keyed back instead, which is exact. */
        const auto steady = [](const auto &ch) {
          return std::all_of(ch.keys.begin(), ch.keys.end(), [&](const auto &k) { return k.value == ch.keys.front().value; });
        };
        const auto rigid_inside = [&](uint64_t w) {
          for (const Layer *l : document_layers_flat(doc)) {
            if ((l->id == w || under(l->id, w)) && (!steady(l->rotation_keys) || !steady(l->scale_keys))) {
              return false;
            }
          }
          return true;
        };
        std::vector<uint64_t> repoint;
        std::erase_if(wholes, [&](uint64_t w) {
          const bool holds = std::any_of(sources.begin(), sources.end(), [&](uint64_t s) { return under(s, w); }) &&
                             rigid_inside(w);
          if (holds) {
            for (const uint64_t id : drawings) {
              if (under(id, w) && layer_find(doc, id)->has_drawing()) {
                repoint.push_back(id);
              }
            }
          }
          return holds;
        });
        /* The frames what it moves is keyed on too: a key there left as it was would pull it away for a
         * frame. */
        std::set<uint64_t> moving{bone_id};
        if (move_children) {
          for (const Bone &c : armature->bones) {
            if (bone_is_descendant(*armature, c.id, bone_id)) {
              moving.insert(c.id);
            }
          }
        }
        const auto add_drawing = [&](const Layer &l) {
          for (const MeshPoint &p : l.drawing.points) {
            add(p.pos_keys);
          }
          for (const Curve &curve : l.drawing.curves) {
            for (const CurvePoint &cp : curve.points) {
              add(cp.handle_in_keys);
              add(cp.handle_out_keys);
            }
          }
        };
        for (const uint64_t w : wholes) {
          const Layer *l = layer_find(doc, w);
          if (moving.count(l->followed_bone())) {
            add(l->position_keys);
            add(l->rotation_keys);
            add(l->scale_keys);
          }
        }
        for (const uint64_t id : repoint) {
          add_drawing(*layer_find(doc, id));
        }
        for (const uint64_t id : drawings) {
          const Layer *l = layer_find(doc, id);
          if (l->followed_bone() != 0 || layer_binding_group(doc, id)) {
            continue;
          }
          bool bent = l->points_follow_bone() && moving.count(l->parent_bone);
          for (size_t i = 0; i < l->drawing.points.size() && !bent; i++) {
            for (const BoneWeight &w : l->drawing.points[i].weights) {
              bent = bent || (w.weight > 0.0f && moving.count(w.bone));
            }
          }
          if (bent) {
            add_drawing(*l);
          }
        }
        std::vector<int> frame_list;
        for (auto &[f, interp] : frames) {
          frame_list.push_back(f);
          if (!selection_frames.count(f)) {
            auto at = selection_frames.upper_bound(f);
            interp = at == selection_frames.begin() ? at->second : std::prev(at)->second;
          }
        }
        const int was_frame = doc.frame_current;
        /* Where the selection rests (frame 0) and shows on each frame, in the bone layer's space. */
        std::vector<float2> rest;
        for (const auto &[id, points] : picked) {
          const Transform2D to_armature0 =
              layer_setup_world_transform(doc, armature_id).inverted() * layer_setup_world_transform(doc, id);
          for (const size_t i : points) {
            rest.push_back(to_armature0.apply(point_setup_pos(layer_find(doc, id)->drawing.points[i])));
          }
        }
        const DrawingsShown before = drawings_shown_capture_frames(doc, drawings, frame_list);
        struct Target {
          float2 head;
          float angle = 0.0f, size = 1.0f;
          /* The layers moved as a whole, where they show (document space). */
          std::vector<Transform2D> wholes;
          /* The children kept in place, where they are (bone layer space). */
          std::vector<Transform2D> children;
          /* The repointed drawings' points and handles as they show (document space). */
          std::vector<std::vector<float2>> points;
          std::vector<std::vector<std::pair<float2, float2>>> handles;
        };
        std::vector<Target> targets;
        float2 rest_middle;
        for (const float2 p : rest) {
          rest_middle += p;
        }
        rest_middle = rest_middle / float(rest.size());
        const BonePose setup = *bone_pose_find(armature_keyed_pose(*armature, false), bone_id);
        for (const int f : frame_list) {
          doc.frame_current = f;
          animation_sync(doc);
          std::vector<float2> now;
          float2 middle;
          for (const auto &[id, points] : picked) {
            const Layer *l = layer_find(doc, id);
            const std::vector<Transform2D> skins = layer_point_skins(doc, *l);
            const Transform2D to_armature =
                layer_world_transform(doc, armature_id).inverted() * layer_world_transform(doc, id);
            for (const size_t i : points) {
              now.push_back(to_armature.apply(skins[i].apply(l->drawing.points[i].pos)));
              middle += now.back();
            }
          }
          middle = middle / float(now.size());
          /* The best turn and size taking the resting selection to this one, about their middles. */
          float dots = 0.0f, crosses = 0.0f, was_spread = 0.0f, now_spread = 0.0f;
          for (size_t k = 0; k < now.size(); k++) {
            const float2 a = rest[k] - rest_middle, b = now[k] - middle;
            dots += dot(a, b);
            crosses += a.x * b.y - a.y * b.x;
            was_spread += dot(a, a);
            now_spread += dot(b, b);
          }
          Target t;
          t.angle = turn && (dots != 0.0f || crosses != 0.0f) ? std::atan2(crosses, dots) : 0.0f;
          t.size = grow && was_spread > 1e-12f ? std::sqrt(now_spread / was_spread) : 1.0f;
          t.head = keep_offset ? middle + rotate(setup.setup_head - rest_middle, t.angle) * t.size : middle;
          for (const uint64_t id : wholes) {
            t.wholes.push_back(layer_world_transform(doc, id));
          }
          const std::vector<BonePose> posed = armature_keyed_pose(*layer_find(doc, armature_id), false);
          for (const uint64_t id : children) {
            t.children.push_back(bone_pose_find(posed, id)->pose);
          }
          for (const uint64_t id : repoint) {
            const Layer *l = layer_find(doc, id);
            const std::vector<Transform2D> skins = layer_point_skins(doc, *l);
            const Transform2D world = layer_world_transform(doc, id);
            std::vector<float2> pts;
            for (size_t i = 0; i < l->drawing.points.size() && i < skins.size(); i++) {
              pts.push_back((world * skins[i]).apply(l->drawing.points[i].pos));
            }
            std::vector<std::pair<float2, float2>> hs;
            for (const Curve &curve : l->drawing.curves) {
              for (const CurvePoint &cp : curve.points) {
                const Transform2D m = world * (size_t(cp.point) < skins.size() ? skins[size_t(cp.point)] : Transform2D());
                hs.push_back({m.apply_vector(cp.handle_in), m.apply_vector(cp.handle_out)});
              }
            }
            t.points.push_back(std::move(pts));
            t.handles.push_back(std::move(hs));
          }
          targets.push_back(std::move(t));
        }
        /* The bone keyed there (its parent as posed on that frame), every key moving on as the
         * selection's do; a layer it moves as a whole keyed back where it showed. */
        const bool auto_key = doc.auto_key, auto_freeze = doc.auto_freeze_keys;
        doc.auto_key = true;
        /* Only this bone is keyed: Auto-Freeze would key every other bone on these frames too. */
        doc.auto_freeze_keys = false;
        const float setup_angle = std::atan2(setup.setup.b, setup.setup.a);
        for (size_t fi = 0; fi < frame_list.size(); fi++) {
          const int f = frame_list[fi];
          const Interp interp = frames[f];
          doc.frame_current = f;
          animation_sync(doc);
          armature = layer_find(doc, armature_id);
          Bone &b = *bone_find(*armature, bone_id);
          const BonePose *parent = bone_pose_find(armature_keyed_pose(*armature, false), b.parent);
          const Transform2D up = parent ? parent->pose : Transform2D();
          b.position = up.inverted().apply(targets[fi].head);
          if (turn) {
            b.angle = setup_angle + targets[fi].angle - std::atan2(up.b, up.a);
          }
          if (grow) {
            b.scale = float2{targets[fi].size, targets[fi].size};
          }
          /* Children kept in place: worked out again in the bone's new pose. */
          if (!children.empty()) {
            const Transform2D moved = bone_pose_find(armature_keyed_pose(*armature, false), bone_id)->pose;
            for (size_t ci = 0; ci < children.size(); ci++) {
              Bone &c = *bone_find(*armature, children[ci]);
              const Transform2D local = moved.inverted() * targets[fi].children[ci];
              c.position = {local.tx, local.ty};
              c.angle = std::atan2(local.b, local.a);
              if (grow && targets[fi].size > 1e-6f) {
                c.scale = c.scale * (1.0f / targets[fi].size);
              }
            }
          }
          animation_record(doc);
          for (const uint64_t id : children) {
            Bone &c = *bone_find(*armature, id);
            for (auto *k : {c.position_keys.key_at(f), c.scale_keys.key_at(f)}) {
              if (k) {
                k->interp = interp;
              }
            }
            if (auto *k = c.angle_keys.key_at(f)) {
              k->interp = interp;
            }
          }
          if (auto *k = b.position_keys.key_at(f)) {
            k->interp = interp;
          }
          if (auto *k = b.angle_keys.key_at(f); k && turn) {
            k->interp = interp;
          }
          if (auto *k = b.scale_keys.key_at(f); k && grow) {
            k->interp = interp;
          }
          for (size_t wi = 0; wi < wholes.size(); wi++) {
            Layer *l = layer_find(doc, wholes[wi]);
            const Transform2D above = layer_world_transform(doc, l->id) * l->local_transform().inverted();
            float2 position, scale;
            float rotation = 0.0f;
            local_from_transform(above.inverted() * targets[fi].wholes[wi], l->scale, l->rotation, &position,
                                 &rotation, &scale);
            if (length(position - l->position) < 1e-7f && std::fabs(rotation - l->rotation) < 1e-5f &&
                length(scale - l->scale) < 1e-7f)
            {
              continue;
            }
            /* Unkeyed channels get their setup key too, so the other frames stay as they were. */
            if (!l->position_keys.animated()) {
              l->position_keys.set_key(0, l->position);
            }
            if (!l->rotation_keys.animated()) {
              l->rotation_keys.set_key(0, l->rotation);
            }
            if (!l->scale_keys.animated()) {
              l->scale_keys.set_key(0, l->scale);
            }
            l->position_keys.set_key(f, position);
            l->rotation_keys.set_key(f, rotation);
            l->scale_keys.set_key(f, scale);
            l->position_keys.key_at(f)->interp = interp;
            l->rotation_keys.key_at(f)->interp = interp;
            l->scale_keys.key_at(f)->interp = interp;
            l->position = position;
            l->rotation = rotation;
            l->scale = scale;
          }
          /* The drawings of a group the bone follows, keyed back where they showed. */
          for (size_t ri = 0; ri < repoint.size(); ri++) {
            Layer *l = layer_find(doc, repoint[ri]);
            const std::vector<Transform2D> skins = layer_point_skins(doc, *l);
            const Transform2D world = layer_world_transform(doc, l->id);
            VectorDrawing &d = l->drawing;
            const std::vector<float2> &pts = targets[fi].points[ri];
            for (size_t i = 0; i < d.points.size() && i < skins.size() && i < pts.size(); i++) {
              const float2 at = (world * skins[i]).inverted().apply(pts[i]);
              if (length(at - d.points[i].pos) > 1e-7f) {
                d.points[i].pos = at;
              }
            }
            size_t h = 0;
            for (Curve &curve : d.curves) {
              for (CurvePoint &cp : curve.points) {
                if (cp.handles_set && h < targets[fi].handles[ri].size() && size_t(cp.point) < skins.size()) {
                  const Transform2D undo = (world * skins[size_t(cp.point)]).inverted();
                  cp.handle_in = undo.apply_vector(targets[fi].handles[ri][h].first);
                  cp.handle_out = undo.apply_vector(targets[fi].handles[ri][h].second);
                }
                h++;
              }
            }
          }
          animation_record(doc);
        }
        doc.auto_key = auto_key;
        doc.auto_freeze_keys = auto_freeze;
        doc.frame_current = was_frame;
        animation_sync(doc);
        /* Move Only Bones: what the bone bends goes back where it showed. */
        drawings_shown_keep(doc, before);
        return OpStatus::Finished;
      },
      {follow_offset_property, follow_fit_property, follow_children_property});
  /* Ctrl A: the Apply menu, as Blender's in pose mode. */
  {
    OperatorType ot;
    ot.idname = "armature.apply_menu";
    ot.name = "Apply";
    ot.description = "Choose what to apply to the bones' rest pose";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = bone_layer_active;
    ot.exec = [](Context &C) {
      C.wm->menu_request = "bone_apply";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  /* --- Bone collections, as Blender's ------------------------------------------------------- */
  rig_operator("armature.collection_add", "Add Bone Collection",
               "Add a bone collection after the active one (`name`, default \"Collection\") and make it active",
               bone_layer_active, [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 bone_collection_add(armature, op_arg(C, "name"));
                 return OpStatus::Finished;
               });
  rig_operator("armature.collection_remove", "Remove Bone Collection",
               "Remove the active bone collection (or `collection`); its bones and drawings just leave it",
               bone_layer_active, [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 if (all_arg(C, armature)) {
                   report(ReportType::Warning, "All always holds every bone: it can't be removed");
                   return OpStatus::Cancelled;
                 }
                 const BoneCollection *c = collection_arg(C, armature);
                 if (!c) {
                   return OpStatus::Cancelled;
                 }
                 bone_collection_remove(*C.document, armature, c->id);
                 return OpStatus::Finished;
               });
  rig_operator("armature.collection_move", "Move Bone Collection",
               "Move the active bone collection up or down the list (`direction` UP or DOWN)", bone_layer_active,
               [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 auto &list = armature.bone_collections;
                 const auto it = std::find_if(list.begin(), list.end(),
                                              [&](const BoneCollection &c) { return c.id == armature.active_collection; });
                 if (it == list.end()) {
                   return OpStatus::Cancelled;
                 }
                 const bool up = op_arg(C, "direction", "UP") == "UP";
                 if ((up && it == list.begin()) || (!up && it + 1 == list.end())) {
                   return OpStatus::Cancelled;
                 }
                 std::iter_swap(it, up ? it - 1 : it + 1);
                 return OpStatus::Finished;
               });
  /* Assign / Remove: the selected bones, and the drawings picked in the Layers editor. */
  for (const bool assign : {true, false}) {
    rig_operator(
        assign ? "armature.collection_assign" : "armature.collection_unassign",
        assign ? "Assign to Bone Collection" : "Remove from Bone Collection",
        assign ? "Put the selected bones, and the drawings inside this bone layer picked in the Layers editor, in the "
                 "active bone collection (or `collection`)"
               : "Take the selected bones, and the picked drawings, out of the active bone collection (or `collection`)",
        bone_layer_active, [assign](Context &C) {
          Document &doc = *C.document;
          Layer &armature = *document_active_layer(doc);
          if (all_arg(C, armature)) {
            report(ReportType::Warning, "Every bone is in All: assign to (or remove from) another collection");
            return OpStatus::Cancelled;
          }
          const BoneCollection *c = collection_arg(C, armature);
          if (!c) {
            report(ReportType::Warning, "Add a bone collection first");
            return OpStatus::Cancelled;
          }
          int changed = 0;
          const auto apply = [&](std::vector<uint64_t> &ids) {
            if (assign && !in_collection(ids, c->id)) {
              ids.push_back(c->id);
              changed++;
            }
            else if (!assign && in_collection(ids, c->id)) {
              std::erase(ids, c->id);
              changed++;
            }
          };
          for (Bone &b : armature.bones) {
            if (b.selected || b.select_head || b.select_tail) {
              apply(b.collections);
            }
          }
          for (Layer *d : collection_drawings(doc, armature)) {
            apply(d->collections);
          }
          if (changed == 0) {
            report(ReportType::Warning, assign ? "Select bones (or pick drawings in the Layers editor) to assign"
                                               : "None of the selected bones or picked drawings is in it");
            return OpStatus::Cancelled;
          }
          armature_deselect_hidden(armature);
          return OpStatus::Finished;
        });
  }
  /* Select / Deselect: every shown bone of the collection, and its drawings' points. */
  for (const bool select : {true, false}) {
    rig_operator(select ? "armature.collection_select" : "armature.collection_deselect",
                 select ? "Select Bone Collection" : "Deselect Bone Collection",
                 select ? "Select the bones of the active bone collection (or `collection`), and its drawings' points"
                        : "Deselect the bones of the active bone collection (or `collection`), and its drawings' points",
                 bone_layer_active, [select](Context &C) {
                   Document &doc = *C.document;
                   Layer &armature = *document_active_layer(doc);
                   /* All: every shown bone and editable drawing. */
                   const bool all = all_arg(C, armature);
                   const BoneCollection *c = all ? nullptr : collection_arg(C, armature);
                   if (!all && !c) {
                     return OpStatus::Cancelled;
                   }
                   int touched = 0;
                   for (Bone &b : armature.bones) {
                     if ((all || in_collection(b.collections, c->id)) && bone_shown(armature, b)) {
                       bone_select(b, select);
                       touched++;
                     }
                   }
                   for (const Layer *l : document_layers_flat(doc)) {
                     const Layer *owner = layer_armature(doc, l->id);
                     if (owner && owner->id == armature.id && (all || in_collection(l->collections, c->id)) &&
                         layer_edited_with_bones(doc, *l))
                     {
                       for (MeshPoint &p : layer_find(doc, l->id)->drawing.points) {
                         p.selected = select;
                       }
                       touched++;
                     }
                   }
                   return touched ? OpStatus::Finished : OpStatus::Cancelled;
                 });
  }
  rig_operator("armature.collection_set", "Set Bone Collection",
               "Change a bone collection (`collection`, default the active one): `name`, `visible` (the eye), `solo` "
               "(the star) or `active`",
               bone_layer_active, [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 if (all_arg(C, armature)) {
                   /* All: its eye, its star, or made active; it keeps its name. */
                   if (const std::string v = op_arg(C, "visible"); !v.empty()) {
                     armature.all_bones_visible = v == "1";
                   }
                   if (const std::string v = op_arg(C, "solo"); !v.empty()) {
                     armature.all_bones_solo = v == "1";
                   }
                   if (op_arg(C, "active") == "1") {
                     armature.active_collection = 0;
                   }
                   armature_deselect_hidden(armature);
                   return OpStatus::Finished;
                 }
                 BoneCollection *c = collection_arg(C, armature);
                 if (!c) {
                   return OpStatus::Cancelled;
                 }
                 if (const std::string name = op_arg(C, "name"); !name.empty() && name != c->name) {
                   c->name = bone_collection_unique_name(armature, name);
                 }
                 if (const std::string v = op_arg(C, "visible"); !v.empty()) {
                   c->visible = v == "1";
                 }
                 if (const std::string v = op_arg(C, "solo"); !v.empty()) {
                   c->solo = v == "1";
                 }
                 if (op_arg(C, "active") == "1") {
                   armature.active_collection = c->id;
                 }
                 armature_deselect_hidden(armature);
                 return OpStatus::Finished;
               });
  rig_operator("armature.collection_show_all", "Show All Bone Collections", "Turn every bone collection's eye on",
               bone_layer_active, [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 armature.all_bones_visible = true;
                 for (BoneCollection &c : armature.bone_collections) {
                   c.visible = true;
                 }
                 return OpStatus::Finished;
               });
  rig_operator("armature.collection_unsolo_all", "Unsolo All Bone Collections", "Turn every bone collection's star off",
               bone_layer_active, [](Context &C) {
                 Layer &armature = *document_active_layer(*C.document);
                 armature.all_bones_solo = false;
                 for (BoneCollection &c : armature.bone_collections) {
                   c.solo = false;
                 }
                 return OpStatus::Finished;
               });
  rig_operator("armature.collection_remove_unused", "Remove Unused Bone Collections",
               "Remove the bone collections with no bones and no drawings in them", bone_layer_active, [](Context &C) {
                 Document &doc = *C.document;
                 Layer &armature = *document_active_layer(doc);
                 std::vector<uint64_t> unused;
                 for (const BoneCollection &c : armature.bone_collections) {
                   bool used = std::any_of(armature.bones.begin(), armature.bones.end(),
                                           [&](const Bone &b) { return in_collection(b.collections, c.id); });
                   for (const Layer *l : document_layers_flat(doc)) {
                     const Layer *owner = layer_armature(doc, l->id);
                     used |= owner && owner->id == armature.id && in_collection(l->collections, c.id);
                   }
                   if (!used) {
                     unused.push_back(c.id);
                   }
                 }
                 for (const uint64_t id : unused) {
                   bone_collection_remove(doc, armature, id);
                 }
                 return unused.empty() ? OpStatus::Cancelled : OpStatus::Finished;
               });
  /* M and Shift M: Blender's Move to Collection (leave the others, join one) and Assign to
   * Collection (join one, keep the others); `new` 1 makes a new collection for it. */
  for (const bool move : {true, false}) {
    rig_operator(move ? "armature.move_to_collection" : "armature.assign_to_collection",
                 move ? "Move to Bone Collection" : "Assign to Bone Collection",
                 move ? "Put the selected bones (the bone layer active) or the active and picked drawings inside it in "
                        "bone collection `collection` only (`new` 1: a new one)"
                      : "Add the selected bones (the bone layer active) or the active and picked drawings inside it to "
                        "bone collection `collection` (`new` 1: a new one)",
                 [](Context &C) { return C.document && collection_armature(*C.document); },
                 [move](Context &C) {
                   /* With the bone layer active: its selected bones and the picked drawings. With
                    * a drawing inside it active: that drawing and the other picked ones. */
                   Document &doc = *C.document;
                   Layer &armature = *collection_armature(doc);
                   std::vector<Bone *> bones;
                   for (Bone &b : armature.bones) {
                     if (doc.active_layer == armature.id && (b.selected || b.select_head || b.select_tail)) {
                       bones.push_back(&b);
                     }
                   }
                   const std::vector<Layer *> drawings = collection_drawings(doc, armature);
                   if (bones.empty() && drawings.empty()) {
                     const Layer *active = document_active_layer(doc);
                     report(ReportType::Warning, active && active->type == LayerType::Vector && !active->edit_with_bones
                                                     ? "Turn on Editable in Bone Group (Layer tab) to put this drawing in collections"
                                                     : "Select bones (or pick drawings in the Layers editor) first");
                     return OpStatus::Cancelled;
                   }
                   const BoneCollection *c = op_arg(C, "new") == "1" ? &bone_collection_add(armature, "")
                                                                    : collection_arg(C, armature);
                   if (!c) {
                     return OpStatus::Cancelled;
                   }
                   const uint64_t id = c->id;
                   const auto apply = [&](std::vector<uint64_t> &ids) {
                     if (move) {
                       ids.clear();
                     }
                     if (!in_collection(ids, id)) {
                       ids.push_back(id);
                     }
                   };
                   for (Bone *b : bones) {
                     apply(b->collections);
                   }
                   for (Layer *d : drawings) {
                     apply(d->collections);
                   }
                   armature_deselect_hidden(armature);
                   return OpStatus::Finished;
                 });
  }
  for (const auto &[idname, name, menu] :
       {std::tuple{"armature.move_to_collection_menu", "Move to Bone Collection", "bone_collection_move"},
        std::tuple{"armature.assign_to_collection_menu", "Assign to Bone Collection", "bone_collection_assign"}})
  {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = "Choose a bone collection for the selected bones, or for the active and picked drawings inside a "
                     "bone layer, as Blender's M and Shift M";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) {
      /* In the viewport only for bones: M there on a drawing is left to the drawing's own tools (its
       * bone collections stay in the Layers editor and its Layer tab). */
      const Layer *armature = C.document ? collection_armature(*C.document) : nullptr;
      return armature && (C.area_type != "Viewport" || document_active_layer(*C.document) == armature);
    };
    const std::string menu_name = menu;
    ot.exec = [menu_name](Context &C) {
      C.wm->menu_request = menu_name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  /* A drawing joins or leaves one of its bone layer's collections (its Layer tab, or the Layers
   * editor's right-click menu). */
  {
    OperatorType ot;
    ot.idname = "layer.bone_collection_toggle";
    ot.name = "Drawing in Bone Collection";
    ot.description = "Put drawing `layer` (default the active layer) in bone collection `collection` of its bone "
                     "layer, or take it out (`on` 0)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::string id = op_arg(C, "layer");
      Layer *layer = id.empty() ? document_active_layer(doc) : layer_find(doc, std::strtoull(id.c_str(), nullptr, 10));
      Layer *armature = layer && layer->type == LayerType::Vector ? const_cast<Layer *>(layer_armature(doc, layer->id))
                                                                  : nullptr;
      BoneCollection *c = armature ? bone_collection_find_by_name(*armature, op_arg(C, "collection")) : nullptr;
      if (!c) {
        report(ReportType::Warning, "A drawing inside a bone layer can join that bone layer's collections");
        return OpStatus::Cancelled;
      }
      const bool on = op_arg(C, "on", "1") == "1";
      if (on == in_collection(layer->collections, c->id)) {
        return OpStatus::Cancelled;
      }
      if (on) {
        layer->collections.push_back(c->id);
      }
      else {
        std::erase(layer->collections, c->id);
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  /* Alt G / Alt R / Alt S on bones (armature.clear_location, ...) are with the bone moves in the
   * viewport, sharing their options. */

  rig_operator("armature.symmetrize", "Symmetrize",
               "Make the other side of the selected bones (all bones when none are selected) by their _L and _R "
               "names, mirrored across the bone layer's vertical axis",
               armature_poll, [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Layer &armature = *active_armature(C);
                 const bool any_selected = std::any_of(armature.bones.begin(), armature.bones.end(),
                                                       [](const Bone &b) { return b.selected; });
                 std::vector<uint64_t> sources;
                 for (const Bone &b : armature.bones) {
                   if ((b.selected || !any_selected) && bone_mirror_name(b.name) != b.name) {
                     sources.push_back(b.id);
                   }
                 }
                 if (sources.empty()) {
                   report(ReportType::Warning, "Name bones with _L or _R (or .L, .R, Left, Right) to symmetrize them");
                   return OpStatus::Cancelled;
                 }
                 const auto reflect = [](float2 p) { return float2(-p.x, p.y); };
                 int made = 0;
                 /* Parents before children, so a mirrored child finds its mirrored parent. */
                 for (size_t pass = 0; pass < armature.bones.size() && !sources.empty(); pass++) {
                   for (auto it = sources.begin(); it != sources.end();) {
                     const Bone *src = bone_find(armature, *it);
                     const bool parent_pending =
                         src->parent != 0 && std::find(sources.begin(), sources.end(), src->parent) != sources.end();
                     if (parent_pending) {
                       ++it;
                       continue;
                     }
                     const std::vector<BonePose> poses = armature_pose(armature);
                     const BonePose *sp = bone_pose_find(poses, src->id);
                     const std::string other = bone_mirror_name(src->name);
                     const uint64_t parent = src->parent ? bone_mirror_id(armature, src->parent) : 0;
                     const BonePose *pp = bone_pose_find(poses, parent);
                     const Transform2D parent_setup = pp ? pp->setup : Transform2D();
                     Bone copy = *src;
                     copy.name = other;
                     copy.parent = parent;
                     bone_select(copy, false);
                     copy.connected = src->connected && parent != 0;
                     copy.position_keys = {};
                     copy.angle_keys = {};
                     copy.scale_keys = {};
                     const float length = src->length;
                     bone_place(copy, parent_setup, reflect(sp->setup_head), reflect(sp->setup_tail));
                     copy.length = length;
                     copy.scale = src->scale;
                     if (Bone *existing = const_cast<Bone *>(bone_find_by_name(armature, other))) {
                       /* The other side exists: move it to match, keeping its id and bindings. */
                       existing->parent = copy.parent;
                       existing->position = copy.position;
                       existing->angle = copy.angle;
                       existing->length = copy.length;
                       existing->scale = copy.scale;
                       existing->strength = copy.strength;
                     }
                     else {
                       copy.id = armature.next_bone_id++;
                       armature.bones.push_back(copy);
                     }
                     made++;
                     it = sources.erase(it);
                   }
                 }
                 return made > 0 ? OpStatus::Finished : OpStatus::Cancelled;
               });

  /* --- Modifiers ----------------------------------------------------------------------- */
  const auto modifier_layer = [](Context &C) -> Layer * {
    Layer *layer = C.document ? document_active_layer(*C.document) : nullptr;
    return layer && !layer->is_group() ? layer : nullptr;
  };
  rig_operator("object.modifier_add", "Add Modifier",
               "Add a modifier to the active layer (type=MIRROR, BEVEL, SUBDIVIDE, CURVE_DEFORM, CAGE_DEFORM, MESH_DEFORM "
               "or BOOLEAN; a Boolean takes the settings object.modifier_boolean_set does)",
               [modifier_layer](Context &C) { return modifier_layer(C) != nullptr; },
               [modifier_layer](Context &C) {
                 const std::string type = op_arg(C, "type", "MIRROR");
                 if (type == "BOOLEAN") {
                   Layer &layer = *modifier_layer(C);
                   if (layer.type != LayerType::Vector) {
                     report(ReportType::Warning, "Boolean joins a drawing's shapes: pick a drawing layer");
                     return OpStatus::Cancelled;
                   }
                   Modifier m;
                   m.type = ModifierType::Boolean;
                   m.name = "Boolean";
                   /* At the bottom, as every new modifier: the stack runs top to bottom. */
                   layer.modifiers.push_back(m);
                   if (!merge_settings(C, layer, layer.modifiers.back())) {
                     layer.modifiers.pop_back();
                     return OpStatus::Cancelled;
                   }
                   return OpStatus::Finished;
                 }
                 /* Resample was called Subdivide: scripts saying SUBDIVIDE still add it. */
                 if (type == "RESAMPLE" || type == "SUBDIVIDE") {
                   Layer &layer = *modifier_layer(C);
                   if (layer.type != LayerType::Vector) {
                     report(ReportType::Warning, "Resample adds points to a drawing's segments: pick a drawing layer");
                     return OpStatus::Cancelled;
                   }
                   Modifier m;
                   m.type = ModifierType::Resample;
                   m.name = "Resample";
                   /* Points per segment to start with: it always adds some, and how many is easy to tell. */
                   m.subdiv_by_length = false;
                   /* Above the Armature modifier: the bones then bend the split curve (below it, it would
                    * only split the bent one). */
                   auto at = std::find_if(layer.modifiers.begin(), layer.modifiers.end(),
                                          [](const Modifier &x) { return x.type == ModifierType::Armature; });
                   layer.modifiers.insert(at, m);
                   return OpStatus::Finished;
                 }
                 if (type == "CURVE_DEFORM" || type == "CAGE_DEFORM" || type == "MESH_DEFORM") {
                   Layer &layer = *modifier_layer(C);
                   Modifier m;
                   m.type = type == "CURVE_DEFORM" ? ModifierType::CurveDeform
                            : type == "CAGE_DEFORM" ? ModifierType::CageDeform
                                                    : ModifierType::MeshDeform;
                   if (layer.type != LayerType::Vector) {
                     report(ReportType::Warning, std::string(deform_type_name(m.type)) + " bends a drawing: pick a drawing layer");
                     return OpStatus::Cancelled;
                   }
                   m.name = deform_type_name(m.type);
                   /* A free name: its Custom point group is named after it. */
                   for (int k = 1; std::any_of(layer.modifiers.begin(), layer.modifiers.end(),
                                               [&](const Modifier &x) { return x.name == m.name; });
                        k++)
                   {
                     char name[48];
                     std::snprintf(name, sizeof(name), "%s.%03d", deform_type_name(m.type), k);
                     m.name = name;
                   }
                   /* At the bottom, below Armature: it measures from the drawing before the bones and shares
                    * each point between them by its weight. */
                   layer.modifiers.push_back(m);
                   return OpStatus::Finished;
                 }
                 if (type == "BEVEL") {
                   Layer &layer = *modifier_layer(C);
                   if (layer.type != LayerType::Vector) {
                     report(ReportType::Warning, "Bevel cuts a drawing's corners: pick a drawing layer");
                     return OpStatus::Cancelled;
                   }
                   Modifier m;
                   m.type = ModifierType::Bevel;
                   m.name = "Bevel";
                   /* At the bottom, as in Blender: drag it above the Armature modifier to bevel the
                    * drawing before the bones bend it. */
                   layer.modifiers.push_back(m);
                   return OpStatus::Finished;
                 }
                 if (type != "MIRROR") {
                   report(ReportType::Error, type == "ARMATURE" ? "The Armature modifier comes with a bone layer"
                                                                : "Unknown modifier type " + type);
                   return OpStatus::Cancelled;
                 }
                 Layer &layer = *modifier_layer(C);
                 Modifier m;
                 /* At the bottom, as in Blender: the stack runs top to bottom. */
                 layer.modifiers.push_back(m);
                 return OpStatus::Finished;
               });
  rig_operator("object.modifier_remove", "Remove Modifier", "Remove modifier `index` from the active layer",
               [modifier_layer](Context &C) { return modifier_layer(C) != nullptr; },
               [modifier_layer](Context &C) {
                 Layer &layer = *modifier_layer(C);
                 const int index = op_arg_int(C, "index", -1);
                 if (index < 0 || index >= int(layer.modifiers.size())) {
                   return OpStatus::Cancelled;
                 }
                 if (layer.modifiers[index].type == ModifierType::Armature) {
                   report(ReportType::Warning, "The Armature modifier belongs to the bone layer; move the layer out "
                                               "of it to remove it");
                   return OpStatus::Cancelled;
                 }
                 layer.modifiers.erase(layer.modifiers.begin() + index);
                 return OpStatus::Finished;
               });
  rig_operator("object.modifier_move_to", "Move Modifier to Index",
               "Move modifier `index` to position `to` in the active layer's stack (dragging it in the Modifiers tab)",
               [modifier_layer](Context &C) { return modifier_layer(C) != nullptr; },
               [modifier_layer](Context &C) {
                 Layer &layer = *modifier_layer(C);
                 const int index = op_arg_int(C, "index", -1);
                 const int to = op_arg_int(C, "to", -1);
                 const int n = int(layer.modifiers.size());
                 if (index < 0 || index >= n || to < 0 || to >= n || to == index) {
                   return OpStatus::Cancelled;
                 }
                 Modifier moving = std::move(layer.modifiers[index]);
                 layer.modifiers.erase(layer.modifiers.begin() + index);
                 layer.modifiers.insert(layer.modifiers.begin() + to, std::move(moving));
                 return OpStatus::Finished;
               });
  rig_operator("object.modifier_boolean_set", "Boolean Settings",
               "Change Boolean modifier `index` of the active layer: targets= (layers' names or ids, | between "
               "them), operation= (UNION, SUBTRACT, INTERSECT), smoothing= (a radius, layer units), corners_only=1 "
               "(only grow the rounded corners where they meet, taking nothing in), shapes= "
               "(the layer's own shapes to merge, names or ids, | between; empty: all), target= with "
               "target_shapes= (which shapes of that target; empty: all), pick= with pick_shape= (the eyedropper: a "
               "layer and one of its shapes, added, or in place of layer replace=)",
               [modifier_layer](Context &C) { return modifier_layer(C) != nullptr; },
               [modifier_layer](Context &C) {
                 Layer &layer = *modifier_layer(C);
                 const int index = op_arg_int(C, "index", -1);
                 if (index < 0 || index >= int(layer.modifiers.size()) ||
                     layer.modifiers[index].type != ModifierType::Boolean)
                 {
                   report(ReportType::Warning, "No Boolean modifier there");
                   return OpStatus::Cancelled;
                 }
                 Modifier m = layer.modifiers[index];
                 if (!merge_settings(C, layer, m)) {
                   return OpStatus::Cancelled;
                 }
                 layer.modifiers[index] = std::move(m);
                 return OpStatus::Finished;
               });
  rig_operator("object.modifier_apply", "Apply Modifier",
               "Make modifier `index` of the active layer part of its drawing (Ctrl A over it; on Setup, frame "
               "0), as Blender's Apply: a Mirror's reflection becomes points of its own; a Boolean's outline "
               "replaces the shapes it joined and the shapes it took from other layers leave them. The Armature "
               "modifier can't be applied",
               [modifier_layer](Context &C) { return modifier_layer(C) != nullptr; },
               [modifier_layer](Context &C) {
                 Layer &layer = *modifier_layer(C);
                 const int index = op_arg_int(C, "index", -1);
                 if (index < 0 || index >= int(layer.modifiers.size())) {
                   return OpStatus::Cancelled;
                 }
                 if (!modifier_can_apply(layer.modifiers[index])) {
                   report(ReportType::Warning, modifier_is_deform(layer.modifiers[index].type)
                                                   ? "A deform modifier can't be applied: on the setup frame it changes nothing"
                                                   : "The Armature modifier can't be applied: it comes with the bone layer");
                   return OpStatus::Cancelled;
                 }
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Document &doc = *C.document;
                 const Modifier m = layer.modifiers[index];
                 if (m.type == ModifierType::Resample) {
                   /* The new points would have no keys: on an animated drawing they'd stay put while
                    * their neighbours move. */
                   bool animated = false;
                   for (const MeshPoint &p : layer.drawing.points) {
                     animated = animated || p.pos_keys.animated();
                   }
                   for (const Curve &curve : layer.drawing.curves) {
                     for (const CurvePoint &cp : curve.points) {
                       animated = animated || cp.handle_in_keys.animated() || cp.handle_out_keys.animated();
                     }
                   }
                   if (animated) {
                     report(ReportType::Warning, "Resample can't be applied to a drawing whose points are animated");
                     return OpStatus::Cancelled;
                   }
                   std::vector<int> fresh;
                   layer.drawing = drawing_subdivide(layer.drawing, m, false, &fresh);
                   drawing_ensure_ids(layer.drawing);
                   if (m.subdiv_from_bones && !fresh.empty()) {
                     for (MeshPoint &p : layer.drawing.points) {
                       p.selected = false;
                     }
                     for (const int i : fresh) {
                       layer.drawing.points[size_t(i)].selected = true;
                     }
                     layer_automatic_weights(doc, layer, {}, true);
                     for (const int i : fresh) {
                       layer.drawing.points[size_t(i)].selected = false;
                     }
                   }
                   layer.modifiers.erase(layer.modifiers.begin() + index);
                   C.undo_name = "Apply Resample";
                   return OpStatus::Finished;
                 }
                 if (m.type == ModifierType::Bevel) {
                   layer.drawing = drawing_bevel(layer.drawing, m);
                   drawing_ensure_ids(layer.drawing);
                   layer.modifiers.erase(layer.modifiers.begin() + index);
                   C.undo_name = "Apply Bevel";
                   return OpStatus::Finished;
                 }
                 if (m.type == ModifierType::Mirror) {
                   const Layer *armature = layer_armature(doc, layer.id);
                   const auto flip = [armature](uint64_t bone) { return armature ? bone_mirror_id(*armature, bone) : bone; };
                   layer.drawing = drawing_mirror(layer.drawing, m, flip);
                   layer.modifiers.erase(layer.modifiers.begin() + index);
                   C.undo_name = "Apply Mirror";
                   return OpStatus::Finished;
                 }
                 /* The setup drawings, where the layers are. */
                 const Transform2D to_layer = layer_world_transform(doc, layer.id).inverted();
                 std::vector<MergeSource> sources;
                 std::vector<VectorDrawing> drawings;
                 drawings.reserve(m.merge_targets.size());
                 for (const MergeTarget &t : m.merge_targets) {
                   const Layer *target = layer_find(doc, t.layer);
                   if (target && target->id != layer.id && target->type == LayerType::Vector &&
                       (m.merge_operation != MergeOperation::Union || layer_shown_in_document(doc, target->id)))
                   {
                     drawings.push_back(target->drawing);
                     sources.push_back({nullptr, to_layer * layer_world_transform(doc, target->id), t.shapes});
                   }
                 }
                 for (size_t i = 0; i < sources.size(); i++) {
                   sources[i].drawing = &drawings[i];
                 }
                 if (!drawing_merge(layer.drawing, m.merge_shapes, sources, m.merge_operation, m.merge_smoothing,
                                    m.merge_corners_only))
                 {
                   report(ReportType::Warning, "The layer has no shape going all the way round to merge into");
                   return OpStatus::Cancelled;
                 }
                 /* The shapes it took in leave their layers (their points and curves stay). */
                 for (size_t i = 0; !m.merge_corners_only && i < m.merge_targets.size(); i++) {
                   Layer *target = layer_find(doc, m.merge_targets[i].layer);
                   if (!target || target->id == layer.id || target->type != LayerType::Vector ||
                       (m.merge_operation == MergeOperation::Union && !layer_shown_in_document(doc, target->id)))
                   {
                     continue;
                   }
                   VectorDrawing &d = target->drawing;
                   std::erase_if(d.shapes, [&](const Shape &s) {
                     return merge_takes_shape(d, s, m.merge_targets[i].shapes);
                   });
                 }
                 layer.modifiers.erase(layer.modifiers.begin() + index);
                 C.undo_name = "Apply Boolean";
                 return OpStatus::Finished;
               });
  rig_operator("object.modifier_duplicate", "Duplicate Modifier",
               "Copy modifier `index` of the active layer, just below it (Shift D over it), as Blender's Duplicate. "
               "The Armature modifier comes with the bone layer and isn't copied",
               [modifier_layer](Context &C) { return modifier_layer(C) != nullptr; },
               [modifier_layer](Context &C) {
                 Layer &layer = *modifier_layer(C);
                 const int index = op_arg_int(C, "index", -1);
                 if (index < 0 || index >= int(layer.modifiers.size())) {
                   return OpStatus::Cancelled;
                 }
                 if (layer.modifiers[index].type == ModifierType::Armature) {
                   report(ReportType::Warning, "The Armature modifier comes with the bone layer: there's one");
                   return OpStatus::Cancelled;
                 }
                 Modifier copy = layer.modifiers[index];
                 copy.uid = 0;
                 /* Blender's names: "Boolean.001", the next number free. */
                 std::string base = copy.name;
                 if (const size_t dot = base.rfind('.');
                     dot != std::string::npos && dot + 4 == base.size() &&
                     std::all_of(base.begin() + long(dot) + 1, base.end(), [](char ch) { return ch >= '0' && ch <= '9'; }))
                 {
                   base = base.substr(0, dot);
                 }
                 for (int n = 1;; n++) {
                   char name[16];
                   std::snprintf(name, sizeof(name), ".%03d", n);
                   if (std::none_of(layer.modifiers.begin(), layer.modifiers.end(),
                                    [&](const Modifier &x) { return x.name == base + name; }))
                   {
                     copy.name = base + name;
                     break;
                   }
                 }
                 layer.modifiers.insert(layer.modifiers.begin() + index + 1, std::move(copy));
                 return OpStatus::Finished;
               });
  rig_operator("object.modifier_move", "Move Modifier",
               "Move modifier `index` up (direction=-1) or down (direction=1) the active layer's stack",
               [modifier_layer](Context &C) { return modifier_layer(C) != nullptr; },
               [modifier_layer](Context &C) {
                 Layer &layer = *modifier_layer(C);
                 const int index = op_arg_int(C, "index", -1);
                 const int to = index + (op_arg_int(C, "direction", 1) < 0 ? -1 : 1);
                 if (index < 0 || index >= int(layer.modifiers.size()) || to < 0 || to >= int(layer.modifiers.size())) {
                   return OpStatus::Cancelled;
                 }
                 std::swap(layer.modifiers[index], layer.modifiers[to]);
                 return OpStatus::Finished;
               });
}

}  // namespace oanim
