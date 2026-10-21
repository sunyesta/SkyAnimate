#include "ops/undo.hh"

#include "anim/animation.hh"

namespace oanim {

void UndoStack::reset(const Document &doc)
{
  steps_.clear();
  steps_.push_back({"Original", doc});
  active_ = 0;
  changes_++;
}

void UndoStack::push(const std::string &name, Document &doc)
{
  /* A linked shape's look changed changes its material, and the step holds every shape using it. */
  document_sync_materials(doc);
  animation_record(doc);
  if (steps_.empty()) {
    reset(doc);
  }
  /* A new action discards everything that was undone. */
  steps_.resize(active_ + 1);
  steps_.push_back({name, doc});
  if (steps_.size() > STEPS_MAX) {
    steps_.erase(steps_.begin());
  }
  active_ = steps_.size() - 1;
  changes_++;
}

bool UndoStack::undo(Document &doc)
{
  if (!can_undo()) {
    return false;
  }
  active_--;
  doc = steps_[active_].state;
  changes_++;
  return true;
}

bool UndoStack::redo(Document &doc)
{
  if (!can_redo()) {
    return false;
  }
  active_++;
  doc = steps_[active_].state;
  changes_++;
  return true;
}

}  // namespace oanim
