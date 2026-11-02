/* Copy and paste layers (Ctrl C and Ctrl V over the Layers editor), between files too: the copied
 * layers travel as a small document's JSON on the system clipboard, so they paste into another
 * file, or another Open Animate. Shapes copied over the viewport travel the same way, as their layers
 * holding only those shapes, so pasted over the Layers editor they make a new layer. */

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

#include "anim/animation.hh"
#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "anim/drivers.hh"
#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "data/material.hh"
#include "io/oanim_file.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

using json = nlohmann::ordered_json;

/* Mask steps set aside by name are found by card * MAX_STEPS + step. */
static constexpr size_t MAX_STEPS = 1000;

/* What marks clipboard text as copied layers. */
static const char *CLIPBOARD_KEY = "open_animate_layers";

/* The last copy, for when there's no system clipboard (background runs). */
static std::string &layer_clipboard_fallback()
{
  static std::string text;
  return text;
}

static void clipboard_set(const std::string &text)
{
  layer_clipboard_fallback() = text;
  if (SDL_WasInit(SDL_INIT_VIDEO)) {
    SDL_SetClipboardText(text.c_str());
  }
}

static std::string clipboard_get()
{
  if (SDL_WasInit(SDL_INIT_VIDEO) && SDL_HasClipboardText()) {
    char *text = SDL_GetClipboardText();
    std::string s = text ? text : "";
    SDL_free(text);
    if (s.find(CLIPBOARD_KEY) != std::string::npos) {
      return s;
    }
  }
  return layer_clipboard_fallback();
}

static void tree_walk(Layer &l, const std::function<void(Layer &)> &fn)
{
  fn(l);
  for (Layer &child : l.children) {
    tree_walk(child, fn);
  }
}

/* The active layer and the picked ones, top first, leaving out any inside a picked group (they
 * go with it). */
static std::vector<uint64_t> picked_layers(const Document &doc)
{
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
  return picked;
}

/* The copied layers for the clipboard: {"open_animate_layers": 1, "document": the layers as a
 * document of their own, "wrapped": per top layer, whether it sits in a copy of its bone layer (its
 * bones, so weights and Parent Bone can find theirs by name when pasted), "external": the names of
 * layers outside the copy that masks and Draw Order name}. `pieces` (optional): each layer holds only
 * this, by its id, and nothing inside it. */
static json layers_copy_json(Document &doc, const std::vector<uint64_t> &ids,
                             const std::map<uint64_t, VectorDrawing> *pieces = nullptr)
{
  Document mini;
  mini.next_layer_id = doc.next_layer_id;
  std::set<uint64_t> inside;
  for (const uint64_t id : ids) {
    tree_walk(*layer_find(doc, id), [&](Layer &l) { inside.insert(l.id); });
  }
  json wrapped = json::array(), external = json::object();
  std::set<uint64_t> materials;
  for (const uint64_t id : ids) {
    Layer copy = *layer_find(doc, id);
    if (pieces) {
      copy.children.clear();
      copy.drawing = pieces->at(id);
      copy.reference_source = 0;
      copy.reference = {};
    }
    tree_walk(copy, [&](Layer &l) {
      /* A reference whose original stays behind pastes as what it shows. */
      if (l.is_reference() && !inside.count(l.reference_source)) {
        l.drawing = layer_drawing(doc, l);
        l.reference_source = 0;
        l.reference = {};
      }
      l.selected = false;
      const auto note = [&](uint64_t other) {
        if (other && !inside.count(other)) {
          if (const Layer *o = layer_find(doc, other)) {
            external[std::to_string(other)] = o->name;
          }
        }
      };
      for (const LayerMask &m : l.masks) {
        for (const uint64_t other : mask_card_layers(m)) {
          note(other);
        }
      }
      note(uint64_t(std::fabs(l.draw_order)));
      for (const auto &k : l.draw_order_keys.keys) {
        note(uint64_t(std::fabs(k.value)));
      }
      /* Bone shapes, constraints' and modifiers' layers too, to find by name. */
      layer_links_map(l, [&](uint64_t other) {
        note(other);
        return other;
      });
      for (const Shape &s : l.drawing.shapes) {
        if (s.material) {
          materials.insert(s.material);
        }
      }
    });
    const Layer *armature = layer_armature(doc, id);
    wrapped.push_back(armature != nullptr);
    if (armature) {
      Layer holder = *armature;
      holder.id = mini.next_layer_id++;
      holder.children.clear();
      holder.children.push_back(std::move(copy));
      mini.layers.push_back(std::move(holder));
    }
    else {
      mini.layers.push_back(std::move(copy));
    }
  }
  for (const Material &m : doc.materials) {
    if (materials.count(m.id)) {
      mini.materials.push_back(m);
      mini.next_material_id = std::max(mini.next_material_id, m.id + 1);
    }
  }
  /* Drivers wholly inside the copy come along. */
  for (const Driver &d : doc.drivers) {
    bool all = inside.count(d.target.layer) != 0;
    for (const DriverVar &v : d.vars) {
      all &= inside.count(v.source.layer) != 0;
    }
    if (all) {
      mini.drivers.push_back(d);
    }
  }
  /* The one active when copied is active when pasted. */
  mini.active_layer = std::count(ids.begin(), ids.end(), doc.active_layer) ? doc.active_layer : ids.front();
  json root;
  root[CLIPBOARD_KEY] = 1;
  root["wrapped"] = wrapped;
  root["external"] = external;
  root["document"] = json::parse(oanim_document_to_json(mini));
  return root;
}

