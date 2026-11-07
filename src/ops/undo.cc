#include "ops/undo.hh"

#include <thread>
#include <unordered_map>
#include <vector>

#include "anim/animation.hh"
#include "core/parallel.hh"
#include "assets/brush_presets.hh"
#include "text/text_layer.hh"
#include "core/profile.hh"

namespace oanim {

/* Freeing a big document takes as long as copying it: what is let go of is freed on another thread. */
template<typename T> static void free_later(std::vector<T> &&gone)
{
  if (!gone.empty()) {
    std::thread([gone = std::move(gone)]() {}).detach();
  }
}

UndoStack::Step UndoStack::store(const std::string &name, Document &doc, const Step *previous)
{
  Step step;
  step.name = name;
  /* The previous step's layers by id, to share those that are the same. */
  std::unordered_map<uint64_t, std::shared_ptr<const Layer>> before;
  if (previous) {
    const auto index = [&](auto &&self, const std::vector<Node> &nodes) -> void {
      for (const Node &node : nodes) {
        before.emplace(node.layer->id, node.layer);
        self(self, node.children);
      }
    };
    index(index, previous->layers);
  }
  /* Which layers are as the previous step has them: each compared without its children (they are stored as nodes
   * of their own, taken out for the while and put back), every layer on its own, over the CPU's cores (a big
   * document's comparing is most of a step's time). */
  std::vector<Layer *> flat;
  const auto gather = [&](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      flat.push_back(&layer);
      self(self, layer.children);
    }
  };
  gather(gather, doc.layers);
  std::vector<std::vector<Layer>> kids(flat.size());
  for (size_t i = 0; i < flat.size(); i++) {
    std::swap(kids[i], flat[i]->children);
  }
  std::vector<char> unchanged(flat.size(), 0);
  parallel_for(flat.size(), 8, [&](size_t begin, size_t end) {
    for (size_t i = begin; i < end; i++) {
      const auto it = before.find(flat[i]->id);
      unchanged[i] = it != before.end() && *it->second == *flat[i];
    }
  });
  for (size_t i = flat.size(); i-- > 0;) {
    std::swap(kids[i], flat[i]->children);
  }
  std::unordered_map<const Layer *, bool> same;
  same.reserve(flat.size());
  for (size_t i = 0; i < flat.size(); i++) {
    same.emplace(flat[i], unchanged[i] != 0);
  }
  /* Each layer copied without its children (stored as nodes of their own): taken out for the while and put back. */
  const auto take = [&](auto &&self, std::vector<Layer> &layers) -> std::vector<Node> {
    std::vector<Node> nodes;
    nodes.reserve(layers.size());
    for (Layer &layer : layers) {
      Node node;
      std::vector<Layer> children;
      std::swap(children, layer.children);
      const auto it = before.find(layer.id);
      if (same.at(&layer)) {
        node.layer = it->second;
      }
      else {
        /* Changed (or new): what was drawn of it is out of date. */
        layer_touch(layer);
        node.layer = std::make_shared<const Layer>(layer);
      }
      step.tree_shape = (step.tree_shape ^ layer.id) * 1099511628211ull;
      std::swap(children, layer.children);
      step.tree_shape = (step.tree_shape ^ 0x5bd1e995u) * 1099511628211ull;
      node.children = self(self, layer.children);
      step.tree_shape = (step.tree_shape ^ 0x27d4eb2fu) * 1099511628211ull;
      nodes.push_back(std::move(node));
    }
    return nodes;
  };
  step.layers = take(take, doc.layers);
  if (!previous || previous->tree_shape != step.tree_shape) {
    doc.tree_revision.value = revision_next();
  }
  /* The settings, without the layers; the previous step's when nothing in them changed. */
  std::vector<Layer> layers;
  std::swap(layers, doc.layers);
  if (previous && *previous->settings == doc) {
    step.settings = previous->settings;
  }
  else {
    step.settings = std::make_shared<const Document>(doc);
  }
  std::swap(layers, doc.layers);
  return step;
}

