#include "python/py_runtime.hh"

#include <pybind11/embed.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

#include "anim/animation.hh"
#include "anim/deform.hh"
#include "anim/draw_order.hh"
#include "anim/dynamics.hh"
#include "anim/reference.hh"
#include "data/bones.hh"
#include "assets/asset_library.hh"
#include "assets/brushes.hh"
#include "core/crash.hh"
#include "core/log.hh"
#include "gpu/brush_stroke.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/file_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "rna/rna.hh"
#include "wm/context.hh"
#include "wm/event_system.hh"
#include "wm/keymap.hh"
#include "wm/remote.hh"
#include "editors/editors.hh"

namespace py = pybind11;

namespace sky::python {

static Context *g_context = nullptr;
static bool g_initialized = false;
static std::filesystem::path g_scripts_dir;

static Context &current_context()
{
  if (!g_context) {
    throw std::runtime_error("SkyAnimate context is not available");
  }
  return *g_context;
}

static std::string path_to_utf8(const std::filesystem::path &path)
{
  const std::u8string utf8 = path.u8string();
  return std::string(utf8.begin(), utf8.end());
}

static std::string format_exception(py::error_already_set &error)
{
  try {
    py::module_ traceback = py::module_::import("traceback");
    py::list lines = traceback.attr("format_exception")(error.type(), error.value(), error.trace());
    std::string text;
    for (py::handle line : lines) {
      text += line.cast<std::string>();
    }
    return text;
  }
  catch (...) {
    return error.what();
  }
}

/* ---------------------------------------------------------------------------------
 * RNA <-> Python values
 */

static py::object value_to_py(const rna::Value &value)
{
  return std::visit([](const auto &v) -> py::object { return py::cast(v); }, value);
}

static rna::Value value_from_py(const rna::PropertyRNA &prop, py::handle obj)
{
  const auto type_error = [&]() {
    return py::type_error("'" + prop.identifier + "' expects " + rna::prop_type_name(prop.type) + ", got " +
                          std::string(py::str(py::type::handle_of(obj).attr("__name__"))));
  };
  const bool is_bool = py::isinstance<py::bool_>(obj);
  switch (prop.type) {
    case rna::PropType::Bool:
      if (!is_bool) {
        throw type_error();
      }
      return obj.cast<bool>();
    case rna::PropType::Int:
      if (is_bool || !py::isinstance<py::int_>(obj)) {
        throw type_error();
      }
      return obj.cast<int>();
    case rna::PropType::Float:
      if (is_bool || !(py::isinstance<py::float_>(obj) || py::isinstance<py::int_>(obj))) {
        throw type_error();
      }
      return obj.cast<float>();
    case rna::PropType::String:
      if (!py::isinstance<py::str>(obj)) {
        throw type_error();
      }
      return obj.cast<std::string>();
    case rna::PropType::Enum: {
      if (!py::isinstance<py::str>(obj)) {
        throw type_error();
      }
      const int index = prop.enum_index(obj.cast<std::string>());
      if (index < 0) {
        std::string options;
        for (const rna::EnumItem &item : prop.items) {
          options += (options.empty() ? "" : ", ") + item.identifier;
        }
        throw py::value_error("'" + prop.identifier + "' must be one of: " + options);
      }
      return index;
    }
  }
  throw type_error();
}

static OpStatus op_status_from_name(const std::string &name)
{
  if (name == "FINISHED") {
    return OpStatus::Finished;
  }
  if (name == "PASS_THROUGH") {
    return OpStatus::PassThrough;
  }
  return OpStatus::Cancelled;
}

static std::string to_upper(std::string text)
{
  std::transform(text.begin(), text.end(), text.begin(), [](unsigned char c) { return char(std::toupper(c)); });
  return text;
}

}  // namespace sky::python

/* ---------------------------------------------------------------------------------
 * The `_sky` module. Keep it small: the friendly API lives in scripts/modules/sky.
 */

