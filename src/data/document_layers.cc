/* The layer tree inside a document: finding, adding, moving, copying and referencing layers. */

#include <algorithm>
#include <cmath>
#include <map>
#include <unordered_map>

#include "anim/switch.hh"
#include "data/document.hh"

#include "data/bones.hh"
#include "data/camera.hh"

namespace oanim {

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

/* The index of a LayerIndexScope: the document it is for and its layers by id. Ids are small whole numbers, so
 * they index arrays (kept from one scope to the next: a big document is indexed several times a frame, and
 * filling arrays allocates nothing); one too big for that goes in a map. */
struct LayerIndex {
  static constexpr uint64_t DENSE_MAX = 1u << 20;
  const Document *doc = nullptr;
  std::vector<const Layer *> layers;
  /* Each layer's group (null at the top). */
  std::vector<const Layer *> parents;
  std::unordered_map<uint64_t, std::pair<const Layer *, const Layer *>> sparse;
  /* The ids filled in, to empty them again. */
  std::vector<uint64_t> used;
  /* Whether any layer has a Boolean modifier (what merge_takes_from looks for). */
  bool any_boolean = false;

  void add(const Layer &layer, const Layer *parent)
  {
    if (layer.id >= DENSE_MAX) {
      sparse[layer.id] = {&layer, parent};
      return;
    }
    if (layer.id >= layers.size()) {
      layers.resize(size_t(layer.id) + 1, nullptr);
      parents.resize(size_t(layer.id) + 1, nullptr);
    }
    layers[size_t(layer.id)] = &layer;
    parents[size_t(layer.id)] = parent;
    used.push_back(layer.id);
  }
  const Layer *find(uint64_t id) const
  {
    if (id < layers.size()) {
      return layers[size_t(id)];
    }
    const auto it = sparse.find(id);
    return it == sparse.end() ? nullptr : it->second.first;
  }
  /* The layer's group (null at the top, or for a layer not in the document). */
  const Layer *parent(uint64_t id) const
  {
    if (id < parents.size()) {
      return parents[size_t(id)];
    }
    const auto it = sparse.find(id);
    return it == sparse.end() ? nullptr : it->second.second;
  }
  void clear()
  {
    for (const uint64_t id : used) {
      layers[size_t(id)] = nullptr;
      parents[size_t(id)] = nullptr;
    }
    used.clear();
    sparse.clear();
    any_boolean = false;
    doc = nullptr;
  }
};
static thread_local LayerIndex g_layer_index_storage;
static thread_local LayerIndex *g_layer_index = nullptr;

static void index_layers(const std::vector<Layer> &list, const Layer *parent, LayerIndex &index)
{
  for (const Layer &layer : list) {
    index.add(layer, parent);
    for (const Modifier &m : layer.modifiers) {
      index.any_boolean = index.any_boolean || m.type == ModifierType::Boolean;
    }
    index_layers(layer.children, &layer, index);
  }
}

static LayerIndex *index_for(const Document &doc)
{
  return g_layer_index && g_layer_index->doc == &doc ? g_layer_index : nullptr;
}

static uint64_t g_revision_next = 1;

uint64_t revision_next()
{
  return g_revision_next++;
}

uint64_t revision_latest()
{
  return g_revision_next - 1;
}

void layer_touch(Layer &layer)
{
  layer.revision.value = revision_next();
}

void document_touch_all(Document &doc)
{
  const auto touch = [](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      layer_touch(layer);
      self(self, layer.children);
    }
  };
  touch(touch, doc.layers);
  layer_touch(doc.simulation);
  layer_touch(doc.camera);
  doc.tree_revision.value = revision_next();
}

bool layer_index_active(const Document &doc)
{
  return index_for(doc) != nullptr;
}

bool layer_index_no_booleans(const Document &doc)
{
  const LayerIndex *index = index_for(doc);
  return index && !index->any_boolean;
}

LayerIndexScope::LayerIndexScope(const Document &doc)
{
  if (g_layer_index) {
    return;
  }
  g_layer_index = &g_layer_index_storage;
  g_layer_index->doc = &doc;
  index_layers(doc.layers, nullptr, *g_layer_index);
  owner_ = true;
}

LayerIndexScope::~LayerIndexScope()
{
  if (owner_) {
    g_layer_index->clear();
    g_layer_index = nullptr;
  }
}

static const Layer *indexed(const Document &doc, uint64_t id, bool *r_used)
{
  *r_used = g_layer_index && g_layer_index->doc == &doc;
  if (!*r_used) {
    return nullptr;
  }
  return g_layer_index->find(id);
}

