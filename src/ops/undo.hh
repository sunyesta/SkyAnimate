#pragma once

/* Undo: a list of document states, one per finished undoable operator.
 *
 * Each step stores the state *after* its operator ran, as Blender's undo system does;
 * undoing restores the step before the active one. Whole-document copies are fine
 * while the document is small; milestone 2 switches to copy-on-write per layer. */

#include <string>
#include <vector>

#include "data/document.hh"

namespace sky {

class UndoStack {
 public:
  /* Clears the history and records the starting state. */
  void reset(const Document &doc);
  void push(const std::string &name, const Document &doc);

  bool can_undo() const { return active_ > 0; }
  bool can_redo() const { return active_ + 1 < steps_.size(); }
  bool undo(Document &doc);
  bool redo(Document &doc);

  size_t size() const { return steps_.size(); }
  size_t active() const { return active_; }
  const std::string &step_name(size_t index) const { return steps_[index].name; }

 private:
  struct Step {
    std::string name;
    Document state;
  };
  static constexpr size_t STEPS_MAX = 64;

  std::vector<Step> steps_;
  size_t active_ = 0;
};

}  // namespace sky