PYBIND11_EMBEDDED_MODULE(_sky, m)
{
  using namespace sky;
  using namespace sky::python;

  m.doc() = "Low-level SkyAnimate API. Use the `sky` package instead.";

  py::class_<rna::PointerRNA>(m, "PointerRNA")
      .def("__getattr__",
           [](const rna::PointerRNA &ptr, const std::string &name) -> py::object {
             const rna::PropertyRNA *prop = ptr.type->find(name);
             if (!prop) {
               throw py::attribute_error("'" + ptr.type->identifier + "' has no property '" + name + "'");
             }
             if (prop->type == rna::PropType::Enum) {
               const int index = std::get<int>(prop->get(ptr.data));
               return py::str(prop->items.at(size_t(index)).identifier);
             }
             return value_to_py(prop->get(ptr.data));
           })
      .def("__setattr__",
           [](rna::PointerRNA &ptr, const std::string &name, py::handle value) {
             const rna::PropertyRNA *prop = ptr.type->find(name);
             if (!prop) {
               throw py::attribute_error("'" + ptr.type->identifier + "' has no property '" + name + "'");
             }
             if (!prop->set) {
               throw py::attribute_error("'" + ptr.type->identifier + "." + name + "' is read-only");
             }
             prop->set(ptr.data, rna::clamp(*prop, value_from_py(*prop, value)));
             current_context().undo->mark_changed();
           })
      .def("__dir__",
           [](const rna::PointerRNA &ptr) {
             std::vector<std::string> names;
             for (const rna::PropertyRNA &prop : ptr.type->properties) {
               names.push_back(prop.identifier);
             }
             return names;
           })
      .def("__repr__", [](const rna::PointerRNA &ptr) { return "<sky " + ptr.type->identifier + ">"; });

  py::class_<Context>(m, "Context")
      .def_property_readonly("document",
                             [](Context &C) { return rna::PointerRNA{C.document, &Document::rna()}; })
      .def_property_readonly("area_type", [](const Context &C) { return C.area_type; })
      .def_property_readonly("is_background", [](const Context &C) { return C.wm && C.wm->background; })
      .def_property_readonly("is_playing", [](const Context &C) { return C.wm && C.wm->playing; });

  m.def("context", []() -> Context & { return current_context(); }, py::return_value_policy::reference);

  m.def("rna_properties", [](const rna::PointerRNA &ptr) {
    py::list result;
    for (const rna::PropertyRNA &prop : ptr.type->properties) {
      py::dict info;
      info["identifier"] = prop.identifier;
      info["name"] = prop.name;
      info["description"] = prop.description;
      info["type"] = rna::prop_type_name(prop.type);
      info["read_only"] = !prop.set;
      if (prop.has_range()) {
        info["min"] = prop.min;
        info["max"] = prop.max;
      }
      if (prop.type == rna::PropType::Enum) {
        py::list items;
        for (const rna::EnumItem &item : prop.items) {
          items.append(item.identifier);
        }
        info["items"] = items;
      }
      result.append(info);
    }
    return result;
  });

  m.def(
      "ops_call",
      [](const std::string &idname, const OpArgs &args) {
        if (!operators().find(idname)) {
          throw py::key_error("Operator not found: " + idname);
        }
        return std::string(op_status_name(operator_call(current_context(), idname, args)));
      },
      py::arg("idname"), py::arg("args") = OpArgs{});

  /* Remote control (wm/remote.hh), for tests: start listening, answer what arrived (the main
   * loop does this in the app), answer one request at once, stop. */
  m.def("remote_start", [](int port) { return remote_start(port, false); });
  m.def("remote_token", []() { return remote_token(); });
  m.def("remote_stop", []() { remote_stop(); });
  m.def("remote_port", []() { return remote_port(); });
  m.def("remote_process", []() { remote_process(current_context()); });
  m.def("remote_dispatch", [](const std::string &request) { return remote_dispatch(current_context(), request); });

  /* The timeline's rows with every row open: (depth, name, red, pinned). */
  m.def("timeline_rows", [](bool by_kind) {
    py::list result;
    for (const TimelineRowInfo &row : timeline_rows_list(current_context(), by_kind)) {
      result.append(py::make_tuple(row.depth, row.name, row.red, row.pinned, row.changed));
    }
    return result;
  });
  /* The main loop's live dynamics, for tests: `seconds` of real time. Returns whether it is
   * running, and the active bone layer's dynamic bones' angles as shown (by name). */
  m.def("dynamics_live_tick", [](double seconds) {
    Document &doc = *current_context().document;
    dynamics_live_tick(doc, seconds, true);
    py::dict angles;
    if (const Layer *layer = layer_find(doc, doc.active_layer)) {
      for (const auto &[id, state] : layer->dynamics_now) {
        if (const Bone *b = bone_find(*layer, id)) {
          angles[py::str(b->name)] = state.angle;
        }
      }
    }
    return py::make_tuple(dynamics_live_running(), angles);
  });
  m.def("layer_channel_pinned", [](uint64_t id, const std::string &path) {
    Layer *layer = layer_find(*current_context().document, id);
    if (!layer) {
      throw py::key_error("no layer " + std::to_string(id));
    }
    return layer_channel_pinned(*layer, path);
  });

  m.def("file_state", []() {
    Context &C = current_context();
    return py::make_tuple(C.wm->filepath, document_is_dirty(C));
  });

  m.def("ops_poll", [](const std::string &idname) {
    const OperatorType *ot = operators().find(idname);
    if (!ot) {
      throw py::key_error("Operator not found: " + idname);
    }
    return operator_poll(current_context(), *ot);
  });

  m.def("ops_list", []() {
    py::list result;
    for (const OperatorType &ot : operators().all()) {
      py::dict info;
      info["idname"] = ot.idname;
      info["name"] = ot.name;
      info["description"] = ot.description;
      info["is_python"] = ot.is_python;
      result.append(info);
    }
    return result;
  });

  m.def(
      "register_operator",
      [](const std::string &idname, const std::string &name, const std::string &description,
         const std::vector<std::string> &options, py::object poll, py::object execute) {
        if (!operator_idname_valid(idname)) {
          throw py::value_error("Invalid bl_idname '" + idname + "': use lowercase 'category.name'");
        }
        OperatorType ot;
        ot.idname = idname;
        ot.name = name;
        ot.description = description;
        ot.is_python = true;
        for (const std::string &option : options) {
          if (option == "REGISTER") {
            ot.flag |= OPTYPE_REGISTER;
          }
          else if (option == "UNDO") {
            ot.flag |= OPTYPE_UNDO;
          }
          else {
            throw py::value_error("Unknown bl_options entry '" + option + "'");
          }
        }
        if (!poll.is_none()) {
          ot.poll = [poll, idname](Context &C) {
            try {
              return py::bool_(poll(py::cast(&C, py::return_value_policy::reference))).cast<bool>();
            }
            catch (py::error_already_set &error) {
              report(ReportType::Error, "Error in poll of " + idname + ":\n" + format_exception(error));
              return false;
            }
          };
        }
        ot.exec = [execute, idname](Context &C) {
          try {
            const std::string status =
                execute(py::cast(&C, py::return_value_policy::reference)).cast<std::string>();
            return op_status_from_name(status);
          }
          catch (py::error_already_set &error) {
            report(ReportType::Error, "Error in " + idname + ":\n" + format_exception(error));
            return OpStatus::Cancelled;
          }
        };
        operators().add(std::move(ot));
      },
      py::arg("idname"), py::arg("name"), py::arg("description"), py::arg("options"), py::arg("poll"),
      py::arg("execute"));

  m.def("unregister_operator", [](const std::string &idname) { return operators().remove(idname); });

  m.def(
      "keymap_add",
      [](const std::string &keymap, const std::string &idname, const std::string &type, bool ctrl, bool shift,
         bool alt) {
        keymaps().add(keymap, {idname, {to_upper(type), ctrl, shift, alt}});
      },
      py::arg("keymap"), py::arg("idname"), py::arg("type"), py::arg("ctrl") = false, py::arg("shift") = false,
      py::arg("alt") = false);

  m.def("keymap_items", []() {
    py::list result;
    for (const auto &[keymap, items] : keymaps().all()) {
      for (const KeyMapItem &item : items) {
        result.append(py::make_tuple(keymap, item.idname, key_event_label(item.event)));
      }
    }
    return result;
  });

  /* Crashes on purpose, to try the crash report (sky.wm.debug_crash). */
  m.def("debug_crash", []() {
    volatile bool go = true;
    if (go) {
      crash_now();
    }
  });
  m.def("properties_tab", [](int tab) { current_context().wm->properties_tab_request = std::clamp(tab, 0, 6); });
  /* What the Properties editor was asked to show and flash: (tab asked for, layer id, path, panel). */
  m.def("properties_flash", [] {
    const WindowManager &wm = *current_context().wm;
    return py::make_tuple(wm.properties_tab_request, wm.properties_flash.layer, wm.properties_flash.path,
                          wm.properties_flash.panel);
  });
  m.def(
      "event_simulate",
      [](const std::string &type, bool ctrl, bool shift, bool alt, const std::string &area, bool typing) {
        Context &C = current_context();
        const std::string saved_area = C.area_type;
        if (!area.empty()) {
          C.area_type = area;
        }
        /* A key "over the viewport" has the mouse there, as viewport-only operators expect. */
        const bool saved_mouse = C.wm->mouse_in_viewport;
        C.wm->mouse_in_viewport = saved_mouse || area == "Viewport";
        const bool handled = wm_handle_key_event(C, {to_upper(type), ctrl, shift, alt}, typing);
        C.wm->mouse_in_viewport = saved_mouse;
        C.area_type = saved_area;
        return handled;
      },
      py::arg("type"), py::arg("ctrl") = false, py::arg("shift") = false, py::arg("alt") = false,
      py::arg("area") = "", py::arg("typing") = false);

  /* Mirror editing (the viewport header's butterfly X / Y). */
  m.def("mirror_editing", []() {
    const ToolSettings &ts = current_context().wm->tool_settings;
    return py::make_tuple(ts.mirror_x, ts.mirror_y);
  });
  m.def("set_mirror_editing", [](bool x, bool y) {
    ToolSettings &ts = current_context().wm->tool_settings;
    ts.mirror_x = x;
    ts.mirror_y = y;
  });

  /* Proportional editing (the viewport header's, O). */
  m.def("proportional_editing", []() {
    const ToolSettings &ts = current_context().wm->tool_settings;
    return py::make_tuple(ts.proportional, proportional_falloff_identifier(ts.proportional_falloff),
                          ts.proportional_connected, ts.proportional_size);
  });
  m.def("set_proportional_editing", [](bool on, const std::string &falloff, bool connected, float size) {
    ToolSettings &ts = current_context().wm->tool_settings;
    if (!proportional_falloff_from_identifier(falloff, &ts.proportional_falloff)) {
      throw py::value_error("unknown falloff '" + falloff + "'");
    }
    ts.proportional = on;
    ts.proportional_connected = connected;
    ts.proportional_size = std::max(size, 1e-6f);
  });
  /* How much of a move each point of a layer would get with the proportional settings now (by
   * its stored place). */
  m.def("layer_proportional_weights", [](uint64_t id) {
    const Layer *layer = layer_find(*current_context().document, id);
    if (!layer) {
      throw py::value_error("no layer");
    }
    const ToolSettings &ts = current_context().wm->tool_settings;
    std::vector<float2> positions;
    for (const MeshPoint &p : layer->drawing.points) {
      positions.push_back(p.pos);
    }
    const std::vector<float> dist = drawing_proportional_distances(layer->drawing, positions, ts.proportional_connected);
    std::vector<float> weights;
    for (int i = 0; i < int(dist.size()); i++) {
      weights.push_back(ts.proportional || dist[i] <= 0.0f
                            ? proportional_weight(ts.proportional_falloff, dist[i], ts.proportional_size, i)
                            : 0.0f);
    }
    return weights;
  });

  m.def("undo_history", []() {
    UndoStack &undo = *current_context().undo;
    std::vector<std::string> names;
    for (size_t i = 0; i < undo.size(); i++) {
      names.push_back(undo.step_name(i));
    }
    return py::make_tuple(names, undo.active());
  });

  /* Text blocks, wrapped by sky.data.texts. Looked up by name on every call, so Python never
   * holds a pointer into the document. */
  const auto text_or_raise = [](const std::string &name) -> Text & {
    Text *text = document_text_find(*current_context().document, name);
    if (!text) {
      throw py::key_error("No text named '" + name + "'");
    }
    return *text;
  };
  m.def("text_names", []() {
    std::vector<std::string> names;
    for (const Text &text : current_context().document->texts) {
      names.push_back(text.name);
    }
    return names;
  });
  m.def("text_new", [](const std::string &name) {
    Context &C = current_context();
    const std::string added = document_text_add(*C.document, name).name;
    C.undo->mark_changed();
    if (C.text_name.empty()) {
      C.text_name = added;
    }
    return added;
  });
  m.def("text_remove", [](const std::string &name) {
    Context &C = current_context();
    std::erase_if(C.document->texts, [&](const Text &text) { return text.name == name; });
    C.undo->mark_changed();
    if (C.text_name == name) {
      C.text_name = C.document->texts.empty() ? "" : C.document->texts.front().name;
    }
  });
  m.def("text_body", [text_or_raise](const std::string &name) { return text_or_raise(name).body; });
  m.def("text_set_body",
        [text_or_raise](const std::string &name, const std::string &body) {
          text_or_raise(name).body = body;
          current_context().undo->mark_changed();
        });
  m.def("text_filepath", [text_or_raise](const std::string &name) { return text_or_raise(name).filepath; });
  m.def("text_active", []() { return current_context().text_name; });
  m.def("text_set_active", [text_or_raise](const std::string &name) {
    if (!name.empty()) {
      text_or_raise(name);
    }
    current_context().text_name = name;
  });

  /* Layers, wrapped by sky.data.layers. Looked up by id on every call, like texts. */
  const auto layer_or_raise = [](uint64_t id) -> Layer & {
    Document &doc = *current_context().document;
    Layer *layer = layer_find(doc, id);
    if (!layer) {
      throw py::key_error("No layer with id " + std::to_string(id));
    }
    if (layer->is_reference()) {
      /* As the app does every frame: edits to it become its overrides, and it shows its original
       * as it now is. */
      references_capture(doc);
    }
    return *layer;
  };
  m.def("layer_ids", []() {
    std::vector<uint64_t> ids;
    for (const Layer &layer : current_context().document->layers) {
      ids.push_back(layer.id);
    }
    return ids;
  });
  m.def("layer_children", [layer_or_raise](uint64_t id) {
    std::vector<uint64_t> ids;
    for (const Layer &child : layer_or_raise(id).children) {
      ids.push_back(child.id);
    }
    return ids;
  });
  m.def("layer_pointer", [layer_or_raise](uint64_t id) { return rna::PointerRNA{&layer_or_raise(id), &Layer::rna()}; });
  m.def("layer_active", []() { return current_context().document->active_layer; });
  m.def("layer_set_active", [layer_or_raise](uint64_t id) {
    layer_or_raise(id);
    current_context().document->active_layer = id;
  });
  m.def("layer_parent", [](uint64_t id) {
    const Layer *parent = layer_parent(*current_context().document, id);
    return parent ? parent->id : uint64_t(0);
  });
  m.def("layer_reference_source", [layer_or_raise](uint64_t id) { return layer_or_raise(id).reference_source; });
  /* A reference's settings: {edit_points, follow_animation, match_original}. */
  m.def("layer_reference", [layer_or_raise](uint64_t id) {
    const ReferenceSettings &rs = layer_or_raise(id).reference;
    py::dict d;
    d["edit_points"] = rs.edit_points;
    d["follow_animation"] = rs.follow_animation;
    d["match_original"] = rs.match_original;
    return d;
  });
  /* A reference's own changes: points {id: (dx, dy, keyed)}, looks {id: {setting: value}}, swaps
   * [(from, to, fill, line)], weights [point ids]. */
  m.def("layer_reference_overrides", [layer_or_raise](uint64_t id) {
    const ReferenceSettings &rs = layer_or_raise(id).reference;
    const auto rgba = [](Color c) { return py::make_tuple(c.r, c.g, c.b, c.a); };
    py::dict points, looks;
    for (const auto &[pid, o] : rs.points) {
      points[py::int_(pid)] = py::make_tuple(o.offset.x, o.offset.y, o.offset_keys.animated());
    }
    for (const auto &[sid, look] : rs.looks) {
      py::dict l;
      if (look.fill_set) {
        l["fill"] = rgba(look.fill);
      }
      if (look.line_set) {
        l["line"] = rgba(look.line);
      }
      if (look.width_set) {
        l["line_width"] = look.line_width;
      }
      if (look.brush_set) {
        l["brush"] = look.brush.name;
      }
      if (look.has_fill_set) {
        l["has_fill"] = look.has_fill;
      }
      if (look.has_outline_set) {
        l["has_outline"] = look.has_outline;
      }
      if (look.hidden) {
        l["hidden"] = true;
      }
      looks[py::int_(sid)] = l;
    }
    py::dict handles;
    for (const auto &[key, o] : rs.handles) {
      handles[py::make_tuple(key.point, key.toward, key.in ? "IN" : "OUT")] =
          py::make_tuple(o.offset.x, o.offset.y, o.offset_keys.animated());
    }
    py::list swaps, weights;
    for (const ReferenceSwap &s : rs.swaps) {
      swaps.append(py::make_tuple(rgba(s.from), rgba(s.to), s.fill, s.line));
    }
    for (const auto &[pid, w] : rs.weights) {
      weights.append(pid);
    }
    py::dict d;
    d["points"] = points;
    d["looks"] = looks;
    d["swaps"] = swaps;
    d["weights"] = weights;
    d["handles"] = handles;
    d["channels"] = std::vector<std::string>(rs.own_channels.begin(), rs.own_channels.end());
    return d;
  });
  m.def("layer_point_ids", [layer_or_raise](uint64_t id) {
    std::vector<uint64_t> ids;
    for (const MeshPoint &p : layer_drawing(*current_context().document, layer_or_raise(id)).points) {
      ids.push_back(p.id);
    }
    return ids;
  });
  m.def("layer_shape_ids", [layer_or_raise](uint64_t id) {
    std::vector<uint64_t> ids;
    for (const Shape &s : layer_drawing(*current_context().document, layer_or_raise(id)).shapes) {
      ids.push_back(s.id);
    }
    return ids;
  });
  /* A shape's look: {fill, line (r, g, b, a), line_width, has_fill, has_outline, hidden}. */
  m.def("layer_shape_style", [layer_or_raise](uint64_t id, int index) {
    const VectorDrawing &drawing = layer_drawing(*current_context().document, layer_or_raise(id));
    if (index < 0 || index >= int(drawing.shapes.size())) {
      throw py::index_error("no shape " + std::to_string(index));
    }
    const Shape &s = drawing.shapes[index];
    py::dict d;
    d["fill"] = py::make_tuple(s.style.fill.r, s.style.fill.g, s.style.fill.b, s.style.fill.a);
    d["line"] = py::make_tuple(s.style.line.r, s.style.line.g, s.style.line.b, s.style.line.a);
    d["line_width"] = s.style.line_width;
    d["has_fill"] = s.has_fill;
    d["has_outline"] = s.has_outline;
    d["hidden"] = s.hidden;
    return d;
  });
  /* Changes a shape's look, as the Shape panel does (on a reference, it becomes its override). */
  m.def("layer_set_shape_style", [layer_or_raise](uint64_t id, int index, py::dict values) {
    Layer &layer = layer_or_raise(id);
    if (index < 0 || index >= int(layer.drawing.shapes.size())) {
      throw py::index_error("no shape " + std::to_string(index));
    }
    Shape &s = layer.drawing.shapes[index];
    const auto color = [](py::handle v) {
      const auto t = v.cast<std::vector<float>>();
      return Color{t.at(0), t.at(1), t.at(2), t.size() > 3 ? t[3] : 1.0f};
    };
    for (const auto &[k, v] : values) {
      const std::string key = py::str(k);
      if (key == "fill") {
        s.style.fill = color(v);
      }
      else if (key == "line") {
        s.style.line = color(v);
      }
      else if (key == "line_width") {
        s.style.line_width = std::max(0.0f, v.cast<float>());
      }
      else if (key == "has_fill") {
        s.has_fill = v.cast<bool>();
      }
      else if (key == "has_outline") {
        s.has_outline = v.cast<bool>();
      }
      else {
        throw py::key_error("unknown shape setting '" + key + "'");
      }
    }
    current_context().undo->mark_changed();
  });
  m.def(
      "layer_add_curve",
      [layer_or_raise](uint64_t id, const std::vector<std::pair<float, float>> &points, bool closed, bool fill) {
        Context &C = current_context();
        Layer &layer = layer_or_raise(id);
        if (layer.is_group() || layer.is_reference()) {
          throw py::value_error("Curves can only be added to vector layers that are not references");
        }
        if (points.size() < 2) {
          throw py::value_error("A curve needs at least two points");
        }
        VectorDrawing &drawing = layer.drawing;
        C.undo->mark_changed();
        Curve curve;
        for (const auto &[x, y] : points) {
          curve.points.push_back({drawing_add_point(drawing, {x, y})});
        }
        curve.closed = closed && points.size() >= 3;
        drawing.curves.push_back(curve);
        Shape shape;
        shape.style = C.wm->tool_settings.style;
        shape.has_fill = fill && curve.closed;
        shape.name = "Shape " + std::to_string(drawing.shapes.size() + 1);
        for (int s = 0; s < drawing.curves.back().segment_count(); s++) {
          shape.edges.push_back({int(drawing.curves.size()) - 1, s});
        }
        drawing.shapes.push_back(std::move(shape));
        return int(drawing.curves.size()) - 1;
      },
      py::arg("id"), py::arg("points"), py::arg("closed") = false, py::arg("fill") = false);
  /* Moves one point, as dragging it does (an edit: keyed at the current frame when recorded). */
  m.def("layer_set_point", [layer_or_raise](uint64_t id, int index, std::pair<float, float> pos) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference() && !layer.reference.edit_points) {
      throw py::value_error("This copy's points are locked (reference_set edit_points=1 unlocks them)");
    }
    if (index < 0 || index >= int(layer.drawing.points.size())) {
      throw py::index_error("no point " + std::to_string(index));
    }
    layer.drawing.points[index].pos = {pos.first, pos.second};
    current_context().undo->mark_changed();
  });
  /* G on the selected points by (dx, dy) in the layer's space, with mirror editing as in the
   * viewport (stored positions: where the points are before bones move them). */
  m.def("layer_move_points", [layer_or_raise](uint64_t id, float dx, float dy) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference() && !layer.reference.edit_points) {
      throw py::value_error("This copy's points are locked (reference_set edit_points=1 unlocks them)");
    }
    VectorDrawing &drawing = layer.drawing;
    const ToolSettings &ts = current_context().wm->tool_settings;
    std::vector<int> moving;
    for (int i = 0; i < int(drawing.points.size()); i++) {
      if (drawing.points[i].selected) {
        moving.push_back(i);
      }
    }
    if (moving.empty()) {
      return false;
    }
    const std::vector<MirrorPair> pairs = drawing_mirror_pairs(drawing, moving, ts.mirror_x, ts.mirror_y);
    for (const int i : moving) {
      drawing.points[i].pos += float2(dx, dy);
    }
    drawing_apply_mirror(drawing, pairs);
    current_context().undo->mark_changed();
    return true;
  });
  m.def("layer_points", [layer_or_raise](uint64_t id) {
    const Document &doc = *current_context().document;
    py::list result;
    for (const MeshPoint &p : layer_drawing(doc, layer_or_raise(id)).points) {
      result.append(py::make_tuple(p.pos.x, p.pos.y, p.selected));
    }
    return result;
  });
  /* Brushes: settings travel as dicts with Moho's .mohobrush key names. */
  const auto brush_to_dict = [](const BrushSettings &b) {
    py::dict d;
    d["name"] = b.name;
    d["align"] = b.align;
    d["jitter"] = b.jitter;
    d["spacing"] = b.spacing;
    d["angleDrift"] = b.angle_drift;
    d["randomize"] = b.randomize;
    d["randomOrder"] = b.random_order;
    d["mergedAlpha"] = b.merged_alpha;
    d["sizeVariationAmp"] = b.size_variation_amp;
    d["sizeVariationScale"] = b.size_variation_scale;
    d["randomInterval"] = b.random_interval;
    d["tint"] = b.tint;
    d["hueDrift"] = b.hue_drift;
    d["satDrift"] = b.sat_drift;
    d["valDrift"] = b.val_drift;
    return d;
  };
  m.def("brushes", [brush_to_dict]() {
    py::list result;
    for (const Brush &brush : brushes()) {
      py::dict d = brush_to_dict(brush.defaults);
      d["name"] = brush.name;
      d["images"] = int(brush.files.size());
      result.append(d);
    }
    return result;
  });
  m.def("asset_library", []() { return preferences().asset_library; });
  m.def("layer_shape_brush", [layer_or_raise, brush_to_dict](uint64_t id, int index) {
    const VectorDrawing &drawing = layer_drawing(*current_context().document, layer_or_raise(id));
    if (index < 0 || index >= int(drawing.shapes.size())) {
      throw py::index_error("no shape " + std::to_string(index));
    }
    return brush_to_dict(drawing.shapes[index].style.brush);
  });
  /* How many dabs a brush lays along a straight line (for checking spacing). */
  m.def("brush_dab_count", [](const std::string &name, float length, float width) {
    const Brush *brush = brush_find(name);
    if (!brush) {
      throw py::value_error("no brush named '" + name + "'");
    }
    BrushDabs dabs;
    BrushPath path;
    BrushPath::Edge edge;
    edge.ctrl[0] = {0.0f, 0.0f};
    edge.ctrl[1] = {length / 3.0f, 0.0f};
    edge.ctrl[2] = {length * 2.0f / 3.0f, 0.0f};
    edge.ctrl[3] = {length, 0.0f};
    edge.half0 = edge.half1 = width * 0.5f;
    path.edges.push_back(edge);
    brush_stroke(path, path, brush->defaults, Color{0, 0, 0, 1},
                 std::vector<float>(std::max<size_t>(brush->files.size(), 1), 1.0f), 0.0f, 1, dabs);
    return dabs.count;
  });
  /* A brushed shape's dabs on the frame shown, as drawn: [(x, y, alpha), ...] in layer space. */
  m.def("layer_shape_dabs", [layer_or_raise](uint64_t id, int index) {
    const Document &doc = *current_context().document;
    const Layer &layer = layer_or_raise(id);
    const VectorDrawing drawing = layer_has_modifier_effect(doc, layer, false) ? layer_evaluated_drawing(doc, layer, false)
                                                                               : layer_drawing(doc, layer);
    const VectorDrawing setup = layer_setup_drawing(doc, layer, false);
    if (index < 0 || index >= int(drawing.shapes.size())) {
      throw py::index_error("no shape " + std::to_string(index));
    }
    const Shape &shape = drawing.shapes[index];
    const Brush *brush = brush_find(shape.style.brush.name);
    if (!brush) {
      throw py::value_error("shape " + std::to_string(index) + " has no brush from the library");
    }
    const BrushPath shown = brush_path(drawing, shape, 0.0f);
    const BrushPath rest = setup.shapes.size() == drawing.shapes.size() ? brush_path(setup, setup.shapes[index], 0.0f)
                                                                        : shown;
    BrushDabs dabs;
    brush_stroke(shown, rest, shape.style.brush, shape.style.line,
                 std::vector<float>(std::max<size_t>(brush->files.size(), 1), 1.0f), 0.0f,
                 brush_seed(shape.style.brush, layer.id, index, doc.frame_current), dabs);
    py::list result;
    for (int i = 0; i < dabs.count; i++) {
      result.append(py::make_tuple(dabs.centers[i].x, dabs.centers[i].y, dabs.alphas[i]));
    }
    return result;
  });
  /* Handles: [(type, (in x, y), (out x, y)), ...] for one curve, in layer space. */
  m.def("layer_curve_handles", [layer_or_raise](uint64_t id, int curve_index) {
    const VectorDrawing &drawing = layer_drawing(*current_context().document, layer_or_raise(id));
    if (curve_index < 0 || curve_index >= int(drawing.curves.size())) {
      throw py::index_error("no curve " + std::to_string(curve_index));
    }
    const Curve &curve = drawing.curves[curve_index];
    py::list result;
    for (int i = 0; i < int(curve.points.size()); i++) {
      const float2 in = curve_handle(drawing, curve, i, true), out = curve_handle(drawing, curve, i, false);
      result.append(py::make_tuple(curve.points[i].aligned ? "ALIGNED" : "FREE", py::make_tuple(in.x, in.y),
                                   py::make_tuple(out.x, out.y)));
    }
    return result;
  });
  /* Drags one handle to a layer-space position, as in the viewport. */
  m.def("layer_move_handle", [layer_or_raise](uint64_t id, int curve_index, int index, const std::string &side,
                                              std::pair<float, float> pos) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference() && !layer.reference.edit_points) {
      throw py::value_error("This copy's points are locked (reference_set edit_points=1 unlocks them)");
    }
    VectorDrawing &drawing = layer.drawing;
    if (curve_index < 0 || curve_index >= int(drawing.curves.size()) || index < 0 ||
        index >= int(drawing.curves[curve_index].points.size())) {
      throw py::index_error("no such curve point");
    }
    if (side != "IN" && side != "OUT") {
      throw py::value_error("side must be 'IN' or 'OUT'");
    }
    curve_point_move_handle(drawing, drawing.curves[curve_index], index, side == "IN", {pos.first, pos.second});
    current_context().undo->mark_changed();
  });
  m.def("layer_select_handle", [layer_or_raise](uint64_t id, int curve_index, int index, const std::string &side) {
    VectorDrawing &drawing = layer_or_raise(id).drawing;
    if (curve_index < 0 || curve_index >= int(drawing.curves.size()) || index < 0 ||
        index >= int(drawing.curves[curve_index].points.size())) {
      throw py::index_error("no such curve point");
    }
    CurvePoint &cp = drawing.curves[curve_index].points[index];
    (side == "IN" ? cp.select_in : cp.select_out) = true;
  });
  /* Keys. A channel is named by its path ("position", "points[3].pos", ...). */
  const auto channel_or_raise = [layer_or_raise](uint64_t id, const std::string &path) {
    ChannelRef ref;
    if (!layer_channel(layer_or_raise(id), path, &ref)) {
      throw py::key_error("no animated channel '" + path + "' on this layer");
    }
    return ref;
  };
  m.def("layer_keyframes", [channel_or_raise](uint64_t id, const std::string &path) {
    animation_sync(*current_context().document);
    const ChannelRef ref = channel_or_raise(id, path);
    py::list result;
    const auto add = [&](const auto &ch) {
      for (const auto &k : ch.keys) {
        py::dict d;
        d["frame"] = k.frame;
        if constexpr (std::is_same_v<std::decay_t<decltype(k.value)>, float>) {
          d["value"] = k.value;
        }
        else {
          d["value"] = py::make_tuple(k.value.x, k.value.y);
        }
        d["interp"] = interp_identifier(k.interp);
        d["hold"] = k.hold;
        d["selected"] = k.selected;
        d["interval"] = k.interval;
        d["ease"] = ease_identifier(k.ease);
        d["overshoot"] = k.overshoot;
        d["elastic_amplitude"] = k.elastic_amplitude;
        d["elastic_period"] = k.elastic_period;
        d["noise_amplitude"] = k.noise_amplitude;
        d["noise_frequency"] = k.noise_frequency;
        d["cycle_back"] = k.cycle_back;
        result.append(d);
      }
    };
    if (ref.scalar) {
      add(*ref.scalar);
    }
    else {
      add(*ref.vector);
    }
    return result;
  });
  m.def(
      "layer_keyframe_insert",
      [layer_or_raise, channel_or_raise](uint64_t id, const std::string &path, int frame) {
        Document &doc = *current_context().document;
        animation_sync(doc);
        animation_record(doc);
        if (frame < 0) {
          frame = doc.frame_current;
        }
        if (path.empty()) {
          layer_insert_keys(layer_or_raise(id), frame);
        }
        else {
          const ChannelRef ref = channel_or_raise(id, path);
          const auto insert = [&](auto &ch, const auto &value) {
            if (!ch.animated() && frame != 0) {
              ch.set_key(0, value);
            }
            ch.set_key(frame, value);
          };
          if (ref.scalar) {
            insert(*ref.scalar, *ref.scalar_value);
          }
          else {
            insert(*ref.vector, *ref.vector_value);
          }
        }
        animation_evaluate(doc, doc.frame_current);
        current_context().undo->mark_changed();
      },
      py::arg("id"), py::arg("path") = "", py::arg("frame") = -1);
  m.def("layer_keyframe_delete", [channel_or_raise](uint64_t id, const std::string &path, int frame) {
    Document &doc = *current_context().document;
    animation_sync(doc);
    animation_record(doc);
    const ChannelRef ref = channel_or_raise(id, path);
    const bool removed = ref.scalar ? ref.scalar->remove_key(frame) : ref.vector->remove_key(frame);
    animation_evaluate(doc, doc.frame_current);
    current_context().undo->mark_changed();
    return removed;
  });
  /* Changes one key's interpolation and settings (keyword arguments as in layer_keyframes). */
  m.def("layer_keyframe_set", [channel_or_raise](uint64_t id, const std::string &path, int frame, py::dict settings) {
    Document &doc = *current_context().document;
    animation_sync(doc);
    animation_record(doc);
    const ChannelRef ref = channel_or_raise(id, path);
    const auto apply = [&](auto &ch) {
      auto *key = ch.key_at(frame);
      if (!key) {
        throw py::key_error("no key at frame " + std::to_string(frame));
      }
      for (const auto &[name_obj, value] : settings) {
        const std::string name = py::str(name_obj);
        if (name == "interp") {
          Interp interp;
          if (!interp_from_identifier(value.cast<std::string>(), &interp)) {
            throw py::value_error("unknown interpolation '" + value.cast<std::string>() + "'");
          }
          if (ch.constant && interp != Interp::Step) {
            throw py::value_error("'" + path + "' is a yes/no property: its keys can only be STEP (constant)");
          }
          key->interp = interp;
        }
        else if (name == "hold") {
          key->hold = std::max(0, value.cast<int>());
        }
        else if (name == "interval") {
          key->interval = std::max(1, value.cast<int>());
        }
        else if (name == "ease") {
          Ease ease;
          if (!ease_from_identifier(value.cast<std::string>(), &ease)) {
            throw py::value_error("unknown ease strength '" + value.cast<std::string>() + "'");
          }
          key->ease = ease;
        }
        else if (name == "overshoot") {
          key->overshoot = value.cast<float>();
        }
        else if (name == "elastic_amplitude") {
          key->elastic_amplitude = value.cast<float>();
        }
        else if (name == "elastic_period") {
          key->elastic_period = std::max(0.02f, value.cast<float>());
        }
        else if (name == "noise_amplitude") {
          key->noise_amplitude = value.cast<float>();
        }
        else if (name == "noise_frequency") {
          key->noise_frequency = value.cast<float>();
        }
        else if (name == "cycle_back") {
          key->cycle_back = std::max(0, value.cast<int>());
        }
        else {
          throw py::key_error("unknown key setting '" + name + "'");
        }
      }
    };
    if (ref.scalar) {
      apply(*ref.scalar);
    }
    else {
      apply(*ref.vector);
    }
    animation_evaluate(doc, doc.frame_current);
    current_context().undo->mark_changed();
  });
  /* Hold bars the timeline draws for the active and selected layers: [(frame, next frame)]. */
  m.def("timeline_hold_spans", []() {
    Context &C = current_context();
    animation_sync(*C.document);
    return layers_hold_spans(keyed_layers(C));
  });
  m.def("layer_key_frames", [layer_or_raise](uint64_t id) {
    animation_sync(*current_context().document);
    return layer_key_frames(layer_or_raise(id));
  });
  /* --- Bones ------------------------------------------------------------------------- */
  const auto bone_or_raise = [layer_or_raise](uint64_t id, const std::string &name) -> Bone & {
    Layer &layer = layer_or_raise(id);
    Bone *bone = const_cast<Bone *>(bone_find_by_name(layer, name));
    if (!bone) {
      throw py::key_error("no bone named '" + name + "'");
    }
    return *bone;
  };
  m.def("layer_bones", [layer_or_raise](uint64_t id, bool keyed) {
    animation_sync(*current_context().document);
    const Layer &layer = layer_or_raise(id);
    const bool setup = current_context().document->frame_current == 0;
    /* `keyed`: where the keys put them, without bone dynamics (what editing works from). */
    const std::vector<BonePose> poses = keyed ? armature_keyed_pose(layer, setup) : armature_pose(layer, setup);
    py::list result;
    for (size_t i = 0; i < layer.bones.size(); i++) {
      const Bone &b = layer.bones[i];
      const Bone *parent = bone_find(layer, b.parent);
      py::dict d;
      d["name"] = b.name;
      d["parent"] = parent ? parent->name : std::string();
      d["position"] = py::make_tuple(b.position.x, b.position.y);
      d["angle"] = b.angle;
      d["setup_position"] = py::make_tuple(b.setup_position.x, b.setup_position.y);
      d["setup_angle"] = b.setup_angle;
      d["length"] = b.length;
      d["scale"] = py::make_tuple(b.scale.x, b.scale.y);
      d["strength"] = b.strength;
      d["selected"] = b.selected;
      d["select_head"] = bone_head_selected(layer, b);
      d["select_tail"] = bone_tail_selected(layer, b);
      d["connected"] = b.connected;
      d["shape"] = b.shape == BoneShape::Pin ? "PIN" : b.shape == BoneShape::Custom ? "CUSTOM" : "BONE";
      d["pin_size"] = b.pin_size;
      const Layer *shape_layer = b.shape_layer ? layer_find(*current_context().document, b.shape_layer) : nullptr;
      d["shape_layer"] = shape_layer ? shape_layer->name : std::string();
      d["color_set"] = bone_color_set_identifier(b.color_set);
      py::list in;
      for (const uint64_t c_id : b.collections) {
        if (const BoneCollection *c = bone_collection_find(layer, c_id)) {
          in.append(c->name);
        }
      }
      d["collections"] = in;
      const BoneColors shades = bone_colors(b);
      d["colors"] = py::make_tuple(py::make_tuple(shades.regular.r, shades.regular.g, shades.regular.b),
                                   py::make_tuple(shades.select.r, shades.select.g, shades.select.b),
                                   py::make_tuple(shades.active.r, shades.active.g, shades.active.b));
      d["active"] = b.id == layer.active_bone;
      d["head"] = py::make_tuple(poses[i].head.x, poses[i].head.y);
      d["tail"] = py::make_tuple(poses[i].tail.x, poses[i].tail.y);
      result.append(d);
    }
    return result;
  }, py::arg("id"), py::arg("keyed") = false);
  /* Poses or edits a bone: name, position, angle, scale, length, strength, shape (as edits are, so at
   * frames other than 0 they key with auto-keying). */
  m.def("layer_bone_set", [bone_or_raise](uint64_t id, const std::string &name, py::dict values) {
    Bone &bone = bone_or_raise(id, name);
    for (const auto &[key_obj, value] : values) {
      const std::string key = py::str(key_obj);
      if (key == "position") {
        const auto p = value.cast<std::pair<float, float>>();
        bone.position = {p.first, p.second};
      }
      else if (key == "angle") {
        bone.angle = value.cast<float>();
      }
      else if (key == "scale") {
        /* (x, y), or one number for both. */
        if (py::isinstance<py::float_>(value) || py::isinstance<py::int_>(value)) {
          const float v = value.cast<float>();
          bone.scale = {v, v};
        }
        else {
          const auto s = value.cast<std::pair<float, float>>();
          bone.scale = {s.first, s.second};
        }
      }
      else if (key == "length") {
        bone.length = std::max(0.0f, value.cast<float>());
      }
      else if (key == "strength") {
        bone.strength = std::max(0.0f, value.cast<float>());
      }
      else if (key == "name") {
        bone.name = value.cast<std::string>();
      }
      else if (key == "ik_ignore") {
        bone.ik_ignore = value.cast<bool>();
      }
      else if (key == "arc_ik") {
        bone.arc_ik = value.cast<bool>();
      }
      else if (key == "shape") {
        const std::string shape = value.cast<std::string>();
        if (shape != "BONE" && shape != "NONE" && shape != "PIN" && shape != "CUSTOM") {
          throw py::value_error("bone shape is 'BONE', 'PIN' or 'CUSTOM'");
        }
        bone.shape = shape == "PIN" ? BoneShape::Pin : shape == "CUSTOM" ? BoneShape::Custom : BoneShape::None;
      }
      else if (key == "pin_size") {
        bone.pin_size = std::clamp(value.cast<float>(), 0.1f, 20.0f);
      }
      else if (key == "shape_layer") {
        const std::string layer_name = value.cast<std::string>();
        uint64_t found = 0;
        for (const Layer *l : document_layers_flat(*current_context().document)) {
          if (l->name == layer_name && l->type == LayerType::Vector) {
            found = l->id;
            break;
          }
        }
        if (!layer_name.empty() && !found) {
          throw py::value_error("no vector layer named '" + layer_name + "'");
        }
        bone.shape_layer = found;
      }
      else if (key == "color_set") {
        const int set = bone_color_set_from_identifier(value.cast<std::string>());
        if (set < 0) {
          throw py::value_error("color_set is 'DEFAULT', 'THEME01' to 'THEME15' or 'CUSTOM'");
        }
        bone_set_color_set(bone, set);
      }
      else if (key == "custom_colors") {
        /* ((r, g, b), (r, g, b), (r, g, b)): regular, selected, active; makes the set Custom. */
        const auto shades = value.cast<std::vector<std::tuple<float, float, float>>>();
        if (shades.size() != 3) {
          throw py::value_error("custom_colors is three (r, g, b): regular, selected, active");
        }
        bone_set_color_set(bone, BONE_COLOR_CUSTOM);
        for (size_t k = 0; k < 3; k++) {
          bone.custom_colors[k] = {std::get<0>(shades[k]), std::get<1>(shades[k]), std::get<2>(shades[k]), 1.0f};
        }
      }
      else {
        throw py::key_error("unknown bone setting '" + key + "'");
      }
    }
    current_context().undo->mark_changed();
  });
  m.def("layer_select_bone", [layer_or_raise](uint64_t id, const std::string &name, bool extend,
                                               const std::string &part) {
    Layer &layer = layer_or_raise(id);
    const Bone *picked = bone_find_by_name(layer, name);
    if (!picked && !name.empty()) {
      throw py::key_error("no bone named '" + name + "'");
    }
    if (!extend) {
      armature_deselect_all(layer);
    }
    if (picked) {
      Bone &b = *bone_find(layer, picked->id);
      if (part == "HEAD") {
        b.select_head = true;
      }
      else if (part == "TAIL") {
        b.select_tail = true;
      }
      else {
        bone_select(b, true);
      }
      armature_flush_selection(layer);
    }
    layer.active_bone = picked ? picked->id : 0;
  });
  /* The Transform Bones gizmo: scale (sx, sy) and turn (radians) about (px, py), then move by
   * (dx, dy), all in the bone layer's space. */
  m.def("layer_map_bones", [layer_or_raise](uint64_t id, float dx, float dy, float sx, float sy, float angle,
                                            float px, float py) {
    Layer &layer = layer_or_raise(id);
    if (!armature_any_picked(layer)) {
      return false;
    }
    const bool setup = current_context().document->frame_current == 0;
    const std::vector<Bone> original = layer.bones;
    const Transform2D map = Transform2D::translate(float2(px + dx, py + dy)) * Transform2D::rotate(angle) *
                            Transform2D::scaling({sx, sy}) * Transform2D::translate(float2(-px, -py));
    armature_map_selected(layer, original, map, setup);
    if (current_context().wm->tool_settings.mirror_x) {
      armature_mirror_edits(layer, original, setup);
    }
    current_context().undo->mark_changed();
    return true;
  });
  m.def("layer_box_select_bones", [layer_or_raise](uint64_t id, float x0, float y0, float x1, float y1, bool whole,
                                                   bool extend, bool deselect) {
    Layer &layer = layer_or_raise(id);
    const float2 lo(std::min(x0, x1), std::min(y0, y1)), hi(std::max(x0, x1), std::max(y0, y1));
    const auto inside = [&](float2 p) { return p.x >= lo.x && p.x <= hi.x && p.y >= lo.y && p.y <= hi.y; };
    const bool setup = current_context().document->frame_current == 0;
    if (whole) {
      armature_select_bones_in(layer, setup, inside, extend, deselect);
    }
    else {
      armature_select_joints_in(layer, setup, inside, extend, deselect);
    }
  });
  m.def("layer_picked_bounds", [layer_or_raise](uint64_t id) -> py::object {
    float2 lo, hi;
    if (!armature_picked_bounds(layer_or_raise(id), current_context().document->frame_current == 0, lo, hi)) {
      return py::none();
    }
    return py::make_tuple(py::make_tuple(lo.x, lo.y), py::make_tuple(hi.x, hi.y));
  });
  /* A bone's constraints, top first: dicts of their settings (target as a bone name). */
  m.def("layer_bone_constraints", [bone_or_raise](uint64_t id, const std::string &name) {
    const Layer &layer = *layer_find(*current_context().document, id);
    const Bone &bone = bone_or_raise(id, name);
    py::list result;
    for (const BoneConstraint &c : bone.constraints) {
      const Bone *target = bone_find(layer, c.target);
      const auto spring = [](const BoneSpring &s) {
        py::dict d;
        d["enabled"] = s.enabled;
        d["settle_speed"] = s.settle_speed;
        d["bounce"] = s.bounce;
        d["inertia"] = s.inertia;
        d["stretch"] = s.stretch;
        d["use_limit"] = s.use_limit;
        d["limit"] = s.limit;
        return d;
      };
      py::dict d;
      d["type"] = bone_constraint_identifier(c.type);
      d["name"] = c.name;
      d["enabled"] = c.enabled;
      d["influence"] = c.influence;
      d["min_angle"] = c.min_angle;
      d["max_angle"] = c.max_angle;
      d["amount"] = c.amount;
      d["target"] = target ? target->name : std::string();
      d["factor"] = c.factor;
      d["delay"] = c.delay;
      d["chain"] = c.chain;
      d["max_stretch"] = c.max_stretch;
      d["weight"] = c.weight;
      d["wind_catch"] = c.wind_catch;
      d["own_gravity"] = c.own_gravity;
      d["own_wind"] = c.own_wind;
      d["gravity"] = c.gravity;
      d["wind"] = c.wind;
      d["wind_direction"] = c.wind_direction;
      d["wind_gusts"] = c.wind_gusts;
      d["wind_gust_speed"] = c.wind_gust_speed;
      d["wind_turbulence"] = c.wind_turbulence;
      d["angle_spring"] = spring(c.angle_spring);
      d["position_spring"] = spring(c.position_spring);
      d["scale_spring"] = spring(c.scale_spring);
      result.append(d);
    }
    return result;
  });
  m.def("layer_bone_constraint_set", [bone_or_raise](uint64_t id, const std::string &name, int index, py::dict values) {
    Layer &layer = *layer_find(*current_context().document, id);
    Bone &bone = bone_or_raise(id, name);
    if (index < 0 || index >= int(bone.constraints.size())) {
      throw py::index_error("no constraint " + std::to_string(index));
    }
    BoneConstraint &c = bone.constraints[index];
    const auto spring = [](py::handle value, BoneSpring &s) {
      for (const auto &[k, v] : value.cast<py::dict>()) {
        const std::string key = py::str(k);
        if (key == "enabled") {
          s.enabled = v.cast<bool>();
        }
        else if (key == "inertia") {
          s.inertia = std::max(0.0f, v.cast<float>());
        }
        else if (key == "stretch") {
          s.stretch = std::max(0.0f, v.cast<float>());
        }
        else if (key == "use_limit") {
          s.use_limit = v.cast<bool>();
        }
        else if (key == "limit") {
          s.limit = std::max(0.0f, v.cast<float>());
        }
        else if (key == "settle_speed") {
          s.settle_speed = std::clamp(v.cast<float>(), 0.05f, 20.0f);
        }
        else if (key == "bounce") {
          s.bounce = std::clamp(v.cast<float>(), 0.0f, 0.9f);
        }
        else {
          throw py::key_error("unknown spring setting '" + key + "'");
        }
      }
    };
    for (const auto &[key_obj, value] : values) {
      const std::string key = py::str(key_obj);
      if (key == "enabled") {
        c.enabled = value.cast<bool>();
      }
      else if (key == "influence") {
        c.influence = std::clamp(value.cast<float>(), 0.0f, 1.0f);
      }
      else if (key == "min_angle") {
        c.min_angle = value.cast<float>();
      }
      else if (key == "max_angle") {
        c.max_angle = value.cast<float>();
      }
      else if (key == "amount") {
        c.amount = value.cast<float>();
      }
      else if (key == "target") {
        const std::string t = value.cast<std::string>();
        const Bone *target = t.empty() ? nullptr : bone_find_by_name(layer, t);
        if (!t.empty() && !target) {
          throw py::key_error("no bone named '" + t + "'");
        }
        c.target = target ? target->id : 0;
      }
      else if (key == "factor") {
        c.factor = value.cast<float>();
      }
      else if (key == "delay") {
        c.delay = std::max(0, value.cast<int>());
      }
      else if (key == "chain") {
        c.chain = std::max(0, value.cast<int>());
      }
      else if (key == "max_stretch") {
        c.max_stretch = std::max(1.0f, value.cast<float>());
      }
      else if (key == "own_gravity" || key == "own_wind" || key == "own_forces") {
        /* own_forces: both. */
        const Document &doc = *current_context().document;
        if (key != "own_wind") {
          dynamics_use_own_gravity(doc, c, value.cast<bool>());
        }
        if (key != "own_gravity") {
          dynamics_use_own_wind(doc, c, value.cast<bool>());
        }
      }
      else if (key == "weight") {
        c.weight = std::max(0.0f, value.cast<float>());
      }
      else if (key == "wind_catch") {
        c.wind_catch = std::clamp(value.cast<float>(), 0.0f, 10.0f);
      }
      else if (key == "gravity") {
        c.gravity = value.cast<float>();
        c.own_gravity_set = true;
      }
      else if (key == "wind") {
        c.wind = value.cast<float>();
        c.own_wind_set = true;
      }
      else if (key == "wind_direction") {
        c.wind_direction = value.cast<float>();
        c.own_wind_set = true;
      }
      else if (key == "wind_gusts") {
        c.wind_gusts = std::clamp(value.cast<float>(), 0.0f, 2.0f);
        c.own_wind_set = true;
      }
      else if (key == "wind_gust_speed") {
        c.wind_gust_speed = std::clamp(value.cast<float>(), 0.05f, 10.0f);
        c.own_wind_set = true;
      }
      else if (key == "wind_turbulence") {
        c.wind_turbulence = std::clamp(value.cast<float>(), 0.0f, 2.0f);
        c.own_wind_set = true;
      }
      else if (key == "angle_spring") {
        spring(value, c.angle_spring);
      }
      else if (key == "position_spring") {
        spring(value, c.position_spring);
      }
      else if (key == "scale_spring") {
        spring(value, c.scale_spring);
      }
      else {
        throw py::key_error("unknown constraint setting '" + key + "'");
      }
    }
    /* The change is an edit like any other (keyed when animated), then the frame is worked out
     * again with it. */
    animation_record(*current_context().document);
    animation_evaluate(*current_context().document, current_context().document->frame_current);
    current_context().undo->mark_changed();
  });
  /* R on the selected bones by `angle` radians. */
  m.def("layer_rotate_bones", [layer_or_raise](uint64_t id, float angle) {
    Layer &layer = layer_or_raise(id);
    if (!armature_any_picked(layer)) {
      return false;
    }
    const bool setup = current_context().document->frame_current == 0;
    const std::vector<Bone> original = layer.bones;
    armature_rotate_selected(layer, original, angle, current_context().document->frame_current == 0);
    if (current_context().wm->tool_settings.mirror_x) {
      armature_mirror_edits(layer, original, setup);
    }
    current_context().undo->mark_changed();
    return true;
  });
  /* S on the selected bones by `factor` (axis 0, 1 X, 2 Y; squash for S S). */
  m.def("layer_scale_bones", [layer_or_raise](uint64_t id, float factor, int axis, bool squash) {
    Layer &layer = layer_or_raise(id);
    if (!armature_any_picked(layer)) {
      return false;
    }
    const std::vector<Bone> original = layer.bones;
    armature_scale_selected(layer, original, factor, current_context().document->frame_current == 0, axis, squash);
    if (current_context().wm->tool_settings.mirror_x) {
      armature_mirror_edits(layer, original, current_context().document->frame_current == 0);
    }
    current_context().undo->mark_changed();
    return true;
  });
  /* G on the selected bones by (dx, dy), in the bone layer's space. */
  m.def("layer_move_bone_ends", [layer_or_raise](uint64_t id, float dx, float dy) {
    Layer &layer = layer_or_raise(id);
    if (!armature_any_picked(layer)) {
      return false;
    }
    const std::vector<Bone> original = layer.bones;
    const bool setup = current_context().document->frame_current == 0;
    armature_grab_selected(layer, original, {dx, dy}, setup, current_context().wm->tool_settings.auto_ik);
    if (current_context().wm->tool_settings.mirror_x) {
      armature_mirror_edits(layer, original, setup);
    }
    current_context().undo->mark_changed();
    return true;
  });
  /* One G drag through several mouse offsets (each from the drag's start), as the viewport's modal
   * does it: every step starts again from the bones as the drag found them, and smart actions
   * follow between steps. */
  m.def("layer_drag_bone_ends", [layer_or_raise](uint64_t id, const std::vector<std::pair<float, float>> &offsets) {
    Layer &layer = layer_or_raise(id);
    if (!armature_any_picked(layer)) {
      return false;
    }
    Document &doc = *current_context().document;
    const std::vector<Bone> original = layer.bones;
    const bool setup = doc.frame_current == 0;
    for (const auto &[dx, dy] : offsets) {
      armature_grab_selected(layer, original, {dx, dy}, setup, current_context().wm->tool_settings.auto_ik);
      animation_refresh_smart(doc);
    }
    current_context().undo->mark_changed();
    return true;
  });
  /* The bone that moves each point of a layer most ("" when none does), and the layer's own bone. */
  m.def("layer_point_bones", [layer_or_raise](uint64_t id) {
    const Document &doc = *current_context().document;
    const Layer &layer = layer_or_raise(id);
    const Layer *armature = layer_armature(doc, layer.id);
    py::list result;
    for (const MeshPoint &p : layer.drawing.points) {
      const BoneWeight *best = nullptr;
      for (const BoneWeight &w : p.weights) {
        if (w.weight > 0.0f && (!best || w.weight > best->weight)) {
          best = &w;
        }
      }
      const Bone *b = armature && best ? bone_find(*armature, best->bone) : nullptr;
      result.append(b ? b->name : std::string());
    }
    return result;
  });
  m.def("layer_parent_bone", [layer_or_raise](uint64_t id) {
    const Document &doc = *current_context().document;
    const Layer &layer = layer_or_raise(id);
    const Layer *armature = layer_armature(doc, layer.id);
    const Bone *b = armature ? bone_find(*armature, layer.parent_bone) : nullptr;
    return b ? b->name : std::string();
  });
  /* A bone layer's bone collections, in list order. */
  m.def("layer_bone_collections", [layer_or_raise](uint64_t id) {
    const Document &doc = *current_context().document;
    const Layer &armature = layer_or_raise(id);
    py::list result;
    {
      py::list bones, drawings;
      for (const Bone &b : armature.bones) {
        bones.append(b.name);
      }
      for (const Layer *l : document_layers_flat(doc)) {
        const Layer *owner = l->edit_with_bones ? layer_armature(doc, l->id) : nullptr;
        if (owner && owner->id == armature.id) {
          drawings.append(l->name);
        }
      }
      py::dict d;
      d["name"] = BONE_COLLECTION_ALL;
      d["visible"] = armature.all_bones_visible;
      d["solo"] = armature.all_bones_solo;
      d["shown"] = bone_collection_all_shown(armature);
      d["active"] = !bone_collection_find(armature, armature.active_collection);
      d["bones"] = bones;
      d["layers"] = drawings;
      d["fixed"] = true;
      result.append(d);
    }
    for (const BoneCollection &c : armature.bone_collections) {
      py::list bones, drawings;
      for (const Bone &b : armature.bones) {
        if (std::find(b.collections.begin(), b.collections.end(), c.id) != b.collections.end()) {
          bones.append(b.name);
        }
      }
      for (const Layer *l : document_layers_flat(doc)) {
        const Layer *owner = layer_armature(doc, l->id);
        if (owner && owner->id == armature.id && l->edit_with_bones &&
            std::find(l->collections.begin(), l->collections.end(), c.id) != l->collections.end())
        {
          drawings.append(l->name);
        }
      }
      py::dict d;
      d["name"] = c.name;
      d["visible"] = c.visible;
      d["solo"] = c.solo;
      d["shown"] = bone_collection_shown(armature, c);
      d["active"] = c.id == armature.active_collection;
      d["bones"] = bones;
      d["layers"] = drawings;
      d["fixed"] = false;
      result.append(d);
    }
    return result;
  });
  /* The bone collections a drawing is in (of its bone layer), and whether its points are edited
   * along with the bones now. */
  m.def("layer_collections", [layer_or_raise](uint64_t id) {
    const Document &doc = *current_context().document;
    const Layer &layer = layer_or_raise(id);
    const Layer *armature = layer_armature(doc, layer.id);
    py::list names;
    for (const uint64_t c_id : layer.collections) {
      if (const BoneCollection *c = armature ? bone_collection_find(*armature, c_id) : nullptr) {
        names.append(c->name);
      }
    }
    return py::make_tuple(names, layer_edited_with_bones(doc, layer));
  });
  /* The paths of the layer's channels keyed at `frame` (the timeline's Keyed Here list). */
  m.def("layer_keyed_at", [layer_or_raise](uint64_t id, int frame) {
    std::vector<std::string> paths;
    for (const auto &[layer, path] : layers_keyed_at({&layer_or_raise(id)}, KeyFilter(), frame)) {
      paths.push_back(path);
    }
    return paths;
  });
  /* The group whose Parent Bone overrides the layer's ("" when none). */
  m.def("layer_parent_bone_group", [layer_or_raise](uint64_t id) {
    const Layer *group = layer_binding_group(*current_context().document, layer_or_raise(id).id);
    return group ? group->name : std::string();
  });
  /* Where a point of the layer (layer space) shows in the document, bones included. */
  m.def("layer_to_document", [layer_or_raise](uint64_t id, std::pair<float, float> p) {
    const float2 q = layer_world_transform(*current_context().document, layer_or_raise(id).id).apply({p.first, p.second});
    return py::make_tuple(q.x, q.y);
  });

  /* --- Modifiers and the evaluated drawing ------------------------------------------------ */
  m.def("layer_modifiers", [layer_or_raise](uint64_t id) {
    py::list result;
    for (const Modifier &mod : layer_or_raise(id).modifiers) {
      py::dict d;
      d["type"] = mod.type == ModifierType::Armature ? "ARMATURE" : "MIRROR";
      d["name"] = mod.name;
      d["show_viewport"] = mod.show_viewport;
      d["show_render"] = mod.show_render;
      if (mod.type == ModifierType::Mirror) {
        d["axis_x"] = mod.axis_x;
        d["axis_y"] = mod.axis_y;
        d["center"] = py::make_tuple(mod.center.x, mod.center.y);
        d["merge"] = mod.merge;
        d["merge_distance"] = mod.merge_distance;
        d["mirror_bones"] = mod.mirror_bones;
      }
      result.append(d);
    }
    return result;
  });
  m.def("layer_modifier_set", [layer_or_raise](uint64_t id, int index, py::dict values) {
    Layer &layer = layer_or_raise(id);
    if (index < 0 || index >= int(layer.modifiers.size())) {
      throw py::index_error("no modifier " + std::to_string(index));
    }
    Modifier &mod = layer.modifiers[index];
    for (const auto &[key_obj, value] : values) {
      const std::string key = py::str(key_obj);
      if (key == "show_viewport") {
        mod.show_viewport = value.cast<bool>();
      }
      else if (key == "show_render") {
        mod.show_render = value.cast<bool>();
      }
      else if (key == "name") {
        mod.name = value.cast<std::string>();
      }
      else if (mod.type == ModifierType::Mirror && key == "axis_x") {
        mod.axis_x = value.cast<bool>();
      }
      else if (mod.type == ModifierType::Mirror && key == "axis_y") {
        mod.axis_y = value.cast<bool>();
      }
      else if (mod.type == ModifierType::Mirror && key == "center") {
        const auto p = value.cast<std::pair<float, float>>();
        mod.center = {p.first, p.second};
      }
      else if (mod.type == ModifierType::Mirror && key == "merge") {
        mod.merge = value.cast<bool>();
      }
      else if (mod.type == ModifierType::Mirror && key == "merge_distance") {
        mod.merge_distance = std::max(0.0f, value.cast<float>());
      }
      else if (mod.type == ModifierType::Mirror && key == "mirror_bones") {
        mod.mirror_bones = value.cast<bool>();
      }
      else {
        throw py::key_error("unknown modifier setting '" + key + "'");
      }
    }
    current_context().undo->mark_changed();
  });
  /* The drawing as it shows: (points [(x, y)], curve count, shapes [(name, closed)]). */
  m.def(
      "layer_evaluated",
      [layer_or_raise](uint64_t id, bool render) {
        Document &doc = *current_context().document;
        animation_sync(doc);
        const VectorDrawing d = layer_evaluated_drawing(doc, layer_or_raise(id), render);
        py::list points;
        for (const MeshPoint &p : d.points) {
          points.append(py::make_tuple(p.pos.x, p.pos.y));
        }
        py::list shapes;
        std::vector<float2> outline;
        for (const Shape &shape : d.shapes) {
          bool closed = false;
          const bool connected = shape_outline(d, shape, 4, outline, nullptr, &closed);
          shapes.append(py::make_tuple(shape.name, closed, connected));
        }
        return py::make_tuple(points, int(d.curves.size()), shapes);
      },
      py::arg("id"), py::arg("render") = false);
  /* Weights: how the active bone (or a named one) moves each point, and painting them. */
  m.def("layer_bone_weights", [layer_or_raise](uint64_t id, const std::string &bone_name) {
    Document &doc = *current_context().document;
    animation_sync(doc);
    const Layer &layer = layer_or_raise(id);
    const Layer *armature = layer_armature(doc, layer.id);
    const Bone *bone = armature ? bone_find_by_name(*armature, bone_name) : nullptr;
    if (!bone) {
      throw py::key_error("no bone named '" + bone_name + "' in this layer's bone layer");
    }
    return layer_point_bone_weights(doc, layer, bone->id);
  });
  m.def("layer_painted_weights", [layer_or_raise](uint64_t id) {
    const Document &doc = *current_context().document;
    const Layer &layer = layer_or_raise(id);
    const Layer *armature = layer_armature(doc, layer.id);
    py::list result;
    for (const MeshPoint &p : layer.drawing.points) {
      py::dict d;
      for (const BoneWeight &w : p.weights) {
        const Bone *b = armature ? bone_find(*armature, w.bone) : nullptr;
        if (b) {
          d[py::str(b->name)] = w.weight;
        }
      }
      result.append(d);
    }
    return result;
  });
  m.def("layer_weight_paint", [layer_or_raise](uint64_t id, std::pair<float, float> center, float radius,
                                               const std::string &bone_name, float weight, float strength,
                                               const std::string &mode, bool normalize, bool selected_only) {
    Document &doc = *current_context().document;
    animation_sync(doc);
    Layer &layer = layer_or_raise(id);
    const Layer *armature = layer_armature(doc, layer.id);
    const Bone *bone = armature ? bone_find_by_name(*armature, bone_name) : nullptr;
    if (!bone) {
      throw py::key_error("no bone named '" + bone_name + "' in this layer's bone layer");
    }
    WeightPaintBrush brush;
    brush.center = {center.first, center.second};
    brush.radius = radius;
    brush.bone = bone->id;
    brush.weight = weight;
    brush.strength = strength;
    brush.mode = mode == "ADD" ? WeightPaintMode::Add : mode == "SUBTRACT" ? WeightPaintMode::Subtract : WeightPaintMode::Draw;
    brush.normalize = normalize;
    brush.selected_only = selected_only;
    const int changed = layer_weight_paint(doc, layer, brush);
    current_context().undo->mark_changed();
    return changed;
  });
  /* Mean positions: of the selected points, or of a shape's points. Setting one moves them all. */
  const auto points_of = [layer_or_raise](uint64_t id, int shape) {
    const VectorDrawing &d = layer_or_raise(id).drawing;
    if (shape < 0) {
      return drawing_selected_points(d);
    }
    if (shape >= int(d.shapes.size())) {
      throw py::index_error("no shape " + std::to_string(shape));
    }
    return shape_points(d, d.shapes[shape]);
  };
  m.def("layer_mean_position", [layer_or_raise, points_of](uint64_t id, int shape) -> py::object {
    float2 mean;
    if (!drawing_points_mean(layer_or_raise(id).drawing, points_of(id, shape), &mean)) {
      return py::none();
    }
    return py::make_tuple(mean.x, mean.y);
  });
  m.def("layer_set_mean_position", [layer_or_raise, points_of](uint64_t id, int shape, std::pair<float, float> pos) {
    VectorDrawing &d = layer_or_raise(id).drawing;
    const std::vector<int> points = points_of(id, shape);
    float2 mean;
    if (drawing_points_mean(d, points, &mean)) {
      drawing_points_move(d, points, float2(pos.first, pos.second) - mean);
      current_context().undo->mark_changed();
    }
  });
  m.def("layer_shapes", [layer_or_raise](uint64_t id) {
    /* Bottom first: [(name, [curve indices of its edges]), ...]. */
    py::list result;
    for (const Shape &shape : layer_drawing(*current_context().document, layer_or_raise(id)).shapes) {
      py::list curves;
      for (const ShapeEdge &edge : shape.edges) {
        if (!curves.contains(edge.curve)) {
          curves.append(edge.curve);
        }
      }
      result.append(py::make_tuple(shape.name, curves));
    }
    return result;
  });
  m.def(
      "layer_add_primitive",
      [layer_or_raise](uint64_t id, const std::string &kind_name, const std::vector<std::pair<float, float>> &points,
                       bool fill) {
        Context &C = current_context();
        Layer &layer = layer_or_raise(id);
        if (layer.is_group() || layer.is_reference()) {
          throw py::value_error("Shapes can only be added to vector layers that are not references");
        }
        PrimitiveKind kind;
        if (!primitive_from_identifier(kind_name, &kind)) {
          throw py::value_error("add_primitive draws BOX or CIRCLE; use add_curve for curves");
        }
        std::vector<float2> pts;
        for (const auto &[x, y] : points) {
          pts.push_back({x, y});
        }
        const VectorDrawing shape = primitive_drawing(kind, pts, C.wm->tool_settings.style, fill);
        if (shape.curves.empty()) {
          return false;
        }
        drawing_append_selected(layer.drawing, shape);
        C.undo->mark_changed();
        return true;
      },
      py::arg("id"), py::arg("kind"), py::arg("points"), py::arg("fill") = false);
  m.def("layer_shape_outline", [layer_or_raise](uint64_t id, int shape, int steps) {
    /* A shape's outline as drawn: steps samples per curve segment. */
    const VectorDrawing &drawing = layer_drawing(*current_context().document, layer_or_raise(id));
    if (shape < 0 || shape >= int(drawing.shapes.size())) {
      throw py::index_error("Shape index out of range");
    }
    std::vector<float2> outline;
    bool closed = false;
    shape_outline(drawing, drawing.shapes[shape], std::clamp(steps, 1, 256), outline, nullptr, &closed);
    py::list result;
    for (const float2 &p : outline) {
      result.append(py::make_tuple(p.x, p.y));
    }
    return py::make_tuple(result, closed);
  });
  /* A layer's masks: [{"outside", "layers" (names; "" for one that's gone), "enabled"}]. */
  m.def("layer_masks", [layer_or_raise](uint64_t id) {
    Document &doc = *current_context().document;
    py::list result;
    for (const LayerMask &mask : layer_or_raise(id).masks) {
      py::dict d;
      d["outside"] = mask.outside;
      d["enabled"] = mask.enabled;
      d["render"] = mask.render;
      d["expand"] = mask.expand;
      d["fills_only"] = mask.fills_only;
      d["filled"] = mask.filled;
      d["extend"] = mask_extend_identifier(mask.extend);
      py::list names;
      for (const uint64_t l : mask.layers) {
        const Layer *found = layer_find(doc, l);
        names.append(found ? found->name : std::string());
      }
      d["layers"] = names;
      result.append(d);
    }
    return result;
  });
  /* The layers whose masks name this one. */
  m.def("layer_masking", [layer_or_raise](uint64_t id) {
    Document &doc = *current_context().document;
    layer_or_raise(id);
    py::list names;
    for (const Layer *l : document_layers_flat(doc)) {
      for (const LayerMask &mask : l->masks) {
        if (std::find(mask.layers.begin(), mask.layers.end(), id) != mask.layers.end()) {
          names.append(l->name);
          break;
        }
      }
    }
    return names;
  });
  /* A layer's Draw Order now: ("NORMAL", ""), ("FRONT", target name) or ("BEHIND", target name). */
  m.def("layer_draw_order", [layer_or_raise](uint64_t id) {
    Document &doc = *current_context().document;
    animation_sync(doc);
    const DrawOrderRule rule = draw_order_rule(layer_or_raise(id).draw_order);
    const Layer *target = rule.mode == DrawOrderMode::Normal ? nullptr : layer_find(doc, rule.target);
    return py::make_tuple(draw_order_mode_identifier(rule.mode), target ? target->name : std::string());
  });
  /* The drawing layers as drawn now (rules applied), bottom first, by name; hidden ones left out. */
  m.def("draw_order", []() {
    Document &doc = *current_context().document;
    animation_sync(doc);
    py::list names;
    for (const Layer *l : draw_order_layers(doc, [](const Layer &layer) { return layer.visible; })) {
      names.append(l->name);
    }
    return names;
  });
  m.def("layer_hidden_edges", [layer_or_raise](uint64_t id) {
    py::list result;
    const VectorDrawing &drawing = layer_drawing(*current_context().document, layer_or_raise(id));
    for (int c = 0; c < int(drawing.curves.size()); c++) {
      for (int s = 0; s < drawing.curves[c].segment_count(); s++) {
        if (drawing.curves[c].points[s].edge_hidden) {
          result.append(py::make_tuple(c, s));
        }
      }
    }
    return result;
  });
  m.def("layer_set_selected_edges_visible", [layer_or_raise](uint64_t id, bool visible) {
    Layer &layer = layer_or_raise(id);
    /* On a reference, the edges shown or hidden are its own (the original's stay as they are). */
    animation_record(*current_context().document);
    const std::vector<ShapeEdge> edges = drawing_selected_edges(layer.drawing);
    drawing_set_edges_hidden(layer.drawing, edges, !visible);
    animation_record(*current_context().document);
    current_context().undo->mark_changed();
    return int(edges.size());
  });
  m.def("layer_corners", [layer_or_raise](uint64_t id) {
    py::list result;
    for (const MeshPoint &p : layer_drawing(*current_context().document, layer_or_raise(id)).points) {
      result.append(p.corner == CornerType::Sharp ? "SHARP" : "ROUND");
    }
    return result;
  });
  m.def("layer_set_selected_corner", [layer_or_raise](uint64_t id, const std::string &corner) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference()) {
      throw py::value_error("A reference's points are edited through its original");
    }
    if (corner != "ROUND" && corner != "SHARP") {
      throw py::value_error("corner must be 'ROUND' or 'SHARP'");
    }
    drawing_set_selected_corner(layer.drawing, corner == "SHARP" ? CornerType::Sharp : CornerType::Round);
    current_context().undo->mark_changed();
  });
  m.def("layer_radii", [layer_or_raise](uint64_t id) {
    py::list result;
    for (const MeshPoint &p : layer_drawing(*current_context().document, layer_or_raise(id)).points) {
      result.append(p.radius);
    }
    return result;
  });
  m.def("layer_mean_radius", [layer_or_raise](uint64_t id) {
    const float mean = drawing_selected_mean_radius(layer_drawing(*current_context().document, layer_or_raise(id)));
    return mean < 0.0f ? py::object(py::none()) : py::object(py::float_(mean));
  });
  m.def("layer_set_mean_radius", [layer_or_raise](uint64_t id, float mean) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference()) {
      throw py::value_error("A reference's points are edited through its original");
    }
    drawing_set_selected_mean_radius(layer.drawing, mean);
    current_context().undo->mark_changed();
  });
  m.def("layer_counts", [layer_or_raise](uint64_t id) {
    /* Counts for the drawing the layer shows, which for a reference is its original's. */
    const VectorDrawing &drawing = layer_drawing(*current_context().document, layer_or_raise(id));
    return py::make_tuple(drawing.points.size(), drawing.curves.size(), drawing.shapes.size());
  });
  m.def("layer_select_points", [layer_or_raise](uint64_t id, const std::vector<int> &indices, bool select) {
    VectorDrawing &drawing = layer_or_raise(id).drawing;
    for (const int i : indices) {
      if (i < 0 || i >= int(drawing.points.size())) {
        throw py::index_error("Point index out of range");
      }
      drawing.points[i].selected = select;
    }
  });

  /* The last warning shown at the mouse: (count shown so far, title, message). */
  m.def("mouse_report", [] {
    const MouseReport &r = mouse_report();
    return py::make_tuple(r.serial, r.title, r.message);
  });
  m.def("report", [](const std::string &type, const std::string &message) {
    ReportType report_type = ReportType::Info;
    if (type == "ERROR") {
      report_type = ReportType::Error;
    }
    else if (type == "WARNING") {
      report_type = ReportType::Warning;
    }
    report(report_type, message);
  });
}