Layer *layer_find(Document &doc, uint64_t id)
{
  if (id == SIMULATION_LAYER_ID) {
    return &doc.simulation;
  }
  if (id == CAMERA_LAYER_ID) {
    return &document_camera(doc);
  }
  if (!id) {
    return nullptr;
  }
  bool used = false;
  if (const Layer *found = indexed(doc, id, &used); used) {
    return const_cast<Layer *>(found);
  }
  return find_in<Layer>(doc.layers, id);
}

const Layer *layer_find(const Document &doc, uint64_t id)
{
  if (id == SIMULATION_LAYER_ID) {
    return &doc.simulation;
  }
  if (id == CAMERA_LAYER_ID) {
    return &doc.camera;
  }
  if (!id) {
    return nullptr;
  }
  bool used = false;
  if (const Layer *found = indexed(doc, id, &used); used) {
    return found;
  }
  return find_in<const Layer>(doc.layers, id);
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
  if (const LayerIndex *index = index_for(doc)) {
    return const_cast<Layer *>(index->parent(id));
  }
  return parent_in(doc.layers, nullptr, id);
}

Layer *document_active_layer(Document &doc)
{
  return layer_find(doc, doc.active_layer);
}

/* `armature` is the nearest bone layer above `list` (with its world transform), for layers
 * bound to one of its bones: such a layer moves with the bone's change from the setup pose. */
static bool layers_hold(const std::vector<Layer> &list, uint64_t id)
{
  for (const Layer &layer : list) {
    if (layer.id == id || layers_hold(layer.children, id)) {
      return true;
    }
  }
  return false;
}

static bool world_transform_in(const std::vector<Layer> &list, const Transform2D &parent, const Layer *armature,
                               const Transform2D &armature_world, bool setup_frame, uint64_t id,
                               Transform2D *r_transform, bool bound = false)
{
  for (const Layer &layer : list) {
    /* Only the way down to the layer: the bones' pose is costly to work out. */
    if (layer.id != id && !layers_hold(layer.children, id)) {
      continue;
    }
    Transform2D transform = parent * layer.local_transform();
    /* `bound`: a group above already follows a bone, and its bone overrides this layer's. */
    const bool binds = armature && !bound && layer.followed_bone() != 0;
    if (binds) {
      const std::vector<BonePose> poses = armature_pose(*armature, setup_frame);
      if (const BonePose *bone = bone_pose_find(poses, layer.followed_bone())) {
        transform = armature_world * bone->skin * armature_world.inverted() *
                    transform;
      }
    }
    if (layer.id == id) {
      *r_transform = transform;
      return true;
    }
    const bool bones = layer.is_bone_layer();
    if (world_transform_in(layer.children, transform, bones ? &layer : armature, bones ? transform : armature_world,
                           setup_frame, id, r_transform, !bones && (bound || (binds && layer.is_group()))))
    {
      return true;
    }
  }
  return false;
}

/* world_transform_in by the index: down the layer's own groups, as found from it upward. */
static bool world_transform_indexed(const LayerIndex &index, bool setup_frame, uint64_t id, Transform2D *r_transform)
{
  std::vector<const Layer *> chain;
  for (const Layer *l = index.find(id); l; l = index.parent(l->id)) {
    chain.push_back(l);
  }
  if (chain.empty()) {
    return false;
  }
  Transform2D parent, armature_world;
  const Layer *armature = nullptr;
  bool bound = false;
  for (auto it = chain.rbegin(); it != chain.rend(); ++it) {
    const Layer &layer = **it;
    Transform2D transform = parent * layer.local_transform();
    const bool binds = armature && !bound && layer.followed_bone() != 0;
    if (binds) {
      const std::vector<BonePose> poses = armature_pose(*armature, setup_frame);
      if (const BonePose *bone = bone_pose_find(poses, layer.followed_bone())) {
        transform = armature_world * bone->skin * armature_world.inverted() * transform;
      }
    }
    if (layer.id == id) {
      *r_transform = transform;
      return true;
    }
    const bool bones = layer.is_bone_layer();
    bound = !bones && (bound || (binds && layer.is_group()));
    if (bones) {
      armature = &layer;
      armature_world = transform;
    }
    parent = transform;
  }
  return false;
}

/* The layer's groups and itself, the top first (empty when there's no such layer). */
static std::vector<const Layer *> layer_chain(const Document &doc, uint64_t id)
{
  std::vector<const Layer *> chain;
  for (const Layer *layer = layer_find(doc, id); layer;
       layer = layer_parent(const_cast<Document &>(doc), layer->id))
  {
    chain.push_back(layer);
  }
  std::reverse(chain.begin(), chain.end());
  return chain;
}