/* The small document in clipboard text already parsed. False when there is none to read. */
static bool clipboard_document(const json &root, Document &mini)
{
  if (root.is_discarded() || !root.is_object() || !root.contains(CLIPBOARD_KEY) || !root.contains("document")) {
    return false;
  }
  std::string error;
  if (!oanim_document_from_json(root["document"].dump(), mini, &error)) {
    report(ReportType::Error, "Paste: " + error);
    return false;
  }
  return true;
}

/* Materials: one with the same name and look is reused, otherwise it is added. */
static std::map<uint64_t, uint64_t> clipboard_materials(Document &doc, const Document &mini)
{
  std::map<uint64_t, uint64_t> material_map;
  for (const Material &m : mini.materials) {
    const auto same = std::find_if(doc.materials.begin(), doc.materials.end(),
                                   [&](const Material &o) { return o.name == m.name && o.style == m.style; });
    material_map[m.id] = same != doc.materials.end()
                             ? same->id
                             : material_add(doc, material_unique_name(doc, m.name), m.style).id;
  }
  return material_map;
}

/* Pastes clipboard text above the active layer, in its group. Returns the pasted top layers' ids
 * (empty: nothing to paste), the one that was active when copied first. */
static std::vector<uint64_t> layers_paste_text(Document &doc, const std::string &text)
{
  const json root = json::parse(text, nullptr, false);
  Document mini;
  if (!clipboard_document(root, mini)) {
    return {};
  }
  std::map<uint64_t, std::string> external;
  if (const auto it = root.find("external"); it != root.end() && it->is_object()) {
    for (const auto &[key, name] : it->items()) {
      if (name.is_string()) {
        external[std::stoull(key)] = name.get<std::string>();
      }
    }
  }
  const json wrapped = root.value("wrapped", json::array());

  /* Where they go: above the active layer, in its group; at the top with none. */
  std::vector<Layer> *container = &doc.layers;
  int index = 0;
  uint64_t container_owner = 0;
  if (doc.active_layer) {
    if (std::vector<Layer> *c = layer_container(doc, doc.active_layer, &index)) {
      container = c;
      const Layer *parent = layer_parent(doc, doc.active_layer);
      container_owner = parent ? parent->id : 0;
    }
  }
  /* The bone layer they land in, whose bones take the copied bones' places by name. */
  const Layer *armature = nullptr;
  if (container_owner) {
    const Layer *owner = layer_find(doc, container_owner);
    armature = owner->is_bone_layer() ? owner : layer_armature(doc, container_owner);
  }
  std::map<std::string, uint64_t> dest_bones;
  if (armature) {
    for (const Bone &b : armature->bones) {
      dest_bones[b.name] = b.id;
    }
  }

  const std::map<uint64_t, uint64_t> material_map = clipboard_materials(doc, mini);

  /* Taken out of their holders, with their bones and materials made this document's. */
  std::vector<Layer> items;
  for (size_t i = 0; i < mini.layers.size(); i++) {
    Layer &top = mini.layers[i];
    const bool held = i < wrapped.size() && wrapped[i].is_boolean() && wrapped[i].get<bool>() &&
                      top.is_bone_layer() && top.children.size() == 1;
    Layer item = held ? top.children.front() : top;
    std::map<uint64_t, uint64_t> bone_map;
    if (held) {
      for (const Bone &b : top.bones) {
        const auto it = dest_bones.find(b.name);
        bone_map[b.id] = it != dest_bones.end() ? it->second : 0;
      }
    }
    /* Layers moved by the outer bones: down to any bone layer inside the copy. */
    const auto rebind = [&](auto &&self, Layer &l) -> void {
      if (l.parent_bone) {
        const auto it = bone_map.find(l.parent_bone);
        l.parent_bone = it != bone_map.end() ? it->second : 0;
      }
      for (MeshPoint &p : l.drawing.points) {
        std::vector<BoneWeight> kept;
        for (BoneWeight w : p.weights) {
          if (const auto it = bone_map.find(w.bone); it != bone_map.end() && it->second) {
            w.bone = it->second;
            kept.push_back(w);
          }
        }
        p.weights = std::move(kept);
      }
      if (!l.is_bone_layer()) {
        for (Layer &child : l.children) {
          self(self, child);
        }
      }
    };
    rebind(rebind, item);
    tree_walk(item, [&](Layer &l) {
      for (Shape &s : l.drawing.shapes) {
        if (s.material) {
          const auto it = material_map.find(s.material);
          s.material = it != material_map.end() ? it->second : 0;
        }
      }
    });
    items.push_back(std::move(item));
  }

  /* Masks and Draw Order naming layers left behind: set aside by name, before the ids change. */
  std::set<uint64_t> copied;
  for (Layer &item : items) {
    tree_walk(item, [&](Layer &l) { copied.insert(l.id); });
  }
  struct Outside {
    std::map<size_t, std::vector<std::string>> rows;
    std::string order;
  };
  std::map<uint64_t, Outside> outside;
  for (Layer &item : items) {
    tree_walk(item, [&](Layer &l) {
      for (size_t r = 0; r < l.masks.size() * MAX_STEPS; r++) {
        if (r % MAX_STEPS >= l.masks[r / MAX_STEPS].steps.size()) {
          continue;
        }
        std::vector<uint64_t> &ids = l.masks[r / MAX_STEPS].steps[r % MAX_STEPS].layers;
        for (auto it = ids.begin(); it != ids.end();) {
          if (copied.count(*it)) {
            ++it;
            continue;
          }
          if (const auto e = external.find(*it); e != external.end()) {
            outside[l.id].rows[r].push_back(e->second);
          }
          it = ids.erase(it);
        }
      }
      const uint64_t order = uint64_t(std::fabs(l.draw_order));
      if (order && !copied.count(order)) {
        const auto e = external.find(order);
        outside[l.id].order = e != external.end() ? (l.draw_order < 0.0f ? "-" : "+") + e->second : "";
        l.draw_order = 0.0f;
      }
      for (auto &k : l.draw_order_keys.keys) {
        if (k.value != 0.0f && !copied.count(uint64_t(std::fabs(k.value)))) {
          k.value = 0.0f;
        }
      }
    });
  }

  /* The other links naming layers left behind (bone shapes, constraints' and modifiers' layers): set aside by
   * name as well, each held meanwhile as a tag no layer has (they'd name some other layer of this document). */
  std::vector<std::string> left_behind;
  constexpr uint64_t TAG = uint64_t(1) << 62;
  for (Layer &item : items) {
    tree_walk(item, [&](Layer &l) {
      layer_links_map(l, [&](uint64_t other) -> uint64_t {
        if (copied.count(other)) {
          return other;
        }
        const auto e = external.find(other);
        left_behind.push_back(e != external.end() ? e->second : std::string());
        return TAG + left_behind.size() - 1;
      });
    });
  }

  std::vector<uint64_t> pasted;
  std::map<uint64_t, uint64_t> remap;
  std::set<std::string> taken;
  for (const Layer *l : document_layers_flat(doc)) {
    taken.insert(l->name);
  }
  /* Copied together, under a group held only meanwhile: one map of old ids to new covers every link once,
   * across items too (a second pass could take a new id for an old one of the other file). */
  Layer bundle;
  bundle.type = LayerType::Group;
  bundle.children = std::move(items);
  Layer copies = layer_copy_with_new_ids(doc, bundle, &remap);
  for (Layer &copy : copies.children) {
    tree_walk(copy, [&](Layer &l) {
      if (taken.count(l.name)) {
        l.name = document_layer_unique_name(doc, l.name);
      }
      taken.insert(l.name);
    });
    pasted.push_back(copy.id);
    container->insert(container->begin() + index++, std::move(copy));
  }

  /* Links naming layers left behind: by name, where this document has them. */
  const auto by_name = [&](const std::string &name) -> uint64_t {
    for (const Layer *l : document_layers_flat(doc)) {
      if (l->name == name && !std::count(pasted.begin(), pasted.end(), l->id)) {
        return l->id;
      }
    }
    return 0;
  };
  for (const uint64_t id : pasted) {
    tree_walk(*layer_find(doc, id), [&](Layer &l) {
      layer_links_map(l, [&](uint64_t other) -> uint64_t {
        if (other < TAG || other - TAG >= left_behind.size()) {
          return other;
        }
        const std::string &name = left_behind[size_t(other - TAG)];
        return name.empty() ? 0 : by_name(name);
      });
    });
  }
  for (const auto &[old_id, set_aside] : outside) {
    Layer *l = layer_find(doc, remap[old_id]);
    if (!l) {
      continue;
    }
    for (const auto &[row, names] : set_aside.rows) {
      for (const std::string &name : names) {
        if (const uint64_t found = by_name(name);
            found && row / MAX_STEPS < l->masks.size() && row % MAX_STEPS < l->masks[row / MAX_STEPS].steps.size())
        {
          l->masks[row / MAX_STEPS].steps[row % MAX_STEPS].layers.push_back(found);
        }
      }
    }
    if (!set_aside.order.empty()) {
      if (const uint64_t found = by_name(set_aside.order.substr(1))) {
        l->draw_order = set_aside.order[0] == '-' ? -float(found) : float(found);
      }
    }
    for (LayerMask &m : l->masks) {
      std::erase_if(m.steps, mask_step_empty);
    }
    std::erase_if(l->masks, [](const LayerMask &m) { return m.steps.empty(); });
  }
  if (const auto it = remap.find(mini.active_layer); it != remap.end()) {
    std::stable_partition(pasted.begin(), pasted.end(), [&](uint64_t id) { return id == it->second; });
  }
  /* Drivers that came along drive the copies. */
  for (Driver d : mini.drivers) {
    const auto map = [&](DriverRef &ref) {
      const auto it = remap.find(ref.layer);
      ref.layer = it != remap.end() ? it->second : 0;
      ref.bone = ref.item = 0;
      return ref.layer != 0;
    };
    bool ok = map(d.target);
    for (DriverVar &v : d.vars) {
      ok &= map(v.source);
    }
    if (ok) {
      doc.drivers.push_back(std::move(d));
    }
  }
  return pasted;
}

