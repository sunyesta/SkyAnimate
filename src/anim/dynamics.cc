#include "anim/dynamics.hh"

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

#include "anim/animation.hh"
#include "data/bones.hh"
#include "data/document.hh"

namespace oanim {

namespace {

/* One dynamic bone's motion. The bone hangs from its head like a weight on a stick: when the head
 * moves, the tip lags behind (turning the bone, and stretching or squashing it along its length)
 * before springing back to where the keys put it. The head itself can jiggle around where it
 * should be. */
struct Motion {
  /* Direction (bone layer space) and how fast it turns. */
  float angle = 0.0f, spin = 0.0f;
  /* The head's offset from where it should be, and how fast that changes. */
  float2 offset, drift;
  /* Scale (bone layer space, its parents' included) and how fast it changes. */
  float scale = 1.0f, growth = 0.0f;
  /* Where the strand's head was on the frame before, in document space: how far it has moved
   * since, the bone layer's own movement included. */
  float2 last_head;
  /* Where the bone aimed and how big it was meant to be on the frame before: the swing is damped
   * against the way its aim moves, not against standing still, so a bone moves along with a
   * turning parent rather than dragging behind it (Spine's physics works the same way). */
  float last_angle = 0.0f, last_scale = 1.0f;
};

/* Where the keys put a dynamic bone, aiming from where the bones above it have swung to. */
struct Target {
  float angle = 0.0f;
  float2 head;
  /* The head of the strand it hangs in (the top dynamic bone of a run of them): the strand is
   * thrown off as one piece when that moves, and each bone below then catches up with the one
   * above it (through where it aims), which sends a wave down it. */
  float2 strand_head;
  /* Where its head is in the setup pose: when gusts reach it. Fixed, so a bone moving (or being
   * dragged) doesn't travel through the gusts and speed them up, stall them or run them back. */
  float2 rest_head;
  float scale = 1.0f;
  float length = 1.0f;
  /* The strand's length, from its head to the end of the dynamic bones hanging below. */
  float reach = 1.0f;
  /* Its own scale (the pose's includes its parents'). */
  float local_scale = 1.0f;
};

struct Simulation {
  /* What it was simulated from; a different one starts again. */
  uint64_t signature = 0;
  int start = 1;
  /* Frame start + k, by bone id. */
  std::vector<std::map<uint64_t, Motion>> frames;
  /* The frame shown, as last worked out (where live dynamics start from). */
  std::map<uint64_t, Motion> shown;
};

/* Live dynamics: a step of `seconds` instead of a frame (0: a frame), and how long the live
 * simulation has run (so the wind's gusts keep blowing while it does). */
float g_step_seconds = 0.0f;
float g_live_clock = 0.0f;

/* By bone layer id. */
std::map<uint64_t, Simulation> g_simulations;
/* Something is being dragged (see dynamics_hold_cache). */
bool g_hold_cache = false;

const BoneConstraint *dynamics_of(const Bone &bone)
{
  for (const BoneConstraint &c : bone.constraints) {
    if (c.type == BoneConstraintType::Dynamics && c.enabled &&
        (c.angle_spring.enabled || c.position_spring.enabled || c.scale_spring.enabled))
    {
      return &c;
    }
  }
  return nullptr;
}

bool has_dynamics(const Layer &armature)
{
  for (const Bone &b : armature.bones) {
    if (dynamics_of(b)) {
      return true;
    }
  }
  return false;
}

/* The bone layer and the layers above it, outermost first. */
bool layer_path(const std::vector<Layer> &list, uint64_t id, std::vector<const Layer *> &r_path)
{
  for (const Layer &layer : list) {
    r_path.push_back(&layer);
    if (layer.id == id || layer_path(layer.children, id, r_path)) {
      return true;
    }
    r_path.pop_back();
  }
  return false;
}

/* FNV-1a over what the simulation depends on. */
struct Hash {
  uint64_t h = 1469598103934665603ull;
  void bytes(const void *data, size_t n)
  {
    const auto *p = static_cast<const unsigned char *>(data);
    for (size_t i = 0; i < n; i++) {
      h = (h ^ p[i]) * 1099511628211ull;
    }
  }
  template<typename T> void value(const T &v) { bytes(&v, sizeof(v)); }
  template<typename T> void channel(const Channel<T> &ch, const T &plain)
  {
    value(ch.keys.size());
    for (const Key<T> &k : ch.keys) {
      value(k.frame);
      value(k.value);
      value(int(k.interp));
      value(int(k.ease));
      value(k.hold);
      value(k.interval);
      value(k.overshoot);
      value(k.elastic_amplitude);
      value(k.elastic_period);
    }
    if (!ch.animated()) {
      const T base = plain - ch.from_action_bones;
      value(base);
    }
    /* Its action bone actions' keys: they move the bones on every frame the action bone turns. */
    for (const auto &[name, keys] : ch.actions) {
      bytes(name.data(), name.size());
      value(keys.size());
      for (const Key<T> &k : keys) {
        value(k.frame);
        value(k.value);
        value(int(k.interp));
      }
    }
  }
};

uint64_t signature(const Document &doc, const Layer &armature)
{
  Hash h;
  h.value(doc.gravity);
  h.value(doc.wind);
  h.value(doc.wind_direction);
  h.value(doc.wind_gusts);
  h.value(doc.wind_gust_speed);
  h.value(doc.wind_turbulence);
  h.value(doc.simulation_start);
  for (const CustomChannel &c : doc.simulation.custom_channels) {
    h.channel(c.keys, c.value);
  }
  h.value(doc.fps);
  /* Moving the bone layer (or a layer above it) moves the heads. */
  std::vector<const Layer *> path;
  layer_path(doc.layers, armature.id, path);
  for (const Layer *l : path) {
    h.channel(l->position_keys, l->position);
    h.channel(l->rotation_keys, l->rotation);
    h.channel(l->scale_keys, l->scale);
  }
  for (const Bone &b : armature.bones) {
    h.value(b.id);
    h.value(b.parent);
    h.value(b.length);
    h.value(b.setup_position);
    h.value(b.setup_angle);
    h.channel(b.position_keys, b.position);
    h.channel(b.angle_keys, b.angle);
    h.channel(b.scale_keys, b.scale);
    for (const BoneConstraint &c : b.constraints) {
      h.value(int(c.type));
      h.value(c.enabled);
      h.channel(c.influence_keys, c.influence);
      h.value(c.min_angle);
      h.value(c.max_angle);
      h.value(c.amount);
      h.value(c.target);
      h.value(c.factor);
      h.value(c.delay);
      h.value(c.chain);
      h.value(c.max_stretch);
      h.value(c.own_gravity);
      h.value(c.own_wind);
      h.value(c.weight);
      h.value(c.wind_catch);
      h.value(c.gravity);
      h.value(c.wind);
      h.value(c.wind_direction);
      h.value(c.wind_gusts);
      h.value(c.wind_gust_speed);
      h.value(c.wind_turbulence);
      for (const Channel<float> *k : {&c.weight_keys, &c.wind_catch_keys, &c.gravity_keys, &c.wind_keys, &c.wind_direction_keys, &c.wind_gusts_keys,
                                      &c.wind_gust_speed_keys, &c.wind_turbulence_keys})
      {
        h.channel(*k, 0.0f);
      }
      for (const BoneSpring *s : {&c.angle_spring, &c.position_spring, &c.scale_spring}) {
        h.value(s->enabled);
        h.value(s->settle_speed);
        h.value(s->bounce);
        h.value(s->inertia);
        h.value(s->stretch);
        h.value(s->use_limit);
        h.value(s->limit);
        h.value(s->use_squash_limit);
        h.value(s->squash_limit);
        for (const Channel<float> *k : {&s->settle_speed_keys, &s->bounce_keys, &s->inertia_keys,
                                        &s->stretch_keys, &s->limit_keys, &s->squash_limit_keys})
        {
          h.channel(*k, 0.0f);
        }
      }
    }
  }
  return h.h;
}

template<typename T> T at_frame(const Channel<T> &ch, const T &plain, int frame)
{
  return ch.animated() ? ch.evaluate(frame) : plain - ch.from_action_bones;
}

/* The bone layer as its keys have it on `frame` (`live`: the frame shown, from the bones as they
 * are now, a drag not yet keyed included). */
Layer keyed_layer(const Layer &armature, int frame, bool live)
{
  Layer at;
  at.type = armature.type;
  at.pose_frame = frame;
  at.bones = armature.bones;
  if (live) {
    return at;
  }
  for (Bone &b : at.bones) {
    b.position = bone_position_at(b, frame, true);
    b.angle = bone_angle_at(b, frame, true);
    b.scale = at_frame(b.scale_keys, b.scale, frame);
    for (BoneConstraint &c : b.constraints) {
      c.influence = at_frame(c.influence_keys, c.influence, frame);
      c.weight = at_frame(c.weight_keys, c.weight, frame);
      c.wind_catch = at_frame(c.wind_catch_keys, c.wind_catch, frame);
      c.gravity = at_frame(c.gravity_keys, c.gravity, frame);
      c.wind = at_frame(c.wind_keys, c.wind, frame);
      c.wind_direction = at_frame(c.wind_direction_keys, c.wind_direction, frame);
      c.wind_gusts = at_frame(c.wind_gusts_keys, c.wind_gusts, frame);
      c.wind_gust_speed = at_frame(c.wind_gust_speed_keys, c.wind_gust_speed, frame);
      c.wind_turbulence = at_frame(c.wind_turbulence_keys, c.wind_turbulence, frame);
      for (BoneSpring *s : {&c.angle_spring, &c.position_spring, &c.scale_spring}) {
        s->settle_speed = at_frame(s->settle_speed_keys, s->settle_speed, frame);
        s->bounce = at_frame(s->bounce_keys, s->bounce, frame);
        s->inertia = at_frame(s->inertia_keys, s->inertia, frame);
        s->stretch = at_frame(s->stretch_keys, s->stretch, frame);
        s->limit = at_frame(s->limit_keys, s->limit, frame);
        s->squash_limit = at_frame(s->squash_limit_keys, s->squash_limit, frame);
      }
    }
  }
  /* And what action bone actions add there, as the frame shown has it (a head turned by an action bone turns
   * the hair hanging from it on every frame it is simulated through). */
  action_bone_actions_add_at(armature, frame, at.bones);
  return at;
}

/* Where the bone layer is in the document on `frame`, from its keys and its parent layers'. */
Transform2D layer_world_at(const Document &doc, const Layer &armature, int frame, bool live)
{
  std::vector<const Layer *> path;
  layer_path(doc.layers, armature.id, path);
  Transform2D world;
  for (const Layer *l : path) {
    if (live) {
      world = world * l->local_transform();
      continue;
    }
    Layer at;
    at.position = at_frame(l->position_keys, l->position, frame);
    at.rotation = at_frame(l->rotation_keys, l->rotation, frame);
    at.scale = at_frame(l->scale_keys, l->scale, frame);
    world = world * at.local_transform();
  }
  return world;
}

/* How many dynamic bones each bone has above it (-1: not dynamic). Bones are simulated in that
 * order, so each aims from where the ones above it have swung to. */
std::vector<int> dynamic_levels(const Layer &armature)
{
  const size_t n = armature.bones.size();
  std::map<uint64_t, int> index;
  for (int i = 0; i < int(n); i++) {
    index[armature.bones[i].id] = i;
  }
  std::vector<int> level(n, -1);
  for (size_t i = 0; i < n; i++) {
    if (!dynamics_of(armature.bones[i])) {
      continue;
    }
    int above = 0;
    uint64_t parent = armature.bones[i].parent;
    for (int depth = 0; depth < 256; depth++) {
      const auto it = index.find(parent);
      if (it == index.end() || it->second == int(i)) {
        break;
      }
      above += dynamics_of(armature.bones[it->second]) ? 1 : 0;
      parent = armature.bones[it->second].parent;
    }
    level[i] = above;
  }
  return level;
}

float pose_angle(const Transform2D &t)
{
  return std::atan2(t.b, t.a);
}

float pose_scale(const Transform2D &t)
{
  return std::sqrt(t.a * t.a + t.b * t.b);
}

float wrap_pi(float a)
{
  return a - 2.0f * PI * std::round(a / (2.0f * PI));
}

/* A channel's limit, 0 when it has none. */
float limit_of(const BoneSpring &k)
{
  return k.use_limit ? std::max(k.limit, 1e-4f) : 0.0f;
}

/* Squash & Stretch's Max Squash, 0 when it has none. */
float squash_limit_of(const BoneSpring &k)
{
  return k.use_squash_limit ? std::clamp(k.squash_limit, 1e-4f, 0.9f) : 0.0f;
}

/* A limit approached softly: the nearer it gets, the slower (0: none). */
float soft(float x, float limit)
{
  return limit > 0.0f ? limit * std::tanh(x / limit) : x;
}

float2 soft(float2 v, float limit)
{
  const float l = length(v);
  return limit > 0.0f && l > 1e-9f ? v * (soft(l, limit) / l) : v;
}

/* A damped spring, in seconds so the motion is the same at any frame rate. Settle Speed is how
 * fast the swinging decays (at 1 it falls to 2% in half a second), Bounce the first overshoot,
 * which gives the damping ratio; the two together give the stiffness. */
struct Spring {
  float stiffness = 0.0f; /* omega squared */
  float damping = 0.0f;   /* 2 zeta omega */
};

Spring spring_of(const BoneSpring &k)
{
  const float decay = 8.0f * std::clamp(k.settle_speed, 0.05f, 20.0f);
  const float bounce = std::clamp(k.bounce, 0.0f, 0.9f);
  float zeta = 1.0f;
  if (bounce > 1e-4f) {
    const float l = std::log(bounce);
    zeta = -l / std::sqrt(PI * PI + l * l);
  }
  const float omega = decay / zeta;
  return {omega * omega, 2.0f * decay};
}

/* How far a gravity or wind of 1 bends a bone of weight 1 (radians), the same however stiff it
 * is: the push is a share of the spring's pull. */
constexpr float BEND = 0.056f;
/* The same for a head jiggling (bone layer units). */
constexpr float DROP = 0.4f * BEND;
/* The same for a bone stretching along its length (a fraction of it). */
constexpr float PULL = 0.02f;

/* Smooth noise between -1 and 1 that never repeats: random values at whole numbers, joined by
 * curves through them (Catmull-Rom), the same every time for the same `x` and `seed`. */
float noise_at(int i, uint32_t seed)
{
  uint32_t h = uint32_t(i) * 0x9E3779B1u ^ seed * 0x85EBCA6Bu;
  h ^= h >> 16;
  h *= 0x7FEB352Du;
  h ^= h >> 15;
  h *= 0x846CA68Bu;
  h ^= h >> 16;
  return float(h) / 2147483647.5f - 1.0f;
}

float noise(float x, uint32_t seed)
{
  const float fl = std::floor(x);
  const int i = int(fl);
  const float t = x - fl;
  const float p0 = noise_at(i - 1, seed), p1 = noise_at(i, seed), p2 = noise_at(i + 1, seed),
              p3 = noise_at(i + 2, seed);
  const float v = 0.5f * (2.0f * p1 + (p2 - p0) * t + (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3) * t * t +
                          (3.0f * p1 - p0 - 3.0f * p2 + p3) * t * t * t);
  return std::clamp(v, -1.0f, 1.0f);
}

/* The wind's settings for a bone: the document's, or the bone's own. */
struct WindSettings {
  float strength = 0.0f, direction = 0.0f, gusts = 0.0f, gust_speed = 1.0f, turbulence = 0.0f;
};

/* Gravity and the wind's settings for a bone: the document's, or the bone's own. */
struct Forces {
  float gravity = 0.0f;
  WindSettings wind;
};

/* A keyed setting as the simulation feels it on `frame`: averaged over about a tenth of a second
 * around it, so a key where the setting stops rising (or starts) doesn't jolt the bones into a
 * bounce. Never from before `first`, where the simulation starts: frame 0 is the setup pose, and
 * its value mustn't leak into the first frames. The keys themselves (and the value shown) stay as
 * keyed. */
float eased(const Channel<float> &ch, float plain, int frame, int radius, int first)
{
  if (!ch.animated()) {
    return plain;
  }
  float sum = 0.0f, total = 0.0f;
  for (int d = -radius; d <= radius; d++) {
    const float w = float(radius + 1 - std::abs(d));
    sum += w * ch.evaluate(std::max(frame + d, first));
    total += w;
  }
  return sum / total;
}

Forces forces_at(const Document &doc, const BoneConstraint &c, int frame)
{
  const int radius = std::max(1, int(std::lround(0.1f * doc.fps)));
  const int first = std::max(1, doc.simulation_start);
  const auto at = [&](const char *path, float plain) {
    for (const CustomChannel &ch : doc.simulation.custom_channels) {
      if (ch.path == path) {
        return eased(ch.keys, plain, frame, radius, first);
      }
    }
    return plain;
  };
  const auto own = [&](const Channel<float> &keys, float plain) { return eased(keys, plain, frame, radius, first); };
  Forces f;
  f.gravity = c.own_gravity ? own(c.gravity_keys, c.gravity) : at("gravity", doc.gravity);
  if (c.own_wind) {
    f.wind = {own(c.wind_keys, c.wind), own(c.wind_direction_keys, c.wind_direction),
              own(c.wind_gusts_keys, c.wind_gusts), own(c.wind_gust_speed_keys, c.wind_gust_speed),
              own(c.wind_turbulence_keys, c.wind_turbulence)};
  }
  else {
    f.wind = {at("wind", doc.wind), at("wind_direction", doc.wind_direction), at("wind_gusts", doc.wind_gusts),
              at("wind_gust_speed", doc.wind_gust_speed), at("wind_turbulence", doc.wind_turbulence)};
  }
  return f;
}

/* Between two frames' forces (`u` from 0 to 1), so they change smoothly through a frame rather
 * than a step at each. */
Forces forces_between(const Forces &a, const Forces &b, float u)
{
  const auto mix = [u](float x, float y) { return x + (y - x) * u; };
  return {mix(a.gravity, b.gravity),
          {mix(a.wind.strength, b.wind.strength), mix(a.wind.direction, b.wind.direction),
           mix(a.wind.gusts, b.wind.gusts), mix(a.wind.gust_speed, b.wind.gust_speed),
           mix(a.wind.turbulence, b.wind.turbulence)}};
}

/* How fast a gust sweeps across the drawing (document units a second, at a gust speed of 1). */
constexpr float GUST_TRAVEL = 3.0f;

/* The wind at `seconds` on a bone whose head is at `where` (document space), as Unity's wind
 * zones and GPU Gems' tree wind: a slow layer of gusts that strengthen, weaken and waver it,
 * reaching a bone later the further downwind it is, and a fast, small layer of turbulence along it
 * and across it. Steady when both are 0. */
float2 wind_at(const WindSettings &w, float seconds, float2 where)
{
  const float2 way(std::cos(w.direction), std::sin(w.direction));
  if (w.strength == 0.0f) {
    return {};
  }
  if (w.gusts <= 0.0f && w.turbulence <= 0.0f) {
    return way * w.strength;
  }
  const float speed = std::max(w.gust_speed, 0.01f);
  /* Downwind bones feel a gust later: it sweeps across like a wave through grass. */
  const float time = seconds - dot(where, way) / (GUST_TRAVEL * speed);
  const float slow = time * 0.5f * speed, fast = time * 3.0f * speed;
  const float strength = w.strength * std::max(0.0f, 1.0f + w.gusts * noise(slow, 1u));
  const float angle = w.direction + w.gusts * 0.3f * noise(slow + 37.0f, 2u);
  const float2 along(std::cos(angle), std::sin(angle));
  const float2 across(-along.y, along.x);
  const float flutter = w.turbulence * std::abs(w.strength) * 0.5f;
  return along * (strength + flutter * noise(fast, 3u)) + across * (flutter * 0.7f * noise(fast + 91.0f, 4u));
}

/* A direction in document space, in the bone layer's (keeping its size). */
float2 in_layer(const Transform2D &to_layer, float2 v)
{
  const float l = length(v);
  return l > 1e-9f ? normalize(to_layer.apply_vector(v)) * l : float2();
}

/* Where gravity and the wind as they are on `frame` hold a bone still: the bend, the head's drop
 * and the stretch the springs balance them at. */
struct Balance {
  float angle = 0.0f;
  float2 offset;
  float scale = 1.0f;
};

Balance balance_at(const Document &doc, const BoneConstraint &c, const Target &t, const Transform2D &world,
                   int frame)
{
  Balance b;
  b.angle = t.angle;
  b.scale = t.scale;
  const Transform2D to_layer = world.inverted();
  const Forces forces = forces_at(doc, c, frame);
  const float2 gravity = in_layer(to_layer, {0.0f, -forces.gravity});
  const float2 wind =
      in_layer(to_layer, wind_at(forces.wind, float(frame) / std::max(doc.fps, 1.0f), t.rest_head)) * c.wind_catch;
  if (c.angle_spring.enabled) {
    /* The bend depends on the angle it bends to: home in on it. */
    for (int i = 0; i < 40; i++) {
      const float2 along(std::cos(b.angle), std::sin(b.angle));
      const float push = cross(along, gravity * c.weight + wind) * BEND;
      b.angle += 0.5f * (wrap_pi(t.angle + push - b.angle));
    }
  }
  if (c.position_spring.enabled) {
    b.offset = (gravity * c.weight + wind) * DROP;
  }
  if (c.scale_spring.enabled) {
    const float2 along(std::cos(b.angle), std::sin(b.angle));
    b.scale = t.scale * (1.0f + dot(along, gravity * c.weight + wind) * PULL);
  }
  return b;
}

/* A bone starting to simulate: already bent by gravity and the wind as they are then (where the
 * springs balance them), so it doesn't swing over into that bend on the first frames. When they
 * are already changing (keys under way), it is also already moving with them, as far behind as a
 * spring following them steadily is, so it doesn't fall behind and wobble catching up. */
void start(const Document &doc, const BoneConstraint &c, Motion &m, const Target &t, const Transform2D &world,
           int frame)
{
  m = Motion();
  m.last_head = world.apply(t.strand_head);
  m.last_angle = t.angle;
  m.last_scale = t.scale;
  const Balance now = balance_at(doc, c, t, world, frame), next = balance_at(doc, c, t, world, frame + 1);
  const float fps = std::max(doc.fps, 1.0f);
  const Spring angle = spring_of(c.angle_spring), position = spring_of(c.position_spring),
               scale = spring_of(c.scale_spring);
  const auto lag = [](const Spring &k) { return k.stiffness > 1e-6f ? k.damping / k.stiffness : 0.0f; };
  m.spin = wrap_pi(next.angle - now.angle) * fps;
  m.angle = now.angle - lag(angle) * m.spin;
  m.drift = (next.offset - now.offset) * fps;
  m.offset = now.offset - m.drift * lag(position);
  m.growth = (next.scale - now.scale) * fps;
  m.scale = now.scale - lag(scale) * m.growth;
}

/* One frame forward, in small steps. The head's movement throws the tip off (by Inertia across
 * the bone, by Stretch along it) and the head off its place (by Position's Inertia); each spring
 * pulls back, gravity pulling by the bone's Weight and the wind pushing by its Wind Catch. */
void step(const Document &doc, const BoneConstraint &c, Motion &m, const Target &t, const Transform2D &world,
          int frame)
{
  const BoneSpring &ka = c.angle_spring, &kp = c.position_spring, &ks = c.scale_spring;
  const Spring angle = spring_of(ka), position = spring_of(kp), scale = spring_of(ks);
  const float la = limit_of(ka), lp = limit_of(kp), ls = limit_of(ks);
  const Transform2D to_layer = world.inverted();
  const float2 moved = t.strand_head - to_layer.apply(m.last_head);
  m.last_head = world.apply(t.strand_head);
  /* Gravity pulls down the page, whichever way the bone layer is turned. */
  /* The document's gravity and wind, or the bone's own. The wind changes through the frame. */
  /* Live, the frame doesn't move on: its forces throughout. */
  const Forces forces_now = forces_at(doc, c, frame);
  const Forces forces_before = g_step_seconds > 0.0f ? forces_now : forces_at(doc, c, frame - 1);
  const float2 here = t.rest_head;
  if (!ka.enabled) {
    m.angle = t.angle;
    m.spin = 0.0f;
  }
  if (!kp.enabled) {
    m.offset = m.drift = float2();
  }
  if (!ks.enabled) {
    m.scale = t.scale;
    m.growth = 0.0f;
  }
  /* Enough small steps that even the stiffest spring stays stable. */
  float stiffest = 0.0f;
  for (const auto &[on, k] : {std::pair{ka.enabled, angle}, std::pair{kp.enabled, position}, std::pair{ks.enabled, scale}})
  {
    if (on) {
      stiffest = std::max(stiffest, k.stiffness);
    }
  }
  const float frame_time = g_step_seconds > 0.0f ? g_step_seconds : 1.0f / std::max(doc.fps, 1.0f);
  const int steps = std::clamp(int(std::ceil(std::sqrt(stiffest) * frame_time / 0.3f)), 16, 1024);
  const float dt = frame_time / float(steps);
  const float2 move = moved / float(steps);
  const float rest = std::max(t.length, 1e-4f);
  /* The aim moves smoothly through the frame. */
  const float turn = wrap_pi(t.angle - m.last_angle), grow = t.scale - m.last_scale;
  const float aim_spin = turn / frame_time, aim_growth = grow / frame_time;
  m.last_angle = t.angle;
  m.last_scale = t.scale;
  for (int s = 0; s < steps; s++) {
    const float still = 1.0f - float(s + 1) / float(steps);
    const Forces forces = forces_between(forces_before, forces_now, 1.0f - still);
    const float2 gravity = in_layer(to_layer, {0.0f, -forces.gravity});
    const float2 wind = in_layer(
        to_layer, wind_at(forces.wind, float(frame) / std::max(doc.fps, 1.0f) + g_live_clock - still * frame_time, here)) *
        c.wind_catch;
    const float aim = t.angle - turn * still, size = t.scale - grow * still;
    const float2 along(std::cos(m.angle), std::sin(m.angle));
    const float2 across(-along.y, along.x);
    /* The wind as the bone feels it: drag on how fast the air goes past it (the air's velocity less
     * the bone's own, as Houdini's, Disney's and Unreal's wind), so a bone moving into the wind is
     * pushed harder and one moving with it hardly at all, and strong wind calms swings across it.
     * Still, it is pushed as before. How much the bone's own speed counts grows with the wind:
     * without wind there is no air drag (settling is Settle Speed's). A wind of 1 is air moving at
     * a document unit a second. */
    const float wind_speed = length(wind);
    const float felt = wind_speed / (wind_speed + 1.0f);
    const float2 head_velocity = move / dt;
    /* How much of its own speed counts, kept low enough for the step to stay stable. */
    const auto drag_share = [&](float coefficient) { return std::min(felt, 0.9f / std::max(coefficient * dt, 1e-6f)); };
    if (ka.enabled) {
      /* The strand's tip stays behind as its head moves across it, and the bone stays behind as
       * its aim turns; what Inertia leaves out it is carried along with. */
      const float reach = std::max(t.reach, 1e-4f);
      const float inertia = std::max(ka.inertia, 0.0f);
      m.angle += (1.0f - inertia) * turn / float(steps) - inertia * dot(move, across) / reach;
      const float off = wrap_pi(aim - m.angle);
      /* Its tip's velocity: carried by its head, and swinging round it. */
      const float reach_now = rest * (ks.enabled ? m.scale : t.scale);
      const float2 tip_velocity = head_velocity + across * ((m.spin + (1.0f - inertia) * aim_spin) * reach_now);
      const float share = drag_share(angle.stiffness * BEND * reach_now);
      const float push = cross(along, gravity * c.weight + wind - tip_velocity * share) * BEND;
      m.spin += (angle.stiffness * (off + push) - angle.damping * (m.spin - inertia * aim_spin)) * dt;
      m.angle += m.spin * dt;
      /* Never further round than nearly the other way (or than a little past its limit). */
      const float most = la > 0.0f ? std::min(3.0f * la, 3.0f) : 3.0f;
      const float dev = wrap_pi(m.angle - t.angle);
      if (std::abs(dev) > most) {
        m.angle = t.angle + std::copysign(most, dev);
        m.spin = 0.0f;
      }
    }
    if (ks.enabled) {
      /* ... and as it moves along the bone, which stretches or squashes it. As an elastic
       * pendulum's length, it is also pulled out by its own swing (the faster it swings, the
       * more: centrifugal, by Stretch) and by gravity and the wind along it (by Weight and Wind Catch, as Spine
       * stretches a bone). */
      const float stretch = std::max(ks.stretch, 0.0f);
      m.scale -= stretch * dot(move, along) / rest;
      /* Its own swing (against where it aims, not the body carrying it round), measured against
       * how fast it naturally swings, levelling off so a hard whip stretches it a lot, not
       * without end. */
      const float swing = ka.enabled ? m.spin - std::max(ka.inertia, 0.0f) * aim_spin : 0.0f;
      const float fling = 0.3f * stretch * (1.0f - std::exp(-swing * swing / std::max(angle.stiffness, 1e-3f)));
      /* Along the bone, the air past it less how fast its tip moves that way. */
      const float along_speed = dot(head_velocity, along) + m.growth * rest;
      const float share = drag_share(scale.stiffness * PULL * rest);
      const float pull = (dot(along, gravity * c.weight + wind) - along_speed * share) * PULL;
      m.growth += (scale.stiffness * (size * (1.0f + pull + fling) - m.scale) - scale.damping * (m.growth - aim_growth)) *
                  dt;
      m.scale += m.growth * dt;
      const float ratio = t.scale > 1e-6f ? m.scale / t.scale : 1.0f;
      const float lsq = squash_limit_of(ks);
      const float hi = ls > 0.0f ? 1.0f + 3.0f * ls : 10.0f;
      const float lo = lsq > 0.0f ? std::max(0.1f, 1.0f - 3.0f * lsq) : 0.1f;
      if (ratio > hi || ratio < lo) {
        m.scale = t.scale * std::clamp(ratio, lo, hi);
        m.growth = 0.0f;
      }
    }
    if (kp.enabled) {
      /* The head stays behind as its parent moves. */
      m.offset -= move * std::max(kp.inertia, 0.0f);
      /* The head's velocity: carried by its parent, and jiggling. */
      const float share = drag_share(position.stiffness * DROP);
      const float2 push = (gravity * c.weight + wind - (head_velocity + m.drift) * share) * DROP;
      m.drift += (((push - m.offset) * position.stiffness) - m.drift * position.damping) * dt;
      m.offset += m.drift * dt;
      if (lp > 0.0f && length(m.offset) > 3.0f * lp) {
        m.offset = normalize(m.offset) * (3.0f * lp);
        m.drift = float2();
      }
    }
  }
}

/* Where the springs have the bone, the constraint's influence and limits applied. */
BoneDynamicsState state_of(const BoneConstraint &c, const Motion &m, const Target &t)
{
  const float w = std::clamp(c.influence, 0.0f, 1.0f);
  BoneDynamicsState d;
  d.angle_on = c.angle_spring.enabled;
  d.position_on = c.position_spring.enabled;
  d.scale_on = c.scale_spring.enabled;
  d.angle = t.angle + soft(wrap_pi(m.angle - t.angle), limit_of(c.angle_spring)) * w;
  d.head = t.head + soft(m.offset, limit_of(c.position_spring)) * w;
  const float ratio = t.scale > 1e-6f ? m.scale / t.scale : 1.0f;
  /* Max Stretch and Max Squash each their own way, and never shorter than a tenth. */
  const float change = ratio - 1.0f;
  const float limit = change > 0.0f ? limit_of(c.scale_spring) : squash_limit_of(c.scale_spring);
  d.scale = t.local_scale * std::max(1.0f + soft(change, limit) * w, 0.1f);
  return d;
}

/* How far each dynamic bone's strand reaches: its own length and the longest run of dynamic bones
 * hanging from it. */
std::vector<float> strand_reach(const Layer &armature, const std::vector<int> &level,
                                const std::vector<BonePose> &poses)
{
  const size_t n = armature.bones.size();
  std::vector<float> reach(n, -1.0f);
  const auto of = [&](auto &&self, size_t i, int depth) -> float {
    if (reach[i] >= 0.0f) {
      return reach[i];
    }
    float below = 0.0f;
    for (size_t j = 0; j < n && depth < 256; j++) {
      if (j != i && level[j] >= 0 && armature.bones[j].parent == armature.bones[i].id) {
        below = std::max(below, self(self, j, depth + 1));
      }
    }
    reach[i] = distance(poses[i].head, poses[i].tail) + below;
    return reach[i];
  };
  for (size_t i = 0; i < n; i++) {
    if (level[i] >= 0) {
      of(of, i, 0);
    }
  }
  return reach;
}

/* One frame of the simulation: every dynamic bone stepped on from `before` (or started still
 * where its keys put it when it has no frame before), the ones higher up first so the bones
 * below aim from where they have swung to. `r_states` gets where the frame has them. */
std::map<uint64_t, Motion> advance(const Document &doc, const Layer &armature, int frame, bool live,
                                   const std::map<uint64_t, Motion> *before,
                                   std::map<uint64_t, BoneDynamicsState> &r_states)
{
  Layer at = keyed_layer(armature, frame, live);
  const Transform2D world = layer_world_at(doc, armature, frame, live);
  const std::vector<int> level = dynamic_levels(at);
  const int top = level.empty() ? -1 : *std::max_element(level.begin(), level.end());
  std::map<uint64_t, Motion> now;
  r_states.clear();
  std::vector<float> reach;
  /* Each dynamic bone's strand's top bone, and that bone's head as it is now. */
  std::vector<int> strand(at.bones.size(), -1);
  std::vector<float2> strand_head(at.bones.size());
  {
    std::map<uint64_t, int> index;
    for (int i = 0; i < int(at.bones.size()); i++) {
      index[at.bones[i].id] = i;
    }
    for (int i = 0; i < int(at.bones.size()); i++) {
      int s = level[i] >= 0 ? i : -1;
      for (int depth = 0; s >= 0 && depth < 256; depth++) {
        const auto it = index.find(at.bones[s].parent);
        if (it == index.end() || level[it->second] < 0 || it->second == i) {
          break;
        }
        s = it->second;
      }
      strand[i] = s;
    }
  }
  for (int l = 0; l <= top; l++) {
    at.dynamics_now = r_states;
    const std::vector<BonePose> poses = armature_pose(at, false);
    if (l == 0) {
      reach = strand_reach(at, level, poses);
    }
    for (size_t i = 0; i < at.bones.size(); i++) {
      if (level[i] != l) {
        continue;
      }
      const Bone &b = at.bones[i];
      const BoneConstraint &c = *dynamics_of(b);
      Target t;
      t.angle = pose_angle(poses[i].pose);
      t.head = poses[i].head;
      t.rest_head = poses[i].setup_head;
      if (strand[i] == int(i)) {
        strand_head[i] = poses[i].head;
      }
      t.reach = reach[strand[i]];
      t.strand_head = strand_head[strand[i]];
      t.scale = pose_scale(poses[i].pose);
      t.length = b.length;
      t.local_scale = b.scale.y;
      Motion m;
      const auto it = before ? before->find(b.id) : std::map<uint64_t, Motion>::const_iterator();
      if (!before || it == before->end()) {
        start(doc, c, m, t, world, frame);
      }
      else {
        m = it->second;
        step(doc, c, m, t, world, frame);
      }
      now[b.id] = m;
      r_states[b.id] = state_of(c, m, t);
    }
  }
  return now;
}

/* Where the dynamic bones of `armature` are on `frame`: the frames before it from the cache
 * (simulated from the keys), the frame itself from the bones as they are now, so turning a bone
 * shows its effect at once without changing what is cached. */
bool simulate(const Document &doc, const Layer &armature, int frame,
              std::map<uint64_t, BoneDynamicsState> &r_states)
{
  const int start = std::max(1, doc.simulation_start);
  if (frame < start) {
    return false;
  }
  Simulation &sim = g_simulations[armature.id];
  const uint64_t sig = signature(doc, armature);
  /* While a drag goes on, the frames before keep what they were simulated from: each update would
   * otherwise simulate them all again (a bone without keys changes every frame as it is dragged, and
   * while playing, recording keys it on every frame). Frames played on to are added from there. The
   * signature stays the old one, so they are all simulated again once it ends. */
  const bool held = g_hold_cache && sim.signature != 0 && sim.start == start;
  if ((sim.signature != sig || sim.start != start) && !held) {
    sim = {};
    sim.signature = sig;
    sim.start = start;
  }
  std::map<uint64_t, BoneDynamicsState> unused;
  while (int(sim.frames.size()) < frame - start) {
    const int f = start + int(sim.frames.size());
    std::map<uint64_t, Motion> next =
        advance(doc, armature, f, false, sim.frames.empty() ? nullptr : &sim.frames.back(), unused);
    sim.frames.push_back(std::move(next));
  }
  /* The frame shown, live, on from the frame just before (the cache may hold later frames too). */
  const int before = frame - start - 1;
  sim.shown = advance(doc, armature, frame, true, before >= 0 ? &sim.frames[before] : nullptr, r_states);
  return true;
}

/* --- Live dynamics -------------------------------------------------------------------------- */

/* What the bones aim at right now (their values and the bone layer's place): live dynamics run
 * while it changes. */
uint64_t aim_signature(const Document &doc, const Layer &armature)
{
  Hash h;
  for (const Bone &b : armature.bones) {
    h.value(b.position);
    h.value(b.angle);
    h.value(b.scale);
  }
  std::vector<const Layer *> path;
  layer_path(doc.layers, armature.id, path);
  for (const Layer *l : path) {
    h.value(l->position);
    h.value(l->rotation);
    h.value(l->scale);
  }
  return h.h;
}

/* A bone layer's live simulation: started when its aim changes on a frame and run in real time
 * for as long as that frame is shown; scrubbing (or playing) goes back to the frames' own results. */
struct Live {
  int frame = -1;
  uint64_t aim = 0;
  bool running = false;
  float clock = 0.0f;
  std::map<uint64_t, Motion> motion;
};
std::map<uint64_t, Live> g_live;

}  // namespace

void dynamics_live_tick(Document &doc, double seconds, bool allowed)
{
  const int frame = doc.frame_current;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer()) {
      continue;
    }
    Layer &armature = *layer_find(doc, flat->id);
    Live &live = g_live[armature.id];
    const uint64_t aim = aim_signature(doc, armature);
    if (!allowed || frame == 0 || armature.dynamics_now.empty() || live.frame != frame) {
      /* A new frame (or none to run on): nothing live, until the aim changes here. */
      live = Live();
      live.frame = frame;
      live.aim = aim;
      if (const auto sim = g_simulations.find(armature.id); sim != g_simulations.end() && allowed && frame != 0) {
        live.motion = sim->second.shown;
      }
      continue;
    }
    if (aim != live.aim) {
      /* Starting: from the bones as they were shown just before the change (the frame's result now
       * may already be another, an unkeyed change applying to every frame). */
      if (!live.running && !live.motion.empty()) {
        live.running = true;
      }
      live.aim = aim;
    }
    if (!live.running) {
      /* Quiet: keep what is shown, to start from. */
      if (const auto sim = g_simulations.find(armature.id); sim != g_simulations.end()) {
        live.motion = sim->second.shown;
      }
      continue;
    }
    /* In real time, capped so a hitch doesn't throw the bones. */
    const float dt = float(std::clamp(seconds, 0.0, 0.05));
    live.clock += dt;
    std::map<uint64_t, BoneDynamicsState> states;
    g_step_seconds = std::max(dt, 1e-4f);
    g_live_clock = live.clock;
    live.motion = advance(doc, armature, frame, true, &live.motion, states);
    g_step_seconds = 0.0f;
    g_live_clock = 0.0f;
    for (const auto &[id, state] : states) {
      armature.dynamics_now[id] = state;
    }
  }
}