/* The end of `chain`'s transform in chain[from]'s space (`from` -1: the document's), as world_transform_indexed works
 * it out: the bone binding below as it would be from the top (which bone layer is the armature, whether a group above
 * already binds), with the armature's transform, where it is above chain[from], as the way from it down to there. */
static Transform2D chain_transform_below(const std::vector<const Layer *> &chain, int from, bool setup_frame)
{
  const Layer *armature = nullptr;
  bool bound = false;
  /* From below the armature down to chain[from]; no group between binds while a layer below still can. */
  Transform2D since_armature;
  for (int k = 0; k <= from; k++) {
    const Layer &layer = *chain[size_t(k)];
    const bool binds = armature && !bound && layer.followed_bone() != 0;
    since_armature = since_armature * layer.local_transform();
    const bool bones = layer.is_bone_layer();
    bound = !bones && (bound || (binds && layer.is_group()));
    if (bones) {
      armature = &layer;
      since_armature = Transform2D();
    }
  }
  Transform2D parent, armature_world = since_armature.inverted();
  for (size_t k = size_t(from + 1); k < chain.size(); k++) {
    const Layer &layer = *chain[k];
    Transform2D transform = parent * layer.local_transform();
    const bool binds = armature && !bound && layer.followed_bone() != 0;
    if (binds) {
      const std::vector<BonePose> poses = armature_pose(*armature, setup_frame);
      if (const BonePose *bone = bone_pose_find(poses, layer.followed_bone())) {
        transform = armature_world * bone->skin * armature_world.inverted() * transform;
      }
    }
    const bool bones = layer.is_bone_layer();
    bound = !bones && (bound || (binds && layer.is_group()));
    if (bones) {
      armature = &layer;
      armature_world = transform;
    }
    parent = transform;
  }
  return parent;
}

static Transform2D relative_transform(const Document &doc, uint64_t from, uint64_t to, bool from_setup, bool to_setup)
{
  const std::vector<const Layer *> a = layer_chain(doc, from), b = layer_chain(doc, to);
  int shared = -1;
  while (size_t(shared + 1) < a.size() && size_t(shared + 1) < b.size() && a[size_t(shared + 1)] == b[size_t(shared + 1)])
  {
    shared++;
  }
  return chain_transform_below(a, shared, from_setup).inverted() * chain_transform_below(b, shared, to_setup);
}

Transform2D layer_relative_transform(const Document &doc, uint64_t from, uint64_t to)
{
  return relative_transform(doc, from, to, doc.frame_current == 0, doc.frame_current == 0);
}

Transform2D layer_setup_relative_transform(const Document &doc, uint64_t from, uint64_t to)
{
  return relative_transform(doc, from, to, true, true);
}

Transform2D layer_relative_to_setup(const Document &doc, uint64_t from, uint64_t to)
{
  return relative_transform(doc, from, to, true, doc.frame_current == 0);
}

Transform2D layer_setup_world_transform(const Document &doc, uint64_t id)
{
  Transform2D transform;
  if (const LayerIndex *index = index_for(doc)) {
    world_transform_indexed(*index, true, id, &transform);
    return transform;
  }
  world_transform_in(doc.layers, Transform2D(), nullptr, Transform2D(), true, id, &transform);
  return transform;
}

Transform2D layer_world_transform(const Document &doc, uint64_t id)
{
  Transform2D transform;
  if (const LayerIndex *index = index_for(doc)) {
    world_transform_indexed(*index, doc.frame_current == 0, id, &transform);
    return transform;
  }
  world_transform_in(doc.layers, Transform2D(), nullptr, Transform2D(), doc.frame_current == 0, id, &transform);
  return transform;
}

