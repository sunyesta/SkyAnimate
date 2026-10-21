/* Layer operators, named after Moho's layer commands and Blender's style of idnames. */

#include "ops/edit_ops.hh"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>

#include "anim/draw_order.hh"
#include "anim/reference.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "ops/operator.hh"
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

void layer_operators_register()
{
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
