#include "python/py_runtime.hh"

#include <pybind11/embed.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "core/log.hh"
#include "data/document.hh"
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
      result.append(info);
    }
    return result;
  });

  m.def("ops_call", [](const std::string &idname) {
    if (!operators().find(idname)) {
      throw py::key_error("Operator not found: " + idname);
    }
    return std::string(op_status_name(operator_call(current_context(), idname)));
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
      [](const std::string &type, bool ctrl, bool shift, bool alt, const std::string &area) {
        Context &C = current_context();
        const std::string saved_area = C.area_type;
        if (!area.empty()) {
          C.area_type = area;
        }
        const bool handled = wm_handle_key_event(C, {to_upper(type), ctrl, shift, alt});
        C.area_type = saved_area;
        return handled;
      },
      py::arg("type"), py::arg("ctrl") = false, py::arg("shift") = false, py::arg("alt") = false,
      py::arg("area") = "");

  m.def("undo_history", []() {
    UndoStack &undo = *current_context().undo;
    std::vector<std::string> names;
    for (size_t i = 0; i < undo.size(); i++) {
      names.push_back(undo.step_name(i));
    }
    return py::make_tuple(names, undo.active());
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
