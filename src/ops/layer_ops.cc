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

#include "anim/draw_order.hh"
#include "anim/drivers.hh"
#include "anim/layer_pick.hh"
#include "anim/reference.hh"
#include "core/log.hh"
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
}

/* Sets a layer's position, rotation and scale from `t` (no shear), its scale's x keeping the sign
 * `negative_x` gives. */
static void layer_set_local_transform(Layer &l, const Transform2D &t, bool negative_x)
{
  float sx = std::sqrt(t.a * t.a + t.b * t.b);
  if (sx < 1e-12f) {
    return;
  }
  if (negative_x) {
    sx = -sx;
  }
  l.position = {t.tx, t.ty};
  l.rotation = std::atan2(t.b / sx, t.a / sx) * 180.0f / PI;
  l.scale = {sx, (t.a * t.d - t.b * t.c) / sx};
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

/* The copy's contents mirrored in place: drawings flipped (setup pose, no point keys), weights and
 * Parent Bone moved to the other side's bones, the layers inside flipped about their parent's
 * axis. */
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
  if (!top) {
    l.position.x = -l.position.x;
    l.rotation = -l.rotation;
    clear_transform_keys(l);
  }
  l.selected = false;
  for (Layer &child : l.children) {
    symmetrize_contents(child, bone_flip, false);
  }
}

/* A copy to mirror: references become plain layers showing what they showed. */
static void symmetrize_unreference(const Document &doc, Layer &l)
{
  if (l.is_reference()) {
    l.drawing = layer_drawing(doc, l);
    l.reference_source = 0;
    l.reference = {};
  }
  for (Layer &child : l.children) {
    symmetrize_unreference(doc, child);
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

/* Draw Order holds a layer's id (minus it for Behind). */
static void draw_order_map(float &order, const std::function<uint64_t(uint64_t)> &map)
{
  if (order != 0.0f) {
    const uint64_t id = map(uint64_t(std::fabs(order)));
    order = order < 0.0f ? -float(id) : float(id);
  }
}

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
  symmetrize_unreference(doc, plain);
  std::map<uint64_t, uint64_t> remap;
  Layer copy = layer_copy_with_new_ids(doc, plain, &remap);
  symmetrize_contents(copy, symmetrize_bone_flip(doc, source_id), true);
  layer_set_local_transform(copy, local, source->scale.x < 0.0f);
  clear_transform_keys(copy);
  /* Draw Order naming a layer copied with it names its copy (masks already do). */
  const auto copied = [&](uint64_t i) {
    const auto it = remap.find(i);
    return it != remap.end() ? it->second : i;
  };
  layer_tree_walk(copy, [&](Layer &l) {
    draw_order_map(l.draw_order, copied);
    for (auto &k : l.draw_order_keys.keys) {
      draw_order_map(k.value, copied);
    }
  });

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
    layer_tree_walk(copy, [&](Layer &l) {
      layer_masks_remap(l, reassign);
      draw_order_map(l.draw_order, reassigned);
      for (auto &k : l.draw_order_keys.keys) {
        draw_order_map(k.value, reassigned);
      }
    });
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
      layer_tree_walk(*copy, [&](Layer &l) {
        for (LayerMask &m : l.masks) {
          for (MaskStep &s : m.steps) {
            for (uint64_t &mid : s.layers) {
              mid = other(mid);
            }
          }
        }
        draw_order_map(l.draw_order, other);
        for (auto &k : l.draw_order_keys.keys) {
          draw_order_map(k.value, other);
        }
      });
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
  layer_operator("layer.new", "New Layer", "Add a vector layer above the active layer", nullptr, [](Context &C) {
    document_add_layer(*C.document, LayerType::Vector, "");
    return OpStatus::Finished;
  });

  layer_operator("layer.new_group", "New Group", "Add a group layer above the active layer", nullptr,
                 [](Context &C) {
                   document_add_layer(*C.document, LayerType::Group, "");
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
                   std::map<uint64_t, uint64_t> remap;
                   std::vector<uint64_t> copies;
                   uint64_t active_copy = 0;
                   for (const uint64_t id : picked) {
                     int index = 0;
                     std::vector<Layer> *container = layer_container(doc, id, &index);
                     if (!container) {
                       continue;
                     }
                     Layer copy = layer_copy_with_new_ids(doc, (*container)[index], &remap);
                     copy.name = document_layer_unique_name(doc, (*container)[index].name);
                     copy.selected = false;
                     if (id == doc.active_layer) {
                       active_copy = copy.id;
                     }
                     copies.push_back(copy.id);
                     container->insert(container->begin() + index, std::move(copy));
                   }
                   /* Masks among the copies name the copies. */
                   for (const uint64_t id : copies) {
                     if (Layer *copy = layer_find(doc, id)) {
                       const auto walk = [&](auto &&self, Layer &l) -> void {
                         layer_masks_remap(l, remap);
                         for (Layer &child : l.children) {
                           self(self, child);
                         }
                       };
                       walk(walk, *copy);
                     }
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

  layer_operator(
      "layer.group", "Group With Selection",
      "Put the active and selected layers into a new group where the active one was, as Moho does",
      active_layer_poll, [](Context &C) {
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
        Layer &group = document_add_layer(doc, LayerType::Group, "");
        const uint64_t group_id = group.id;
        for (const uint64_t id : ids) {
          document_reparent_layer(doc, id, group_id, int(layer_find(doc, group_id)->children.size()));
        }
        layers_deselect(doc);
        doc.active_layer = group_id;
        return OpStatus::Finished;
      });

}

}  // namespace oanim