template<typename F> static void drawing_channels(VectorDrawing &drawing, F &&f)
{
  for (MeshPoint &p : drawing.points) {
    f(p.pos_keys);
    f(p.radius_keys);
  }
  for (Curve &curve : drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      f(cp.curvature_keys);
      f(cp.handle_in_keys);
      f(cp.handle_out_keys);
    }
  }
}

/* Leaves in `drawing` only what drawing_duplicate_selected just copied, nothing selected. */
static void only_the_copy(VectorDrawing &drawing)
{
  std::vector<bool> others(drawing.points.size());
  for (size_t i = 0; i < drawing.points.size(); i++) {
    others[i] = !drawing.points[i].selected;
  }
  drawing_remove_points(drawing, others);
  for (MeshPoint &p : drawing.points) {
    p.selected = false;
  }
}

/* Copies what Shift D would of each drawing being edited, as its layer holding only that, and beside each how it
 * shows now (on this frame, bones and deforms included: "points" [x, y, radius], "curves" [in x, in y, out x, out y,
 * curvature] per curve point, in its layer's space) and where that layer is ("world"), for a paste without keys.
 * Its keys are kept with the mainline's out, whichever timeline is being edited. Returns how many shapes. */
static int shapes_copy(Context &C)
{
  Document &doc = *C.document;
  std::map<uint64_t, VectorDrawing> pieces;
  std::vector<uint64_t> ids;
  json shown = json::array();
  int shapes = 0;
  for (Layer *layer : editable_layers(C)) {
    if (layer->type != LayerType::Vector) {
      continue;
    }
    VectorDrawing piece = layer_drawing(doc, *layer);
    VectorDrawing flat = piece;
    const std::vector<Transform2D> skins = layer_point_skins(doc, *layer);
    for (size_t i = 0; i < flat.points.size() && i < skins.size(); i++) {
      flat.points[i].pos = skins[i].apply(flat.points[i].pos);
    }
    for (Curve &curve : flat.curves) {
      for (CurvePoint &cp : curve.points) {
        if (size_t(cp.point) < skins.size()) {
          cp.handle_in = skins[size_t(cp.point)].apply_vector(cp.handle_in);
          cp.handle_out = skins[size_t(cp.point)].apply_vector(cp.handle_out);
        }
      }
    }
    if (!drawing_duplicate_selected(piece)) {
      continue;
    }
    drawing_duplicate_selected(flat);
    only_the_copy(piece);
    only_the_copy(flat);
    const bool in_action = !doc.action.empty() &&
                           (layer->id == doc.action_layer || layer_inside(doc, layer->id, doc.action_layer));
    if (in_action) {
      drawing_timeline_swap(piece, doc.action, "");
    }
    json points = json::array(), curves = json::array();
    for (const MeshPoint &p : flat.points) {
      points.push_back({p.pos.x, p.pos.y, p.radius});
    }
    for (const Curve &curve : flat.curves) {
      json row = json::array();
      for (const CurvePoint &cp : curve.points) {
        row.push_back({cp.handle_in.x, cp.handle_in.y, cp.handle_out.x, cp.handle_out.y, cp.curvature});
      }
      curves.push_back(std::move(row));
    }
    const Transform2D world = layer_world_transform(doc, layer->id);
    shown.push_back({{"world", {world.a, world.b, world.c, world.d, world.tx, world.ty}},
                     {"points", std::move(points)},
                     {"curves", std::move(curves)}});
    shapes += int(piece.shapes.size());
    ids.push_back(layer->id);
    pieces[layer->id] = std::move(piece);
  }
  if (ids.empty()) {
    return 0;
  }
  json root = layers_copy_json(doc, ids, &pieces);
  root["shapes"] = std::move(shown);
  clipboard_set(root.dump());
  return std::max(shapes, 1);
}

