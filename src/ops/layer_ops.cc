/* Layer operators, named after Moho's layer commands and Blender's style of idnames. */

#include "ops/edit_ops.hh"

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

  layer_operator("layer.group", "Group Layer", "Put the active layer into a new group in its place",
                 active_layer_poll, [](Context &C) {
                   Document &doc = *C.document;
                   const uint64_t layer_id = doc.active_layer;
                   Layer &group = document_add_layer(doc, LayerType::Group, "");
                   const uint64_t group_id = group.id;
                   document_reparent_layer(doc, layer_id, group_id, 0);
                   doc.active_layer = layer_id;
                   return OpStatus::Finished;
                 });
}

}  // namespace sky
