#include "anim/animation.hh"

#include <algorithm>
#include <cmath>
#include <climits>
#include <cstdio>
#include <set>

#include "anim/deform.hh"
#include "data/bones.hh"
#include "data/document.hh"

namespace sky {

/* Calls f(channel, value) for every channel of a layer (not its children). */
template<typename F> static void layer_for_each_channel(Layer &layer, F &&f)
{
  f(layer.position_keys, layer.position);
  f(layer.rotation_keys, layer.rotation);
  f(layer.scale_keys, layer.scale);
  f(layer.opacity_keys, layer.opacity);
  for (Bone &bone : layer.bones) {
    f(bone.position_keys, bone.position);
    f(bone.angle_keys, bone.angle);
    f(bone.scale_keys, bone.scale);
  }
  for (MeshPoint &p : layer.drawing.points) {
    f(p.pos_keys, p.pos);
    f(p.radius_keys, p.radius);
  }
  for (Curve &curve : layer.drawing.curves) {
    for (CurvePoint &cp : curve.points) {
      f(cp.curvature_keys, cp.curvature);
      f(cp.handle_in_keys, cp.handle_in);
      f(cp.handle_out_keys, cp.handle_out);
    }
  }
}

template<typename F> static void layers_for_each_channel(std::vector<Layer> &layers, F &&f)
{
  for (Layer &layer : layers) {
    layer_for_each_channel(layer, f);
    layers_for_each_channel(layer.children, f);
  }
}

template<typename T> static void record(Channel<T> &ch, const T &value, int frame, bool auto_key)
{
  if (!ch.has_last) {
    /* New data (a point just drawn): it is simply there, on every frame. */
    ch.last = value;
    ch.has_last = true;
    return;
  }
  if (value == ch.last) {
    return;
  }
  if (frame == 0) {
    /* The setup frame: the frame-0 key, or the value itself while it isn't animated. */
    if (ch.animated()) {
      ch.set_key(0, value);
    }
  }
  else if (auto_key) {
    if (!ch.animated()) {
      ch.set_key(0, ch.last);
    }
    ch.set_key(frame, value);
  }
  ch.last = value;
}

template<typename T> static void evaluate(Channel<T> &ch, T &value, int frame)
{
  if (ch.animated()) {
    value = ch.evaluate(frame);
  }
  ch.last = value;
  ch.has_last = true;
}

void animation_record(Document &doc)
{
  if (doc.evaluated_frame < 0) {
    /* Nothing evaluated yet: the plain values are where everything starts. */
    animation_evaluate(doc, doc.frame_current);
    return;
  }
  const int frame = doc.evaluated_frame;
  const bool auto_key = doc.auto_key;
  /* New points get their weights, so they follow the bones like their neighbours. */
  document_weight_new_points(doc);
  /* New points get their handles now, from the setup pose, before anything counts as an edit. */
  const auto init = [](auto &&self, std::vector<Layer> &layers) -> void {
    for (Layer &layer : layers) {
      drawing_init_handles(layer.drawing);
      self(self, layer.children);
    }
  };
  init(init, doc.layers);
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) { record(ch, value, frame, auto_key); });
}

void animation_evaluate(Document &doc, int frame)
{
  layers_for_each_channel(doc.layers, [&](auto &ch, auto &value) { evaluate(ch, value, frame); });
  doc.evaluated_frame = frame;
}

void animation_sync(Document &doc)
{
  if (doc.evaluated_frame == doc.frame_current) {
    return;
  }
  if (doc.evaluated_frame >= 0) {
    animation_record(doc);
  }
  animation_evaluate(doc, doc.frame_current);
}

/* --- Actions ----------------------------------------------------------------------------------- */

Layer *action_owner(Document &doc, uint64_t layer_id)
{
  Layer *layer = layer_find(doc, layer_id);
  if (!layer) {
    return nullptr;
  }
  if (layer->is_bone_layer()) {
    return layer;
  }
  const Layer *armature = layer_armature(doc, layer_id);
  return armature ? layer_find(doc, armature->id) : nullptr;
}