/* `piece` with no keys, as it showed when copied: that pose is its setup. */
static void piece_as_shown(VectorDrawing &piece, const json &shown)
{
  drawing_channels(piece, [](auto &ch) { ch = {}; });
  const json &points = shown["points"];
  for (size_t i = 0; i < piece.points.size() && i < points.size(); i++) {
    MeshPoint &p = piece.points[i];
    p.pos = p.setup_pos = {points[i][0].get<float>(), points[i][1].get<float>()};
    p.transform = {};
    p.radius = points[i][2].get<float>();
  }
  const json &curves = shown["curves"];
  for (size_t c = 0; c < piece.curves.size() && c < curves.size(); c++) {
    for (size_t k = 0; k < piece.curves[c].points.size() && k < curves[c].size(); k++) {
      CurvePoint &cp = piece.curves[c].points[k];
      const json &row = curves[c][k];
      cp.handle_in = cp.setup_in = {row[0].get<float>(), row[1].get<float>()};
      cp.handle_out = cp.setup_out = {row[2].get<float>(), row[3].get<float>()};
      cp.change_in = cp.change_out = {};
      cp.curvature = row[4].get<float>();
    }
  }
}

/* `piece` carried by `to`, keys and all: a key holds a change from setup, so it takes the turn and scale only. A
 * handle's key is a turn and a length. */