void dynamics_live_quiet(Document &doc, uint64_t armature_id)
{
  Layer *armature = layer_find(doc, armature_id);
  if (!armature) {
    return;
  }
  Live &live = g_live[armature_id];
  live = Live();
  live.frame = doc.frame_current;
  live.aim = aim_signature(doc, *armature);
  if (const auto sim = g_simulations.find(armature_id); sim != g_simulations.end()) {
    live.motion = sim->second.shown;
  }
}

bool dynamics_live_running()
{
  for (const auto &[id, live] : g_live) {
    if (live.running) {
      return true;
    }
  }
  return false;
}

void dynamics_use_own_gravity(const Document &doc, BoneConstraint &c, bool on)
{
  if (on && !c.own_gravity_set) {
    c.gravity = doc.gravity;
    c.own_gravity_set = true;
  }
  c.own_gravity = on;
}

void dynamics_use_own_wind(const Document &doc, BoneConstraint &c, bool on)
{
  if (on && !c.own_wind_set) {
    c.wind = doc.wind;
    c.wind_direction = doc.wind_direction;
    c.wind_gusts = doc.wind_gusts;
    c.wind_gust_speed = doc.wind_gust_speed;
    c.wind_turbulence = doc.wind_turbulence;
    c.own_wind_set = true;
  }
  c.own_wind = on;
}

