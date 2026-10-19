/* Keyframe operators: Blender's I and Alt I, and jumping between keys. */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

#include "anim/animation.hh"
#include "core/log.hh"
#include "data/bones.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace sky {

std::vector<Layer *> keyed_layers(Context &C)
{
  std::vector<Layer *> layers;
  if (!C.document) {
    return layers;
  }
  Document &doc = *C.document;
  for (const Layer *layer : document_layers_flat(doc)) {
    if ((layer->id == doc.active_layer || layer->selected) && !layer->locked) {
      layers.push_back(layer_find(doc, layer->id));
    }
  }
  return layers;
}

/* The layers an operator's `layers` argument names (comma-separated ids) among those being
 * animated; all of them when it is empty. */
static std::vector<Layer *> op_layers(Context &C)
{
  std::vector<Layer *> all = timeline_layers(C);
  const std::string list = op_arg(C, "layers");
  if (list.empty()) {
    return all;
  }
  std::set<uint64_t> ids;
  size_t at = 0;
  while (at <= list.size()) {
    const size_t comma = std::min(list.find(',', at), list.size());
    const std::string part = list.substr(at, comma - at);
    if (!part.empty()) {
      ids.insert(std::strtoull(part.c_str(), nullptr, 10));
    }
    at = comma + 1;
  }
  std::erase_if(all, [&](const Layer *l) { return !ids.count(l->id); });
  return all;
}

std::vector<Layer *> timeline_layers(Context &C)
{
  const std::vector<Layer *> picked = keyed_layers(C);
  std::vector<Layer *> layers;
  const auto add = [&](Layer *l) {
    if (std::find(layers.begin(), layers.end(), l) == layers.end()) {
      layers.push_back(l);
    }
  };
  /* A selected bone layer brings the animated layers inside it, listed right after it, as
   * Blender's summary shows an armature's children. */
  const auto add_inside = [&](auto &&self, Layer &group) -> void {
    for (Layer &child : group.children) {
      if (!child.locked && !layer_key_frames(child).empty()) {
        add(&child);
      }
      self(self, child);
    }
  };
  for (Layer *l : picked) {
    add(l);
    if (l->is_bone_layer()) {
      add_inside(add_inside, *l);
    }
  }
  return layers;
}

static void anim_operator_add(const char *idname, const char *name, const char *description, unsigned flag,
                              std::function<OpStatus(Context &C)> exec)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = flag;
  ot.poll = [](Context &C) { return !keyed_layers(C).empty(); };
  ot.exec = std::move(exec);
  operators().add(std::move(ot));
}

/* The next (dir > 0) or previous key frame of the layers being animated, or -1. */
static int key_frame_beside(Context &C, int dir)
{
  std::set<int> frames;
  for (Layer *layer : timeline_layers(C)) {
    for (const int f : layer_key_frames(*layer)) {
      frames.insert(f);
    }
  }
  const int now = C.document->frame_current;
  if (dir > 0) {
    const auto it = frames.upper_bound(now);
    return it == frames.end() ? -1 : *it;
  }
  const auto it = frames.lower_bound(now);
  return it == frames.begin() ? -1 : *std::prev(it);
}

