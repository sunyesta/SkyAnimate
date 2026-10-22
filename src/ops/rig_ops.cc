/* Rigging and modifier operators: bones, binding, symmetrize, and the modifier stack.
 *
 * As in Moho, changing the rig (adding or deleting bones, binding) only happens on the setup
 * frame, 0; posing (moving and turning bones) works on any frame and keys like other edits. */

#include <algorithm>
#include <cmath>
#include <map>
#include <tuple>

#include "anim/animation.hh"
#include "anim/deform.hh"
#include "anim/dynamics.hh"
#include "anim/merge.hh"
#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

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
                         std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot.poll = std::move(poll);
  ot.exec = std::move(exec);
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
                 layer.bones.clear();
                 layer.active_bone = 0;
                 return OpStatus::Finished;
               });

  const auto bone_layer_active = [](Context &C) {
    const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
    return l && l->is_bone_layer();
  };
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
  rig_operator("weight.automatic", "Automatic Weights",
               "Give the selected points (all points when none are selected) of the layers being edited "
               "weights from every bone: nearer bones pull harder, as Blender's Automatic Weights. "
               "bones=SELECTED uses only the selected bones",
               weight_poll, [](Context &C) {
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
                 return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
               });

  rig_operator("weight.assign", "Assign",
               "Set the active bone's weight on the selected points to `weight` (default: the tool's Weight), as "
               "Blender's Assign; with Normalize the other bones share what is left",
               weight_poll, [](Context &C) {
                 const ToolSettings &ts = C.wm->tool_settings;
                 const float value = std::clamp(op_arg_float(C, "weight", ts.weight_value), 0.0f, 1.0f);
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
                 return OpStatus::Finished;
               });

  rig_operator("weight.remove", "Remove",
               "Take the selected points out of the active bone (as Blender's Remove); a point left with no bones "
               "stays where it is",
               weight_poll, [](Context &C) {
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
                 return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
               });

  rig_operator("weight.normalize", "Normalize",
               "Make each selected point's weights (all points' when none are selected) add up to 1",
               weight_poll, [](Context &C) {
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
                 return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
               });

  rig_operator("layer.parent_bone_set", "Parent to Bone",
               "Make the active and selected layers inside a bone layer follow bone `bone` as a whole, as "
               "parenting to a bone in Blender (\"\" frees them); `deform`=LAYER moves the layer (a bone's "
               "squash and stretch squashes its lines too), POINTS its points (lines keep their width). "
               "Either alone, or both. Setup frame only",
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

  rig_operator("armature.bone_primitive_add", "Add Bone",
               "Add a new bone at the bone layer's origin, pointing up (Shift A, as in Blender)", armature_poll,
               [](Context &C) {
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 armature_add_bone(*active_armature(C), op_arg(C, "name"), {0.0f, 0.0f}, {0.0f, 0.5f}, 0);
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
    ot.poll = [](Context &C) { return C.document && collection_armature(*C.document); };
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

  /* Alt G / Alt R / Alt S: clear the pose of the selected bones (any frame but the setup one). */
  for (const auto &[idname, name, what] :
       {std::tuple{"armature.clear_location", "Clear Location", 0}, std::tuple{"armature.clear_rotation", "Clear Rotation", 1},
        std::tuple{"armature.clear_scale", "Clear Scale", 2}})
  {
    const int kind = what;
    rig_operator(
        idname, name,
        kind == 0   ? "Put the selected bones back where they are in the setup pose (Alt G, away from frame 0)"
        : kind == 1 ? "Turn the selected bones back to their setup angle (Alt R, away from frame 0)"
                    : "Give the selected bones back their setup scale (Alt S, away from frame 0)",
        [](Context &C) {
          const Layer *l = C.document ? document_active_layer(*C.document) : nullptr;
          return l && l->is_bone_layer() && C.document->frame_current != 0 && armature_any_picked(*l);
        },
        [kind](Context &C) {
          Layer &armature = *document_active_layer(*C.document);
          return armature_clear_pose(armature, kind == 0, kind == 1, kind == 2) ? OpStatus::Finished
                                                                                : OpStatus::Cancelled;
        });
  }

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
               "Add a modifier to the active layer (type=MIRROR or BOOLEAN; a Boolean takes the settings "
               "object.modifier_boolean_set does)",
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
                   /* It works on the drawing as posed, so it goes last (it runs last wherever it is). */
                   layer.modifiers.push_back(m);
                   if (!merge_settings(C, layer, layer.modifiers.back())) {
                     layer.modifiers.pop_back();
                     return OpStatus::Cancelled;
                   }
                   return OpStatus::Finished;
                 }
                 if (type != "MIRROR") {
                   report(ReportType::Error, type == "ARMATURE" ? "The Armature modifier comes with a bone layer"
                                                                : "Unknown modifier type " + type);
                   return OpStatus::Cancelled;
                 }
                 Layer &layer = *modifier_layer(C);
                 Modifier m;
                 /* Like Blender: a new modifier goes above the Armature one, so it works on the setup
                  * drawing and the bones move the result. */
                 const auto armature = std::find_if(layer.modifiers.begin(), layer.modifiers.end(),
                                                    [](const Modifier &x) { return x.type == ModifierType::Armature; });
                 layer.modifiers.insert(armature, m);
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
                   report(ReportType::Warning, "The Armature modifier can't be applied: it comes with the bone layer");
                   return OpStatus::Cancelled;
                 }
                 if (!on_setup_frame(C)) {
                   return OpStatus::Cancelled;
                 }
                 Document &doc = *C.document;
                 const Modifier m = layer.modifiers[index];
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
