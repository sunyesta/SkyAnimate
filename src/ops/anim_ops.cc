/* Keyframe operators: Blender's I and Alt I, and jumping between keys. */

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <set>

#include "anim/animation.hh"
#include "assets/asset_library.hh"
#include "core/log.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

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
  /* What the rows show, and the layers of pinned channels (listed under Pinned, whatever layer is
   * picked): key tools work on all of them. */
  std::vector<Layer *> layers = timeline_shown_layers(C);
  const auto pins = [](Layer &l) { return !layer_filtered_channels(l, KeyFilter{KEY_ALL, false, "", true}).empty(); };
  if (C.document) {
    for (const Layer *flat : document_layers_flat(*C.document)) {
      Layer *l = layer_find(*C.document, flat->id);
      if (!l->locked && pins(*l) && std::find(layers.begin(), layers.end(), l) == layers.end()) {
        layers.push_back(l);
      }
    }
    for (Layer *own : {&document_simulation(*C.document), &document_camera(*C.document)}) {
      if (pins(*own) && std::find(layers.begin(), layers.end(), own) == layers.end()) {
        layers.push_back(own);
      }
    }
  }
  return layers;
}

std::vector<Layer *> timeline_shown_layers(Context &C)
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
  /* The document's animated gravity and wind (a Simulation row). */
  if (C.document && !layer_key_frames(document_simulation(*C.document)).empty()) {
    add(&C.document->simulation);
  }
  /* The camera's keys, while it is picked or once it has any (a Camera row). */
  if (C.document) {
    Layer &camera = document_camera(*C.document);
    if (camera.selected || !layer_key_frames(camera).empty()) {
      add(&camera);
    }
  }
  return layers;
}

