/* Driver operators (data/driver.hh, anim/drivers.hh): add, set, remove, and Blender's clipboard of
 * Copy As New Driver / Copy Driver / Paste Driver. A field is named by its layer, path and component
 * (-1: all of it: both numbers of a position, the four of a colour). A shape colour's operators can
 * act on every selected shape's same colour (`selected_shapes`), as the Material panel edits do. */

#include <algorithm>
#include <cstdlib>
#include <string>

#include <nlohmann/json.hpp>

#include "anim/animation.hh"
#include "anim/drivers.hh"
#include "core/log.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

using json = nlohmann::json;

/* The field the operator names: one reference per number. With `selected_shapes` on a shape colour,
 * that colour of every selected shape. */
static std::vector<DriverRef> op_refs(Context &C)
{
  Document &doc = *C.document;
  const uint64_t layer = std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10);
  const std::string path = op_arg(C, "path");
  const std::string component_arg = op_arg(C, "component");
  const int component = component_arg.empty() ? -1 : std::atoi(component_arg.c_str());
  std::vector<DriverRef> refs = driver_field_refs(doc, layer, path, component);
  const bool fill = path.size() >= 5 && path.compare(path.size() - 5, 5, ".fill") == 0;
  const bool line = path.size() >= 5 && path.compare(path.size() - 5, 5, ".line") == 0;
  if (op_arg(C, "selected_shapes") == "1" && path.rfind("shapes[", 0) == 0 && (fill || line)) {
    for (Layer *l : editable_layers(C)) {
      const std::vector<bool> selected = drawing_selected_shapes(l->drawing);
      for (size_t s = 0; s < selected.size(); s++) {
        const std::string other = driver_shape_color_path(l->drawing.shapes[s].id, fill);
        if (selected[s] && !(l->id == layer && other == path)) {
          for (const DriverRef &r : driver_field_refs(doc, l->id, other, component)) {
            refs.push_back(r);
          }
        }
      }
    }
  }
  return refs;
}

/* The fields one field stands for (several when selected_shapes adds shapes): in groups of `size`. */
static size_t field_size(Context &C)
{
  const uint64_t layer = std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10);
  const std::string component_arg = op_arg(C, "component");
  const int component = component_arg.empty() ? -1 : std::atoi(component_arg.c_str());
  return driver_field_refs(*C.document, layer, op_arg(C, "path"), component).size();
}

static void put_driver(Document &doc, Driver d, const DriverRef &target)
{
  d.target = target;
  d.error.clear();
  if (Driver *existing = driver_find(doc, target)) {
    *existing = std::move(d);
  }
  else {
    doc.drivers.push_back(std::move(d));
  }
}

static bool vars_from_json(const std::string &text, std::vector<DriverVar> *r_vars)
{
  const json j = json::parse(text, nullptr, false);
  if (!j.is_array()) {
    return false;
  }
  r_vars->clear();
  for (const json &v : j) {
    if (!v.is_object()) {
      return false;
    }
    /* Numbers may come as numbers or as text ("12"), as the other arguments do; anything else is
     * refused rather than thrown. */
    const auto number = [&](const char *key, double fallback, double *r_value) {
      const auto it = v.find(key);
      if (it == v.end() || it->is_null()) {
        *r_value = fallback;
        return true;
      }
      if (it->is_number()) {
        *r_value = it->get<double>();
        return true;
      }
      if (it->is_string()) {
        const std::string t = it->get<std::string>();
        char *end = nullptr;
        *r_value = std::strtod(t.c_str(), &end);
        return !t.empty() && end && *end == '\0';
      }
      return false;
    };
    const auto text_of = [&](const char *key, const char *fallback, std::string *r_text) {
      const auto it = v.find(key);
      if (it == v.end() || it->is_null()) {
        *r_text = fallback;
        return true;
      }
      if (!it->is_string()) {
        return false;
      }
      *r_text = it->get<std::string>();
      return true;
    };
    DriverVar var;
    double layer = 0.0, component = -1.0;
    if (!text_of("name", "var", &var.name) || !text_of("path", "", &var.source.path) ||
        !number("layer", 0.0, &layer) || !number("component", -1.0, &component) || layer < 0.0)
    {
      return false;
    }
    /* Layer ids above 2^53 (the camera's, the simulation's) don't survive a double: those come as text. */
    const auto it = v.find("layer");
    var.source.layer = it != v.end() && it->is_number_unsigned() ? it->get<uint64_t>()
                       : it != v.end() && it->is_string()   ? std::strtoull(it->get<std::string>().c_str(), nullptr, 10)
                                                            : uint64_t(layer);
    var.source.component = int(component);
    r_vars->push_back(var);
  }
  return true;
}