static void piece_transform(VectorDrawing &piece, const Transform2D &to)
{
  const float scale = std::sqrt(std::fabs(to.a * to.d - to.b * to.c));
  const auto keys_along = [&](Channel<float2> &ch) {
    const auto each = [&](std::vector<Key<float2>> &keys) {
      for (Key<float2> &k : keys) {
        k.value = to.apply_vector(k.value);
      }
    };
    each(ch.keys);
    for (auto &[name, keys] : ch.actions) {
      each(keys);
    }
  };
  const auto keys_scaled = [&](Channel<float2> &ch) {
    const auto each = [&](std::vector<Key<float2>> &keys) {
      for (Key<float2> &k : keys) {
        k.value.y *= scale;
      }
    };
    each(ch.keys);
    for (auto &[name, keys] : ch.actions) {
      each(keys);
    }
  };
  for (MeshPoint &p : piece.points) {
    p.pos = to.apply(p.pos);
    p.setup_pos = to.apply(p.setup_pos);
    p.transform = to.apply_vector(p.transform);
    keys_along(p.pos_keys);
  }
  for (Curve &curve : piece.curves) {
    for (CurvePoint &cp : curve.points) {
      cp.handle_in = to.apply_vector(cp.handle_in);
      cp.handle_out = to.apply_vector(cp.handle_out);
      cp.setup_in = to.apply_vector(cp.setup_in);
      cp.setup_out = to.apply_vector(cp.setup_out);
      cp.change_in.y *= scale;
      cp.change_out.y *= scale;
      keys_scaled(cp.handle_in_keys);
      keys_scaled(cp.handle_out_keys);
    }
  }
}