/* The number in the first "[n]" of `path` from `from` (-1 when there is none); `r_end`: just past it. */
static int path_index(const std::string &path, size_t from, size_t *r_end = nullptr)
{
  const size_t open = path.find('[', from);
  if (open == std::string::npos) {
    return -1;
  }
  const char *start = path.c_str() + open + 1;
  char *end = nullptr;
  const long n = std::strtol(start, &end, 10);
  if (end == start || *end != ']') {
    return -1;
  }
  if (r_end) {
    *r_end = size_t(end - path.c_str()) + 1;
  }
  return int(n);
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

/* Copy and Paste Interpolation: one key's settings. */
static KeySettings g_interp_clipboard;
static bool g_interp_copied = false;

/* The settings an operator's arguments name, over `s`: only those given change. False (and a
 * report) for an unknown mode or strength. */
static bool interp_args_apply(Context &C, KeySettings &s, std::string *r_what)
{
  if (op_has_arg(C, "interp")) {
    Interp interp;
    if (!interp_from_identifier(op_arg(C, "interp"), &interp)) {
      report(ReportType::Error, "Unknown interpolation '" + op_arg(C, "interp") + "'");
      return false;
    }
    s.interp = interp;
    *r_what = interp_name(interp);
  }
  if (op_has_arg(C, "ease")) {
    Ease ease;
    if (!ease_from_identifier(op_arg(C, "ease"), &ease)) {
      report(ReportType::Error, "Unknown ease strength '" + op_arg(C, "ease") + "'");
      return false;
    }
    s.ease = ease;
    if (r_what->empty()) {
      *r_what = std::string("Ease Strength ") + ease_name(ease);
    }
  }
  const auto number = [&](const char *name, auto &r_value) {
    if (op_has_arg(C, name)) {
      r_value = std::decay_t<decltype(r_value)>(op_arg_float(C, name, float(r_value)));
      if (r_what->empty()) {
        *r_what = "Interpolation Settings";
      }
    }
  };
  number("hold", s.hold);
  number("interval", s.interval);
  number("overshoot", s.overshoot);
  number("elastic_amplitude", s.elastic_amplitude);
  number("elastic_period", s.elastic_period);
  number("noise_amplitude", s.noise_amplitude);
  number("noise_frequency", s.noise_frequency);
  number("cycle_back", s.cycle_back);
  s.hold = std::max(0, s.hold);
  s.interval = std::clamp(s.interval, 1, 3);
  s.cycle_back = std::max(0, s.cycle_back);
  s.elastic_period = std::max(0.02f, s.elastic_period);
  return true;
}

void anim_operators_register()
{
  {
    OperatorType ot;
    ot.idname = "anim.keys_interpolation_set";
    ot.name = "Set Interpolation";
    ot.description =
        "Set the selected keys' interpolation (with no keys selected: what new keys get). interp: LINEAR, SMOOTH, "
        "EASE_IN, EASE_OUT, EASE_IN_OUT, STEP, BACK, BOUNCE, ELASTIC, NOISY, CYCLE; ease: SINE, QUAD, CUBIC, QUART, "
        "QUINT, EXPO, CIRC; and hold, interval, overshoot, elastic_amplitude, elastic_period, noise_amplitude, "
        "noise_frequency, cycle_back. Only the settings given change";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      animation_record(doc);
      const std::vector<Layer *> layers = timeline_layers(C);
      std::string what;
      if (layers_selected_keys(layers).count == 0) {
        /* Nothing selected: the mode for new keys, as Moho's timeline menu. */
        KeySettings &s = new_key_settings().settings;
        KeySettings changed = s;
        if (!interp_args_apply(C, changed, &what)) {
          return OpStatus::Cancelled;
        }
        s = changed;
        preferences_save();
        report(ReportType::Info, "New keys: " + std::string(interp_name(s.interp)));
        return OpStatus::Finished;
      }
      bool ok = true;
      const int count = layers_change_selected_keys(layers, [&](KeySettings &s) { ok &= interp_args_apply(C, s, &what); });
      if (!ok) {
        return OpStatus::Cancelled;
      }
      animation_evaluate(doc, doc.frame_current);
      report(ReportType::Info, what + ": " + std::to_string(count) + (count == 1 ? " key" : " keys"));
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* T over the timeline: the Interpolation menu at the mouse, as Blender's. */
    OperatorType ot;
    ot.idname = "anim.interpolation_menu";
    ot.name = "Interpolation Mode";
    ot.description = "Choose how the selected keys move on to the next key (with none selected: what new keys get)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      C.wm->menu_request = "key_interpolation";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.interpolation_settings";
    ot.name = "Interpolation Settings";
    ot.description = "Show the selected keys' interpolation settings (with none selected: new keys'): the curve, "
                     "strength, overshoot, hold, animating on 2s";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      C.wm->menu_request = "key_interpolation_settings";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  anim_operator_add("anim.interpolation_copy", "Copy Interpolation",
                    "Copy the first selected key's interpolation and settings, to paste onto other keys",
                    OPTYPE_REGISTER, [](Context &C) {
                      animation_record(*C.document);
                      const SelectedKeys sel = layers_selected_keys(timeline_layers(C));
                      if (sel.count == 0) {
                        report(ReportType::Warning, "Select a key to copy its interpolation");
                        return OpStatus::Cancelled;
                      }
                      g_interp_clipboard = sel.first;
                      g_interp_copied = true;
                      report(ReportType::Info, std::string("Copied ") + interp_name(sel.first.interp));
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.interpolation_paste", "Paste Interpolation",
                    "Give the selected keys the copied interpolation and settings (their values stay)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      if (!g_interp_copied) {
                        report(ReportType::Warning, "Copy a key's interpolation first");
                        return OpStatus::Cancelled;
                      }
                      Document &doc = *C.document;
                      animation_record(doc);
                      const int count = layers_change_selected_keys(timeline_layers(C),
                                                                    [](KeySettings &s) { s = g_interp_clipboard; });
                      if (count == 0) {
                        report(ReportType::Warning, "Select keys to paste the interpolation onto");
                        return OpStatus::Cancelled;
                      }
                      animation_evaluate(doc, doc.frame_current);
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.interpolation_use_for_new_keys", "Use for New Keys",
                    "New keys get the first selected key's interpolation and settings",
                    OPTYPE_REGISTER, [](Context &C) {
                      animation_record(*C.document);
                      const SelectedKeys sel = layers_selected_keys(timeline_layers(C));
                      if (sel.count == 0) {
                        report(ReportType::Warning, "Select a key whose interpolation new keys should get");
                        return OpStatus::Cancelled;
                      }
                      new_key_settings().settings = sel.first;
                      preferences_save();
                      report(ReportType::Info, std::string("New keys: ") + interp_name(sel.first.interp));
                      return OpStatus::Finished;
                    });
  {
    OperatorType ot;
    ot.idname = "anim.interpolation_copy_previous";
    ot.name = "Copy Previous Key";
    ot.description = "New keys copy the interpolation of the key before them, when there is one, instead of "
                     "getting the new-key mode (enable=1/0; toggles without it)";
    ot.flag = OPTYPE_REGISTER;
    ot.exec = [](Context &C) {
      bool &on = new_key_settings().copy_previous;
      on = op_has_arg(C, "enable") ? op_arg_bool(C, "enable") : !on;
      preferences_save();
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "screen.stepped_preview";
    ot.name = "Stepped Preview";
    ot.description = "Play everything as Step, holding each key until the next, to judge the timing of the "
                     "poses (blocking); no key changes (enable=1/0; toggles without it)";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      animation_record(doc);
      stepped_preview_set(op_has_arg(C, "enable") ? op_arg_bool(C, "enable") : !stepped_preview());
      animation_evaluate(doc, doc.frame_current);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
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
      std::string name = op_arg(C, "name", "Action");
      if (op_arg_bool(C, "smart")) {
        /* A smart action is named after its bone; a second one (" 2") turns the other way. */
        const auto has = [&](const std::string &n) {
          return std::find(owner->actions.begin(), owner->actions.end(), n) != owner->actions.end();
        };
        if (!bone_find_by_name(*owner, name)) {
          report(ReportType::Warning, "No bone named " + name);
          return OpStatus::Cancelled;
        }
        if (has(name)) {
          name += " 2";
        }
        if (has(name)) {
          report(ReportType::Warning, "That bone has both smart actions already");
          return OpStatus::Cancelled;
        }
        owner->actions.push_back(name);
        document_set_action(*C.document, owner->id, name);
        return OpStatus::Finished;
      }
      name = document_action_new(*C.document, owner->id, name);
      return name.empty() ? OpStatus::Cancelled : OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.action_delete";
    ot.name = "Delete Action";
    ot.description = "Delete action `name` of the bone layer the active layer is in (or is), with its keys "
                     "(right-click an action in the timeline's action menu)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document && action_owner(*C.document, C.document->active_layer); };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::string name = op_arg(C, "name");
      /* The action being edited may belong to another bone layer than the active one. */
      Layer *owner = !doc.action.empty() && doc.action == name ? layer_find(doc, doc.action_layer)
                                                                 : action_owner(doc, doc.active_layer);
      if (!owner || !document_action_delete(doc, owner->id, name)) {
        report(ReportType::Warning, "No action named " + name);
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
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
  {
    OperatorType ot;
    ot.idname = "anim.keyframe_toggle";
    ot.name = "Toggle Keyframe";
    ot.description = "Key properties of a layer at the current frame, or take their keys away when keyed there "
                     "(Blender's keyframe button). layer: the layer's id; paths: channel paths separated by |";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      Layer *layer = layer_find(doc, std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10));
      if (!layer) {
        layer = document_active_layer(doc);
      }
      if (!layer) {
        return OpStatus::Cancelled;
      }
      std::vector<std::string> paths;
      const std::string list = op_arg(C, "paths");
      for (size_t at = 0; at <= list.size();) {
        const size_t bar = std::min(list.find('|', at), list.size());
        if (bar > at) {
          paths.push_back(list.substr(at, bar - at));
        }
        at = bar + 1;
      }
      animation_record(doc);
      const int changed = layer_toggle_keys(*layer, paths, doc.frame_current);
      if (changed == 0) {
        if (doc.frame_current == 0) {
          report(ReportType::Warning, "Frame 0 is the setup key: delete the later keys first");
        }
        return OpStatus::Cancelled;
      }
      animation_evaluate(doc, doc.frame_current);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  anim_operator_add("anim.keys_insert", "Insert Keys in Row",
                    "Key a timeline row's channels on frame= (double-clicking the row there): the playhead goes to "
                    "that frame and what it shows is keyed. layers: the row's layers (comma-separated ids); kinds, "
                    "selected_only, path and pinned: which of their channels, as anim.keys_select takes them",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      Document &doc = *C.document;
                      /* Edits not yet recorded belong to the frame they were made at. */
                      animation_record(doc);
                      const int frame = std::max(0, op_arg_int(C, "frame", doc.frame_current));
                      if (frame != doc.frame_current) {
                        doc.frame_current = frame;
                        C.wm->playing = false;
                        animation_evaluate(doc, frame);
                      }
                      KeyFilter filter;
                      filter.kinds = uint32_t(op_arg_int(C, "kinds", int(KEY_ALL)));
                      filter.selected_only = op_arg_bool(C, "selected_only");
                      filter.path = op_arg(C, "path");
                      filter.pinned_only = op_arg_bool(C, "pinned");
                      int keys = 0;
                      for (Layer *layer : op_layers(C)) {
                        keys += layer_insert_filtered_keys(*layer, filter, frame);
                      }
                      if (keys == 0) {
                        report(ReportType::Info, "Nothing in this row to key");
                        return OpStatus::Cancelled;
                      }
                      animation_evaluate(doc, frame);
                      return OpStatus::Finished;
                    });
  anim_operator_add("anim.keys_select", "Select Keys",
                    "Select the keys on frames frame_start..frame_end (extend keeps the others; deselect takes "
                    "them out). layers: only these layers' keys (comma-separated ids); kinds: only these kinds "
                    "of channel (KeyKind bits); selected_only: only selected points' and bones'; path: only this "
                    "channel (or, ending in '.', those under it); pinned: only pinned channels (a timeline row)",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      const std::vector<Layer *> layers = op_layers(C);
                      const int first = op_arg_int(C, "frame_start", 0);
                      const int last = op_arg_int(C, "frame_end", first);
                      const bool deselect = op_arg_bool(C, "deselect");
                      if (!op_arg_bool(C, "extend") && !deselect) {
                        layers_deselect_keys(timeline_layers(C));
                      }
                      KeyFilter filter;
                      filter.kinds = uint32_t(op_arg_int(C, "kinds", int(KEY_ALL)));
                      filter.selected_only = op_arg_bool(C, "selected_only");
                      filter.path = op_arg(C, "path");
                      filter.pinned_only = op_arg_bool(C, "pinned");
                      layers_select_keys(layers, std::min(first, last), std::max(first, last), !deselect, filter);
                      return OpStatus::Finished;
                    });
  {
    OperatorType ot;
    ot.idname = "anim.channel_pin";
    ot.name = "Pin to Timeline";
    ot.description = "Pin channels to the timeline (pinned=0: unpin; pinned=TOGGLE: the other way from the first): "
                     "they are listed under Pinned whatever is selected, even with no keys. layer: the layer's id; "
                     "paths: channel paths separated by |";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      Layer *layer = layer_find(doc, std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10));
      if (!layer) {
        return OpStatus::Cancelled;
      }
      if (layer->id == SIMULATION_LAYER_ID) {
        document_simulation(doc);
      }
      std::vector<std::string> paths;
      const std::string list = op_arg(C, "paths");
      for (size_t at = 0; at <= list.size();) {
        const size_t bar = std::min(list.find('|', at), list.size());
        if (bar > at) {
          paths.push_back(list.substr(at, bar - at));
        }
        at = bar + 1;
      }
      if (paths.empty()) {
        return OpStatus::Cancelled;
      }
      const std::string how = op_arg(C, "pinned", "1");
      const bool pin = how == "TOGGLE" ? !layer_channel_pinned(*layer, paths.front()) : how != "0";
      return layer_pin_channels(*layer, paths, pin) > 0 ? OpStatus::Finished : OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "anim.pins_clear";
    ot.name = "Clear Pins";
    ot.description = "Unpin every channel pinned to the timeline";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      return document_clear_pins(*C.document) > 0 ? OpStatus::Finished : OpStatus::Cancelled;
    };
    operators().add(std::move(ot));
  }
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
  /* A property picked from the timeline's Keyed Here list. */
  anim_operator_add(
      "anim.keyed_go", "Go to Keyed Property",
      "Show a property keyed in the timeline (`layer`, `paths` joined by |): jump to `frame` (-1 stays), then "
      "select its bone, or its points or handles (going to their layer when they can't be edited from here), or "
      "open it in the Properties editor and flash it. `extend` adds to the selection",
      OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
        Document &doc = *C.document;
        const std::string id_arg = op_arg(C, "layer");
        const uint64_t id = id_arg.empty() ? 0 : std::strtoull(id_arg.c_str(), nullptr, 10);
        Layer &simulation = document_simulation(doc);
        Layer *layer = id != 0 && id == simulation.id ? &simulation : layer_find(doc, id);
        std::vector<std::string> paths;
        const std::string all_paths = op_arg(C, "paths");
        for (size_t at = 0; !all_paths.empty();) {
          const size_t bar = all_paths.find('|', at);
          paths.push_back(all_paths.substr(at, bar == std::string::npos ? std::string::npos : bar - at));
          if (bar == std::string::npos) {
            break;
          }
          at = bar + 1;
        }
        if (!layer || paths.empty() || paths.front().empty()) {
          return OpStatus::Cancelled;
        }
        const int frame = op_arg_int(C, "frame", -1);
        if (frame >= 0) {
          doc.frame_current = frame;
        }
        const bool extend = op_arg(C, "extend") == "1";
        const std::string &path = paths.front();
        WindowManager &wm = *C.wm;
        const auto show = [&](int tab, const char *panel) {
          wm.properties_tab_request = tab;
          wm.properties_flash = {};
          wm.properties_flash.layer = layer->id;
          wm.properties_flash.path = path;
          wm.properties_flash.panel = panel;
        };
        /* The document's Simulation settings: the Document tab. */
        if (layer == &simulation) {
          show(0, "Simulation");
          return OpStatus::Finished;
        }
        /* The camera's: pick it, and show its tab. */
        if (layer->id == CAMERA_LAYER_ID) {
          layer->selected = true;
          show(7, "Camera");
          return OpStatus::Finished;
        }
        /* A bone's: select the bone (its constraint settings also show in Bone Constraints). */
        if (path.rfind("bones[\"", 0) == 0) {
          const size_t close = path.find("\"]", 7);
          const std::string name = close == std::string::npos ? std::string() : path.substr(7, close - 7);
          Bone *bone = nullptr;
          for (Bone &b : layer->bones) {
            bone = b.name == name ? &b : bone;
          }
          if (!bone) {
            return OpStatus::Cancelled;
          }
          doc.active_layer = layer->id;
          if (!extend) {
            armature_deselect_all(*layer);
          }
          bone_select(*bone, true);
          layer->active_bone = bone->id;
          if (const size_t c = path.find(".constraints[", close); c != std::string::npos) {
            const size_t index = size_t(std::atoi(path.c_str() + c + 13));
            if (index < bone->constraints.size()) {
              bone->constraints[index].expanded = true;
            }
            show(4, "");
          }
          return OpStatus::Finished;
        }
        /* Points, curve points and handles: select them, staying in the layer worked in when they
         * can be edited from it, else going to theirs. */
        if (path.rfind("points[", 0) == 0 || path.rfind("curves[", 0) == 0) {
          std::vector<Layer *> editing = editable_layers(C);
          if (std::find(editing.begin(), editing.end(), layer) == editing.end()) {
            doc.active_layer = layer->id;
            editing = editable_layers(C);
          }
          if (!extend) {
            for (Layer *l : editing) {
              for (MeshPoint &p : l->drawing.points) {
                p.selected = false;
              }
              for (Curve &curve : l->drawing.curves) {
                for (CurvePoint &cp : curve.points) {
                  cp.select_in = cp.select_out = false;
                }
              }
            }
          }
          wm.tool_settings.select_mode = SelectMode::Point;
          VectorDrawing &d = layer->drawing;
          for (const std::string &p : paths) {
            size_t after = 0;
            const int a = path_index(p, 0, &after);
            const int b = p.rfind("curves[", 0) == 0 ? path_index(p, after) : -1;
            if (p.rfind("points[", 0) == 0 && a >= 0 && a < int(d.points.size())) {
              d.points[a].selected = true;
            }
            else if (p.rfind("curves[", 0) == 0 && a >= 0 && a < int(d.curves.size()) && b >= 0 &&
                     b < int(d.curves[a].points.size()))
            {
              CurvePoint &cp = d.curves[a].points[b];
              if (p.find(".handle_in") != std::string::npos) {
                cp.select_in = true;
              }
              else if (p.find(".handle_out") != std::string::npos) {
                cp.select_out = true;
              }
              else if (cp.point >= 0 && cp.point < int(d.points.size())) {
                d.points[cp.point].selected = true;
              }
            }
          }
          return OpStatus::Finished;
        }
        /* The layer's own settings: the Layer tab. */
        doc.active_layer = layer->id;
        show(1, path == "visible"                                               ? "Layer"
                : path == "opacity"                                             ? "Compositing"
                : path == "position" || path == "rotation" || path == "scale" ? "Transform"
                                                                                : "");
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
                    "channels, in whichever action is being edited; pasted on frame 0, they become the setup pose",
                    OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                      if (key_clipboard_empty()) {
                        report(ReportType::Warning, "Copy keys first (Ctrl C in the timeline)");
                        return OpStatus::Cancelled;
                      }
                      Document &doc = *C.document;
                      animation_record(doc);
                      /* On frame 0 the first copied key becomes the setup pose. */
                      const int pasted = layers_paste_keys(timeline_layers(C), doc.frame_current);
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

}  // namespace oanim