/* Calls f(channel, value) for every channel of a bone layer and everything inside it. */
template<typename F> static void owner_for_each_channel(Layer &owner, F &&f)
{
  layer_for_each_channel(owner, f);
  layers_for_each_channel(owner.children, f);
}

/* Puts the timeline being edited away under `from` and brings out `to` ("" = mainline). */
template<typename T> static void swap_timeline(Channel<T> &ch, T &value, const std::string &from, const std::string &to)
{
  if (from == to) {
    return;
  }
  /* The setup pose as it is now (frame 0 of what's being edited). */
  const T setup = ch.animated() ? ch.evaluate(0) : value;
  std::vector<Key<T>> away = std::move(ch.keys);
  ch.keys.clear();
  const bool mainline_animated = from.empty() && !away.empty();
  if (!from.empty()) {
    /* An action keeps its keys after frame 0; frame 0 belongs to the setup pose. */
    std::erase_if(away, [](const Key<T> &k) { return k.frame == 0; });
  }
  if (!away.empty()) {
    ch.actions[from] = std::move(away);
  }
  else {
    ch.actions.erase(from);
  }
  if (auto it = ch.actions.find(to); it != ch.actions.end()) {
    ch.keys = std::move(it->second);
    ch.actions.erase(it);
  }
  if (to.empty()) {
    /* Back on the mainline: setup edits made in the action land on its frame 0. */
    if (ch.animated()) {
      if (Key<T> *k = ch.key_at(0)) {
        k->value = setup;
      }
      else {
        ch.set_key(0, setup);
      }
    }
  }
  else if ((ch.animated() || mainline_animated) && !ch.key_at(0)) {
    /* An action starts from the setup pose: frame 0 shows the setup keys of everything animated,
     * as on the mainline. */
    ch.set_key(0, setup);
  }
  value = setup;
}

bool document_set_action(Document &doc, uint64_t owner_id, const std::string &name)
{
  Layer *owner = name.empty() ? nullptr : layer_find(doc, owner_id);
  if (!name.empty() && (!owner || std::find(owner->actions.begin(), owner->actions.end(), name) == owner->actions.end()))
  {
    return false;
  }
  if (doc.action == name && (name.empty() || doc.action_layer == owner_id)) {
    return true;
  }
  const int frame = doc.frame_current;
  animation_record(doc);
  /* Leave the action being edited (back to the mainline), then go into the new one. */
  if (!doc.action.empty()) {
    if (Layer *from = layer_find(doc, doc.action_layer)) {
      const std::string from_name = doc.action;
      owner_for_each_channel(*from, [&](auto &ch, auto &value) { swap_timeline(ch, value, from_name, ""); });
    }
  }
  doc.action.clear();
  doc.action_layer = 0;
  if (owner) {
    owner_for_each_channel(*owner, [&](auto &ch, auto &value) { swap_timeline(ch, value, "", name); });
    doc.action = name;
    doc.action_layer = owner->id;
  }
  animation_evaluate(doc, frame);
  return true;
}

bool document_set_action_by_name(Document &doc, const std::string &name)
{
  if (name.empty()) {
    return document_set_action(doc, 0, "");
  }
  if (Layer *owner = action_owner(doc, doc.active_layer)) {
    if (std::find(owner->actions.begin(), owner->actions.end(), name) != owner->actions.end()) {
      return document_set_action(doc, owner->id, name);
    }
  }
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->is_bone_layer() && std::find(layer->actions.begin(), layer->actions.end(), name) != layer->actions.end()) {
      return document_set_action(doc, layer->id, name);
    }
  }
  return false;
}

