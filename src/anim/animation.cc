#include "anim/animation.hh"

#include <algorithm>
#include <set>

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

}  // namespace sky
