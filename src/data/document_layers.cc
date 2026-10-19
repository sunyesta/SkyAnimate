/* The layer tree inside a document: finding, adding, moving, copying and referencing layers. */

#include <algorithm>
#include <map>

#include "data/document.hh"

#include "data/bones.hh"

namespace sky {

template<typename LayerT, typename ListT> static LayerT *find_in(ListT &list, uint64_t id)
{
  for (auto &layer : list) {
    if (layer.id == id) {
      return &layer;
    }
    if (LayerT *found = find_in<LayerT>(layer.children, id)) {
      return found;
    }
  }
  return nullptr;
}

Layer *layer_find(Document &doc, uint64_t id)
{
  return id ? find_in<Layer>(doc.layers, id) : nullptr;
}

const Layer *layer_find(const Document &doc, uint64_t id)
{
  return id ? find_in<const Layer>(doc.layers, id) : nullptr;
}

static std::vector<Layer> *container_in(std::vector<Layer> &list, uint64_t id, int *r_index)
{
  for (size_t i = 0; i < list.size(); i++) {
    if (list[i].id == id) {
      *r_index = int(i);
      return &list;
    }
    if (std::vector<Layer> *found = container_in(list[i].children, id, r_index)) {
      return found;
    }
  }
  return nullptr;
}

std::vector<Layer> *layer_container(Document &doc, uint64_t id, int *r_index)
{
  return container_in(doc.layers, id, r_index);
}

static Layer *parent_in(std::vector<Layer> &list, Layer *parent, uint64_t id)
{
  for (Layer &layer : list) {
    if (layer.id == id) {
      return parent;
    }
    if (Layer *found = parent_in(layer.children, &layer, id)) {
      return found;
    }
  }
  return nullptr;
}

Layer *layer_parent(Document &doc, uint64_t id)
{
  return parent_in(doc.layers, nullptr, id);
}

Layer *document_active_layer(Document &doc)
{
  return layer_find(doc, doc.active_layer);
}

/* `armature` is the nearest bone layer above `list` (with its world transform), for layers
 * bound to one of its bones: such a layer moves with the bone's change from the setup pose. */
static bool world_transform_in(const std::vector<Layer> &list, const Transform2D &parent, const Layer *armature,
                               const Transform2D &armature_world, bool setup_frame, uint64_t id,
                               Transform2D *r_transform)
{
  for (const Layer &layer : list) {
    Transform2D transform = parent * layer.local_transform();
    if (armature && layer.parent_bone != 0) {
      const std::vector<BonePose> poses = armature_pose(*armature, setup_frame);
      if (const BonePose *bone = bone_pose_find(poses, layer.parent_bone)) {
        transform = armature_world * bone->skin * armature_world.inverted() * transform;
      }
    }
    if (layer.id == id) {
      *r_transform = transform;
      return true;
    }
    const bool bones = layer.is_bone_layer();
    if (world_transform_in(layer.children, transform, bones ? &layer : armature, bones ? transform : armature_world,
                           setup_frame, id, r_transform))
    {
      return true;
    }
  }
  return false;
}

Transform2D layer_world_transform(const Document &doc, uint64_t id)
{
  Transform2D transform;
  world_transform_in(doc.layers, Transform2D(), nullptr, Transform2D(), doc.frame_current == 0, id, &transform);
  return transform;
}

const VectorDrawing &layer_drawing(const Document &doc, const Layer &layer)
{
  /* Follow the reference chain; a broken or circular chain shows the layer's own drawing. */
  const Layer *current = &layer;
  for (int depth = 0; current->is_reference() && depth < 32; depth++) {
    const Layer *source = layer_find(doc, current->reference_source);
    if (!source) {
      break;
    }
    current = source;
  }
  return current->drawing;
}

static void flatten(const std::vector<Layer> &list, std::vector<const Layer *> &r_flat)
{
  for (const Layer &layer : list) {
    r_flat.push_back(&layer);
    flatten(layer.children, r_flat);
  }
}

std::vector<const Layer *> document_layers_flat(const Document &doc)
{
  std::vector<const Layer *> flat;
  flatten(doc.layers, flat);
  return flat;
}

std::string document_layer_unique_name(const Document &doc, const std::string &base)
{
  const std::vector<const Layer *> flat = document_layers_flat(doc);
  const auto taken = [&](const std::string &name) {
    return std::any_of(flat.begin(), flat.end(), [&](const Layer *l) { return l->name == name; });
  };
  for (int i = 1;; i++) {
    const std::string name = base + " " + std::to_string(i);
    if (!taken(name)) {
      return name;
    }
  }
}

Layer &document_add_layer(Document &doc, LayerType type, const std::string &name)
{
  Layer layer;
  layer.id = doc.next_layer_id++;
  layer.type = type;
  layer.name = name.empty() ? document_layer_unique_name(doc, type == LayerType::Bone ? "Bones" : type == LayerType::Group ? "Group" : "Layer")
                            : name;

  /* Above the active layer, in the same group, as Moho and Blender add new items. */
  int index = 0;
  std::vector<Layer> *container = doc.active_layer ? layer_container(doc, doc.active_layer, &index) : nullptr;
  if (!container) {
    container = &doc.layers;
    index = 0;
  }
  container->insert(container->begin() + index, std::move(layer));
  Layer &added = (*container)[index];
  doc.active_layer = added.id;
  return added;
}

static void assign_new_ids(Document &doc, Layer &layer, std::map<uint64_t, uint64_t> &remap)
{
  const uint64_t new_id = doc.next_layer_id++;
  remap[layer.id] = new_id;
  layer.id = new_id;
  for (Layer &child : layer.children) {
    assign_new_ids(doc, child, remap);
  }
}

static void remap_references(Layer &layer, const std::map<uint64_t, uint64_t> &remap)
{
  /* References inside a copied group point at the copies of their sources when those were
   * copied too, so a duplicated rig stays self-contained. */
  if (auto it = remap.find(layer.reference_source); it != remap.end()) {
    layer.reference_source = it->second;
  }
  for (Layer &child : layer.children) {
    remap_references(child, remap);
  }
}

Layer layer_copy_with_new_ids(Document &doc, const Layer &layer)
{
  Layer copy = layer;
  std::map<uint64_t, uint64_t> remap;
  assign_new_ids(doc, copy, remap);
  remap_references(copy, remap);
  return copy;
}

Layer layer_make_reference(Document &doc, const Layer &source)
{
  Layer ref;
  ref.id = doc.next_layer_id++;
  ref.name = source.name;
  ref.type = source.type;
  ref.reference_source = source.id;
  ref.position = source.position;
  ref.rotation = source.rotation;
  ref.scale = source.scale;
  ref.opacity = source.opacity;
  ref.group_mask = source.group_mask;
  ref.mask_mode = source.mask_mode;
  for (const Layer &child : source.children) {
    ref.children.push_back(layer_make_reference(doc, child));
  }
  return ref;
}

static void unlink_references_to(Document &doc, std::vector<Layer> &list, const Layer &removed)
{
  for (Layer &layer : list) {
    if (layer.reference_source == removed.id) {
      layer.drawing = layer_drawing(doc, removed);
      layer.reference_source = 0;
    }
    unlink_references_to(doc, layer.children, removed);
  }
}

static void unlink_references_tree(Document &doc, const Layer &removed)
{
  unlink_references_to(doc, doc.layers, removed);
  for (const Layer &child : removed.children) {
    unlink_references_tree(doc, child);
  }
}

bool document_remove_layer(Document &doc, uint64_t id)
{
  int index = 0;
  std::vector<Layer> *container = layer_container(doc, id, &index);
  if (!container) {
    return false;
  }
  const Layer removed = (*container)[index];
  Layer *parent = layer_parent(doc, id);
  const uint64_t parent_id = parent ? parent->id : 0;
  container->erase(container->begin() + index);
  /* References to the removed layers keep showing what they showed, as ordinary layers. */
  unlink_references_tree(doc, removed);

  /* The layer below becomes active, else the one above, else the parent group. */
  if (doc.active_layer == id || !layer_find(doc, doc.active_layer)) {
    if (!container->empty()) {
      doc.active_layer = (*container)[std::min<size_t>(size_t(index), container->size() - 1)].id;
    }
    else {
      doc.active_layer = parent_id;
    }
  }
  return true;
}

bool document_move_layer(Document &doc, uint64_t id, int direction)
{
  int index = 0;
  std::vector<Layer> *container = layer_container(doc, id, &index);
  const int target = index + direction;
  if (!container || target < 0 || target >= int(container->size())) {
    return false;
  }
  std::swap((*container)[index], (*container)[target]);
  return true;
}

static bool is_inside(const Layer &layer, uint64_t id)
{
  for (const Layer &child : layer.children) {
    if (child.id == id || is_inside(child, id)) {
      return true;
    }
  }
  return false;
}

bool document_reparent_layer(Document &doc, uint64_t id, uint64_t new_parent, int index)
{
  if (id == new_parent) {
    return false;
  }
  int old_index = 0;
  std::vector<Layer> *old_container = layer_container(doc, id, &old_index);
  if (!old_container) {
    return false;
  }
  if (new_parent) {
    const Layer *target = layer_find(doc, new_parent);
    if (!target || !target->is_group() || is_inside((*old_container)[old_index], new_parent)) {
      return false;
    }
  }
  Layer moving = std::move((*old_container)[old_index]);
  old_container->erase(old_container->begin() + old_index);
  std::vector<Layer> &destination = new_parent ? layer_find(doc, new_parent)->children : doc.layers;
  index = std::clamp(index, 0, int(destination.size()));
  destination.insert(destination.begin() + index, std::move(moving));
  return true;
}

void layer_break_reference(Document &doc, uint64_t id)
{
  Layer *layer = layer_find(doc, id);
  if (!layer || !layer->is_reference()) {
    return;
  }
  layer->drawing = layer_drawing(doc, *layer);
  layer->reference_source = 0;
}

}  // namespace sky