/* Pastes copied shapes into the active drawing, on top, and selects them. */
static OpStatus shapes_paste(Context &C, const json &root)
{
  Document &doc = *C.document;
  Layer *target = document_active_layer(doc);
  if (!target || target->type != LayerType::Vector || target->is_reference()) {
    report(ReportType::Warning, target && target->is_reference() ? "A copy (reference layer) can't get new shapes: pick a "
                                                                   "drawing layer to paste into"
                                                                 : "Pick a drawing layer to paste shapes into");
    return OpStatus::Cancelled;
  }
  Document mini;
  if (!clipboard_document(root, mini)) {
    return OpStatus::Cancelled;
  }
  const bool keyframes = op_arg_bool(C, "keyframes");
  const bool keep_place = !op_has_arg(C, "keep_place") || op_arg_bool(C, "keep_place");
  op_arg_used(C, "keyframes", keyframes ? "1" : "0");
  op_arg_used(C, "keep_place", keep_place ? "1" : "0");
  animation_record(doc);

  std::map<std::string, uint64_t> dest_bones;
  if (const Layer *armature = layer_armature(doc, target->id)) {
    for (const Bone &b : armature->bones) {
      dest_bones[b.name] = b.id;
    }
  }
  std::set<std::string> dest_actions;
  bool in_action = false;
  for (const Layer *owner : action_owners(doc, target->id)) {
    dest_actions.insert(owner->actions.begin(), owner->actions.end());
    in_action = in_action || (!doc.action.empty() && owner->id == doc.action_layer);
  }
  const std::map<uint64_t, uint64_t> material_map = clipboard_materials(doc, mini);
  const json wrapped = root.value("wrapped", json::array());
  const json &shown = root["shapes"];
  const Transform2D here = layer_world_transform(doc, target->id).inverted();
  std::set<std::string> skipped;
  const size_t had_points = target->drawing.points.size();
  int shapes = 0;

  for (size_t i = 0; i < mini.layers.size() && i < shown.size(); i++) {
    Layer &top = mini.layers[i];
    const bool held = i < wrapped.size() && wrapped[i].is_boolean() && wrapped[i].get<bool>() && top.is_bone_layer() &&
                      top.children.size() == 1;
    VectorDrawing piece = std::move((held ? top.children.front() : top).drawing);
    if (keyframes) {
      /* Keys of actions this layer has no part in have nowhere to go. */
      drawing_channels(piece, [&](auto &ch) {
        std::vector<std::string> gone;
        for (const auto &[name, keys] : ch.actions) {
          if (!name.empty() && !dest_actions.count(name)) {
            gone.push_back(name);
          }
        }
        for (const std::string &name : gone) {
          ch.actions.erase(name);
          skipped.insert(name);
        }
      });
      if (in_action) {
        drawing_timeline_swap(piece, "", doc.action);
      }
    }
    else {
      piece_as_shown(piece, shown[i]);
    }
    if (keep_place) {
      const json &w = shown[i]["world"];
      Transform2D world;
      world.a = w[0].get<float>();
      world.b = w[1].get<float>();
      world.c = w[2].get<float>();
      world.d = w[3].get<float>();
      world.tx = w[4].get<float>();
      world.ty = w[5].get<float>();
      piece_transform(piece, here * world);
    }
    /* Weights find this layer's bones by name, as a pasted layer's do; a point left with none is a new point. */
    std::map<uint64_t, uint64_t> bone_map;
    if (held) {
      for (const Bone &b : top.bones) {
        const auto it = dest_bones.find(b.name);
        bone_map[b.id] = it != dest_bones.end() ? it->second : 0;
      }
    }
    for (MeshPoint &p : piece.points) {
      std::vector<BoneWeight> kept;
      for (BoneWeight w : p.weights) {
        if (const auto it = bone_map.find(w.bone); it != bone_map.end() && it->second) {
          w.bone = it->second;
          kept.push_back(w);
        }
      }
      p.weights = std::move(kept);
    }
    for (Shape &s : piece.shapes) {
      const auto it = material_map.find(s.material);
      s.material = s.material && it != material_map.end() ? it->second : 0;
    }
    VectorDrawing &drawing = target->drawing;
    const size_t first_point = drawing.points.size(), first_shape = drawing.shapes.size();
    drawing_append_selected(drawing, piece);
    for (size_t k = first_shape; k < drawing.shapes.size(); k++) {
      drawing.shapes[k].id = 0;
      /* Its own name, numbered only if this drawing has it already. */
      const std::string &name = piece.shapes[k - first_shape].name;
      std::string free_name = name;
      for (int n = 2; std::any_of(drawing.shapes.begin(), drawing.shapes.begin() + long(k),
                                  [&](const Shape &s) { return s.name == free_name; });
           n++)
      {
        free_name = name + " " + std::to_string(n);
      }
      drawing.shapes[k].name = free_name;
    }
    drawing_ensure_ids(drawing);
    /* Point groups this drawing has too keep what's in them. */
    for (const PointGroup &group : piece.groups) {
      const auto own = std::find_if(drawing.groups.begin(), drawing.groups.end(),
                                    [&](const PointGroup &g) { return g.name == group.name; });
      if (own == drawing.groups.end()) {
        continue;
      }
      for (size_t k = 0; k < piece.points.size(); k++) {
        if (const auto it = group.weights.find(piece.points[k].id); it != group.weights.end()) {
          own->weights[drawing.points[first_point + k].id] = it->second;
        }
      }
    }
    shapes += int(piece.shapes.size());
  }
  VectorDrawing &drawing = target->drawing;
  if (drawing.points.size() == had_points) {
    report(ReportType::Warning, "Nothing to paste: copy shapes with Ctrl C over the viewport first");
    return OpStatus::Cancelled;
  }
  layer_touch(*target);
  /* Off the setup frame, under what moves the layer there: stored so that it shows where it was copied from. */
  if (keep_place && !keyframes && doc.frame_current != 0) {
    const std::vector<Transform2D> skins = layer_point_skins(doc, *target);
    for (size_t i = had_points; i < drawing.points.size() && i < skins.size(); i++) {
      MeshPoint &p = drawing.points[i];
      p.pos = p.setup_pos = skins[i].inverted().apply(p.pos);
    }
    for (Curve &curve : drawing.curves) {
      for (CurvePoint &cp : curve.points) {
        if (size_t(cp.point) >= had_points && size_t(cp.point) < skins.size()) {
          const Transform2D back = skins[size_t(cp.point)].inverted();
          cp.handle_in = cp.setup_in = back.apply_vector(cp.handle_in);
          cp.handle_out = cp.setup_out = back.apply_vector(cp.handle_out);
        }
      }
    }
  }
  for (size_t i = 0; i < drawing.points.size(); i++) {
    drawing.points[i].selected = i >= had_points;
  }
  layer_touch(*target);
  /* Keys brought along say where the points are on this frame, not the frame they were copied on. */
  if (keyframes) {
    animation_evaluate(doc, doc.frame_current);
  }
  std::string message = "Pasted " + std::to_string(std::max(shapes, 1)) + (shapes == 1 ? " shape" : " shapes") + " into " +
                        target->name;
  if (!skipped.empty()) {
    message += ". Left out the keys of ";
    for (const std::string &name : skipped) {
      message += (name == *skipped.begin() ? "" : ", ") + name;
    }
    message += ": this layer isn't in those actions";
  }
  report(ReportType::Info, message);
  C.undo_name = "Paste Shapes";
  return OpStatus::Finished;
}