std::string document_action_new(Document &doc, uint64_t owner_id, const std::string &name)
{
  Layer *owner = layer_find(doc, owner_id);
  if (!owner || !owner->is_bone_layer()) {
    return {};
  }
  const std::string base = name.empty() ? "Action" : name;
  std::string unique = base;
  for (int i = 1; std::find(owner->actions.begin(), owner->actions.end(), unique) != owner->actions.end(); i++) {
    char suffix[16];
    std::snprintf(suffix, sizeof(suffix), ".%03d", i);
    unique = base + suffix;
  }
  owner->actions.push_back(unique);
  document_set_action(doc, owner_id, unique);
  return unique;
}

std::vector<ChannelRef> layer_channels(Layer &layer, bool drawing)
{
  std::vector<ChannelRef> refs;
  const auto add = [&](std::string path, auto &ch, auto &value) {
    ChannelRef ref;
    ref.path = std::move(path);
    if constexpr (std::is_same_v<std::decay_t<decltype(value)>, float>) {
      ref.scalar = &ch;
      ref.scalar_value = &value;
    }
    else {
      ref.vector = &ch;
      ref.vector_value = &value;
    }
    refs.push_back(std::move(ref));
  };
  add("position", layer.position_keys, layer.position);
  add("rotation", layer.rotation_keys, layer.rotation);
  add("scale", layer.scale_keys, layer.scale);
  add("opacity", layer.opacity_keys, layer.opacity);
  for (Bone &bone : layer.bones) {
    const std::string base = "bones[\"" + bone.name + "\"].";
    add(base + "position", bone.position_keys, bone.position);
    add(base + "angle", bone.angle_keys, bone.angle);
    add(base + "scale", bone.scale_keys, bone.scale);
  }
  if (!drawing) {
    return refs;
  }
  for (size_t i = 0; i < layer.drawing.points.size(); i++) {
    MeshPoint &p = layer.drawing.points[i];
    const std::string base = "points[" + std::to_string(i) + "].";
    add(base + "pos", p.pos_keys, p.pos);
    add(base + "radius", p.radius_keys, p.radius);
  }
  for (size_t c = 0; c < layer.drawing.curves.size(); c++) {
    Curve &curve = layer.drawing.curves[c];
    for (size_t i = 0; i < curve.points.size(); i++) {
      CurvePoint &cp = curve.points[i];
      const std::string base = "curves[" + std::to_string(c) + "].points[" + std::to_string(i) + "].";
      add(base + "curvature", cp.curvature_keys, cp.curvature);
      add(base + "handle_in", cp.handle_in_keys, cp.handle_in);
      add(base + "handle_out", cp.handle_out_keys, cp.handle_out);
    }
  }
  return refs;
}

bool layer_channel(Layer &layer, const std::string &path, ChannelRef *r_ref)
{
  const bool drawing = path.find('[') != std::string::npos;
  for (ChannelRef &ref : layer_channels(layer, drawing)) {
    if (ref.path == path) {
      *r_ref = ref;
      return true;
    }
  }
  return false;
}

int layer_insert_keys(Layer &layer, int frame)
{
  int count = 0;
  layer_for_each_channel(layer, [&](auto &ch, auto &value) {
    if (!ch.animated() && frame != 0) {
      ch.set_key(0, ch.has_last ? ch.last : value);
    }
    ch.set_key(frame, value);
    ch.last = value;
    ch.has_last = true;
    count++;
  });
  return count;
}

int layer_delete_keys(Layer &layer, int frame)
{
  int count = 0;
  layer_for_each_channel(layer, [&](auto &ch, auto & /*value*/) { count += ch.remove_key(frame) ? 1 : 0; });
  return count;
}

std::vector<int> layer_key_frames(const Layer &layer)
{
  std::set<int> frames;
  layer_for_each_channel(const_cast<Layer &>(layer), [&](auto &ch, auto & /*value*/) {
    for (const auto &key : ch.keys) {
      frames.insert(key.frame);
    }
  });
  return {frames.begin(), frames.end()};
}

