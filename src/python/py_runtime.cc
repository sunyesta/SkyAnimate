#include "python/py_runtime.hh"

#include <pybind11/embed.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <stdexcept>

#include "core/log.hh"
#include "data/document.hh"
#include "ops/file_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "rna/rna.hh"
#include "wm/context.hh"
#include "wm/event_system.hh"
#include "wm/keymap.hh"

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

  m.def(
      "event_simulate",
      [](const std::string &type, bool ctrl, bool shift, bool alt, const std::string &area, bool typing) {
        Context &C = current_context();
        const std::string saved_area = C.area_type;
        if (!area.empty()) {
          C.area_type = area;
        }
        const bool handled = wm_handle_key_event(C, {to_upper(type), ctrl, shift, alt}, typing);
        C.area_type = saved_area;
        return handled;
      },
      py::arg("type"), py::arg("ctrl") = false, py::arg("shift") = false, py::arg("alt") = false,
      py::arg("area") = "", py::arg("typing") = false);

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
    Layer *layer = layer_find(*current_context().document, id);
    if (!layer) {
      throw py::key_error("No layer with id " + std::to_string(id));
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
  m.def("layer_points", [layer_or_raise](uint64_t id) {
    const Document &doc = *current_context().document;
    py::list result;
    for (const MeshPoint &p : layer_drawing(doc, layer_or_raise(id)).points) {
      result.append(py::make_tuple(p.pos.x, p.pos.y, p.selected));
    }
    return result;
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
    if (layer.is_reference()) {
      throw py::value_error("A reference's edges are edited through its original");
    }
    const std::vector<ShapeEdge> edges = drawing_selected_edges(layer.drawing);
    drawing_set_edges_hidden(layer.drawing, edges, !visible);
    current_context().undo->mark_changed();
    return int(edges.size());
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