namespace sky::python {

bool init(const std::filesystem::path &scripts_dir, Context *C)
{
  g_context = C;
  g_scripts_dir = scripts_dir;

  /* Isolated: ignore PYTHONPATH and user site-packages so the app behaves the same
   * on every machine. Home points at the Python installation the build used. */
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  const std::wstring home = std::filesystem::path(SKY_PYTHON_HOME).wstring();
  PyStatus status = PyConfig_SetString(&config, &config.home, home.c_str());
  if (PyStatus_Exception(status)) {
    PyConfig_Clear(&config);
    report(ReportType::Error, "Could not configure Python");
    return false;
  }

  try {
    py::initialize_interpreter(&config, 0, nullptr, false);
  }
  catch (const std::exception &e) {
    PyConfig_Clear(&config);
    report(ReportType::Error, std::string("Could not start Python: ") + e.what());
    return false;
  }
  PyConfig_Clear(&config);
  g_initialized = true;

  try {
    py::module_ sys = py::module_::import("sys");
    sys.attr("path").attr("insert")(0, path_to_utf8(scripts_dir / "modules"));
    py::module_ startup = py::module_::import("sky._startup");
    return startup.attr("load_startup_scripts")(path_to_utf8(scripts_dir / "startup")).cast<bool>();
  }
  catch (py::error_already_set &error) {
    report(ReportType::Error, "Error starting the sky module:\n" + format_exception(error));
    return false;
  }
}

void shutdown()
{
  if (!g_initialized) {
    return;
  }
  /* Python operators hold Python objects; release them while the interpreter is alive. */
  operators().remove_python();
  py::finalize_interpreter();
  g_initialized = false;
  g_context = nullptr;
}

static bool handle_script_error(py::error_already_set &error, const std::string &where)
{
  if (error.matches(PyExc_SystemExit)) {
    py::object code = error.value().attr("code");
    return code.is_none() || (py::isinstance<py::int_>(code) && code.cast<int>() == 0);
  }
  report(ReportType::Error, "Error in " + where + ":\n" + format_exception(error));
  return false;
}

bool run_file(const std::filesystem::path &path)
{
  const std::string path_utf8 = path_to_utf8(path);
  try {
    py::dict globals;
    globals["__name__"] = "__main__";
    globals["__file__"] = path_utf8;
    globals["__builtins__"] = py::module_::import("builtins");
    py::eval_file(path_utf8, globals);
    return true;
  }
  catch (py::error_already_set &error) {
    return handle_script_error(error, path_utf8);
  }
}

const std::filesystem::path &scripts_dir()
{
  return g_scripts_dir;
}

/* Calls a method of the console object. Python errors are reported, and give nullopt. */
template<typename... Args> static std::optional<py::object> console_call(const char *method, Args &&...args)
{
  if (!g_initialized) {
    return std::nullopt;
  }
  try {
    py::object console = py::module_::import("sky._console").attr("get")();
    return console.attr(method)(std::forward<Args>(args)...);
  }
  catch (py::error_already_set &error) {
    report(ReportType::Error, std::string("Console error in ") + method + ":\n" + format_exception(error));
  }
  return std::nullopt;
}

uint64_t console_version()
{
  if (!g_initialized) {
    return 0;
  }
  try {
    return py::module_::import("sky._console").attr("get")().attr("version").cast<uint64_t>();
  }
  catch (py::error_already_set &error) {
    report(ReportType::Error, "Console error:\n" + format_exception(error));
    return 0;
  }
}

std::vector<ConsoleLine> console_lines()
{
  std::vector<ConsoleLine> lines;
  if (!g_initialized) {
    return lines;
  }
  try {
    py::list items = py::module_::import("sky._console").attr("get")().attr("lines");
    lines.reserve(items.size());
    for (py::handle item : items) {
      py::tuple pair = item.cast<py::tuple>();
      lines.push_back({pair[0].cast<std::string>(), pair[1].cast<std::string>()});
    }
  }
  catch (py::error_already_set &error) {
    report(ReportType::Error, "Console error:\n" + format_exception(error));
  }
  return lines;
}

std::string console_prompt()
{
  const std::optional<py::object> result = console_call("prompt");
  return result ? result->cast<std::string>() : ">>> ";
}

std::vector<std::string> console_history()
{
  if (!g_initialized) {
    return {};
  }
  try {
    return py::module_::import("sky._console").attr("get")().attr("history").cast<std::vector<std::string>>();
  }
  catch (py::error_already_set &error) {
    report(ReportType::Error, "Console error:\n" + format_exception(error));
    return {};
  }
}

void console_push(const std::string &line)
{
  console_call("push", line);
}

std::string console_complete(const std::string &line)
{
  const std::optional<py::object> result = console_call("complete", line);
  return result ? result->cast<std::string>() : line;
}

void console_clear()
{
  console_call("clear");
}

bool text_run(const std::string &name, const std::string &source)
{
  const std::optional<py::object> result = console_call("run_text", name, source);
  return result && result->cast<bool>();
}

std::string remote_run(const std::string &code)
{
  try {
    const py::object result = py::module_::import("sky._remote").attr("run")(code);
    return py::module_::import("json").attr("dumps")(result).cast<std::string>();
  }
  catch (py::error_already_set &error) {
    return py::module_::import("json").attr("dumps")(py::dict(py::arg("error") = std::string(error.what())))
        .cast<std::string>();
  }
}

bool run_string(const std::string &code)
{
  try {
    py::dict globals;
    globals["__name__"] = "__main__";
    globals["__builtins__"] = py::module_::import("builtins");
    py::exec(code, globals);
    return true;
  }
  catch (py::error_already_set &error) {
    return handle_script_error(error, "--python-expr");
  }
}

}  // namespace sky::python