std::map<int, KeyFrameSummary> layers_key_summary(const std::vector<Layer *> &layers)
{
  std::map<int, KeyFrameSummary> summary;
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      for (const auto &key : ch.keys) {
        KeyFrameSummary &s = summary[key.frame];
        s.keys++;
        s.selected += key.selected ? 1 : 0;
      }
    });
  }
  return summary;
}

void layers_select_keys(const std::vector<Layer *> &layers, int first, int last, bool select)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      for (auto &key : ch.keys) {
        if (key.frame >= first && key.frame <= last) {
          key.selected = select;
        }
      }
    });
  }
}

void layers_deselect_keys(const std::vector<Layer *> &layers)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      for (auto &key : ch.keys) {
        key.selected = false;
      }
    });
  }
}

bool layers_any_key_selected(const std::vector<Layer *> &layers)
{
  for (const auto &[frame, s] : layers_key_summary(layers)) {
    if (s.selected > 0) {
      return true;
    }
  }
  return false;
}

void layers_move_selected_keys(const std::vector<Layer *> &layers, const std::function<int(int)> &to)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      using KeyT = std::decay_t<decltype(ch.keys.front())>;
      std::vector<KeyT> moved, kept;
      for (const KeyT &key : ch.keys) {
        if (key.selected && key.frame != 0) {
          KeyT k = key;
          k.frame = std::max(1, to(key.frame));
          moved.push_back(k);
        }
        else {
          kept.push_back(key);
        }
      }
      if (moved.empty()) {
        return;
      }
      /* Keys moved onto the same frame: the last one wins; then they replace what was there. */
      std::map<int, KeyT> landed;
      for (const KeyT &k : moved) {
        landed[k.frame] = k;
      }
      std::erase_if(kept, [&](const KeyT &other) { return landed.count(other.frame) > 0; });
      for (const auto &[frame, k] : landed) {
        kept.push_back(k);
      }
      std::sort(kept.begin(), kept.end(), [](const KeyT &a, const KeyT &b) { return a.frame < b.frame; });
      ch.keys = std::move(kept);
    });
  }
}

int layers_delete_selected_keys(const std::vector<Layer *> &layers)
{
  int removed = 0;
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      const size_t before = ch.keys.size();
      std::erase_if(ch.keys, [](const auto &key) { return key.selected && key.frame != 0; });
      removed += int(before - ch.keys.size());
    });
  }
  return removed;
}

void layers_duplicate_selected_keys(const std::vector<Layer *> &layers, int offset)
{
  for (Layer *layer : layers) {
    layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
      using KeyT = std::decay_t<decltype(ch.keys.front())>;
      std::vector<KeyT> copies;
      for (KeyT &key : ch.keys) {
        if (key.selected) {
          KeyT copy = key;
          copy.frame = std::max(1, key.frame + offset);
          copies.push_back(copy);
          key.selected = false;
        }
      }
      for (const KeyT &copy : copies) {
        std::erase_if(ch.keys, [&](const KeyT &k) { return k.frame == copy.frame; });
      }
      for (const KeyT &copy : copies) {
        if (!ch.key_at(copy.frame)) {
          ch.keys.push_back(copy);
        }
      }
      std::sort(ch.keys.begin(), ch.keys.end(), [](const KeyT &a, const KeyT &b) { return a.frame < b.frame; });
    });
  }
}

std::vector<std::pair<int, int>> layers_hold_spans(const std::vector<Layer *> &layers)
{
  std::vector<int> frames;
  for (const auto &[frame, s] : layers_key_summary(layers)) {
    frames.push_back(frame);
  }
  std::vector<std::pair<int, int>> spans;
  for (size_t i = 0; i + 1 < frames.size(); i++) {
    const int a = frames[i], b = frames[i + 1];
    bool holds = true, any = false;
    for (Layer *layer : layers) {
      layer_for_each_channel(*layer, [&](auto &ch, auto & /*value*/) {
        const auto *ka = ch.key_at(a);
        const auto *kb = ch.key_at(b);
        if (!holds || (!ka && !kb)) {
          return;
        }
        if (!ka || !kb || !(ka->value == kb->value)) {
          holds = false;
          return;
        }
        any = true;
      });
    }
    if (holds && any) {
      spans.push_back({a, b});
    }
  }
  return spans;
}

