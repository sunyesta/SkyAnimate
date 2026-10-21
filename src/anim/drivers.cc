#include "anim/drivers.hh"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>

#include "anim/animation.hh"
#include "anim/expression.hh"
#include "data/document.hh"

namespace oanim {

/* --- The numbers drivers set and read ---------------------------------------------------------- */

namespace {

/* Where a referenced number lives: the value, and for a channel what it last recorded and what smart
 * actions add, so a driven value never reads as an edit. */
struct Slot {
  float *value = nullptr;
  float *last = nullptr;
  bool *has_last = nullptr;
  float *smart = nullptr;
};

/* "shapes[12].fill": the shape's id and which colour. */
bool parse_shape_color(const std::string &path, uint64_t *r_id, bool *r_fill)
{
  if (path.rfind("shapes[", 0) != 0) {
    return false;
  }
  const size_t close = path.find(']');
  if (close == std::string::npos) {
    return false;
  }
  const std::string rest = path.substr(close + 1);
  if (rest != ".fill" && rest != ".line") {
    return false;
  }
  *r_id = std::strtoull(path.c_str() + 7, nullptr, 10);
  *r_fill = rest == ".fill";
  return true;
}

Color *shape_color(Document &doc, uint64_t layer_id, const std::string &path)
{
  uint64_t id = 0;
  bool fill = false;
  if (!parse_shape_color(path, &id, &fill)) {
    return nullptr;
  }
  Layer *layer = layer_find(doc, layer_id);
  if (!layer || !layer->has_drawing()) {
    return nullptr;
  }
  const int s = drawing_shape_index(layer->drawing, id);
  if (s < 0) {
    return nullptr;
  }
  Style &style = layer->drawing.shapes[size_t(s)].style;
  return fill ? &style.fill : &style.line;
}

bool slot_find(Document &doc, const DriverRef &ref, Slot *r_slot)
{
  if (Color *c = shape_color(doc, ref.layer, ref.path)) {
    if (ref.component < 0 || ref.component > 3) {
      return false;
    }
    float *parts[4] = {&c->r, &c->g, &c->b, &c->a};
    r_slot->value = parts[ref.component];
    return true;
  }
  Layer *layer = layer_find(doc, ref.layer);
  ChannelRef ch;
  if (!layer || !layer_channel(*layer, ref.path, &ch)) {
    return false;
  }
  if (ch.scalar) {
    if (ref.component > 0) {
      return false;
    }
    *r_slot = {ch.scalar_value, &ch.scalar->last, &ch.scalar->has_last, &ch.scalar->smart};
    return true;
  }
  if (ch.vector && (ref.component == 0 || ref.component == 1)) {
    const bool x = ref.component == 0;
    *r_slot = {x ? &ch.vector_value->x : &ch.vector_value->y, x ? &ch.vector->last.x : &ch.vector->last.y,
               &ch.vector->has_last, x ? &ch.vector->smart.x : &ch.vector->smart.y};
    return true;
  }
  return false;
}

}  // namespace

int driver_ref_size(Document &doc, uint64_t layer_id, const std::string &path)
{
  if (shape_color(doc, layer_id, path)) {
    return 4;
  }
  Layer *layer = layer_find(doc, layer_id);
  ChannelRef ch;
  if (!layer || !layer_channel(*layer, path, &ch)) {
    return 0;
  }
  return ch.vector ? 2 : 1;
}

bool driver_ref_read(Document &doc, const DriverRef &ref, float *r_value)
{
  Slot slot;
  if (!slot_find(doc, ref, &slot)) {
    return false;
  }
  *r_value = *slot.value;
  return true;
}

std::string driver_ref_label(Document &doc, const DriverRef &ref)
{
  Layer *layer = layer_find(doc, ref.layer);
  if (!layer) {
    return "(a layer that's gone)";
  }
  uint64_t id = 0;
  bool fill = false;
  if (parse_shape_color(ref.path, &id, &fill)) {
    const int s = layer->has_drawing() ? drawing_shape_index(layer->drawing, id) : -1;
    const std::string shape = s >= 0 && !layer->drawing.shapes[size_t(s)].name.empty() ? layer->drawing.shapes[size_t(s)].name
                                                                                      : std::string("a shape");
    const char *parts = "RGBA";
    return layer->name + " › " + shape + ": " + (fill ? "Fill Color" : "Line Color") +
           (ref.component >= 0 && ref.component < 4 ? std::string(" ") + parts[ref.component] : std::string());
  }
  std::string label = channel_label(*layer, ref.path, true);
  if (driver_ref_size(doc, ref.layer, ref.path) == 2 && ref.component >= 0) {
    label += ref.component == 0 ? " X" : " Y";
  }
  return label;
}

Driver *driver_find(Document &doc, const DriverRef &target)
{
  for (Driver &d : doc.drivers) {
    if (d.target == target) {
      return &d;
    }
  }
  return nullptr;
}

std::vector<DriverRef> driver_field_refs(Document &doc, uint64_t layer, const std::string &path, int component)
{
  const int size = driver_ref_size(doc, layer, path);
  std::vector<DriverRef> refs;
  if (size == 0) {
    return refs;
  }
  if (size == 1) {
    refs.push_back({layer, path, -1});
  }
  else if (component >= 0 && component < size) {
    refs.push_back({layer, path, component});
  }
  else {
    for (int c = 0; c < size; c++) {
      refs.push_back({layer, path, c});
    }
  }
  return refs;
}

std::string driver_shape_color_path(uint64_t shape_id, bool fill)
{
  return "shapes[" + std::to_string(shape_id) + "]" + (fill ? ".fill" : ".line");
}

std::vector<Driver> &driver_clipboard()
{
  static std::vector<Driver> clipboard;
  return clipboard;
}

Driver driver_copying(const DriverRef &source)
{
  Driver d;
  d.type = DriverType::Expression;
  d.expression = "var";
  d.vars.push_back({"var", source});
  return d;
}

/* --- Working them out --------------------------------------------------------------------------- */

/* Compiled expressions, by their text (a file has a handful; they're kept for good). */
static const Expression *compiled(const std::string &text, std::string *r_error)
{
  struct Entry {
    Expression expression;
    std::string error;
  };
  static std::map<std::string, Entry> cache;
  auto it = cache.find(text);
  if (it == cache.end()) {
    Entry e;
    expression_compile(text, &e.expression, &e.error);
    it = cache.emplace(text, std::move(e)).first;
  }
  *r_error = it->second.error;
  return it->second.error.empty() ? &it->second.expression : nullptr;
}

static bool driver_value(Document &doc, Driver &d, double *r_value)
{
  std::vector<double> values;
  values.reserve(d.vars.size());
  for (const DriverVar &v : d.vars) {
    float f = 0.0f;
    if (!driver_ref_read(doc, v.source, &f)) {
      d.error = "Variable '" + v.name + "' reads something that's gone";
      return false;
    }
    values.push_back(double(f));
  }
  double result = 0.0;
  if (d.type == DriverType::Expression) {
    std::string error;
    const Expression *e = compiled(d.expression, &error);
    if (!e) {
      d.error = error;
      return false;
    }
    std::vector<double> named;
    named.reserve(e->names.size());
    for (const std::string &name : e->names) {
      if (name == "frame") {
        named.push_back(double(doc.frame_current));
        continue;
      }
      const auto it = std::find_if(d.vars.begin(), d.vars.end(), [&](const DriverVar &v) { return v.name == name; });
      if (it == d.vars.end()) {
        d.error = "No variable called '" + name + "'";
        return false;
      }
      named.push_back(values[size_t(it - d.vars.begin())]);
    }
    result = expression_run(*e, named);
  }
  else {
    if (values.empty()) {
      d.error = "No variables to combine";
      return false;
    }
    double sum = 0.0, lo = values[0], hi = values[0];
    for (const double v : values) {
      sum += v;
      lo = std::min(lo, v);
      hi = std::max(hi, v);
    }
    result = d.type == DriverType::Sum ? sum : d.type == DriverType::Average ? sum / double(values.size())
                                          : d.type == DriverType::Min      ? lo
                                                                            : hi;
  }
  if (d.remap) {
    const double span = double(d.from_max) - double(d.from_min);
    double t = span != 0.0 ? (result - double(d.from_min)) / span : 0.0;
    if (d.clamp) {
      t = std::clamp(t, 0.0, 1.0);
    }
    result = double(d.to_min) + (double(d.to_max) - double(d.to_min)) * t;
  }
  if (!std::isfinite(result)) {
    d.error = "The result isn't a number (a division by zero?)";
    return false;
  }
  *r_value = result;
  return true;
}

void drivers_evaluate(Document &doc)
{
  std::vector<Driver> &drivers = doc.drivers;
  if (drivers.empty()) {
    return;
  }
  /* Each after the drivers whose numbers it reads; a loop stops the drivers in it. */
  const size_t n = drivers.size();
  std::vector<int> state(n, 0);
  std::vector<size_t> order;
  order.reserve(n);
  std::vector<bool> looped(n, false);
  const auto visit = [&](auto &&self, size_t i) -> bool {
    if (state[i] == 2) {
      return true;
    }
    if (state[i] == 1) {
      looped[i] = true;
      return false;
    }
    state[i] = 1;
    bool ok = true;
    for (const DriverVar &v : drivers[i].vars) {
      for (size_t j = 0; j < n; j++) {
        if (drivers[j].target == v.source && !self(self, j)) {
          looped[i] = true;
          ok = false;
        }
      }
    }
    state[i] = 2;
    order.push_back(i);
    return ok;
  };
  for (size_t i = 0; i < n; i++) {
    visit(visit, i);
  }
  bool simulation = false;
  for (const size_t i : order) {
    Driver &d = drivers[i];
    d.error.clear();
    if (looped[i]) {
      d.error = "Loop: it depends on its own value";
      continue;
    }
    Slot slot;
    if (!slot_find(doc, d.target, &slot)) {
      d.error = "What it drives is gone";
      continue;
    }
    double value = 0.0;
    if (!driver_value(doc, d, &value)) {
      continue;
    }
    float v = float(value);
    if (shape_color(doc, d.target.layer, d.target.path)) {
      v = std::clamp(v, 0.0f, 1.0f);
    }
    *slot.value = v;
    if (slot.last) {
      /* Recorded as evaluated, with nothing of smart actions in it: never an edit, never keyed. */
      *slot.smart = 0.0f;
      *slot.last = v;
      *slot.has_last = true;
    }
    simulation = simulation || d.target.layer == SIMULATION_LAYER_ID;
  }
  if (simulation) {
    simulation_values_out(doc);
  }
}

}  // namespace oanim