void layer_clipboard_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "mesh.copy";
    ot.name = "Copy Shapes";
    ot.description = "Copy the selected shapes and pieces of line, as Shift D would, to the clipboard, to paste into "
                     "another layer or file (Ctrl C over the viewport)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) {
      if (!C.document) {
        return false;
      }
      const std::vector<Layer *> editing = editable_layers(C);
      return std::any_of(editing.begin(), editing.end(), [](const Layer *l) { return l->type == LayerType::Vector; });
    };
    ot.exec = [](Context &C) {
      const int shapes = shapes_copy(C);
      if (shapes == 0) {
        report(ReportType::Warning, "Select a shape, or at least two connected points of a line, to copy");
        return OpStatus::Cancelled;
      }
      report(ReportType::Info, "Copied " + std::to_string(shapes) + (shapes == 1 ? " shape" : " shapes"));
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "mesh.paste";
    ot.name = "Paste Shapes";
    ot.description = "Paste copied shapes into the active drawing, on top, and select them (Ctrl V over the viewport). "
                     "Copied layers paste as layers";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    const auto for_shapes = [](const OpArgs &args) {
      const auto it = args.find("shapes");
      return it != args.end() && it->second == "1";
    };
    OpProperty keep_place = op_bool_property(
        "keep_place", "Keep Place",
        "They land exactly where they looked when copied, whatever this layer's place. Off, the same numbers in this "
        "layer's own space",
        "1");
    OpProperty keyframes = op_bool_property(
        "keyframes", "Keyframes",
        "Bring their keys too, on the mainline and on the action lines this layer has. Off, they're pasted as they looked "
        "when copied, and that pose is their setup",
        "0");
    keep_place.visible = for_shapes;
    keyframes.visible = for_shapes;
    ot.props = {std::move(keep_place), std::move(keyframes)};
    ot.exec = [](Context &C) {
      const json root = json::parse(clipboard_get(), nullptr, false);
      if (!root.is_discarded() && root.is_object() && root.contains("shapes")) {
        op_arg_used(C, "shapes", "1");
        return shapes_paste(C, root);
      }
      return operator_call(C, "layer.paste");
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "layer.copy";
    ot.name = "Copy Layers";
    ot.description = "Copy the active layer and the picked ones (with everything in them) to the clipboard, to "
                     "paste into this file or another (Ctrl C over the Layers editor)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document && document_active_layer(*C.document); };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::vector<uint64_t> ids = picked_layers(doc);
      if (ids.empty()) {
        return OpStatus::Cancelled;
      }
      clipboard_set(layers_copy_json(doc, ids).dump());
      report(ReportType::Info, ids.size() == 1 ? "Copied " + layer_find(doc, ids.front())->name
                                               : "Copied " + std::to_string(ids.size()) + " layers");
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "layer.paste";
    ot.name = "Paste Layers";
    ot.description = "Paste copied layers above the active layer, in its group (Ctrl V over the Layers editor). "
                     "Masks and Draw Order naming layers that weren't copied find this file's layers of the same "
                     "name; inside a bone layer, weights and Parent Bone find its bones by name";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::vector<uint64_t> pasted = layers_paste_text(doc, clipboard_get());
      if (pasted.empty()) {
        report(ReportType::Warning, "Nothing to paste: copy layers with Ctrl C over the Layers editor first");
        return OpStatus::Cancelled;
      }
      for (const Layer *l : document_layers_flat(doc)) {
        layer_find(doc, l->id)->selected = false;
      }
      for (const uint64_t id : pasted) {
        layer_find(doc, id)->selected = id != pasted.front();
      }
      doc.active_layer = pasted.front();
      C.undo_name = pasted.size() == 1 ? "Paste " + layer_find(doc, pasted.front())->name
                                       : "Paste " + std::to_string(pasted.size()) + " Layers";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace oanim
