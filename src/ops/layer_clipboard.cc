/* Copy and paste layers (Ctrl C and Ctrl V over the Layers editor), between files too: the copied
 * layers travel as a small document's JSON on the system clipboard, so they paste into another
 * file, or another Open Animate. */

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>

#include <nlohmann/json.hpp>

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

/* The copied layers as clipboard text: {"open_animate_layers": 1, "document": the layers as a
 * document of their own, "wrapped": per top layer, whether it sits in a copy of its bone layer (its
 * bones, so weights and Parent Bone can find theirs by name when pasted), "external": the names of
 * layers outside the copy that masks and Draw Order name}. */
static std::string layers_copy_text(Document &doc, const std::vector<uint64_t> &ids)
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
  return root.dump();
}

/* Pastes clipboard text above the active layer, in its group. Returns the pasted top layers' ids
 * (empty: nothing to paste), the one that was active when copied first. */
static std::vector<uint64_t> layers_paste_text(Document &doc, const std::string &text)
{
  const json root = json::parse(text, nullptr, false);
  if (root.is_discarded() || !root.is_object() || !root.contains(CLIPBOARD_KEY) || !root.contains("document")) {
    return {};
  }
  Document mini;
  std::string error;
  if (!oanim_document_from_json(root["document"].dump(), mini, &error)) {
    report(ReportType::Error, "Paste Layers: " + error);
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

  /* Materials: one with the same name and look is reused, otherwise it is added. */
  std::map<uint64_t, uint64_t> material_map;
  for (const Material &m : mini.materials) {
    const auto same = std::find_if(doc.materials.begin(), doc.materials.end(),
                                   [&](const Material &o) { return o.name == m.name && o.style == m.style; });
    material_map[m.id] = same != doc.materials.end()
                             ? same->id
                             : material_add(doc, material_unique_name(doc, m.name), m.style).id;
  }

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

  std::vector<uint64_t> pasted;
  std::map<uint64_t, uint64_t> remap;
  std::set<std::string> taken;
  for (const Layer *l : document_layers_flat(doc)) {
    taken.insert(l->name);
  }
  for (Layer &item : items) {
    Layer copy = layer_copy_with_new_ids(doc, item, &remap);
    tree_walk(copy, [&](Layer &l) {
      if (taken.count(l.name)) {
        l.name = document_layer_unique_name(doc, l.name);
      }
      taken.insert(l.name);
    });
    pasted.push_back(copy.id);
    container->insert(container->begin() + index++, std::move(copy));
  }

  /* Draw Order naming copied layers names their copies; then the ones left behind, by name, where
   * this document has them. */
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
      const auto map_order = [&](float &order) {
        const auto it = remap.find(uint64_t(std::fabs(order)));
        if (order != 0.0f && it != remap.end()) {
          order = order < 0.0f ? -float(it->second) : float(it->second);
        }
      };
      map_order(l.draw_order);
      for (auto &k : l.draw_order_keys.keys) {
        map_order(k.value);
      }
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
      std::erase_if(m.steps, [](const MaskStep &s) { return s.layers.empty(); });
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

void layer_clipboard_operators_register()
{
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
      clipboard_set(layers_copy_text(doc, ids));
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
