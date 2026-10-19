/* Layer operators, named after Moho's layer commands and Blender's style of idnames. */

#include "ops/edit_ops.hh"

#include <algorithm>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace sky {

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

  layer_operator("layer.delete", "Delete Layer", "Delete the active layer and everything in it", active_layer_poll,
                 [](Context &C) {
                   document_remove_layer(*C.document, C.document->active_layer);
                   return OpStatus::Finished;
                 });

  layer_operator("layer.duplicate", "Duplicate Layer", "Copy the active layer, placing the copy above it",
                 active_layer_poll, [](Context &C) {
                   Document &doc = *C.document;
                   int index = 0;
                   std::vector<Layer> *container = layer_container(doc, doc.active_layer, &index);
                   Layer copy = layer_copy_with_new_ids(doc, (*container)[index]);
                   copy.name = document_layer_unique_name(doc, (*container)[index].name);
                   const uint64_t id = copy.id;
                   container->insert(container->begin() + index, std::move(copy));
                   doc.active_layer = id;
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
                   return OpStatus::Finished;
                 });

  layer_operator(
      "layer.break_reference", "Break Reference", "Turn the active reference layer into an ordinary layer",
      [](Context &C) {
        const Layer *layer = C.document ? document_active_layer(*C.document) : nullptr;
        return layer && layer->is_reference();
      },
      [](Context &C) {
        layer_break_reference(*C.document, C.document->active_layer);
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

  /* Moho's group right-click shortcuts for the commonest masks, set from the bottom layer:
   * INSIDE shows the layers above only inside it (Hide all, bottom layer + Add to mask),
   * OUTSIDE only outside it (Reveal all, bottom layer - Subtract from mask); NONE turns off
   * masking. */
  layer_operator(
      "layer.group_mask_set", "Group Mask",
      "Mask the group's layers inside or outside its bottom layer (mode=\"INSIDE\", \"OUTSIDE\" or \"NONE\")",
      [](Context &C) {
        const Layer *layer = document_active_layer(*C.document);
        return layer && layer->is_group();
      },
      [](Context &C) {
        Layer &group = *document_active_layer(*C.document);
        const std::string mode = op_arg(C, "mode", "INSIDE");
        if (mode == "NONE") {
          group.group_mask = GroupMask::None;
          return OpStatus::Finished;
        }
        if ((mode != "INSIDE" && mode != "OUTSIDE") || group.children.empty()) {
          report(ReportType::Error, group.children.empty() ? "The group has no bottom layer to mask with"
                                                           : "mode must be INSIDE, OUTSIDE or NONE");
          return OpStatus::Cancelled;
        }
        Layer &bottom = group.children.back();
        group.group_mask = mode == "INSIDE" ? GroupMask::HideAll : GroupMask::ShowAll;
        bottom.mask_mode = mode == "INSIDE" ? MaskMode::Add : MaskMode::SubtractInvisible;
        return OpStatus::Finished;
      });
}

}  // namespace sky