/* --- Copy and paste keys ----------------------------------------------------------------------- */

namespace {

struct ClipboardChannel {
  uint64_t layer = 0;
  std::string layer_name;
  std::string path;
  std::vector<Key<float>> scalar;
  std::vector<Key<float2>> vector;
};

struct KeyClipboard {
  std::vector<ClipboardChannel> channels;
  int first_frame = 0;
};

}  // namespace

static KeyClipboard &key_clipboard()
{
  static KeyClipboard clipboard;
  return clipboard;
}

bool key_clipboard_empty()
{
  return key_clipboard().channels.empty();
}

int layers_copy_selected_keys(const std::vector<Layer *> &layers)
{
  KeyClipboard copied;
  int count = 0;
  int first = INT32_MAX;
  for (Layer *layer : layers) {
    for (const ChannelRef &ref : layer_channels(*layer)) {
      ClipboardChannel c{layer->id, layer->name, ref.path, {}, {}};
      const auto take = [&](const auto &channel, auto &into) {
        for (const auto &key : channel.keys) {
          if (key.selected) {
            into.push_back(key);
            first = std::min(first, key.frame);
            count++;
          }
        }
      };
      if (ref.scalar) {
        take(*ref.scalar, c.scalar);
      }
      else {
        take(*ref.vector, c.vector);
      }
      if (!c.scalar.empty() || !c.vector.empty()) {
        copied.channels.push_back(std::move(c));
      }
    }
  }
  if (count == 0) {
    return 0;
  }
  copied.first_frame = first;
  key_clipboard() = std::move(copied);
  return count;
}

int layers_paste_keys(const std::vector<Layer *> &layers, int frame)
{
  const KeyClipboard &clipboard = key_clipboard();
  if (clipboard.channels.empty() || layers.empty()) {
    return 0;
  }
  std::set<uint64_t> sources;
  for (const ClipboardChannel &c : clipboard.channels) {
    sources.insert(c.layer);
  }
  /* Where a copied channel goes: the same layer, else one with the same name, else (keys copied
   * from a single layer) the first layer that has the channel. */
  const auto target = [&](const ClipboardChannel &c, ChannelRef *r_ref) -> bool {
    for (Layer *layer : layers) {
      if (layer->id == c.layer && layer_channel(*layer, c.path, r_ref)) {
        return true;
      }
    }
    for (Layer *layer : layers) {
      if (layer->name == c.layer_name && layer_channel(*layer, c.path, r_ref)) {
        return true;
      }
    }
    if (sources.size() == 1) {
      for (Layer *layer : layers) {
        if (layer_channel(*layer, c.path, r_ref)) {
          return true;
        }
      }
    }
    return false;
  };
  layers_deselect_keys(layers);
  const int offset = frame - clipboard.first_frame;
  int count = 0;
  for (const ClipboardChannel &c : clipboard.channels) {
    ChannelRef ref;
    if (!target(c, &ref)) {
      continue;
    }
    const auto put = [&](auto &channel, const auto &keys) {
      for (const auto &key : keys) {
        const int at = key.frame + offset;
        if (at < 1) {
          continue;
        }
        auto &placed = channel.set_key(at, key.value);
        placed = key;
        placed.frame = at;
        placed.selected = true;
        count++;
      }
    };
    if (ref.scalar && !c.scalar.empty()) {
      put(*ref.scalar, c.scalar);
    }
    else if (ref.vector && !c.vector.empty()) {
      put(*ref.vector, c.vector);
    }
  }
  return count;
}

}  // namespace sky
