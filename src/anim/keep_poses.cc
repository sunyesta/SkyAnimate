#include "anim/keep_poses.hh"

#include <cmath>

#include "data/document.hh"

namespace oanim {

SetupSnapshot setup_snapshot(const Document &doc, const std::vector<uint64_t> &layers)
{
  SetupSnapshot snap;
  for (const uint64_t id : layers) {
    const Layer *layer = layer_find(doc, id);
    if (!layer || layer->is_reference()) {
      continue;
    }
    SetupSnapshot::LayerSetup s;
    s.layer = id;
    for (const MeshPoint &p : layer->drawing.points) {
      /* Detail points' keys hold offsets along the level below, not positions: they stay as they are. */
      if (p.detail.level == 0) {
        s.points[p.id] = p.setup_pos;
      }
    }
    for (const Curve &curve : layer->drawing.curves) {
      std::vector<std::pair<float2, float2>> handles;
      for (const CurvePoint &cp : curve.points) {
        handles.emplace_back(handle_setup(cp, true), handle_setup(cp, false));
      }
      s.handles.push_back(std::move(handles));
    }
    for (const Bone &b : layer->bones) {
      s.bones[b.id] = {b.setup_position, b.setup_angle, b.length};
    }
    snap.layers.push_back(std::move(s));
  }
  return snap;
}

/* `f(key value)` for every key on another frame than the setup's, in every timeline of the channel. */
template<typename T, typename F> static int each_posed_key(Channel<T> &ch, F &&f)
{
  int n = 0;
  const auto run = [&](std::vector<Key<T>> &keys) {
    for (Key<T> &k : keys) {
      if (k.frame != 0) {
        f(k.value);
        n++;
      }
    }
  };
  run(ch.keys);
  for (auto &[name, keys] : ch.actions) {
    run(keys);
  }
  return n;
}

int keep_poses(Document &doc, const SetupSnapshot &before)
{
  int changed = 0;
  for (const SetupSnapshot::LayerSetup &s : before.layers) {
    Layer *layer = layer_find(doc, s.layer);
    if (!layer) {
      continue;
    }
    /* Points: a key holds the change from the setup position, so the setup's move comes off it. */
    for (MeshPoint &p : layer->drawing.points) {
      const auto it = s.points.find(p.id);
      if (it == s.points.end()) {
        continue;
      }
      const float2 moved = p.setup_pos - it->second;
      if (length(moved) > 1e-9f) {
        changed += each_posed_key(p.pos_keys, [&](float2 &v) { v -= moved; });
      }
    }
    /* Handles: a key holds the turn and the length added to the setup handle; the handle it made is
     * worked out against the new setup handle. */
    VectorDrawing &d = layer->drawing;
    for (size_t c = 0; c < d.curves.size() && c < s.handles.size(); c++) {
      Curve &curve = d.curves[c];
      for (size_t k = 0; k < curve.points.size() && k < s.handles[c].size(); k++) {
        CurvePoint &cp = curve.points[k];
        for (const bool in : {true, false}) {
          const float2 was = in ? s.handles[c][k].first : s.handles[c][k].second;
          const float2 now = handle_setup(cp, in);
          if (length(now - was) <= 1e-9f) {
            continue;
          }
          changed += each_posed_key(in ? cp.handle_in_keys : cp.handle_out_keys, [&](float2 &v) {
            v = handle_change(now, handle_from_change(was, v), v.x);
          });
        }
      }
    }
    /* Bones: a key holds the position along the setup's axes and the angle added to the setup's. */
    for (Bone &b : layer->bones) {
      const auto it = s.bones.find(b.id);
      if (it == s.bones.end()) {
        continue;
      }
      const SetupSnapshot::BoneSetup &was = it->second;
      const bool moved = length(b.setup_position - was.position) > 1e-9f || std::fabs(b.setup_angle - was.angle) > 1e-9f;
      if (moved) {
        changed += each_posed_key(b.position_keys, [&](float2 &t) {
          const float2 where = was.position + rotate(t, was.angle);
          t = rotate(where - b.setup_position, -b.setup_angle);
        });
        changed += each_posed_key(b.angle_keys, [&](float &u) { u += was.angle - b.setup_angle; });
      }
      /* Lengthened on the setup frame: its keyed scale along it shrinks to keep its posed length. */
      if (std::fabs(b.length - was.length) > 1e-9f && b.length > 1e-9f) {
        const float k = was.length / b.length;
        changed += each_posed_key(b.scale_keys, [&](float2 &v) { v.y *= k; });
      }
    }
  }
  return changed;
}

}  // namespace oanim