void anim_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "anim.action_new";
    ot.name = "New Action";
    ot.description = "Add an action named `name` to the bone layer the active layer is in (or is), and start "
                     "editing it: a separate animation for that bone layer and everything inside it";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document && action_owner(*C.document, C.document->active_layer); };
    ot.exec = [](Context &C) {
      Layer *owner = action_owner(*C.document, C.document->active_layer);
      const std::string name = document_action_new(*C.document, owner->id, op_arg(C, "name", "Action"));
      return name.empty() ? OpStatus::Cancelled : OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.action_set";
    ot.name = "Edit Action";
    ot.description = "Edit action `name` in the timeline (\"\" for the mainline)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      const std::string name = op_arg(C, "name");
      if (name == C.document->action) {
        return OpStatus::Cancelled;
      }
      if (!document_set_action_by_name(*C.document, name)) {
        report(ReportType::Warning, "No action named " + name);
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  anim_operator_add("anim.keyframe_insert", "Insert Keyframe",
                    "Key the active and selected layers at the current frame: transform, opacity and every point",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      /* Edits not yet recorded belong to the frame they were made at. */
                      animation_record(*C.document);
                      int keys = 0;
                      for (Layer *layer : keyed_layers(C)) {
                        keys += layer_insert_keys(*layer, C.document->frame_current);
                      }
                      return keys > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                    });
  anim_operator_add("anim.keyframe_delete", "Delete Keyframe",
                    "Remove the active and selected layers' keys at the current frame",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      animation_record(*C.document);
                      int removed = 0;
                      for (Layer *layer : keyed_layers(C)) {
                        removed += layer_delete_keys(*layer, C.document->frame_current);
                      }
                      if (removed == 0) {
                        report(ReportType::Info, "No keys on this frame");
                        return OpStatus::Cancelled;
                      }
                      /* Without those keys the frame shows the values around it. */
                      animation_evaluate(*C.document, C.document->frame_current);
                      return OpStatus::Finished;
                    });

  /* --- Keys in the timeline ------------------------------------------------------------ */
  anim_operator_add("anim.keys_select", "Select Keys",
                    "Select the keys on frames frame_start..frame_end (extend keeps the others; deselect takes "
                    "them out). layers: only these layers' keys (comma-separated ids, a timeline channel row)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      const std::vector<Layer *> layers = op_layers(C);
                      const int first = op_arg_int(C, "frame_start", 0);
                      const int last = op_arg_int(C, "frame_end", first);
                      const bool deselect = op_arg_bool(C, "deselect");
                      if (!op_arg_bool(C, "extend") && !deselect) {
                        layers_deselect_keys(timeline_layers(C));
                      }
                      layers_select_keys(layers, std::min(first, last), std::max(first, last), !deselect);
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_select_all", "Select All Keys", "Select every key (action=DESELECT: none)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      const std::vector<Layer *> layers = timeline_layers(C);
                      if (op_arg(C, "action", "SELECT") == "DESELECT") {
                        layers_deselect_keys(layers);
                      }
                      else {
                        layers_select_keys(layers, 0, 1 << 30, true);
                      }
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_deselect_all", "Deselect All Keys", "Deselect every key (Alt A)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      layers_deselect_keys(timeline_layers(C));
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_offset", "Move Keys", "Move the selected keys by `frames` (G in the timeline)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      const int frames = op_arg_int(C, "frames", 0);
                      if (frames == 0) {
                        return OpStatus::Cancelled;
                      }
                      animation_record(*C.document);
                      layers_move_selected_keys(timeline_layers(C), [frames](int f) { return f + frames; });
                      animation_evaluate(*C.document, C.document->frame_current);
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_scale", "Scale Keys",
                    "Scale the spacing of the selected keys by `factor` around the current frame (S in the timeline)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      const float factor = op_arg_float(C, "factor", 1.0f);
                      const int center = C.document->frame_current;
                      animation_record(*C.document);
                      layers_move_selected_keys(timeline_layers(C), [&](int f) {
                        return int(std::lround(float(center) + float(f - center) * factor));
                      });
                      animation_evaluate(*C.document, C.document->frame_current);
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_delete", "Delete Keys", "Delete the selected keys (frame-0 setup keys stay)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      animation_record(*C.document);
                      if (layers_delete_selected_keys(timeline_layers(C)) == 0) {
                        return OpStatus::Cancelled;
                      }
                      animation_evaluate(*C.document, C.document->frame_current);
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_duplicate", "Duplicate Keys",
                    "Copy the selected keys `frames` later; the copies become the selection",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      const std::vector<Layer *> layers = timeline_layers(C);
                      if (!layers_any_key_selected(layers)) {
                        return OpStatus::Cancelled;
                      }
                      animation_record(*C.document);
                      layers_duplicate_selected_keys(layers, op_arg_int(C, "frames", 1));
                      animation_evaluate(*C.document, C.document->frame_current);
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_copy", "Copy Keys",
                    "Copy the selected keys (Ctrl C in the timeline); they can be pasted in another action too",
                    OPTYPE_REGISTER, [](Context &C) {
                      animation_record(*C.document);
                      const int copied = layers_copy_selected_keys(timeline_layers(C));
                      if (copied == 0) {
                        report(ReportType::Warning, "Select keys to copy");
                        return OpStatus::Cancelled;
                      }
                      report(ReportType::Info, "Copied " + std::to_string(copied) + (copied == 1 ? " key" : " keys"));
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_paste", "Paste Keys",
                    "Paste the copied keys at the current frame (Ctrl V in the timeline), onto the same layers' "
                    "channels, in whichever action is being edited",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      if (key_clipboard_empty()) {
                        report(ReportType::Warning, "Copy keys first (Ctrl C in the timeline)");
                        return OpStatus::Cancelled;
                      }
                      Document &doc = *C.document;
                      animation_record(doc);
                      /* The setup frame isn't pasted over: from frame 0 the keys start at 1. */
                      const int pasted = layers_paste_keys(timeline_layers(C), std::max(1, doc.frame_current));
                      if (pasted == 0) {
                        report(ReportType::Warning, "The copied keys' layers aren't selected");
                        return OpStatus::Cancelled;
                      }
                      animation_evaluate(doc, doc.frame_current);
                      return OpStatus::Finished;
                    });
  /* G, S and Shift D over the timeline: ask the timeline to move, scale or duplicate-and-move the
   * selected keys by mouse. */
  for (const char kind : {'G', 'S', 'D'}) {
    OperatorType ot;
    ot.idname = kind == 'G' ? "anim.keys_transform_move"
                : kind == 'S' ? "anim.keys_transform_scale"
                              : "anim.keys_duplicate_move";
    ot.name = kind == 'G' ? "Grab Keys" : kind == 'S' ? "Scale Keys" : "Duplicate Keys";
    ot.description = kind == 'G'   ? "Move the selected keys with the mouse"
                     : kind == 'S' ? "Scale the selected keys with the mouse"
                                   : "Copy the selected keys and move the copies with the mouse";
    ot.poll = [](Context &C) { return layers_any_key_selected(timeline_layers(C)); };
    ot.exec = [kind](Context &C) {
      C.wm->timeline_transform_request = kind;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  for (const int dir : {1, -1}) {
    OperatorType ot;
    ot.idname = dir > 0 ? "screen.keyframe_jump_next" : "screen.keyframe_jump_prev";
    ot.name = dir > 0 ? "Jump to Next Keyframe" : "Jump to Previous Keyframe";
    ot.description = dir > 0 ? "Jump to the next keyframe of the active and selected layers"
                             : "Jump to the previous keyframe of the active and selected layers";
    ot.exec = [dir](Context &C) {
      const int frame = key_frame_beside(C, dir);
      if (frame < 0) {
        report(ReportType::Info, "No more keyframes to jump to in this direction");
        return OpStatus::Cancelled;
      }
      C.document->frame_current = frame;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace sky