void dynamics_hold_cache(bool hold)
{
  g_hold_cache = hold;
}

void dynamics_clear_cache()
{
  g_simulations.clear();
  g_live.clear();
}

void dynamics_update(Document &doc)
{
  const int frame = doc.frame_current;
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer()) {
      continue;
    }
    Layer &armature = *layer_find(doc, flat->id);
    armature.pose_frame = frame;
    armature.dynamics_now.clear();
    if (frame == 0 || !has_dynamics(armature)) {
      continue;
    }
    std::map<uint64_t, BoneDynamicsState> states;
    if (simulate(doc, armature, frame, states)) {
      armature.dynamics_now = std::move(states);
    }
  }
}

int dynamics_bake(Document &doc, Layer *only)
{
  struct Baked {
    Layer *armature;
    uint64_t bone;
    int frame;
    float angle;
    float2 position;
    float scale;
    bool angle_on, position_on, scale_on;
  };
  std::vector<Baked> baked;
  const int keep = doc.frame_current;
  const int start = std::max(1, doc.simulation_start);
  /* Work every frame out first: keys set while baking would change what later frames aim at. */
  for (int f = start; f <= doc.frame_end; f++) {
    /* The frame from its keys (dynamics work from the bones as shown). */
    animation_evaluate(doc, f);
    for (const Layer *flat : document_layers_flat(doc)) {
      if (!flat->is_bone_layer() || (only && flat->id != only->id)) {
        continue;
      }
      Layer &armature = *layer_find(doc, flat->id);
      if (armature.dynamics_now.empty()) {
        continue;
      }
      Layer at = Layer();
      at.type = armature.type;
      at.pose_frame = f;
      at.bones = armature.bones;
      at.dynamics_now = armature.dynamics_now;
      for (Bone &b : at.bones) {
        b.position = bone_position_at(b, f, true);
        b.angle = bone_angle_at(b, f, true);
        b.scale = at_frame(b.scale_keys, b.scale, f);
      }
      action_bone_actions_add_at(armature, f, at.bones);
      const std::vector<BonePose> poses = armature_pose(at, false);
      for (size_t i = 0; i < at.bones.size(); i++) {
        const Bone &b = at.bones[i];
        const auto it = armature.dynamics_now.find(b.id);
        if (it == armature.dynamics_now.end()) {
          continue;
        }
        const BonePose *parent = bone_pose_find(poses, b.parent);
        const Transform2D local = (parent ? parent->pose : bone_outer_frame(b, false)).inverted() * poses[i].pose;
        const BoneDynamicsState &d = it->second;
        baked.push_back({&armature, b.id, f, pose_angle(local), float2(local.tx, local.ty), d.scale, d.angle_on,
                         d.position_on, d.scale_on});
      }
    }
  }
  int count = 0;
  for (const Baked &k : baked) {
    Bone *bone = bone_find(*k.armature, k.bone);
    if (!bone) {
      continue;
    }
    const auto key = [&](auto &ch, const auto &value, const auto &plain) {
      if (!ch.animated()) {
        ch.set_key(0, plain - ch.from_action_bones);
      }
      ch.set_key(k.frame, value);
      count++;
    };
    if (k.angle_on) {
      /* Whole turns are kept: the nearest angle to the keyed one. */
      /* Its keys hold its Angle and Transform: the change from the setup pose. */
      const float near = bone_angle_at(*bone, k.frame, true);
      key(bone->angle_keys, near + wrap_pi(k.angle - near) - bone->setup_angle, bone->angle - bone->setup_angle);
    }
    if (k.position_on) {
      key(bone->position_keys, bone_transform_from_position(*bone, k.position),
          bone_transform_from_position(*bone, bone->position));
    }
    if (k.scale_on) {
      /* Dynamics stretch the length (Y); the width stays as keyed. */
      const float2 keyed = at_frame(bone->scale_keys, bone->scale, k.frame);
      key(bone->scale_keys, float2(keyed.x, k.scale), bone->scale);
    }
  }
  /* The keys now hold the motion: the springs stop. */
  for (const Layer *flat : document_layers_flat(doc)) {
    if (!flat->is_bone_layer() || (only && flat->id != only->id)) {
      continue;
    }
    for (Bone &b : layer_find(doc, flat->id)->bones) {
      for (BoneConstraint &c : b.constraints) {
        if (c.type == BoneConstraintType::Dynamics) {
          c.enabled = false;
        }
      }
    }
  }
  doc.frame_current = keep;
  dynamics_clear_cache();
  animation_evaluate(doc, keep);
  return count;
}

}  // namespace oanim
