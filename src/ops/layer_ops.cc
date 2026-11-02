/* Layer operators, named after Moho's layer commands and Blender's style of idnames. */

#include "ops/edit_ops.hh"

#include <algorithm>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <tuple>

#include "anim/draw_order.hh"
#include "anim/drivers.hh"
#include "anim/layer_pick.hh"
#include "anim/reference.hh"
#include "anim/switch.hh"
#include "anim/bone_family.hh"
#include "anim/animation.hh"
#include "core/log.hh"
#include "data/layer_mirror.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/vector.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

static bool active_layer_poll(Context &C)
{
  return C.document && document_active_layer(*C.document) != nullptr;
}

/* Picks `value` on `sw` (for the whole hold with `hold`). While the active layer was inside one of its
 * children, it moves to the picked child (or the switch, for None or Rig), so the viewport shows the
 * pick rather than the child being edited. */
static void switch_pick(Document &doc, Layer &sw, float value, bool hold)
{
  const uint64_t sw_id = sw.id;
  const bool editing_child = switch_child_holding(doc, sw, doc.active_layer) != nullptr;
  if (doc.frame_current != 0 && !doc.auto_key && !hold) {
    report(ReportType::Info, sw.switch_keys.animated() ? "Auto Keying is off: shown until the frame changes, not keyed (I keys it)"
                                                       : "Auto Keying is off: not keyed, so it shows on every frame (I keys it)");
  }
  if (hold) {
    switch_set_hold(doc, sw, value);
  }
  else {
    switch_set(doc, sw, value);
  }
  Layer &now = *layer_find(doc, sw_id);
  if (editing_child) {
    /* Working in the outgoing child's bones: their counterparts in the incoming child are selected
     * (bone swaps), and it becomes the bone layer worked in. */
    const Layer *was = document_active_layer(doc);
    std::vector<uint64_t> selected;
    const uint64_t was_id = was && was->is_bone_layer() ? was->id : 0;
    if (was_id) {
      for (const Bone &b : was->bones) {
        if (b.selected) {
          selected.push_back(b.id);
        }
      }
    }
    Layer *child = const_cast<Layer *>(switch_child(now, value));
    doc.active_layer = child ? child->id : sw_id;
    if (Layer *to = child && was_id ? switch_child_armature(*child) : nullptr) {
      armature_deselect_all(*to);
      for (const uint64_t id : selected) {
        if (Bone *b = bone_find(*to, bone_counterpart(doc, was_id, id, *to))) {
          bone_select(*b, true);
          to->active_bone = b->id;
        }
      }
      doc.active_layer = to->id;
    }
    switches_resolve(doc, doc.frame_current);
  }
}

static void layer_operator(const char *idname, const char *name, const char *description,
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

/* Clears the Layers editor's extra selection (Ctrl or Shift click). */
static void layers_deselect(Document &doc)
{
  const auto clear = [](auto &self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      layer.selected = false;
      self(self, layer.children);
    }
  };
  clear(clear, doc.layers);
}

/* --- Layer Symmetrize -------------------------------------------------------------------- */

/* The space a layer mirrors in: its bone layer's (across the line bone Symmetrize uses), or the
 * document's. */
static Transform2D symmetrize_axis_space(Document &doc, uint64_t id)
{
  for (const Layer *p = layer_parent(doc, id); p; p = layer_parent(doc, p->id)) {
    if (p->is_bone_layer()) {
      return layer_setup_world_transform(doc, p->id);
    }
  }
  return Transform2D();
}

static Transform2D parent_setup_space(Document &doc, uint64_t id)
{
  const Layer *p = layer_parent(doc, id);
  return p ? layer_setup_world_transform(doc, p->id) : Transform2D();
}

/* Which side of its axis a layer's drawings sit on (the middle of their box, on the setup frame):
 * 1 the + side, -1 the - side, 0 on the axis or nothing drawn. */
static int symmetrize_side(Document &doc, const Layer &layer)
{
  const Transform2D axis = symmetrize_axis_space(doc, layer.id).inverted();
  float lo = std::numeric_limits<float>::max(), hi = -lo;
  const auto walk = [&](auto &&self, const Layer &l) -> void {
    if (l.has_drawing()) {
      const Transform2D to_axis = axis * layer_setup_world_transform(doc, l.id);
      for (const MeshPoint &p : layer_drawing(doc, l).points) {
        const float x = to_axis.apply(p.pos).x;
        lo = std::min(lo, x);
        hi = std::max(hi, x);
      }
    }
    for (const Layer &child : l.children) {
      self(self, child);
    }
  };
  walk(walk, layer);
  if (lo > hi) {
    return 0;
  }
  const float middle = (lo + hi) * 0.5f;
  return middle > 1e-4f ? 1 : middle < -1e-4f ? -1 : 0;
}

static void clear_transform_keys(Layer &l)
{
  l.position_keys = {};
  l.rotation_keys = {};
  l.scale_keys = {};
  l.shear_keys = {};
}

/* Sets a layer's position, rotation and scale from `t` (no shear), its scale's x keeping the sign
 * `negative_x` gives. */
static void layer_set_local_transform(Layer &l, const Transform2D &t, bool negative_x)
{
  l.set_local_transform(t, negative_x);
}

/* The bones of the bone layer `id` is in, each to its other side's (_L to _R), or to itself. */
static std::map<uint64_t, uint64_t> symmetrize_bone_flip(const Document &doc, uint64_t id)
{
  std::map<uint64_t, uint64_t> flip;
  if (const Layer *armature = layer_armature(doc, id)) {
    for (const Bone &b : armature->bones) {
      const Bone *o = bone_find_by_name(*armature, bone_mirror_name(b.name));
      flip[b.id] = o ? o->id : b.id;
    }
  }
  return flip;
}

/* A bone layer copied with a mirrored group: its bones mirrored across its own vertical axis, as its drawings are,
 * from the setup pose (their keys go, as the drawings' do). Each takes its other side's name (arm_L and arm_R
 * swap) but keeps its id, so the weights, Parent Bone, custom shapes and constraints of what is inside still find
 * it, now on the mirrored bone. */
static void symmetrize_bones(Layer &armature)
{
  const std::vector<BonePose> was = armature_pose(armature, true);
  const auto reflect = [](float2 p) { return float2(-p.x, p.y); };
  /* Roots come before their children, so each finds its parent mirrored already. */
  for (size_t i = 0; i < armature.bones.size(); i++) {
    const BonePose &bp = was[i];
    const std::vector<BonePose> now = armature_pose(armature, true);
    Bone &b = armature.bones[i];
    const BonePose *parent = b.parent ? bone_pose_find(now, b.parent) : nullptr;
    const Transform2D to_parent = (parent ? parent->setup : Transform2D()).inverted();
    const float2 h = to_parent.apply(reflect(bp.setup_head)), t = to_parent.apply(reflect(bp.setup_tail));
    b.position = b.setup_position = h;
    b.angle = b.setup_angle = std::atan2(t.y - h.y, t.x - h.x);
    b.position_keys = {};
    b.angle_keys = {};
    b.scale_keys = {};
    /* Mirrored, a turn one way is a turn the other. */
    for (BoneConstraint &c : b.constraints) {
      const float lo = c.min_angle;
      c.min_angle = -c.max_angle;
      c.max_angle = -lo;
      /* A Follow Point's place on its deform goes across with the drawings, from the setup. */
      if (c.type == BoneConstraintType::FollowPoint) {
        c.follow_point.x = -c.follow_point.x;
        c.follow_point2.x = -c.follow_point2.x;
        c.follow_rest.x = -c.follow_rest.x;
        c.follow_rest2.x = -c.follow_rest2.x;
        c.follow_point_id = c.follow_point2_id = 0;
        c.follow_shift = c.follow_shift2 = {};
        c.follow_shift_keys = {};
        c.follow_shift2_keys = {};
      }
    }
  }
  for (Bone &b : armature.bones) {
    b.name = bone_mirror_name(b.name);
  }
}