/* The settings arguments of driver_add / driver_set onto `d`: type, expression, vars (JSON),
 * remap (1/0) with from_min, from_max, to_min, to_max, clamp. */
static bool apply_settings(Context &C, Driver &d)
{
  const std::string type = op_arg(C, "type");
  if (!type.empty()) {
    if (type == "EXPRESSION") {
      d.type = DriverType::Expression;
    }
    else if (type == "AVERAGE") {
      d.type = DriverType::Average;
    }
    else if (type == "SUM") {
      d.type = DriverType::Sum;
    }
    else if (type == "MIN") {
      d.type = DriverType::Min;
    }
    else if (type == "MAX") {
      d.type = DriverType::Max;
    }
    else {
      report(ReportType::Error, "A driver's type is EXPRESSION, AVERAGE, SUM, MIN or MAX, not '" + type + "'");
      return false;
    }
  }
  if (const std::string e = op_arg(C, "expression"); !e.empty()) {
    d.expression = e;
  }
  if (const std::string v = op_arg(C, "vars"); !v.empty() && !vars_from_json(v, &d.vars)) {
    report(ReportType::Error, "`vars` is a JSON list: [{\"name\", \"layer\", \"path\", \"component\"}]");
    return false;
  }
  if (const std::string r = op_arg(C, "remap"); !r.empty()) {
    d.remap = r == "1";
  }
  const auto number = [&](const char *name, float *field) {
    if (const std::string s = op_arg(C, name); !s.empty()) {
      *field = std::strtof(s.c_str(), nullptr);
    }
  };
  number("from_min", &d.from_min);
  number("from_max", &d.from_max);
  number("to_min", &d.to_min);
  number("to_max", &d.to_max);
  if (const std::string c = op_arg(C, "clamp"); !c.empty()) {
    d.clamp = c == "1";
  }
  return true;
}

static void driver_operator_add(const char *idname, const char *name, const char *description, unsigned flag,
                                std::function<OpStatus(Context &C)> exec)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = flag;
  ot.poll = [](Context &C) { return C.document != nullptr; };
  ot.exec = std::move(exec);
  operators().add(std::move(ot));
}

/* After drivers changed: worked out at once, so their values show. */
static OpStatus drivers_changed(Context &C)
{
  drivers_evaluate(*C.document);
  return OpStatus::Finished;
}