Document UndoStack::rebuild(const Step &step)
{
  Document doc = *step.settings;
  const auto put = [&](auto &&self, const std::vector<Node> &nodes) -> std::vector<Layer> {
    std::vector<Layer> layers;
    layers.reserve(nodes.size());
    for (const Node &node : nodes) {
      layers.push_back(*node.layer);
      layers.back().children = self(self, node.children);
    }
    return layers;
  };
  doc.layers = put(put, step.layers);
  return doc;
}

void UndoStack::reset(const Document &doc)
{
  std::vector<Step> gone(std::make_move_iterator(steps_.begin()), std::make_move_iterator(steps_.end()));
  free_later(std::move(gone));
  steps_.clear();
  unknown_changes_++;
  Document copy = doc;
  steps_.push_back(store("Original", copy, nullptr));
  active_ = 0;
  changes_++;
}

/* Whether `doc` is `before` but for what undo leaves as it is (the view, the frame shown, the timeline's switches: see
 * view_state_restore in builtin_ops.cc), which `before`, a copy, takes from it. */
static bool unchanged_since(Document &before, const Document &doc)
{
  before.frame_current = doc.frame_current;
  before.texts = doc.texts;
  before.overlays = doc.overlays;
  before.ui_open = doc.ui_open;
  before.view = doc.view;
  before.motion_paths = doc.motion_paths;
  before.auto_key = doc.auto_key;
  before.auto_key_mode = doc.auto_key_mode;
  before.time_together = doc.time_together;
  return before == doc;
}

void UndoStack::push(const std::string &name, Document &doc)
{
  OANIM_PROFILE("undo_push");
  /* A text changed has its letters made again, before the step holds them. */
  document_sync_text(doc);
  /* A linked shape's look changed changes its material, and the step holds every shape using it. */
  document_sync_materials(doc);
  std::unique_ptr<Document> previous;
  const auto was = [&]() -> const Document & {
    if (!previous) {
      previous = std::make_unique<Document>(rebuild(steps_[active_]));
    }
    return *previous;
  };
  if (before_push && !steps_.empty()) {
    before_push(doc, was, before_push_data);
  }
  animation_record(doc);
  fix_actions_tidy(doc);
  if (steps_.empty()) {
    reset(doc);
  }
  else if (was(); unchanged_since(*previous, doc)) {
    /* Nothing changed (a move by nothing, a key type it already has): no step, as there's nothing to undo, and what
     * can be redone stays. */
    return;
  }
  /* A new action discards everything that was undone. */
  std::vector<Step> gone;
  while (steps_.size() > active_ + 1) {
    gone.push_back(std::move(steps_.back()));
    steps_.pop_back();
  }
  steps_.push_back(store(name, doc, &steps_.back()));
  if (steps_.size() > STEPS_MAX) {
    gone.push_back(std::move(steps_.front()));
    steps_.pop_front();
  }
  free_later(std::move(gone));
  active_ = steps_.size() - 1;
  changes_++;
}

bool UndoStack::undo(Document &doc)
{
  if (!can_undo()) {
    return false;
  }
  active_--;
  std::vector<Document> gone;
  gone.push_back(std::move(doc));
  free_later(std::move(gone));
  doc = rebuild(steps_[active_]);
  /* What it was drawn as may be from any time: everything is drawn anew. */
  document_touch_all(doc);
  brush_presets_register_shapes(doc);
  changes_++;
  return true;
}

bool UndoStack::redo(Document &doc)
{
  if (!can_redo()) {
    return false;
  }
  active_++;
  std::vector<Document> gone;
  gone.push_back(std::move(doc));
  free_later(std::move(gone));
  doc = rebuild(steps_[active_]);
  /* What it was drawn as may be from any time: everything is drawn anew. */
  document_touch_all(doc);
  brush_presets_register_shapes(doc);
  changes_++;
  return true;
}

}  // namespace oanim