/* The copy's contents mirrored in place: drawings flipped (setup pose, no point keys), weights and
 * Parent Bone moved to the other side's bones, the layers inside flipped about their parent's
 * axis. A bone layer inside has its bones mirrored in place (symmetrize_bones), so what is inside it keeps
 * its bones by id. */
static void symmetrize_contents(Layer &l, const std::map<uint64_t, uint64_t> &bone_flip, bool top)
{
  const auto flip_bone = [&](uint64_t id) {
    const auto it = bone_flip.find(id);
    return it != bone_flip.end() ? it->second : id;
  };
  if (l.has_drawing()) {
    drawing_mirror_setup_x(l.drawing);
    for (MeshPoint &p : l.drawing.points) {
      for (BoneWeight &w : p.weights) {
        w.bone = flip_bone(w.bone);
      }
    }
  }
  l.parent_bone = flip_bone(l.parent_bone);
  /* A reference kept (its original mirrored with it): what it moved, mirrored too; as the drawings, from the
   * setup frame. Its own weights take the other side's bones. */
  if (l.is_reference()) {
    const auto mirrored = [](ReferencePoint &rp) {
      rp.offset.x = -rp.offset.x;
      rp.offset_keys = {};
    };
    for (auto &[id, rp] : l.reference.points) {
      mirrored(rp);
    }
    for (auto &[key, rp] : l.reference.handles) {
      mirrored(rp);
    }
    for (auto &[id, weights] : l.reference.weights) {
      for (BoneWeight &w : weights) {
        w.bone = flip_bone(w.bone);
      }
    }
    l.reference.built = false;
  }
  /* A Mirror modifier's axis goes across with the drawing. */
  for (Modifier &mod : l.modifiers) {
    mod.center.x = -mod.center.x;
    /* A Follow Deform's points too, each its own place across from where it was: a picked point is on the far side, and
     * as a drawing's, its keys don't come. */
    mod.deform_point.x = -mod.deform_point.x;
    mod.deform_point2.x = -mod.deform_point2.x;
    mod.deform_point_id = mod.deform_point2_id = 0;
    mod.deform_shift = mod.deform_shift2 = {};
    mod.deform_shift_keys = {};
    mod.deform_shift2_keys = {};
  }
  if (!top) {
    l.position.x = -l.position.x;
    l.rotation = -l.rotation;
    /* Mirrored, each axis tilts the other way. */
    l.shear = -l.shear;
    clear_transform_keys(l);
  }
  l.selected = false;
  if (l.is_bone_layer()) {
    symmetrize_bones(l);
  }
  const std::map<uint64_t, uint64_t> same;
  for (Layer &child : l.children) {
    symmetrize_contents(child, l.is_bone_layer() ? same : bone_flip, false);
  }
}

/* A copy to mirror: a reference stays one when its original is mirrored with it (it then shows that one's
 * copy) or has a side of its own whose other side is there (Ear_L's reference shows Ear_R); any other becomes a
 * plain layer showing what it showed, there being nothing mirrored for it to show. */
static void symmetrize_unreference(const Document &doc, Layer &l, const std::set<uint64_t> &inside)
{
  if (l.is_reference() && !inside.count(l.reference_source)) {
    const Layer *source = layer_find(doc, l.reference_source);
    const std::string other = source ? bone_mirror_name(source->name) : std::string();
    bool partner = false;
    for (const Layer *o : document_layers_flat(doc)) {
      partner |= source && other != source->name && o->name == other;
    }
    if (!partner) {
      l.drawing = layer_drawing(doc, l);
      l.reference_source = 0;
      l.reference = {};
    }
  }
  for (Layer &child : l.children) {
    symmetrize_unreference(doc, child, inside);
  }
}

static const Layer *layer_named(const Document &doc, const std::string &name)
{
  for (const Layer *l : document_layers_flat(doc)) {
    if (l->name == name) {
      return l;
    }
  }
  return nullptr;
}

/* What Symmetrize did, for the report, and the copies it made (`copies` their top layers,
 * `copy_ids` every layer in them). */
struct SymmetrizeLog {
  std::vector<std::string> made, replaced, centred, skipped;
  std::vector<uint64_t> copies;
  std::set<uint64_t> copy_ids;
};

static void layer_tree_walk(Layer &l, const std::function<void(Layer &)> &fn)
{
  fn(l);
  for (Layer &child : l.children) {
    layer_tree_walk(child, fn);
  }
}

/* Makes (or remakes) the other side of the sided layer `id`, from whichever of the pair sits on the
 * `from` side (1 +, -1 -). */
static void symmetrize_pair(Document &doc, uint64_t id, int from, std::set<uint64_t> &done, SymmetrizeLog &log)
{
  const Layer *layer = layer_find(doc, id);
  const std::string other_name = bone_mirror_name(layer->name);
  /* The partner: a layer beside it with that name, or one anywhere. */
  const Layer *partner = nullptr;
  int index = 0;
  if (const std::vector<Layer> *siblings = layer_container(doc, id, &index)) {
    for (const Layer &l : *siblings) {
      if (l.name == other_name) {
        partner = &l;
      }
    }
  }
  if (!partner) {
    partner = layer_named(doc, other_name);
  }
  if (done.count(id) || (partner && done.count(partner->id))) {
    return;
  }
  done.insert(id);
  if (partner) {
    done.insert(partner->id);
  }
  const std::string side = from > 0 ? "+" : "-";
  const Layer *source = nullptr, *target = nullptr;
  if (symmetrize_side(doc, *layer) == from) {
    source = layer;
    target = partner;
  }
  else if (partner && symmetrize_side(doc, *partner) == from) {
    source = partner;
    target = layer;
  }
  else {
    log.skipped.push_back(layer->name + (partner ? " (neither it nor " + partner->name + " is on the " + side + " side)"
                                                 : " (not on the " + side + " side)"));
    return;
  }
  const uint64_t source_id = source->id, target_id = target ? target->id : 0;

  /* Where it goes: flipped across the source's axis, in the target's parent's space. */
  const Transform2D flip_x = Transform2D::scaling({-1.0f, 1.0f});
  const Transform2D axis = symmetrize_axis_space(doc, source_id);
  const Transform2D target_parent = parent_setup_space(doc, target_id ? target_id : source_id);
  const Transform2D local = target_parent.inverted() * axis * flip_x * axis.inverted() *
                            parent_setup_space(doc, source_id) * source->local_transform() * flip_x;

  Layer plain = *source;
  std::set<uint64_t> inside;
  layer_tree_walk(plain, [&](Layer &l) { inside.insert(l.id); });
  symmetrize_unreference(doc, plain, inside);
  std::map<uint64_t, uint64_t> remap;
  Layer copy = layer_copy_with_new_ids(doc, plain, &remap);
  symmetrize_contents(copy, symmetrize_bone_flip(doc, source_id), true);
  layer_set_local_transform(copy, local, source->scale.x < 0.0f);
  clear_transform_keys(copy);

  /* Remaking: the old contents go, and the new layers take the ids of the old ones of the same name,
   * so masks naming those still find them. */
  std::map<std::string, uint64_t> old_ids;
  if (target_id) {
    Layer *t = layer_find(doc, target_id);
    for (Layer &child : t->children) {
      layer_tree_walk(child, [&](Layer &l) { old_ids.emplace(l.name, l.id); });
    }
    t->children.clear();
  }
  std::map<uint64_t, uint64_t> reassign;
  for (Layer &child : copy.children) {
    layer_tree_walk(child, [&](Layer &l) {
      const std::string flipped = bone_mirror_name(l.name);
      if (const auto it = old_ids.find(flipped); it != old_ids.end()) {
        reassign[l.id] = it->second;
        l.id = it->second;
        old_ids.erase(it);
        l.name = flipped;
      }
      else {
        l.name = layer_named(doc, flipped) ? document_layer_unique_name(doc, flipped) : flipped;
      }
    });
  }
  if (!reassign.empty()) {
    for (auto &[from_id, to_id] : remap) {
      if (const auto it = reassign.find(to_id); it != reassign.end()) {
        to_id = it->second;
      }
    }
    const auto reassigned = [&](uint64_t i) {
      const auto it = reassign.find(i);
      return it != reassign.end() ? it->second : i;
    };
    layer_tree_walk(copy, [&](Layer &l) { layer_links_map(l, reassigned); });
  }

  uint64_t placed = 0;
  if (target_id) {
    Layer &t = *layer_find(doc, target_id);
    copy.id = t.id;
    copy.name = t.name;
    copy.expanded = t.expanded;
    remap[source_id] = t.id;
    t = std::move(copy);
    placed = target_id;
    log.replaced.push_back(t.name);
  }
  else {
    copy.name = layer_named(doc, other_name) ? document_layer_unique_name(doc, other_name) : other_name;
    placed = copy.id;
    log.made.push_back(copy.name);
    /* Above the source, so a Clip to Below source and its copy clip to the same layer. */
    std::vector<Layer> *container = layer_container(doc, source_id, &index);
    container->insert(container->begin() + index, std::move(copy));
  }
  log.copies.push_back(placed);
  log.copy_ids.insert(placed);
  for (const auto &[from_id, to_id] : remap) {
    log.copy_ids.insert(to_id);
  }
}