const VectorDrawing &layer_drawing(const Document &doc, const Layer &layer)
{
  /* A reference's drawing is built from its original's as the document is evaluated (see
   * anim/reference.hh); until it first is, it shows the original's. */
  if (layer.is_reference() && !layer.reference.built) {
    if (const Layer *source = layer_find(doc, layer.reference_source)) {
      return source->drawing;
    }
  }
  return layer.drawing;
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

const Layer *layer_clip_base(const Document &doc, const Layer &layer)
{
  int index = 0;
  const std::vector<Layer> *container = layer_container(const_cast<Document &>(doc), layer.id, &index);
  if (!container) {
    return nullptr;
  }
  for (size_t i = size_t(index) + 1; i < container->size(); i++) {
    if (!(*container)[i].clip_to_below) {
      return &(*container)[i];
    }
  }
  return nullptr;
}

/* Whether `a` has a Boolean modifier (on in the viewport or renders) taking in shapes of `b`. */
static bool merges(const Layer &a, uint64_t b)
{
  for (const Modifier &m : a.modifiers) {
    if (m.type == ModifierType::Boolean && (m.show_viewport || m.show_render) &&
        std::any_of(m.merge_targets.begin(), m.merge_targets.end(), [&](const MergeTarget &t) { return t.layer == b; }))
    {
      return true;
    }
  }
  return false;
}

std::vector<LayerMask> layer_mask_cards(const Document &doc, const Layer &layer)
{
  std::vector<LayerMask> cards;
  cards.reserve(layer.masks.size());
  for (const LayerMask &card : layer.masks) {
    cards.push_back(mask_card_flatten(card));
  }
  /* Two layers merged into one outline don't mask each other (a Join Lines pair, say): the outline
   * holds both, so it would cut the whole of either away. Steps left naming nothing go, and a card
   * left with no steps. */
  bool pruned = false;
  for (LayerMask &card : cards) {
    for (MaskStep &step : card.steps) {
      const auto prune = [&](MaskStep &s) {
        const size_t before = s.layers.size();
        std::erase_if(s.layers, [&](uint64_t id) {
          const Layer *other = layer_find(doc, id);
          return other && (merges(*other, layer.id) || merges(layer, id));
        });
        pruned |= s.layers.size() != before;
      };
      prune(step);
      for (MaskStep &child : step.children) {
        prune(child);
      }
      std::erase_if(step.children, mask_step_empty);
    }
    if (pruned) {
      std::erase_if(card.steps, mask_step_empty);
    }
  }
  if (pruned) {
    std::erase_if(cards, [](const LayerMask &card) { return card.steps.empty(); });
  }
  if (layer.clip_to_below) {
    if (const Layer *base = layer_clip_base(doc, layer)) {
      /* Hidden outside the base. */
      MaskStep step;
      step.mode = MaskMode::Subtract;
      step.invert = true;
      step.layers = {base->id};
      step.clip = true;
      step.fills_only = layer.clip_exclude_strokes;
      LayerMask clip;
      clip.start_shown = true;
      clip.clip = true;
      clip.steps.push_back(step);
      cards.push_back(clip);
    }
  }
  return cards;
}

void document_set_active_shape(Document &doc, const Layer &layer, int point, ShapeEdge edge)
{
  const VectorDrawing &drawing = layer.drawing;
  for (int s = int(drawing.shapes.size()) - 1; s >= 0; s--) {
    const Shape &shape = drawing.shapes[size_t(s)];
    for (const ShapeEdge &e : shape.edges) {
      bool hit = false;
      if (edge.curve >= 0) {
        hit = e == edge;
      }
      else if (e.curve < int(drawing.curves.size())) {
        int a, b;
        curve_segment_points(drawing.curves[size_t(e.curve)], e.segment, &a, &b);
        hit = a == point || b == point;
      }
      if (hit) {
        doc.active_shape_layer = layer.id;
        doc.active_shape = shape.id;
        return;
      }
    }
  }
}

Layer &document_add_layer(Document &doc, LayerType type, const std::string &name)
{
  Layer layer;
  layer.id = doc.next_layer_id++;
  layer.type = type;
  layer.edit_descendants = type != LayerType::Bone;
  layer.name = name.empty() ? document_layer_unique_name(doc, type == LayerType::Bone     ? "Bones"
                                                              : type == LayerType::Group  ? "Group"
                                                              : type == LayerType::Switch ? "Switch"
                                                                                          : "Layer")
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

/* The step's layers and its group's: a global area they change is its own from then on. */
template<typename F> static void mask_step_map_layers(MaskStep &step, const F &change)
{
  const auto walk = [&](auto &&self, MaskStep &s, bool &changed) -> void {
    for (uint64_t &id : s.layers) {
      const uint64_t was = id;
      change(id);
      changed = changed || id != was;
    }
    for (MaskStep &child : s.children) {
      self(self, child, changed);
    }
  };
  bool changed = false;
  walk(walk, step, changed);
  if (changed) {
    step.global = 0;
  }
}

void layer_links_map(Layer &layer, const std::function<uint64_t(uint64_t)> &map)
{
  const auto link = [&](uint64_t &id) {
    if (id != 0) {
      id = map(id);
    }
  };
  link(layer.reference_source);
  for (LayerMask &m : layer.masks) {
    for (MaskStep &s : m.steps) {
      mask_step_map_layers(s, link);
    }
  }
  /* Draw Order holds a layer's id, minus it for Behind. */
  const auto order = [&](float &o) {
    if (o != 0.0f) {
      const uint64_t id = map(uint64_t(std::fabs(o)));
      o = o < 0.0f ? -float(id) : float(id);
    }
  };
  order(layer.draw_order);
  for (auto &k : layer.draw_order_keys.keys) {
    order(k.value);
  }
  for (Bone &b : layer.bones) {
    link(b.shape_layer);
    link(b.swap_layer);
    link(b.outer_layer);
    for (BoneConstraint &c : b.constraints) {
      link(c.shape_layer);
      link(c.fit_layer);
      link(c.follow_layer);
    }
  }
  for (Modifier &mod : layer.modifiers) {
    link(mod.deform_layer);
    for (MergeTarget &t : mod.merge_targets) {
      link(t.layer);
    }
  }
}

static void remap_references(Layer &layer, const std::map<uint64_t, uint64_t> &remap)
{
  /* Links inside a copied group (references, masks, Draw Order, bone shapes, constraints' and modifiers'
   * layers) point at the copies of what they name when that was copied too, so a duplicated rig stays
   * self-contained; and a switch's keys its copied children. */
  layer_links_map(layer, [&](uint64_t id) {
    const auto it = remap.find(id);
    return it != remap.end() ? it->second : id;
  });
  switch_remap(layer, remap);
  for (Layer &child : layer.children) {
    remap_references(child, remap);
  }
}

Layer layer_copy_with_new_ids(Document &doc, const Layer &layer, std::map<uint64_t, uint64_t> *r_remap)
{
  Layer copy = layer;
  std::map<uint64_t, uint64_t> remap;
  assign_new_ids(doc, copy, remap);
  remap_references(copy, remap);
  if (r_remap) {
    r_remap->insert(remap.begin(), remap.end());
  }
  return copy;
}

void layer_masks_remap(Layer &layer, const std::map<uint64_t, uint64_t> &remap)
{
  for (LayerMask &m : layer.masks) {
    for (MaskStep &s : m.steps) {
      mask_step_map_layers(s, [&](uint64_t &id) {
        if (auto it = remap.find(id); it != remap.end()) {
          id = it->second;
        }
      });
    }
  }
}

/* The ids a reference tree took for its sources' layers. */
static void reference_ids(const Layer &source, const Layer &ref, std::map<uint64_t, uint64_t> &r_map)
{
  r_map[source.id] = ref.id;
  for (size_t i = 0; i < source.children.size() && i < ref.children.size(); i++) {
    reference_ids(source.children[i], ref.children[i], r_map);
  }
}

static void remap_masks_tree(Layer &layer, const std::map<uint64_t, uint64_t> &remap)
{
  layer_masks_remap(layer, remap);
  for (Layer &child : layer.children) {
    remap_masks_tree(child, remap);
  }
}

static Layer reference_of(Document &doc, const Layer &source);

Layer layer_make_reference(Document &doc, const Layer &source)
{
  Layer ref = reference_of(doc, source);
  /* Masks inside a referenced group name the copies, so the copy masks itself as the original does. */
  std::map<uint64_t, uint64_t> remap;
  reference_ids(source, ref, remap);
  remap_masks_tree(ref, remap);
  return ref;
}

static Layer reference_of(Document &doc, const Layer &source)
{
  Layer ref;
  ref.id = doc.next_layer_id++;
  ref.name = source.name;
  ref.type = source.type;
  /* A reference of a reference follows the original: references never chain. */
  ref.reference_source = source.is_reference() ? source.reference_source : source.id;
  ref.position = source.position;
  ref.rotation = source.rotation;
  ref.scale = source.scale;
  ref.opacity = source.opacity;
  ref.masks = source.masks;
  for (const Layer &child : source.children) {
    ref.children.push_back(reference_of(doc, child));
  }
  return ref;
}

static void unlink_references_to(Document &doc, std::vector<Layer> &list, const Layer &removed)
{
  for (Layer &layer : list) {
    if (layer.reference_source == removed.id) {
      /* It keeps what it showed, as an ordinary layer. */
      layer.reference_source = 0;
      layer.reference = ReferenceSettings();
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
  /* A linked switches' lead inside it hands the group's keys on first. */
  switch_links_before_remove(doc, id);
  container = layer_container(doc, id, &index);
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
  if (new_parent) {
    switch_child_added(*layer_find(doc, new_parent), id);
  }
  return true;
}

void layer_break_reference(Document &doc, uint64_t id)
{
  Layer *layer = layer_find(doc, id);
  if (!layer || !layer->is_reference()) {
    return;
  }
  layer->reference_source = 0;
  layer->reference = ReferenceSettings();
}

}  // namespace oanim
