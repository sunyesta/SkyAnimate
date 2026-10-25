#pragma once

/* Undo: a list of document states, one per finished undoable operator.
 *
 * Each step stores the state *after* its operator ran, as Blender's undo system does;
 * undoing restores the step before the active one. A step holds the document's settings and
 * its layers as shared, read-only copies (each without its children, the tree kept beside
 * them): a layer the same as in the step before is that step's copy, so a step costs only
 * what changed, as Blender's memfile undo keeps only changed chunks. */

#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "data/document.hh"

namespace oanim {

class UndoStack {
 public:
  /* Clears the history and records the starting state. */
  void reset(const Document &doc);
  /* Records the document's edits as keys first (anim/animation.hh), so each step holds them. */
  void push(const std::string &name, Document &doc);
  /* Runs on the document just before each push; `previous` gives the state of the step before, made
   * only when asked for (the app sets it: X mirror editing carries shape looks over to their mirror
   * shapes). */
  void (*before_push)(Document &doc, const std::function<const Document &()> &previous, void *data) = nullptr;
  void *before_push_data = nullptr;

  bool can_undo() const { return active_ > 0; }
  bool can_redo() const { return active_ + 1 < steps_.size(); }
  bool undo(Document &doc);
  bool redo(Document &doc);

  size_t size() const { return steps_.size(); }
  size_t active() const { return active_; }
  const std::string &step_name(size_t index) const { return steps_[index].name; }
  /* Grows with every push, undo, redo and reset, so a saved count tells whether the
   * document changed since (the unsaved-changes mark and autosave use it). */
  uint64_t change_count() const { return changes_; }
  /* A change that has no undo step of its own (typing in a text, a script setting a
   * property) still counts as unsaved. */
  void mark_changed()
  {
    changes_++;
    unknown_changes_++;
  }
  /* Grows with changes not made through a push (mark_changed, reset): what changed isn't known. */
  uint64_t unknown_changes() const { return unknown_changes_; }

 private:
  /* A layer as stored (no children) and the stored layers inside it. */
  struct Node {
    std::shared_ptr<const Layer> layer;
    std::vector<Node> children;
  };
  struct Step {
    std::string name;
    /* Everything but the layers (they are left out of it). */
    std::shared_ptr<const Document> settings;
    std::vector<Node> layers;
    /* The layer tree's shape (ids in order, nesting): when it differs from the step before, layers came, went
     * or moved. */
    uint64_t tree_shape = 0;
  };
  static constexpr size_t STEPS_MAX = 64;

  /* `doc` as a step, sharing what is the same as in `previous` (may be null). */
  static Step store(const std::string &name, Document &doc, const Step *previous);
  /* The document a step holds. */
  static Document rebuild(const Step &step);

  /* A deque: growing never moves the steps. */
  std::deque<Step> steps_;
  size_t active_ = 0;
  uint64_t changes_ = 0;
  uint64_t unknown_changes_ = 0;
};

}  // namespace oanim