/* After every copy is made: their mask rows and Draw Order naming a sided layer outside them name
 * that layer's other side instead, when there is one. */
static void symmetrize_flip_links(Document &doc, const SymmetrizeLog &log)
{
  const std::function<uint64_t(uint64_t)> other = [&](uint64_t id) {
    const Layer *l = log.copy_ids.count(id) ? nullptr : layer_find(doc, id);
    if (!l) {
      return id;
    }
    const std::string name = bone_mirror_name(l->name);
    const Layer *o = name != l->name ? layer_named(doc, name) : nullptr;
    return o ? o->id : id;
  };
  for (const uint64_t id : log.copies) {
    if (Layer *copy = layer_find(doc, id)) {
      layer_tree_walk(*copy, [&](Layer &l) { layer_links_map(l, other); });
    }
  }
}

void layer_operators_register()
{
  {
    /* Blender's Outliner Show Active (. or Numpad .). */
    OperatorType ot;
    ot.idname = "layer.show_active";
    ot.name = "Show Active";
    ot.description = "Scroll the Layers editor to the active layer, opening the groups it's in (. or Numpad ., as "
                     "Blender's Outliner)";
    ot.poll = [](Context &C) { return C.document && C.wm && document_active_layer(*C.document); };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const uint64_t id = doc.active_layer;
      bool opened = false;
      for (Layer *p = layer_parent(doc, id); p; p = layer_parent(doc, p->id)) {
        opened |= !p->expanded;
        p->expanded = true;
      }
      if (opened && C.undo) {
        /* Folding is the project's (saved), not an edit to undo. */
        C.undo->mark_changed();
      }
      C.wm->layers_show_active = id;
      C.wm->layers_show_active_serial++;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "layer.auto_focus";
    ot.name = "Auto Focus";
    ot.description = "Fold every group in the Layers editor except those holding the selected layers, and open "
                     "those (a selected group stays as it is)";
    ot.poll = [](Context &C) { return C.document && document_active_layer(*C.document); };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      std::set<uint64_t> holding, picked;
      for (const Layer *layer : document_layers_flat(doc)) {
        if (layer->selected || layer->id == doc.active_layer) {
          picked.insert(layer->id);
          for (const Layer *p = layer_parent(doc, layer->id); p; p = layer_parent(doc, p->id)) {
            holding.insert(p->id);
          }
        }
      }
      /* Not marked as a change: it follows the selection, so an unsaved mark would add nothing to keep. */
      for (const Layer *flat : document_layers_flat(doc)) {
        if (holding.count(flat->id)) {
          layer_find(doc, flat->id)->expanded = true;
        }
        else if (flat->is_group() && !picked.count(flat->id)) {
          layer_find(doc, flat->id)->expanded = false;
        }
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* The Select Layer tool and Alt-click: the layer shown at a point. */
    OperatorType ot;
    ot.idname = "layer.select_at";
    ot.name = "Select Layer";
    ot.description = "Select the drawing layer shown at x=, y= (document space; the topmost, or layer= names it), as "
                     "the Select Layer tool and Alt-click do: mode=SET (it alone), TOGGLE (add it, or take it out "
                     "if picked; Shift) or REMOVE (Ctrl). group=1 picks its outermost group instead. Media layers, "
                     "hidden layers and where a layer's masks hide it are never picked; tolerance= is how near a "
                     "line counts, in document units";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      uint64_t id = 0;
      if (op_has_arg(C, "layer")) {
        const Layer *l = layer_by_name_or_id(doc, op_arg(C, "layer"));
        id = l ? l->id : 0;
      }
      else {
        LayerPickOptions options;
        options.tolerance = std::max(0.0f, op_arg_float(C, "tolerance", 0.0f));
        options.show_guides = doc.overlays.show && doc.overlays.guides;
        if (C.wm) {
          options.local_view = C.wm->local_view;
        }
        const std::vector<uint64_t> hits =
            layers_at(doc, {op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f)}, options);
        id = hits.empty() ? 0 : hits.front();
      }
      if (id != 0 && op_arg_bool(C, "group")) {
        id = layer_pick_group(doc, id);
      }
      if (id == 0) {
        return OpStatus::Cancelled;
      }
      const std::string mode = op_arg(C, "mode", "SET");
      const LayerPickMode pick = mode == "TOGGLE" ? LayerPickMode::Toggle
                                 : mode == "REMOVE" ? LayerPickMode::Remove
                                                    : LayerPickMode::Set;
      if (mode != "SET" && mode != "TOGGLE" && mode != "REMOVE") {
        report(ReportType::Error, "mode is SET, TOGGLE or REMOVE");
        return OpStatus::Cancelled;
      }
      if (!layer_pick_apply(doc, id, pick)) {
        return OpStatus::Cancelled;
      }
      const Layer *picked = layer_find(doc, id);
      C.undo_name = std::string(pick == LayerPickMode::Remove ? "Deselect " : "Select ") + picked->name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* = : up the layer tree, as Blender's Select Parent. */
    OperatorType ot;
    ot.idname = "layer.select_parent";
    ot.name = "Select Parent";
    ot.description = "Select the group holding the active layer (=), alone, as clicking it in the Layers editor does";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) {
      return C.document && document_active_layer(*C.document) &&
             layer_parent(*C.document, C.document->active_layer) != nullptr;
    };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const Layer *parent = layer_parent(doc, doc.active_layer);
      if (!parent || !layer_pick_apply(doc, parent->id, LayerPickMode::Set)) {
        return OpStatus::Cancelled;
      }
      C.undo_name = "Select " + parent->name;
      if (C.wm) {
        /* The Layers editor scrolls to it. */
        C.wm->layers_show_active = parent->id;
        C.wm->layers_show_active_serial++;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  /* With X mirror editing on, inside a sided group (Arm_L) its partner (Arm_R) gets one too. */
  layer_operator("layer.new", "New Layer", "Add a vector layer above the active layer", nullptr, [](Context &C) {
    document_add_layer_mirrored(*C.document, LayerType::Vector, C.wm && C.wm->tool_settings.mirror_x);
    return OpStatus::Finished;
  });

  layer_operator("layer.new_group", "New Group", "Add a group layer above the active layer", nullptr,
                 [](Context &C) {
                   document_add_layer_mirrored(*C.document, LayerType::Group, C.wm && C.wm->tool_settings.mirror_x);
                   return OpStatus::Finished;
                 });

  layer_operator("layer.new_switch", "New Switch",
                 "Add a switch layer above the active layer: a group that shows one of its children, or none",
                 nullptr, [](Context &C) {
                   document_add_layer_mirrored(*C.document, LayerType::Switch, C.wm && C.wm->tool_settings.mirror_x);
                   return OpStatus::Finished;
                 });

  layer_operator("layer.switch_set", "Set Switch",
                 "Show one child of a switch (layer=, else the active layer or the switch holding it): child= its "
                 "name, empty for none, RIG to hand it back to the rig. On frame 0 it's the default; on other "
                 "frames it's keyed, and a key made by hand wins over smart actions until a Rig key. hold=1 "
                 "(Alt) picks it for the whole hold the frame is in instead; default=1 makes it the default (frame 0's)",
                 [](Context &C) { return C.document != nullptr; }, [](Context &C) {
                   Document &doc = *C.document;
                   const Layer *named = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : nullptr;
                   Layer *sw = op_has_arg(C, "layer") ? (named ? switch_of(doc, named->id) : nullptr)
                                                      : switch_of(doc, doc.active_layer);
                   if (!sw) {
                     report(ReportType::Warning, "Not a switch layer, nor inside one");
                     return OpStatus::Cancelled;
                   }
                   float value = 0.0f;
                   if (!switch_value_from_name(*sw, op_arg(C, "child"), &value)) {
                     report(ReportType::Warning, "\"" + sw->name + "\" has no layer named \"" + op_arg(C, "child") + "\"");
                     return OpStatus::Cancelled;
                   }
                   animation_record(doc);
                   if (op_arg_int(C, "default", 0) != 0) {
                     /* Set as Default: frame 0's pick, whatever frame this is. */
                     const int now = doc.frame_current;
                     doc.frame_current = 0;
                     switch_set(doc, *sw, value);
                     doc.frame_current = now;
                     Channel<float> &ch = sw->switch_keys;
                     sw->switch_value = ch.animated() ? ch.evaluate(now) : value;
                     ch.last = sw->switch_value;
                     switches_resolve(doc, now);
                     C.undo_name = sw->name + ": Default " + switch_value_label(*sw, value);
                     return OpStatus::Finished;
                   }
                   switch_pick(doc, *sw, value, op_arg_int(C, "hold", 0) != 0);
                   C.undo_name = sw->name + ": " + switch_value_label(*sw, value);
                   return OpStatus::Finished;
                 });

  for (const auto &[idname, name, step, tip] :
       {std::tuple{"layer.switch_next", "Next in Switch", 1,
                   "Show the next child of the switch holding the active layer (the one below it in the Layers "
                   "editor), keyed at this frame"},
        std::tuple{"layer.switch_previous", "Previous in Switch", -1,
                   "Show the previous child of the switch holding the active layer (the one above it in the "
                   "Layers editor), keyed at this frame"}})
  {
    const int dir = step;
    layer_operator(
        idname, name, tip, [](Context &C) { return C.document && switch_of(*C.document, C.document->active_layer); },
        [dir](Context &C) {
          Document &doc = *C.document;
          Layer *sw = switch_of(doc, doc.active_layer);
          if (!sw || sw->children.empty()) {
            return OpStatus::Cancelled;
          }
          /* From what the viewport shows: the child being edited, else the pick. */
          const Layer *editing = switch_child_holding(doc, *sw, doc.active_layer);
          const uint64_t from = editing ? editing->id : sw->switch_shown;
          const int n = int(sw->children.size());
          int at = -1;
          for (int i = 0; i < n; i++) {
            if (sw->children[size_t(i)].id == from) {
              at = i;
            }
          }
          const int next = at < 0 ? (dir > 0 ? 0 : n - 1) : ((at + dir) % n + n) % n;
          const float value = float(sw->children[size_t(next)].id);
          animation_record(doc);
          switch_pick(doc, *sw, value, false);
          C.undo_name = sw->name + ": " + switch_value_label(*sw, value);
          return OpStatus::Finished;
        });
  }

  {
    /* Duplicate and Duplicate with Keys, for a switch's children (the asset shelf's right-click menu). */
    OperatorType ot;
    ot.idname = "layer.switch_duplicate";
    ot.name = "Duplicate";
    ot.description = "Copy a child of a switch (child=, else the one holding the active layer) to change into another "
                     "drawing: placed after it, picked and keyed at this frame (on frame 0 only added). Without "
                     "keys= its timeline keys are left out; its smart actions' keys, its bones' swaps and its "
                     "drivers come along";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document && switch_of(*C.document, C.document->active_layer); };
    ot.props = {
        op_bool_property("keys", "Timeline Keys", "Keep the timeline keys too (Duplicate with Keys)", "0"),
        [] {
          OpProperty p;
          p.name = "start_from";
          p.label = "Start From";
          p.description = "Without timeline keys: the copy's setup pose is the original's (Setup), or how it looks on this "
                          "frame (This Frame: its drawings and bones take this pose as their setup)";
          p.items = {{"SETUP", "Setup"}, {"THIS_FRAME", "This Frame"}};
          p.fallback = "SETUP";
          p.visible = [](const OpArgs &args) {
            const auto it = args.find("keys");
            return it == args.end() || (it->second != "1" && it->second != "True" && it->second != "true");
          };
          return p;
        }(),
        op_bool_property("smart_keys", "Smart Action Keys", "Keep the keys its smart actions hold", "1"),
    };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      Layer *sw = nullptr;
      uint64_t child_id = 0;
      if (op_has_arg(C, "child")) {
        const Layer *named = layer_by_name_or_id(doc, op_arg(C, "child"));
        sw = named ? layer_parent(doc, named->id) : nullptr;
        child_id = named ? named->id : 0;
      }
      else if ((sw = switch_of(doc, doc.active_layer))) {
        const Layer *holding = switch_child_holding(doc, *sw, doc.active_layer);
        child_id = holding ? holding->id : 0;
      }
      if (!sw || !sw->is_switch() || !child_id) {
        report(ReportType::Warning, "Name a child of a switch (child=)");
        return OpStatus::Cancelled;
      }
      const auto truthy = [&](const char *name, const char *fallback) {
        const std::string v = op_arg(C, name, fallback);
        return v == "1" || v == "True" || v == "true";
      };
      const bool keys = truthy("keys", "0");
      const bool from_now = op_arg(C, "start_from", "SETUP") == "THIS_FRAME" && !keys && doc.frame_current != 0;
      const bool smart_keys = truthy("smart_keys", "1");
      animation_record(doc);
      const uint64_t sw_id = sw->id;
      int index = 0;
      std::vector<Layer> *container = layer_container(doc, child_id, &index);
      std::map<uint64_t, uint64_t> remap;
      Layer copy = layer_copy_with_new_ids(doc, (*container)[size_t(index)], &remap);
      copy.name = document_layer_unique_name(doc, (*container)[size_t(index)].name);
      copy.selected = false;
      /* Its bones stand in for the original's (or for what those stand in for). */
      const auto swaps = [&](auto &&self, Layer &l) -> void {
        if (l.is_bone_layer()) {
          for (auto &[old_id, new_id] : remap) {
            if (new_id != l.id) {
              continue;
            }
            for (Bone &b : l.bones) {
              if (b.swap_layer == 0) {
                b.swap_layer = old_id;
                b.swap_bone = b.id;
              }
              else if (const auto it = remap.find(b.swap_layer); it != remap.end()) {
                b.swap_layer = it->second;
              }
            }
          }
        }
        layer_masks_remap(l, remap);
        for (Layer &c : l.children) {
          self(self, c);
        }
      };
      swaps(swaps, copy);
      const uint64_t copy_id = copy.id;
      container->insert(container->begin() + index + 1, std::move(copy));
      drivers_copy_for_layers(doc, remap);
      Layer &placed = *layer_find(doc, copy_id);
      if (!keys) {
        if (from_now) {
          /* Its own bones first take this pose as their rest, bending its drawings to it. */
          std::vector<uint64_t> armatures;
          const auto gather = [&](auto &&self, const Layer &l) -> void {
            if (l.is_bone_layer()) {
              armatures.push_back(l.id);
            }
            for (const Layer &c : l.children) {
              self(self, c);
            }
          };
          gather(gather, placed);
          const uint64_t was_active = doc.active_layer;
          for (const uint64_t id : armatures) {
            doc.active_layer = id;
            operator_call(C, "armature.apply_pose_as_rest", {{"keep_animation", "0"}});
          }
          doc.active_layer = was_active;
        }
        layer_strip_keys(*layer_find(doc, copy_id), from_now, smart_keys);
      }
      Layer &now_sw = *layer_find(doc, sw_id);
      const int frame = doc.frame_current;
      if (frame != 0) {
        switch_set(doc, now_sw, float(copy_id));
      }
      doc.active_layer = copy_id;
      switches_resolve(doc, frame);
      if (C.wm) {
        C.wm->shelf_rename = copy_id;
      }
      C.undo_name = std::string(keys ? "Duplicate with Keys: " : "Duplicate: ") + layer_find(doc, copy_id)->name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  layer_operator("layer.switch_add_child", "Add to Switch",
                 "Add an empty drawing to a switch (layer=, else the one holding the active layer), last, to draw "
                 "in: picked and keyed at this frame (on frame 0 only added)",
                 [](Context &C) { return C.document != nullptr; }, [](Context &C) {
                   Document &doc = *C.document;
                   const Layer *named = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : nullptr;
                   Layer *sw = switch_of(doc, named ? named->id : doc.active_layer);
                   if (!sw) {
                     report(ReportType::Warning, "Not a switch layer, nor inside one");
                     return OpStatus::Cancelled;
                   }
                   animation_record(doc);
                   const uint64_t sw_id = sw->id;
                   doc.active_layer = sw_id;
                   Layer &added = document_add_layer(doc, LayerType::Vector, "");
                   const uint64_t id = added.id;
                   /* Added above the switch: into it, last. */
                   document_reparent_layer(doc, id, sw_id, int(layer_find(doc, sw_id)->children.size()));
                   if (doc.frame_current != 0) {
                     switch_set(doc, *layer_find(doc, sw_id), float(id));
                   }
                   doc.active_layer = id;
                   switches_resolve(doc, doc.frame_current);
                   C.undo_name = "Add to " + layer_find(doc, sw_id)->name;
                   return OpStatus::Finished;
                 });

  layer_operator("layer.switch_link", "Link Switches",
                 "Link the picked switches (or layers=, names or ids separated by commas) by name: picking a drawing in "
                 "one picks the same-named drawing in all. The active one's keys are kept; the others' go",
                 [](Context &C) { return C.document != nullptr; }, [](Context &C) {
                   Document &doc = *C.document;
                   std::vector<Layer *> members;
                   if (op_has_arg(C, "layers")) {
                     std::string list = op_arg(C, "layers");
                     size_t at = 0;
                     while (at <= list.size()) {
                       const size_t comma = std::min(list.find(',', at), list.size());
                       std::string part = list.substr(at, comma - at);
                       part.erase(0, part.find_first_not_of(' '));
                       part.erase(part.find_last_not_of(' ') + 1);
                       if (Layer *l = part.empty() ? nullptr : layer_by_name_or_id(doc, part); l && l->is_switch()) {
                         members.push_back(l);
                       }
                       at = comma + 1;
                     }
                   }
                   else {
                     for (const Layer *l : document_layers_flat(doc)) {
                       if (l->is_switch() && (l->selected || l->id == doc.active_layer)) {
                         members.push_back(layer_find(doc, l->id));
                       }
                     }
                   }
                   std::erase_if(members, [](const Layer *l) { return l->switch_link != 0; });
                   if (members.size() < 2) {
                     report(ReportType::Warning, "Pick two or more switches that aren't linked yet");
                     return OpStatus::Cancelled;
                   }
                   animation_record(doc);
                   Layer *keep = nullptr;
                   for (Layer *m : members) {
                     keep = m->id == doc.active_layer ? m : keep;
                   }
                   int dropped = 0;
                   switch_link_create(doc, members, keep, &dropped);
                   switches_resolve(doc, doc.frame_current);
                   report(ReportType::Info, "Linked " + std::to_string(members.size()) + " switches by name" +
                                                (dropped > 0 ? " (" + std::to_string(dropped) + " keys of the others went)" : ""));
                   C.undo_name = "Link Switches";
                   return OpStatus::Finished;
                 });

  layer_operator("layer.switch_unlink", "Unlink Switch",
                 "Take a switch (layer=, else the one holding the active layer) out of its link by name: its own keys "
                 "then show what it showed, and same-named switches don't link it again",
                 [](Context &C) { return C.document != nullptr; }, [](Context &C) {
                   Document &doc = *C.document;
                   const Layer *named = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : nullptr;
                   Layer *sw = switch_of(doc, named ? named->id : doc.active_layer);
                   while (sw && !sw->switch_link) {
                     Layer *p = layer_parent(doc, sw->id);
                     sw = p ? switch_of(doc, p->id) : nullptr;
                   }
                   animation_record(doc);
                   if (!sw || !switch_unlink(doc, *sw)) {
                     report(ReportType::Warning, "Not a linked switch, nor inside one");
                     return OpStatus::Cancelled;
                   }
                   C.undo_name = "Unlink " + sw->name;
                   return OpStatus::Finished;
                 });

  layer_operator("layer.switch_aliases_set", "Set Aliases",
                 "The other names a drawing (layer=, else the active layer) answers to in a linked switch, separated "
                 "by commas (aliases=): the profile \"Oh\" also answering to O",
                 [](Context &C) { return C.document != nullptr; }, [](Context &C) {
                   Document &doc = *C.document;
                   Layer *l = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : document_active_layer(doc);
                   if (!l) {
                     return OpStatus::Cancelled;
                   }
                   l->switch_aliases.clear();
                   const std::string list = op_arg(C, "aliases");
                   size_t at = 0;
                   while (at <= list.size()) {
                     const size_t comma = std::min(list.find(',', at), list.size());
                     std::string part = list.substr(at, comma - at);
                     part.erase(0, part.find_first_not_of(' '));
                     part.erase(part.find_last_not_of(' ') + 1);
                     if (!part.empty() && part != l->name) {
                       l->switch_aliases.push_back(part);
                     }
                     at = comma + 1;
                   }
                   switches_resolve(doc, doc.frame_current);
                   C.undo_name = l->name + ": Aliases";
                   return OpStatus::Finished;
                 });

  layer_operator("layer.rename_here", "Rename Here Only",
                 "Rename a drawing of a linked switch (layer=, name=) without renaming its partners in the linked "
                 "switches: it no longer answers to its old name there",
                 [](Context &C) { return C.document != nullptr; }, [](Context &C) {
                   Document &doc = *C.document;
                   Layer *l = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : document_active_layer(doc);
                   const std::string name = op_arg(C, "name");
                   if (!l || name.empty()) {
                     return OpStatus::Cancelled;
                   }
                   animation_record(doc);
                   l->name = name;
                   switch_rename_here_next();
                   switch_links_update(doc);
                   C.undo_name = "Rename Layer";
                   return OpStatus::Finished;
                 });

  layer_operator("layer.switch_replace_keys", "Replace Keys With",
                 "Point every key of a switch (layer=, else the one holding the active layer) that shows child "
                 "from= (a name, or the id a missing child had) at child= instead (a name, empty for none)",
                 [](Context &C) { return C.document != nullptr; }, [](Context &C) {
                   Document &doc = *C.document;
                   const Layer *named = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : nullptr;
                   Layer *sw = switch_of(doc, named ? named->id : doc.active_layer);
                   if (!sw) {
                     report(ReportType::Warning, "Not a switch layer, nor inside one");
                     return OpStatus::Cancelled;
                   }
                   float from = 0.0f, to = 0.0f;
                   const std::string from_text = op_arg(C, "from");
                   bool by_entry = false;
                   if (const SwitchLink *link = switch_link_of(doc, *sw); link && !switch_value_from_name(*sw, from_text, &from)) {
                     /* Linked: a name of its group it has no drawing for (keys showing "Special"). */
                     for (const SwitchLinkName &n : link->names) {
                       if (n.name == from_text) {
                         from = float(n.id);
                         by_entry = true;
                       }
                     }
                   }
                   if (!by_entry && !switch_value_from_name(*sw, from_text, &from)) {
                     char *stop = nullptr;
                     const unsigned long long id = std::strtoull(from_text.c_str(), &stop, 10);
                     if (from_text.empty() || *stop != '\0') {
                       report(ReportType::Warning, "\"" + sw->name + "\" has no layer named \"" + from_text + "\"");
                       return OpStatus::Cancelled;
                     }
                     from = float(id);
                   }
                   if (!switch_value_from_name(*sw, op_arg(C, "child"), &to)) {
                     report(ReportType::Warning, "\"" + sw->name + "\" has no layer named \"" + op_arg(C, "child") + "\"");
                     return OpStatus::Cancelled;
                   }
                   animation_record(doc);
                   const std::string from_label = by_entry ? from_text : switch_value_label(*sw, from);
                   const int count = switch_replace_keys(doc, *sw, from, to, by_entry);
                   if (count == 0) {
                     report(ReportType::Info, "No keys of \"" + sw->name + "\" show " + from_label);
                     return OpStatus::Cancelled;
                   }
                   report(ReportType::Info, std::to_string(count) + (count == 1 ? " key" : " keys") + " of \"" +
                                                sw->name + (count == 1 ? "\" now shows " : "\" now show ") +
                                                switch_value_label(*sw, to) +
                                                " instead of " + from_label);
                   C.undo_name = sw->name + ": Replace Keys";
                   return OpStatus::Finished;
                 });

  layer_operator("layer.move_to_group", "Move to Group",
                 "Move the active layer into a group (group=<layer id>), at the top; group=0 moves it to the top level",
                 active_layer_poll, [](Context &C) {
                   Document &doc = *C.document;
                   const uint64_t group = uint64_t(std::max(0, op_arg_int(C, "group", 0)));
                   const Layer *target = group ? layer_find(doc, group) : nullptr;
                   if (group && (!target || !target->is_group())) {
                     report(ReportType::Error, "Not a group");
                     return OpStatus::Cancelled;
                   }
                   return document_reparent_layer(doc, doc.active_layer, group, 0) ? OpStatus::Finished
                                                                                   : OpStatus::Cancelled;
                 });

  layer_operator("layer.delete", "Delete Layer",
                 "Delete the active layer and every layer picked with it (Ctrl or Shift click), and everything in them",
                 active_layer_poll, [](Context &C) {
                   Document &doc = *C.document;
                   /* The active layer and the picked ones; a layer inside one that goes, goes with it. */
                   std::vector<uint64_t> ids{doc.active_layer};
                   for (const Layer *layer : document_layers_flat(doc)) {
                     if (layer->selected && layer->id != doc.active_layer) {
                       ids.push_back(layer->id);
                     }
                   }
                   const auto inside_another = [&](uint64_t id) {
                     for (const Layer *parent = layer_parent(doc, id); parent; parent = layer_parent(doc, parent->id)) {
                       if (std::find(ids.begin(), ids.end(), parent->id) != ids.end()) {
                         return true;
                       }
                     }
                     return false;
                   };
                   std::vector<uint64_t> doomed;
                   for (const uint64_t id : ids) {
                     if (!inside_another(id)) {
                       doomed.push_back(id);
                     }
                   }
                   /* The active one last, so the layer that takes its place is one that stays. */
                   std::stable_partition(doomed.begin(), doomed.end(), [&](uint64_t id) { return id != doc.active_layer; });
                   for (const uint64_t id : doomed) {
                     document_remove_layer(doc, id);
                   }
                   layers_deselect(doc);
                   return OpStatus::Finished;
                 });

  layer_operator("layer.duplicate", "Duplicate Layer",
                 "Copy the active layer (and the picked ones), each copy above its original. Copied together, "
                 "a layer and its mask layer stay a pair: the copy is masked by the copy",
                 active_layer_poll, [](Context &C) {
                   Document &doc = *C.document;
                   /* The picked layers, leaving out any inside a picked group (they go with it). */
                   std::vector<uint64_t> picked;
                   for (const Layer *l : document_layers_flat(doc)) {
                     if (l->id != doc.active_layer && !l->selected) {
                       continue;
                     }
                     bool inside = false;
                     for (const uint64_t p : picked) {
                       inside |= p != l->id && layer_inside(doc, l->id, p);
                     }
                     if (!inside) {
                       picked.push_back(l->id);
                     }
                   }
                   /* Copied together, under a group held only meanwhile: links among them (references, masks, Draw
                    * Order, bone shapes, modifiers' layers) name the copies, across picked layers too. */
                   Layer bundle;
                   bundle.type = LayerType::Group;
                   std::erase_if(picked, [&](uint64_t id) { return !layer_find(doc, id); });
                   for (const uint64_t id : picked) {
                     bundle.children.push_back(*layer_find(doc, id));
                   }
                   std::map<uint64_t, uint64_t> remap;
                   Layer copied = layer_copy_with_new_ids(doc, bundle, &remap);
                   std::vector<uint64_t> copies;
                   uint64_t active_copy = 0;
                   for (size_t k = 0; k < picked.size(); k++) {
                     const uint64_t id = picked[k];
                     int index = 0;
                     std::vector<Layer> *container = layer_container(doc, id, &index);
                     if (!container) {
                       continue;
                     }
                     Layer copy = std::move(copied.children[k]);
                     copy.name = document_layer_unique_name(doc, (*container)[index].name);
                     copy.selected = false;
                     if (id == doc.active_layer) {
                       active_copy = copy.id;
                     }
                     copies.push_back(copy.id);
                     container->insert(container->begin() + index, std::move(copy));
                   }
                   /* So do their drivers (as Blender copies an object's with it). */
                   drivers_copy_for_layers(doc, remap);
                   for (const Layer *l : document_layers_flat(doc)) {
                     layer_find(doc, l->id)->selected = false;
                   }
                   for (const uint64_t id : copies) {
                     if (id != active_copy) {
                       layer_find(doc, id)->selected = true;
                     }
                   }
                   doc.active_layer = active_copy ? active_copy : (copies.empty() ? doc.active_layer : copies.front());
                   return OpStatus::Finished;
                 });

  {
    /* The Layers editor's right-click Symmetrize + to - and - to +. */
    OperatorType ot;
    ot.idname = "layer.symmetrize";
    ot.name = "Symmetrize Layers";
    ot.description =
        "Make the other side of the active and picked layers (and of the layers in picked groups), mirrored "
        "across the document's vertical axis (a bone layer's, inside one). A layer named for a side (_L, .L, L_, "
        "Left...) gets its partner made, or remade, from whichever of the two sits on the side direction= copies "
        "from (POSITIVE_TO_NEGATIVE: + to -, or NEGATIVE_TO_POSITIVE); a layer named for neither is symmetrized "
        "in place, as point Symmetrize does. On the setup frame; the copies rest in the setup pose (no point or "
        "transform keys)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = active_layer_poll;
    OpProperty direction;
    direction.name = "direction";
    direction.label = "Direction";
    direction.description = "Which side is copied onto the other";
    direction.items = {{"POSITIVE_TO_NEGATIVE", "+ to -"}, {"NEGATIVE_TO_POSITIVE", "- to +"}};
    direction.fallback = "POSITIVE_TO_NEGATIVE";
    ot.props = {direction};
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::string direction = op_arg(C, "direction", "POSITIVE_TO_NEGATIVE");
      if (direction != "POSITIVE_TO_NEGATIVE" && direction != "NEGATIVE_TO_POSITIVE") {
        report(ReportType::Error, "direction is POSITIVE_TO_NEGATIVE or NEGATIVE_TO_POSITIVE");
        return OpStatus::Cancelled;
      }
      if (doc.frame_current != 0) {
        report_banner(ReportType::Warning, "Switch to Setup (frame 0) to symmetrize layers");
        return OpStatus::Cancelled;
      }
      const int from = direction == "POSITIVE_TO_NEGATIVE" ? 1 : -1;
      /* The picked layers, leaving out any inside a picked group (they go with it). */
      std::vector<uint64_t> picked;
      for (const Layer *l : document_layers_flat(doc)) {
        if (l->id != doc.active_layer && !l->selected) {
          continue;
        }
        bool inside = false;
        for (const uint64_t p : picked) {
          inside |= p != l->id && layer_inside(doc, l->id, p);
        }
        if (!inside) {
          picked.push_back(l->id);
        }
      }
      /* Sided layers (a group named for a side goes as a whole), and the drawings in the middle. */
      std::vector<uint64_t> sided, centre;
      const auto gather = [&](auto &&self, const Layer &l) -> void {
        if (!l.is_bone_layer() && bone_mirror_name(l.name) != l.name) {
          sided.push_back(l.id);
        }
        else if (l.is_group()) {
          for (const Layer &child : l.children) {
            self(self, child);
          }
        }
        else if (l.has_drawing() && !l.is_reference() && !l.drawing.points.empty()) {
          centre.push_back(l.id);
        }
      };
      for (const uint64_t id : picked) {
        gather(gather, *layer_find(doc, id));
      }
      SymmetrizeLog log;
      std::set<uint64_t> done;
      for (const uint64_t id : sided) {
        if (layer_find(doc, id)) {
          symmetrize_pair(doc, id, from, done, log);
        }
      }
      symmetrize_flip_links(doc, log);
      for (const uint64_t id : centre) {
        Layer *layer = layer_find(doc, id);
        if (!layer) {
          continue;
        }
        const std::map<uint64_t, uint64_t> bone_flip = symmetrize_bone_flip(doc, id);
        std::vector<bool> was;
        for (MeshPoint &p : layer->drawing.points) {
          was.push_back(p.selected);
          p.selected = true;
        }
        const SymmetrizeResult r = drawing_symmetrize(layer->drawing, from > 0, 0.01f, true, &bone_flip);
        for (size_t i = 0; i < was.size(); i++) {
          layer->drawing.points[i].selected = was[i];
        }
        if (r.done) {
          log.centred.push_back(layer->name);
        }
        else {
          log.skipped.push_back(layer->name + " (" + std::to_string(r.right) + " points on the right, " +
                                std::to_string(r.left) + " on the left)");
        }
      }
      std::string message;
      const auto add = [&](const char *what, const std::vector<std::string> &names) {
        for (size_t i = 0; i < names.size(); i++) {
          message += (i == 0 ? std::string(message.empty() ? "" : "; ") + what + " " : std::string(", ")) + names[i];
        }
      };
      add("made", log.made);
      add("remade", log.replaced);
      add("symmetrized", log.centred);
      add("skipped", log.skipped);
      const bool changed = !log.made.empty() || !log.replaced.empty() || !log.centred.empty();
      if (message.empty()) {
        message = "nothing to do: name layers with _L or _R (or .L, L_, Left...) to make their other side";
      }
      report(changed ? ReportType::Info : ReportType::Warning, "Symmetrize: " + message);
      C.undo_name = std::string("Symmetrize ") + (from > 0 ? "+ to -" : "- to +");
      return changed ? OpStatus::Finished : OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }

  layer_operator("layer.make_reference", "Duplicate as Reference",
                 "Add a live copy of the active layer that follows its drawing but keeps its own settings",
                 active_layer_poll, [](Context &C) {
                   Document &doc = *C.document;
                   int index = 0;
                   std::vector<Layer> *container = layer_container(doc, doc.active_layer, &index);
                   Layer ref = layer_make_reference(doc, (*container)[index]);
                   const uint64_t id = ref.id;
                   container->insert(container->begin() + index, std::move(ref));
                   doc.active_layer = id;
                   references_build(doc);
                   return OpStatus::Finished;
                 });

  layer_operator(
      "layer.break_reference", "Break Reference", "Turn the active reference layer into an ordinary layer",
      [](Context &C) {
        const Layer *layer = C.document ? document_active_layer(*C.document) : nullptr;
        return layer && layer->is_reference();
      },
      [](Context &C) {
        references_capture(*C.document);
        /* A referenced group's references inside it break too. */
        const auto brk = [&](auto &&self, Layer &layer) -> void {
          for (Layer &child : layer.children) {
            self(self, child);
          }
          reference_break(*C.document, layer);
        };
        brk(brk, *document_active_layer(*C.document));
        return OpStatus::Finished;
      });

  /* --- A reference's own changes ---------------------------------------------------------- */
  const auto active_reference = [](Context &C) -> Layer * {
    Layer *layer = C.document ? document_active_layer(*C.document) : nullptr;
    return layer && layer->is_reference() ? layer : nullptr;
  };
  const auto reference_poll = [active_reference](Context &C) { return active_reference(C) != nullptr; };
  layer_operator(
      "layer.reference_set", "Reference Settings",
      "Set the active reference's Edit Points, Follow Original's Animation and Match Original (1 or 0; left out: "
      "unchanged)",
      reference_poll, [active_reference](Context &C) {
        references_capture(*C.document);
        Layer &ref = *active_reference(C);
        if (const std::string v = op_arg(C, "edit_points"); !v.empty()) {
          ref.reference.edit_points = op_arg_bool(C, "edit_points");
        }
        if (const std::string v = op_arg(C, "follow_animation"); !v.empty()) {
          ref.reference.follow_animation = op_arg_bool(C, "follow_animation");
        }
        if (const std::string v = op_arg(C, "match_original"); !v.empty()) {
          reference_set_match(*C.document, ref, op_arg_bool(C, "match_original"));
        }
        references_build(*C.document);
        return OpStatus::Finished;
      });
  /* kind: ALL, POINTS, LOOKS, SWAPS, WEIGHTS, or one: POINT or LOOK (id: the point's or shape's id)
   * or SWAP (id: its index). */
  const auto kind_of = [](Context &C, ReferenceKind *r_kind, uint64_t *r_which) {
    if (!reference_kind_from_identifier(op_arg(C, "kind", "ALL"), r_kind)) {
      report(ReportType::Error, "Unknown kind '" + op_arg(C, "kind") + "'");
      return false;
    }
    *r_which = std::strtoull(op_arg(C, "id", "0").c_str(), nullptr, 10);
    return true;
  };
  layer_operator("layer.reference_revert", "Reset to Original",
                 "Drop the active reference's changes (kind, id; setting or path for one), so those follow its original again", reference_poll,
                 [active_reference, kind_of](Context &C) {
                   references_capture(*C.document);
                   ReferenceKind kind;
                   uint64_t which;
                   if (!kind_of(C, &kind, &which)) {
                     return OpStatus::Cancelled;
                   }
                   const int n = reference_revert(*C.document, *active_reference(C), kind, which,
                                                  op_arg(C, "setting", op_arg(C, "path")));
                   references_build(*C.document);
                   return n > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                 });
  layer_operator("layer.reference_apply", "Push to Original",
                 "Write the active reference's changes (kind, id) into its original, so the original and every "
                 "reference get them",
                 reference_poll, [active_reference, kind_of](Context &C) {
        references_capture(*C.document);
                   ReferenceKind kind;
                   uint64_t which;
                   if (!kind_of(C, &kind, &which)) {
                     return OpStatus::Cancelled;
                   }
                   const int n = reference_apply(*C.document, *active_reference(C), kind, which,
                                                 op_arg(C, "setting", op_arg(C, "path")));
                   references_build(*C.document);
                   return n > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                 });
  layer_operator(
      "layer.reference_sync", "Reset Channel to Original",
      "A layer channel the reference took over follows its original again (path: position, rotation, scale or "
      "opacity; left out: every one, Sync Layer to Original). layer: the reference (default: the active layer)",
      [](Context &C) { return C.document != nullptr; },
      [](Context &C) {
        references_capture(*C.document);
        const std::string id = op_arg(C, "layer");
        Layer *ref = id.empty() ? document_active_layer(*C.document)
                                : layer_find(*C.document, std::strtoull(id.c_str(), nullptr, 10));
        if (!ref || !ref->is_reference() || ref->reference.own_channels.empty()) {
          return OpStatus::Cancelled;
        }
        const std::string path = op_arg(C, "path");
        if (!path.empty() && !ref->reference.own_channels.count(path)) {
          return OpStatus::Cancelled;
        }
        for (const std::string &name : std::set<std::string>(ref->reference.own_channels)) {
          if (path.empty() || name == path) {
            reference_sync_channel(*ref, name);
          }
        }
        references_build(*C.document);
        return OpStatus::Finished;
      });
  layer_operator("layer.reference_remove_unused", "Remove Unused Overrides",
                 "Drop the active reference's changes to points and shapes its original no longer has",
                 reference_poll, [active_reference](Context &C) {
        references_capture(*C.document);
                   const int n = reference_remove_unused(*C.document, *active_reference(C));
                   report(ReportType::Info, std::to_string(n) + (n == 1 ? " unused override removed" : " unused overrides removed"));
                   return n > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                 });
  layer_operator("layer.reference_hide_shape", "Hide Shape on Reference",
                 "Hide one of the original's shapes on the active reference (shape: its id; hide: 1 or 0)",
                 reference_poll, [active_reference](Context &C) {
        references_capture(*C.document);
                   const uint64_t shape = std::strtoull(op_arg(C, "shape", "0").c_str(), nullptr, 10);
                   reference_hide_shape(*active_reference(C), shape, op_arg(C, "hide", "1") != "0");
                   references_build(*C.document);
                   return OpStatus::Finished;
                 });
  layer_operator("layer.reference_swap_color", "Swap Colour on Reference",
                 "Show every shape of the original using a colour (from: \"r,g,b,a\") with another (to) on the active "
                 "reference, as fill and/or line (fill, line: 1 or 0)",
                 reference_poll, [active_reference](Context &C) {
        references_capture(*C.document);
                   const auto color = [&](const char *name, Color *r_color) {
                     float c[4] = {0, 0, 0, 1};
                     const std::string text = op_arg(C, name);
                     const char *at = text.c_str();
                     int n = 0;
                     for (; n < 4 && *at; n++) {
                       char *end = nullptr;
                       c[n] = std::strtof(at, &end);
                       if (end == at) {
                         break;
                       }
                       at = *end == ',' ? end + 1 : end;
                     }
                     *r_color = {c[0], c[1], c[2], c[3]};
                     return n >= 3;
                   };
                   Color from, to;
                   if (!color("from", &from) || !color("to", &to)) {
                     report(ReportType::Error, "Colours are given as \"r,g,b\" or \"r,g,b,a\" (0 to 1)");
                     return OpStatus::Cancelled;
                   }
                   reference_swap_color(*active_reference(C), from, to, op_arg(C, "fill", "1") != "0",
                                        op_arg(C, "line", "1") != "0");
                   references_build(*C.document);
                   return OpStatus::Finished;
                 });

  /* Ctrl L > Copy Masks: the active layer's mask cards (and Clip to Below) replace the other picked
   * layers', as Copy Constraints replaces bones'. A copied step naming the layer it lands on leaves that
   * layer out (nothing masks itself); a step left naming none goes. */
  layer_operator(
      "layer.masks_copy", "Copy Masks",
      "Give the other picked layers the active layer's masks (and Clip to Below), replacing theirs (Ctrl L)",
      [](Context &C) {
        if (!C.document || !document_active_layer(*C.document)) {
          return false;
        }
        for (const Layer *l : document_layers_flat(*C.document)) {
          if (l->selected && l->id != C.document->active_layer) {
            return true;
          }
        }
        return false;
      },
      [](Context &C) {
        Document &doc = *C.document;
        const Layer source = *document_active_layer(doc);
        std::vector<uint64_t> targets;
        for (const Layer *l : document_layers_flat(doc)) {
          if (l->selected && l->id != source.id) {
            targets.push_back(l->id);
          }
        }
        for (const uint64_t id : targets) {
          Layer *target = layer_find(doc, id);
          std::vector<LayerMask> masks = source.masks;
          for (LayerMask &card : masks) {
            for (MaskStep &step : card.steps) {
              std::erase(step.layers, id);
              for (MaskStep &child : step.children) {
                std::erase(child.layers, id);
              }
              std::erase_if(step.children, mask_step_empty);
            }
            std::erase_if(card.steps, mask_step_empty);
          }
          target->masks = std::move(masks);
          target->clip_to_below = source.clip_to_below;
        }
        report(ReportType::Info, "Copied " + std::to_string(source.masks.size()) +
                                     (source.masks.size() == 1 ? " mask to " : " masks to ") +
                                     std::to_string(targets.size()) + (targets.size() == 1 ? " layer" : " layers"));
        return OpStatus::Finished;
      });

  layer_operator("layer.move_up", "Raise Layer", "Move the active layer up the stack", active_layer_poll,
                 [](Context &C) {
                   return document_move_layer(*C.document, C.document->active_layer, -1) ? OpStatus::Finished
                                                                                         : OpStatus::Cancelled;
                 });

  layer_operator("layer.move_down", "Lower Layer", "Move the active layer down the stack", active_layer_poll,
                 [](Context &C) {
                   return document_move_layer(*C.document, C.document->active_layer, 1) ? OpStatus::Finished
                                                                                        : OpStatus::Cancelled;
                 });

  for (const auto &[idname, name, type, tip] :
       {std::tuple{"layer.group", "Group With Selection", LayerType::Group,
                   "Put the active and selected layers into a new group where the active one was, as Moho does"},
        std::tuple{"layer.wrap_in_switch", "Wrap in Switch", LayerType::Switch,
                   "Put the active and selected layers into a new switch where the active one was: it shows one "
                   "of them at a time, the top one to start with"}})
  {
    const LayerType kind = type;
    layer_operator(idname, name, tip, active_layer_poll, [kind](Context &C) {
      Document &doc = *C.document;
      /* The layers to move, top first; a layer inside another picked group goes with it. */
      std::vector<uint64_t> ids;
      for (const Layer *layer : document_layers_flat(doc)) {
        if (layer->id != doc.active_layer && !layer->selected) {
          continue;
        }
        bool inside_picked = false;
        for (const Layer *p = layer_parent(doc, layer->id); p; p = layer_parent(doc, p->id)) {
          inside_picked = inside_picked || p->id == doc.active_layer || p->selected;
        }
        if (!inside_picked) {
          ids.push_back(layer->id);
        }
      }
      Layer &group = document_add_layer(doc, kind, "");
      const uint64_t group_id = group.id;
      for (const uint64_t id : ids) {
        document_reparent_layer(doc, id, group_id, int(layer_find(doc, group_id)->children.size()));
      }
      layers_deselect(doc);
      doc.active_layer = group_id;
      return OpStatus::Finished;
    });
  }

  /* The active and picked groups become switches where they are, their children its drawings, the top one the
   * default (as a switch reset to its own picks starts). Name, place, masks and everything else stay. */
  const auto groups_to_convert = [](Document &doc) {
    std::vector<Layer *> groups;
    const auto gather = [&](auto &self, std::vector<Layer> &layers) -> void {
      for (Layer &layer : layers) {
        if (layer.type == LayerType::Group && (layer.id == doc.active_layer || layer.selected)) {
          groups.push_back(&layer);
        }
        self(self, layer.children);
      }
    };
    gather(gather, doc.layers);
    return groups;
  };
  layer_operator("layer.convert_to_switch", "Convert to Switch",
                 "Turn the active and picked groups into switches: their children become the drawings each shows "
                 "one of at a time, the top one to start with",
                 [groups_to_convert](Context &C) { return C.document && !groups_to_convert(*C.document).empty(); },
                 [groups_to_convert](Context &C) {
                   Document &doc = *C.document;
                   for (Layer *layer : groups_to_convert(doc)) {
                     layer->type = LayerType::Switch;
                     layer->switch_keys.keys.clear();
                     layer->switch_keys.actions.clear();
                     layer->switch_value = layer->children.empty() ? 0.0f : float(layer->children.front().id);
                     layer->switch_keys.last = layer->switch_value;
                     layer->switch_keys.has_last = true;
                     layer_touch(*layer);
                   }
                   switches_resolve(doc, doc.frame_current);
                   return OpStatus::Finished;
                 });

}

}  // namespace oanim