void driver_operators_register()
{
  driver_operator_add("anim.driver_add", "Add Driver",
                      "Drive a field (`layer`, `path`, `component`): its value worked out from `vars` (a JSON list "
                      "of {name, layer, path, component}) by `expression` (or `type` AVERAGE, SUM, MIN, MAX), every "
                      "frame. An expression alone (\"sin(frame / 4)\") needs no variables",
                      OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                        const std::vector<DriverRef> refs = op_refs(C);
                        if (refs.empty()) {
                          report(ReportType::Error, "That can't have a driver (only numbers, checkboxes, dropdowns and colours of the scene can)");
                          return OpStatus::Cancelled;
                        }
                        Driver d;
                        if (!apply_settings(C, d)) {
                          return OpStatus::Cancelled;
                        }
                        const bool given = !op_arg(C, "expression").empty();
                        for (const DriverRef &r : refs) {
                          Driver one = d;
                          if (!given) {
                            /* No expression: the first variable as it is, or (none) the value now. */
                            float v = 0.0f;
                            driver_ref_read(*C.document, r, &v);
                            one.expression = one.vars.empty() ? std::to_string(v) : one.vars.front().name;
                          }
                          put_driver(*C.document, one, r);
                        }
                        return drivers_changed(C);
                      });

  driver_operator_add("anim.driver_set", "Change Driver",
                      "Change a field's drivers: any of `type`, `expression`, `vars`, `remap` (1/0), `from_min`, "
                      "`from_max`, `to_min`, `to_max`, `clamp`",
                      OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                        int changed = 0;
                        for (const DriverRef &r : op_refs(C)) {
                          if (Driver *d = driver_find(*C.document, r)) {
                            if (!apply_settings(C, *d)) {
                              return OpStatus::Cancelled;
                            }
                            changed++;
                          }
                        }
                        if (changed == 0) {
                          report(ReportType::Warning, "That field has no driver");
                          return OpStatus::Cancelled;
                        }
                        return drivers_changed(C);
                      });

  driver_operator_add("anim.driver_remove", "Delete Driver",
                      "Take a field's drivers off: it keeps the value they last gave it",
                      OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                        const std::vector<DriverRef> refs = op_refs(C);
                        std::vector<Driver> &drivers = C.document->drivers;
                        const size_t before = drivers.size();
                        std::erase_if(drivers, [&](const Driver &d) {
                          return std::find(refs.begin(), refs.end(), d.target) != refs.end();
                        });
                        if (drivers.size() == before) {
                          report(ReportType::Warning, "That field has no driver");
                          return OpStatus::Cancelled;
                        }
                        return drivers_changed(C);
                      });

  driver_operator_add("anim.driver_copy_new", "Copy As New Driver",
                      "Copy a driver that reads this field: Paste Driver on another field makes it follow this one",
                      OPTYPE_REGISTER, [](Context &C) {
                        const uint64_t layer = std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10);
                        const std::string component_arg = op_arg(C, "component");
                        const int component = component_arg.empty() ? -1 : std::atoi(component_arg.c_str());
                        const std::vector<DriverRef> refs = driver_field_refs(*C.document, layer, op_arg(C, "path"),
                                                                              component);
                        if (refs.empty()) {
                          report(ReportType::Error, "That can't drive anything");
                          return OpStatus::Cancelled;
                        }
                        driver_clipboard().clear();
                        for (const DriverRef &r : refs) {
                          driver_clipboard().push_back(driver_copying(r));
                        }
                        report(ReportType::Info, "Copied as a new driver: " + driver_ref_label(*C.document, refs.front()) +
                                                     (refs.size() > 1 ? " (and the rest of it)" : ""));
                        return OpStatus::Cancelled;
                      });

  driver_operator_add("anim.driver_copy", "Copy Driver", "Copy a field's drivers, to paste on another field",
                      OPTYPE_REGISTER, [](Context &C) {
                        const uint64_t layer = std::strtoull(op_arg(C, "layer").c_str(), nullptr, 10);
                        const std::string component_arg = op_arg(C, "component");
                        const int component = component_arg.empty() ? -1 : std::atoi(component_arg.c_str());
                        std::vector<Driver> copied;
                        for (const DriverRef &r : driver_field_refs(*C.document, layer, op_arg(C, "path"), component)) {
                          if (const Driver *d = driver_find(*C.document, r)) {
                            copied.push_back(*d);
                          }
                          else {
                            /* A part without a driver: copied as its own value. */
                            float v = 0.0f;
                            driver_ref_read(*C.document, r, &v);
                            Driver plain;
                            plain.expression = std::to_string(v);
                            copied.push_back(plain);
                          }
                        }
                        if (copied.empty()) {
                          report(ReportType::Warning, "That field has no driver");
                          return OpStatus::Cancelled;
                        }
                        driver_clipboard() = std::move(copied);
                        report(ReportType::Info, "Copied the driver");
                        return OpStatus::Cancelled;
                      });

  driver_operator_add("anim.driver_paste", "Paste Driver",
                      "Drive a field with the copied driver: one copied number onto each part of it, or as many "
                      "as it has, part by part; `part` picks one of several copied (Scale's Y onto a single number)",
                      OPTYPE_REGISTER | OPTYPE_UNDO, [](Context &C) {
                        const std::vector<Driver> &clip = driver_clipboard();
                        if (clip.empty()) {
                          report(ReportType::Warning, "No driver copied: right-click a field > Copy As New Driver");
                          return OpStatus::Cancelled;
                        }
                        const std::vector<DriverRef> refs = op_refs(C);
                        const size_t size = std::max<size_t>(field_size(C), 1);
                        const int part = op_arg_int(C, "part", -1);
                        if (part >= int(clip.size())) {
                          report(ReportType::Warning, "The copied driver has " + std::to_string(clip.size()) + " numbers");
                          return OpStatus::Cancelled;
                        }
                        if (refs.empty() || (part < 0 && clip.size() != 1 && clip.size() != size)) {
                          report(ReportType::Warning, "The copied driver has " + std::to_string(clip.size()) +
                                                          " numbers; this field has " + std::to_string(size));
                          return OpStatus::Cancelled;
                        }
                        int pasted = 0;
                        for (size_t i = 0; i < refs.size(); i++) {
                          const Driver &d = part >= 0 ? clip[size_t(part)] : clip.size() == 1 ? clip[0] : clip[i % size];
                          const bool reads_itself = std::any_of(d.vars.begin(), d.vars.end(), [&](const DriverVar &v) {
                            return v.source == refs[i];
                          });
                          if (reads_itself) {
                            /* A field driven by its own value would loop. */
                            continue;
                          }
                          put_driver(*C.document, d, refs[i]);
                          pasted++;
                        }
                        if (pasted == 0) {
                          report(ReportType::Warning, "A field can't be driven by its own value");
                          return OpStatus::Cancelled;
                        }
                        return drivers_changed(C);
                      });
}

}  // namespace oanim
