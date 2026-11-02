#include "python/py_runtime.hh"

#include <pybind11/embed.h>
#include <pybind11/functional.h>
#include <pybind11/stl.h>

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <stdexcept>

#include "anim/keep_poses.hh"
#include "anim/onion.hh"
#include "anim/sculpt.hh"
#include "assets/draw_presets.hh"
#include "wm/pen_wintab.hh"
#include "assets/sculpt_presets.hh"
#include "anim/animation.hh"
#include "anim/follow_point.hh"
#include "anim/deform.hh"
#include "anim/vector_deform.hh"
#include "anim/draw_order.hh"
#include "anim/switch.hh"
#include "anim/bone_family.hh"
#include "anim/dynamics.hh"
#include "anim/reference.hh"
#include "wm/clipboard_image.hh"
#include "data/layer_mirror.hh"
#include "data/bones.hh"
#include "data/camera.hh"
#include "data/detail.hh"
#include "data/media_layer.hh"
#include "text/text_layer.hh"
#include "text/text_outline.hh"
#include "media/media.hh"
#include "assets/asset_library.hh"
#include "anim/drivers.hh"
#include "anim/layer_pick.hh"
#include "assets/brush_presets.hh"
#include "assets/brushes.hh"
#include "core/crash.hh"
#include "core/log.hh"
#include "gpu/brush_stroke.hh"
#include "gpu/scene_renderer.hh"
#include "gpu/tessellate.hh"
#include "data/document.hh"
#include "ops/edit_ops.hh"
#include "ops/file_ops.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "rna/rna.hh"
#include "wm/context.hh"
#include "wm/pen_input.hh"
#include "wm/event_system.hh"
#include "wm/file_drop.hh"
#include "wm/keymap.hh"
#include "wm/remote.hh"
#include "editors/brush_editor.hh"
#include "editors/editors.hh"
#include "editors/statistics.hh"

namespace py = pybind11;

namespace oanim {
/* editors/ui.hh (which needs ImGui): the Link Report window. */
void ui_switch_link_report_open(uint64_t link);
}  // namespace oanim

namespace oanim::python {

/* See python_runs. */
static uint64_t g_python_runs = 0;

static Context *g_context = nullptr;
static bool g_initialized = false;
static std::filesystem::path g_scripts_dir;

static Context &current_context()
{
  if (!g_context) {
    throw std::runtime_error("Open Animate context is not available");
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

/* The id of the point numbered `value` on shape `shape` of `target`, as a Follow takes it: 0 for None. */
static uint64_t follow_shape_point(Layer *target, uint64_t shape, const py::handle value)
{
  if (value.is_none()) {
    return 0;
  }
  const int index = target ? drawing_shape_index(target->drawing, shape) : -1;
  if (index < 0) {
    throw py::value_error("Pick its shape first");
  }
  drawing_ensure_ids(target->drawing);
  const int point = value.cast<int>();
  const std::vector<int> on = shape_points(target->drawing, target->drawing.shapes[size_t(index)]);
  if (std::find(on.begin(), on.end(), point) == on.end()) {
    throw py::value_error("point " + std::to_string(point) + " isn't on the shape");
  }
  return target->drawing.points[size_t(point)].id;
}

/* That point's number, None when there's none (or it's gone). */
static py::object follow_shape_point_number(const Layer *target, uint64_t id)
{
  const int at = target && id != 0 ? drawing_point_index(target->drawing, id) : -1;
  return at >= 0 ? py::object(py::int_(at)) : py::object(py::none());
}

static const char *draw_mode_id(DrawMode mode)
{
  return mode == DrawMode::Pen            ? "PEN"
         : mode == DrawMode::FillPen      ? "FILL_PEN"
         : mode == DrawMode::FillEraser   ? "FILL_ERASER"
         : mode == DrawMode::StrokeEraser ? "STROKE_ERASER"
         : mode == DrawMode::Trimmer      ? "TRIMMER"
                                          : "FREEHAND";
}

/* The Stroke Pen's settings by name, as oanim.wm.draw_settings gives them. */
static void ink_settings_read(const InkSettings &ink, py::dict &d)
{
  d["use_pressure"] = ink.use_pressure;
  d["stabilizer"] = ink.stabilizer == InkStabilizer::Off     ? "OFF"
                    : ink.stabilizer == InkStabilizer::Smart ? "SMART"
                                                             : "PULLED_STRING";
  d["smoothing"] = ink.smoothing;
  d["string_length"] = ink.string_length;
  d["catch_up"] = ink.catch_up;
  d["fidelity"] = ink.fidelity;
  d["pressure_smoothing"] = ink.pressure_smoothing;
  d["pressure_curve"] = py::make_tuple(ink.pressure_curve_a.x, ink.pressure_curve_a.y, ink.pressure_curve_b.x,
                                       ink.pressure_curve_b.y);
  d["min_size"] = ink.min_size;
  d["speed_width"] = ink.speed_width;
  d["tilt_width"] = ink.tilt_width;
  d["taper_start"] = ink.taper_start;
  d["taper_end"] = ink.taper_end;
  d["erase_whole"] = ink.erase_whole;
}

/* A Stroke Pen setting by name, as oanim.wm.set_draw takes it; false: no such setting. */
static bool ink_setting(InkSettings &ink, const std::string &key, const py::handle value)
{
  const auto amount = [&]() { return std::clamp(value.cast<float>(), 0.0f, 1.0f); };
  if (key == "stabilizer") {
    const std::string name = value.cast<std::string>();
    if (name != "OFF" && name != "SMART" && name != "PULLED_STRING") {
      throw py::value_error("stabilizer is OFF, SMART or PULLED_STRING");
    }
    ink.stabilizer = name == "OFF"     ? InkStabilizer::Off
                     : name == "SMART" ? InkStabilizer::Smart
                                       : InkStabilizer::PulledString;
  }
  else if (key == "use_pressure") {
    ink.use_pressure = value.cast<bool>();
  }
  else if (key == "smoothing") {
    ink.smoothing = amount();
  }
  else if (key == "string_length") {
    ink.string_length = std::max(value.cast<float>(), 0.0f);
  }
  else if (key == "catch_up") {
    ink.catch_up = value.cast<bool>();
  }
  else if (key == "fidelity") {
    ink.fidelity = amount();
  }
  else if (key == "pressure_smoothing") {
    ink.pressure_smoothing = amount();
  }
  else if (key == "pressure_curve") {
    const auto [x1, y1, x2, y2] = value.cast<std::tuple<float, float, float, float>>();
    ink.pressure_curve_a = {std::clamp(x1, 0.0f, 1.0f), std::clamp(y1, 0.0f, 1.0f)};
    ink.pressure_curve_b = {std::clamp(x2, 0.0f, 1.0f), std::clamp(y2, 0.0f, 1.0f)};
  }
  else if (key == "min_size") {
    ink.min_size = amount();
  }
  else if (key == "speed_width") {
    ink.speed_width = amount();
  }
  else if (key == "tilt_width") {
    ink.tilt_width = amount();
  }
  else if (key == "taper_start") {
    ink.taper_start = std::max(value.cast<float>(), 0.0f);
  }
  else if (key == "taper_end") {
    ink.taper_end = std::max(value.cast<float>(), 0.0f);
  }
  else if (key == "erase_whole") {
    ink.erase_whole = value.cast<bool>();
  }
  else {
    return false;
  }
  return true;
}

}  // namespace oanim::python

/* ---------------------------------------------------------------------------------
 * The `_oanim` module. Keep it small: the friendly API lives in scripts/modules/oanim.
 */

PYBIND11_EMBEDDED_MODULE(_oanim, m)
{
  using namespace oanim;
  using namespace oanim::python;

  m.doc() = "Low-level Open Animate API. Use the `oanim` package instead.";

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
             /* A text layer's text or layout changed: its letters made again now, for what reads them next. */
             document_sync_text(*current_context().document);
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
      .def("__repr__", [](const rna::PointerRNA &ptr) { return "<oanim " + ptr.type->identifier + ">"; });

  py::class_<Context>(m, "Context")
      .def_property_readonly("document",
                             [](Context &C) { return rna::PointerRNA{C.document, &Document::rna()}; })
      .def_property_readonly("area_type", [](const Context &C) { return C.area_type; })
      .def_property_readonly("is_background", [](const Context &C) { return C.wm && C.wm->background; })
      .def_property_readonly("is_playing", [](const Context &C) { return C.wm && C.wm->playing; })
      /* The status bar's statistics: what is being worked in, counts and the text shown. */
      .def_property_readonly("statistics", [](Context &C) {
        const Statistics s = document_statistics(C, true);
        const char *mode = s.mode == Statistics::Mode::Drawings ? "DRAWINGS"
                           : s.mode == Statistics::Mode::Bones  ? "BONES"
                                                                : "DOCUMENT";
        std::string text;
        for (const std::string &part : statistics_parts(s, true, false, nullptr)) {
          text += (text.empty() ? "" : " | ") + part;
        }
        py::dict d;
        d["mode"] = mode;
        d["name"] = s.name;
        d["drawings"] = s.drawings;
        d["points"] = py::make_tuple(s.points_selected, s.points);
        d["curves"] = py::make_tuple(s.curves_selected, s.curves);
        d["segments"] = py::make_tuple(s.segments_selected, s.segments);
        d["shapes"] = py::make_tuple(s.shapes_selected, s.shapes);
        d["bones"] = py::make_tuple(s.bones_selected, s.bones);
        d["layers"] = py::make_tuple(s.layers_selected, s.layers);
        d["text"] = text;
        return d;
      });

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
          g_python_runs++;
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

  /* R on the picked bones as the viewport's drag does it, one mouse update per angle (degrees, from
   * where the drag began), then confirmed (for tests: a real drag needs a mouse). */
  m.def("debug_rotate_bones", [](uint64_t id, const std::vector<float> &degrees) {
    Context &C = current_context();
    Layer *armature = layer_find(*C.document, id);
    if (!armature || !armature->is_bone_layer()) {
      throw py::value_error("not a bone layer");
    }
    const std::vector<Bone> original = armature->bones;
    const bool setup = C.document->frame_current == 0;
    for (const float d : degrees) {
      armature_rotate_selected(*armature, original, d * PI / 180.0f, setup);
      animation_refresh_smart(*C.document);
      /* The frame after it, as the main loop does. */
      animation_sync(*C.document);
    }
    C.undo->push("Rotate Bones", *C.document);
  });
  /* Crashes on purpose, to try the crash report (oanim.wm.debug_crash). */
  m.def("debug_crash", []() {
    volatile bool go = true;
    if (go) {
      crash_now();
    }
  });
  /* A file dropped on the window at (x, y), as from Explorer (oanim.wm.drop_simulate): the same SDL
   * event, handled on the next frame. */
  m.def("drop_simulate", [](const std::string &path, float x, float y) {
    /* SDL keeps the pointer until the event is handled. */
    static std::vector<std::unique_ptr<std::string>> paths;
    paths.push_back(std::make_unique<std::string>(path));
    SDL_Event e{};
    e.type = SDL_EVENT_DROP_FILE;
    e.drop.x = x;
    e.drop.y = y;
    e.drop.data = paths.back()->c_str();
    return SDL_PushEvent(&e);
  });
  /* A key pressed and let go as the keyboard sends it (oanim.wm.key_press_simulate): SDL events, so
   * ImGui sees them too, handled on the next frames. `name` is SDL's key name ("Tab", "A"). */
  m.def("key_press_simulate", [](const std::string &name, bool ctrl, bool shift, bool alt) {
    const SDL_Keycode key = SDL_GetKeyFromName(name.c_str());
    if (key == SDLK_UNKNOWN) {
      throw py::value_error("no key named '" + name + "'");
    }
    SDL_Keymod mod = SDL_KMOD_NONE;
    if (ctrl) {
      mod |= SDL_KMOD_LCTRL;
    }
    if (shift) {
      mod |= SDL_KMOD_LSHIFT;
    }
    if (alt) {
      mod |= SDL_KMOD_LALT;
    }
    /* For the main window, as the keyboard's are: ImGui takes only keys for a window of its. */
    int count = 0;
    SDL_Window **windows = SDL_GetWindows(&count);
    const SDL_WindowID window = windows && count > 0 ? SDL_GetWindowID(windows[0]) : 0;
    SDL_free(windows);
    for (const bool down : {true, false}) {
      SDL_Event e{};
      e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
      e.key.windowID = window;
      e.key.key = key;
      e.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
      e.key.mod = mod;
      e.key.down = down;
      SDL_PushEvent(&e);
    }
  });
  /* The project's Viewport Shading (oanim.wm.shading / set_shading). */
  m.def("shading", []() {
    const ViewportShading &sh = current_context().document->shading;
    py::dict d;
    d["layer_caching"] = sh.layer_caching;
    d["zoom_preview"] = sh.zoom_preview;
    d["gpu_geometry"] = sh.gpu_geometry;
    return d;
  });
  m.def("set_shading", [](const std::string &name, bool on) {
    Context &C = current_context();
    ViewportShading &sh = C.document->shading;
    if (name == "layer_caching") {
      sh.layer_caching = on;
    }
    else if (name == "zoom_preview") {
      sh.zoom_preview = on;
    }
    else if (name == "gpu_geometry") {
      sh.gpu_geometry = on;
    }
    else {
      throw py::value_error("no Viewport Shading setting '" + name +
                            "' (layer_caching, zoom_preview, gpu_geometry)");
    }
    C.undo->mark_changed();
  });
  /* Render statistics (oanim.wm.set_render_stats / render_stats), for benchmarks. */
  m.def("set_render_stats", [](bool on) { render_stats_set(on); });
  m.def("fill_covers", [](const std::vector<std::pair<float, float>> &outline) {
    std::vector<float2> polygon;
    for (const auto &[x, y] : outline) {
      polygon.emplace_back(x, y);
    }
    std::vector<Vertex> triangles;
    tessellate_fill(polygon, Color{}, 0.0f, triangles);
    double area = 0.0;
    for (size_t i = 0; i + 2 < triangles.size(); i += 3) {
      const Vertex &a = triangles[i], &b = triangles[i + 1], &c = triangles[i + 2];
      area += std::abs((double(b.x) - a.x) * (double(c.y) - a.y) - (double(b.y) - a.y) * (double(c.x) - a.x)) * 0.5;
    }
    return area;
  });
  m.def("render_stats", []() {
    py::list out;
    for (const RenderStats &r : render_stats_take()) {
      py::dict d;
      d["width"] = r.width;
      d["height"] = r.height;
      d["final"] = r.final_render;
      d["cpu_ms"] = r.cpu_ms;
      d["prepare_ms"] = r.prepare_ms;
      d["evaluate_ms"] = r.evaluate_ms;
      d["make_ms"] = r.make_ms;
      d["mask_make_ms"] = r.mask_make_ms;
      d["outline_ms"] = r.outline_ms;
      d["earcut_ms"] = r.earcut_ms;
      d["fan_ms"] = r.fan_ms;
      d["stroke_ms"] = r.stroke_ms;
      d["drawings_made"] = r.drawings_made;
      d["gpu_ms"] = r.gpu_ms;
      py::dict gpu, cpu, calls;
      for (const gpu::Timing &t : r.kinds) {
        gpu[py::str(t.kind)] = t.gpu_ms;
        cpu[py::str(t.kind)] = t.cpu_ms;
        calls[py::str(t.kind)] = t.calls;
      }
      d["gpu"] = gpu;
      d["cpu"] = cpu;
      d["calls"] = calls;
      out.append(d);
    }
    return out;
  });
  /* Characters typed (oanim.wm.text_simulate), as the keyboard's text input sends them. */
  m.def("text_simulate", [](const std::string &text) { ImGui::GetIO().AddInputCharactersUTF8(text.c_str()); });
  /* Files being dragged over the window at (x, y) (oanim.wm.file_drag_simulate): the previews show
   * them as for a drag from Explorer. No files: the drag ends. */
  m.def("file_drag_simulate", [](const std::vector<std::string> &files, float x, float y) {
    file_drag_set({!files.empty(), files, x, y});
  });
  /* Queues a mouse move (button -1) or press/release (oanim.wm.mouse_simulate), one per frame. */
  m.def(
      "mouse_simulate",
      [](float x, float y, int button, bool down, bool ctrl, bool alt, bool shift, float wheel, const py::object &pen,
         bool eraser) {
        WindowManager::SimulatedMouse e{x, y, button, down, ctrl, alt, shift, wheel};
        if (!pen.is_none()) {
          /* (x, y, pressure, tilt_x, tilt_y, time), all but x and y optional. */
          e.has_pen = true;
          e.pen_eraser = eraser;
          for (const py::handle item : pen) {
            const py::sequence v = py::reinterpret_borrow<py::sequence>(item);
            PenSample sample;
            sample.x = v[0].cast<float>();
            sample.y = v[1].cast<float>();
            sample.pressure = v.size() > 2 ? std::clamp(v[2].cast<float>(), 0.0f, 1.0f) : 1.0f;
            sample.tilt_x = v.size() > 3 ? v[3].cast<float>() : 0.0f;
            sample.tilt_y = v.size() > 4 ? v[4].cast<float>() : 0.0f;
            sample.time = v.size() > 5 ? v[5].cast<double>() + pen_now() : 0.0;
            e.pen.push_back(sample);
          }
        }
        current_context().wm->mouse_simulated.push_back(std::move(e));
      },
      py::arg("x"), py::arg("y"), py::arg("button"), py::arg("down"), py::arg("ctrl"), py::arg("alt"),
      py::arg("shift"), py::arg("wheel"), py::arg("pen") = py::none(), py::arg("eraser") = false);
  /* The pen as last heard from, and its current (or last) touch's samples. */
  m.def("pen", [](bool samples) {
    const WindowManager &wm = *current_context().wm;
    const TabletState &state = tablet_state();
    py::dict d;
    d["windows_ink"] = pen_input_seen();
    const TabletApi api = pen_input_api();
    d["api"] = api == TabletApi::Automatic ? "AUTOMATIC" : api == TabletApi::WindowsInk ? "WINDOWS_INK" : "WINTAB";
    d["wintab_installed"] = wintab_installed();
    d["wintab_open"] = wintab_started();
    d["from_wintab"] = pen_input_from_wintab();
    d["near"] = wm.pen_near;
    d["touching"] = state.touching;
    d["eraser"] = wm.pen_eraser;
    d["pressure"] = wm.pen_pressure;
    d["tilt"] = py::make_tuple(state.tilt_x, state.tilt_y);
    d["touch"] = pen_stroke_id();
    d["sample_count"] = pen_stroke().size();
    if (samples) {
      py::list list;
      const double began = pen_stroke().empty() ? 0.0 : pen_stroke().front().time;
      for (const PenSample &p : pen_stroke()) {
        list.append(py::make_tuple(p.x, p.y, p.pressure, p.tilt_x, p.tilt_y, p.time - began));
      }
      d["samples"] = list;
    }
    return d;
  });
  m.def("draw_settings", []() {
    const ToolSettings &ts = current_context().wm->tool_settings;
    py::dict d;
    d["mode"] = draw_mode_id(ts.draw_mode);
    d["preset"] = ts.draw_preset;
    d["fill_size"] = ts.fill_size;
    d["erase_all_layers"] = ts.draw_erase_all_layers;
    d["erase_selected"] = ts.draw_erase_selected;
    d["fill"] = ts.draw_fill;
    d["weld"] = ts.draw_weld;
    d["trim"] = ts.draw_trim;
    d["cursor_circle"] = ts.draw_cursor_circle;
    d["prediction"] = ts.draw_prediction;
    d["line_width"] = ts.style.line_width;
    ink_settings_read(ts.ink, d);
    return d;
  });
  /* The pens on the shelf (assets/draw_presets.hh). */
  m.def("draw_presets", []() {
    py::list list;
    for (const DrawPreset &p : draw_presets()) {
      py::dict d;
      d["name"] = p.name;
      d["mode"] = draw_mode_id(p.mode);
      d["line_width"] = p.line_width;
      d["category"] = p.category;
      d["builtin"] = p.builtin;
      ink_settings_read(p.ink, d);
      list.append(d);
    }
    return list;
  });
  m.def("set_draw", [](const py::dict &values) {
    ToolSettings &ts = current_context().wm->tool_settings;
    for (const auto &[key_obj, value] : values) {
      const std::string key = py::str(key_obj);
      if (key == "fill") {
        ts.draw_fill = value.cast<bool>();
      }
      else if (key == "prediction") {
        ts.draw_prediction = std::clamp(value.cast<float>(), 0.0f, 30.0f);
      }
      else if (key == "line_width") {
        ts.style.line_width = std::max(value.cast<float>(), 0.0f);
      }
      else if (key == "fill_color" || key == "line_color") {
        const auto [r, g, b, a] = value.cast<std::tuple<float, float, float, float>>();
        (key == "fill_color" ? ts.style.fill : ts.style.line) = {r, g, b, a};
      }
      else if (key == "fill_size") {
        ts.fill_size = std::max(value.cast<float>(), 0.0f);
      }
      else if (key == "erase_all_layers") {
        ts.draw_erase_all_layers = value.cast<bool>();
      }
      else if (key == "erase_selected") {
        ts.draw_erase_selected = value.cast<bool>();
      }
      else if (key == "weld") {
        ts.draw_weld = value.cast<bool>();
      }
      else if (key == "trim") {
        ts.draw_trim = value.cast<bool>();
      }
      else if (key == "cursor_circle") {
        ts.draw_cursor_circle = value.cast<bool>();
      }
      else if (!ink_setting(ts.ink, key, value)) {
        throw py::value_error("no draw setting '" + key + "' (see oanim.wm.draw_settings)");
      }
    }
  });
  /* The Stroke Pen's engine on samples (x, y, pressure, tilt_x, tilt_y, seconds), without the window: its line as
   * the pen left it and the tail to the last sample, the finished line, and the finished line's Bezier points. */
  m.def("ink_stroke", [](const std::vector<std::tuple<float, float, float, float, float, double>> &samples,
                         const py::dict &values, float pixel, float half_width, bool closed) {
    InkSettings settings;
    for (const auto &[key_obj, value] : values) {
      const std::string key = py::str(key_obj);
      if (!ink_setting(settings, key, value)) {
        throw py::value_error("no ink setting '" + key + "'");
      }
    }
    InkStroke ink;
    ink.begin(settings, {pixel, half_width, 1.0f});
    for (const auto &[x, y, pressure, tilt_x, tilt_y, time] : samples) {
      ink.add({{x, y}, pressure, {tilt_x, tilt_y}, time});
    }
    const auto listed = [](const std::vector<InkPoint> &points) {
      py::list list;
      for (const InkPoint &p : points) {
        list.append(py::make_tuple(p.pos.x, p.pos.y, p.radius, p.speed));
      }
      return list;
    };
    py::dict d;
    d["live"] = listed(ink.points());
    d["tail"] = listed(ink.tail(ink.last_sample()));
    ink.finish();
    d["points"] = listed(ink.points());
    py::list knots;
    for (const InkKnot &k : ink_fit(ink.points(), settings, ink.view(), closed)) {
      knots.append(py::make_tuple(k.pos.x, k.pos.y, k.radius, py::make_tuple(k.handle_in.x, k.handle_in.y),
                                  py::make_tuple(k.handle_out.x, k.handle_out.y), k.corner));
    }
    d["knots"] = knots;
    return d;
  });
  /* A WinTab packet as the app reads it: (screen x, screen y, pressure, tilt_x, tilt_y). */
  m.def("wintab_convert", [](long x, long y, unsigned pressure, int azimuth, int altitude, const py::dict &range) {
    WintabRange r;
    r.in_x = range["in_x"].cast<long>();
    r.in_y = range["in_y"].cast<long>();
    r.in_width = range["in_width"].cast<long>();
    r.in_height = range["in_height"].cast<long>();
    r.screen_x = range["screen_x"].cast<int>();
    r.screen_y = range["screen_y"].cast<int>();
    r.screen_width = range["screen_width"].cast<int>();
    r.screen_height = range["screen_height"].cast<int>();
    r.pressure_max = range["pressure_max"].cast<int>();
    r.azimuth_max = range["azimuth_max"].cast<int>();
    r.altitude_max = range["altitude_max"].cast<int>();
    double screen_x = 0.0, screen_y = 0.0;
    PenSample s;
    wintab_convert(r, {x, y, pressure, azimuth, altitude}, &screen_x, &screen_y, &s);
    return py::make_tuple(screen_x, screen_y, s.pressure, s.tilt_x, s.tilt_y);
  });
  m.def("pen_predict", [](const std::vector<std::tuple<float, float, double>> &samples, double ahead, float max_pixels) {
    std::vector<PenSample> list;
    for (const auto &[x, y, t] : samples) {
      PenSample p;
      p.x = x;
      p.y = y;
      p.time = t;
      list.push_back(p);
    }
    const PenSample p = pen_predict(list, ahead, max_pixels);
    return py::make_tuple(p.x, p.y);
  });
  m.def("media_view", []() { return current_context().wm->media_view; });
  m.def("local_view", []() {
    Context &C = current_context();
    py::list names;
    for (const uint64_t id : C.wm->local_view) {
      if (const Layer *l = layer_find(*C.document, id)) {
        names.append(l->name);
      }
    }
    return names;
  });
  m.def("properties_tab", [](int tab) { current_context().wm->properties_tab_request = std::clamp(tab, 0, 11); });
  /* How long frames take (ms, ImGui's running average), for checking the app's speed. */
  /* The left/right name rule bone Symmetrize and mirror editing pair by (bone_mirror_name). */
  m.def("flip_name", [](const std::string &name) { return bone_mirror_name(name); });
  m.def("frame_ms", []() { return 1000.0f / std::max(ImGui::GetIO().Framerate, 1e-3f); });
  m.def(
      "brush_settings_open",
      [](bool tool, bool presets) { brush_window_open(tool ? BrushTarget::Tool : BrushTarget::Shapes, presets); },
      py::arg("tool"), py::arg("presets") = false);
  /* The project's viewport overlays: their switches by name, and setting one (unsaved, as in the popover). */
  static const std::pair<const char *, bool ViewportOverlays::*> overlay_switches[] = {
      {"show", &ViewportOverlays::show},
      {"grid", &ViewportOverlays::grid},
      {"cursor", &ViewportOverlays::cursor},
      {"camera_frame", &ViewportOverlays::camera_frame},
      {"info", &ViewportOverlays::info},
      {"wireframe", &ViewportOverlays::wireframe},
      {"points", &ViewportOverlays::points},
      {"mask_outlines", &ViewportOverlays::mask_outlines},
      {"relationship_lines", &ViewportOverlays::relationship_lines},
      {"physics_ghosts", &ViewportOverlays::physics_ghosts},
      {"live_physics", &ViewportOverlays::live_physics},
      {"bone_shapes", &ViewportOverlays::bone_shapes},
      {"guides", &ViewportOverlays::guides},
      {"annotations", &ViewportOverlays::annotations},
  };
  /* Panels and folding rows open or folded, as the project keeps them ("<editor>/<panel>": open). */
  m.def("ui_open", []() { return current_context().document->ui_open; });
  m.def("set_ui_open", [](const std::string &key, bool open) {
    Context &C = current_context();
    C.document->ui_open[key] = open;
    C.undo->mark_changed();
  });
  m.def("overlays", []() {
    const ViewportOverlays &ov = current_context().document->overlays;
    py::dict d;
    for (const auto &[name, field] : overlay_switches) {
      d[name] = ov.*field;
    }
    static const char *const handles[] = {"NONE", "SELECTED", "SHAPE", "ALL"};
    static const char *const armature[] = {"OFF", "SELECTED", "ON"};
    d["handles"] = handles[int(ov.handles)];
    d["armature"] = armature[int(ov.armature)];
    d["point_size"] = ov.point_size;
    d["handle_size"] = ov.handle_size;
    return d;
  });
  m.def("set_overlay", [](const std::string &name, py::object value) {
    Context &C = current_context();
    ViewportOverlays &ov = C.document->overlays;
    bool found = false;
    for (const auto &[id, field] : overlay_switches) {
      if (name == id) {
        ov.*field = value.cast<bool>();
        found = true;
      }
    }
    if (name == "point_size" || name == "handle_size") {
      (name == "point_size" ? ov.point_size : ov.handle_size) =
          std::clamp(value.cast<float>(), OVERLAY_SIZE_MIN, OVERLAY_SIZE_MAX);
      found = true;
    }
    if (name == "handles" || name == "armature") {
      const std::string v = value.cast<std::string>();
      static const char *const handles[] = {"NONE", "SELECTED", "SHAPE", "ALL"};
      static const char *const armature[] = {"OFF", "SELECTED", "ON"};
      for (int i = 0; i < (name == "handles" ? 4 : 3); i++) {
        if (v == (name == "handles" ? handles[i] : armature[i])) {
          if (name == "handles") {
            ov.handles = HandleDisplay(i);
          }
          else {
            ov.armature = ArmatureDisplay(i);
          }
          found = true;
        }
      }
    }
    if (!found) {
      throw py::value_error("no overlay setting " + name);
    }
    C.undo->mark_changed();
  });
  /* The ViewportActionPanel's operator: (idname, args, editor) or None, and running it again. */
  m.def("last_operation", []() -> py::object {
    const LastOperation *last = operator_last_redo(current_context());
    if (!last) {
      return py::none();
    }
    return py::make_tuple(last->idname, last->args, last->area);
  });
  m.def("redo_last", [](const std::map<std::string, std::string> &args) {
    return std::string(op_status_name(operator_redo_last(current_context(), args)));
  });
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
        if (C.wm->modal_running) {
          /* A running G/R/S takes the keys itself, as a real key press goes to it. */
          C.wm->modal_keys.push_back({to_upper(type), ctrl, shift, alt});
          return true;
        }
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

  /* An image (RGBA bytes, rows from the top) onto the system clipboard, as Copy Image does a render. */
  m.def("clipboard_set_image", [](py::bytes rgba, int width, int height) {
    const std::string data = rgba;
    if (width <= 0 || height <= 0 || data.size() != size_t(width) * size_t(height) * 4) {
      throw py::value_error("rgba must hold width * height * 4 bytes");
    }
    std::string error;
    if (!clipboard_set_image(reinterpret_cast<const uint8_t *>(data.data()), width, height, &error)) {
      throw std::runtime_error("Could not copy the image: " + error);
    }
  });

  /* A render of the current frame to a PNG, done by the window on its next frame (none in background
   * runs). */
  m.def("render_png", [](const std::string &path, int width, int height, bool guides, bool transparent) {
    WindowManager &wm = *current_context().wm;
    if (wm.background) {
      throw std::runtime_error("No window to render with in a background run");
    }
    wm.render_file = {true, path, width, height, guides, transparent};
  });

  /* The 2D cursor (document space). */
  m.def("cursor_get", []() {
    const float2 c = current_context().wm->tool_settings.cursor;
    return py::make_tuple(c.x, c.y);
  });
  m.def("cursor_set", [](float x, float y) { current_context().wm->tool_settings.cursor = {x, y}; });
  /* Transform Layer's Show Grid Lines. */
  m.def("layer_grid_lines", []() { return current_context().wm->tool_settings.layer_grid; });
  m.def("layer_grid_lines_set", [](bool on) { current_context().wm->tool_settings.layer_grid = on; });
  m.def("gizmo_mode", []() { return current_context().wm->tool_settings.gizmo_mode; });
  m.def("gizmo_mode_set", [](int mode, int cells) {
    ToolSettings &ts = current_context().wm->tool_settings;
    ts.gizmo_mode = std::clamp(mode, 0, 2);
    if (cells > 0) {
      ts.warp_cells = std::clamp(cells, 1, 8);
    }
  });
  m.def("warp_cells", []() { return current_context().wm->tool_settings.warp_cells; });
  /* The viewport tool, by its Tool order. */
  m.def("tool", []() {
    static const char *const names[] = {"SELECT", "SELECT_LAYER", "CURSOR", "ANNOTATE", "DRAW", "SHAPE",
                                        "SPLIT_CURVE", "WEIGHT_SELECT", "WEIGHT_PAINT", "TRANSFORM_BONES",
                                        "TRANSFORM_POINTS", "SCULPT", "TRANSFORM_LAYER", "EDIT_TEXT"};
    static_assert(std::size(names) == size_t(Tool::EditText) + 1);
    return std::string(names[int(current_context().wm->tool_settings.tool)]);
  });

  /* Mirror editing (the viewport header's butterfly X / Y). */
  m.def("mirror_editing", []() {
    const ToolSettings &ts = current_context().wm->tool_settings;
    return py::make_tuple(ts.mirror_x, ts.mirror_y);
  });
  m.def("set_mirror_editing", [](bool x, bool y) {
    ToolSettings &ts = current_context().wm->tool_settings;
    if (ts.mirror_x != x || ts.mirror_y != y) {
      current_context().undo->mark_changed(); /* Saved with the file. */
    }
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
  /* Onion skins (anim/onion.hh): the frames ghosts show on and their wires, for the layers being edited. */
  m.def("onion_frames", []() {
    Context &C = current_context();
    const Document &doc = *C.document;
    py::list out;
    for (const OnionGhost &g : onion_ghosts(doc, onion_layers(doc, editable_layers(C)))) {
      py::dict d;
      d["frame"] = g.frame;
      d["after"] = g.after;
      d["distance"] = g.distance;
      out.append(d);
    }
    return out;
  });
  m.def("onion_wires", []() {
    Context &C = current_context();
    const Document &doc = *C.document;
    const std::vector<uint64_t> layers = onion_layers(doc, editable_layers(C));
    py::list out;
    for (const OnionWires &w : onion_wires(doc, layers, onion_ghosts(doc, layers))) {
      py::list lines;
      for (const std::vector<float2> &line : w.lines) {
        py::list pts;
        for (const float2 p : line) {
          pts.append(py::make_tuple(p.x, p.y));
        }
        lines.append(pts);
      }
      py::dict d;
      d["frame"] = w.ghost.frame;
      d["after"] = w.ghost.after;
      d["lines"] = lines;
      out.append(d);
    }
    return out;
  });
  m.def("onion_colors", []() {
    const Document &doc = *current_context().document;
    const auto rgba = [](const Color &c) { return py::make_tuple(c.r, c.g, c.b, c.a); };
    py::dict d;
    d["before"] = rgba(doc.onion_color_before);
    d["after"] = rgba(doc.onion_color_after);
    return d;
  });
  m.def("set_onion_colors", [](py::object before, py::object after) {
    Context &C = current_context();
    const auto set = [](Color &c, py::object v) {
      if (v.is_none()) {
        return;
      }
      const std::vector<float> f = v.cast<std::vector<float>>();
      if (f.size() < 3) {
        throw std::runtime_error("A colour is (r, g, b) or (r, g, b, a), 0-1");
      }
      c = {f[0], f[1], f[2], f.size() > 3 ? f[3] : 1.0f};
    };
    set(C.document->onion_color_before, before);
    set(C.document->onion_color_after, after);
    if (C.undo) {
      C.undo->mark_changed();
    }
  });
  /* The Sculpt tool's settings, and its presets (assets/sculpt_presets.hh). */
  m.def("sculpt_settings", []() {
    const ToolSettings &ts = current_context().wm->tool_settings;
    py::dict d;
    d["brush"] = sculpt_brush_id(ts.sculpt_brush);
    d["preset"] = ts.sculpt_preset;
    d["radius"] = ts.sculpt_radius;
    d["strength"] = ts.sculpt_strength;
    d["falloff"] = sculpt_falloff_id(ts.sculpt_falloff);
    d["pressure_radius"] = ts.sculpt_pressure_radius;
    d["pressure_strength"] = ts.sculpt_pressure_strength;
    d["selected_only"] = ts.sculpt_selected_only;
    d["surface_sensitivity"] = ts.sculpt_surface_sensitivity;
    return d;
  });
  m.def("set_sculpt", [](const py::dict &values) {
    ToolSettings &ts = current_context().wm->tool_settings;
    for (const auto &[key_obj, value] : values) {
      const std::string key = py::str(key_obj);
      if (key == "brush") {
        if (!sculpt_brush_from_id(py::str(value), &ts.sculpt_brush)) {
          throw py::value_error("brush is one of GRAB, PUSH, SMOOTH, THICKNESS, PINCH, TWIST, SURFACE_SMOOTH");
        }
      }
      else if (key == "falloff") {
        if (!sculpt_falloff_from_id(py::str(value), &ts.sculpt_falloff)) {
          throw py::value_error("falloff is one of SMOOTH, SPHERE, CONSTANT");
        }
      }
      else if (key == "radius") {
        ts.sculpt_radius = std::clamp(value.cast<float>(), 2.0f, 1000.0f);
      }
      else if (key == "strength") {
        ts.sculpt_strength = std::max(value.cast<float>(), 0.0f);
      }
      else if (key == "pressure_radius") {
        ts.sculpt_pressure_radius = value.cast<bool>();
      }
      else if (key == "pressure_strength") {
        ts.sculpt_pressure_strength = value.cast<bool>();
      }
      else if (key == "selected_only") {
        ts.sculpt_selected_only = value.cast<bool>();
      }
      else if (key == "surface_sensitivity") {
        ts.sculpt_surface_sensitivity = std::max(value.cast<float>(), 0.0f);
      }
      else {
        throw py::value_error("no sculpt setting '" + key + "'");
      }
    }
  });
  m.def("sculpt_presets", []() {
    py::list list;
    for (const SculptPreset &p : sculpt_presets()) {
      py::dict d;
      d["name"] = p.name;
      d["brush"] = sculpt_brush_id(p.brush);
      d["radius"] = p.radius;
      d["strength"] = p.strength;
      d["falloff"] = sculpt_falloff_id(p.falloff);
      d["category"] = p.category;
      d["builtin"] = p.builtin;
      list.append(d);
    }
    return list;
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

  /* Text blocks, wrapped by oanim.data.texts. Looked up by name on every call, so Python never
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

  /* Layers, wrapped by oanim.data.layers. Looked up by id on every call, like texts. */
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
  /* The active shape (what clicking a shape makes it): the one Link Materials passes on. */
  m.def("layer_set_active_shape", [layer_or_raise](uint64_t id, int index) {
    Layer &layer = layer_or_raise(id);
    if (index < 0 || index >= int(layer.drawing.shapes.size())) {
      throw py::index_error("no shape " + std::to_string(index));
    }
    Document &doc = *current_context().document;
    doc.active_shape_layer = layer.id;
    doc.active_shape = layer.drawing.shapes[size_t(index)].id;
  });
  m.def("layer_curve_points", [layer_or_raise](uint64_t id, int curve) {
    const VectorDrawing &drawing = layer_or_raise(id).drawing;
    std::vector<int> points;
    for (const CurvePoint &cp : drawing.curves.at(size_t(curve)).points) {
      points.push_back(cp.point);
    }
    return points;
  });
  m.def("layer_shape_contains", [layer_or_raise](uint64_t id, int shape, float x, float y) {
    const VectorDrawing &drawing = layer_or_raise(id).drawing;
    return shape >= 0 && shape < int(drawing.shapes.size()) && shape_fill_contains(drawing, drawing.shapes[shape], {x, y});
  });
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
    d["fill_rule"] = s.fill_rule == FillRule::EvenOdd ? "EVEN_ODD" : s.fill_rule == FillRule::NonZero ? "NONZERO" : "LOOPS";
    d["brush"] = s.style.brush.name;
    const Material *material = material_find(*current_context().document, s.material);
    d["material"] = material ? material->name : std::string();
    return d;
  });
  /* The document's materials: [{"name", "users", "fill", "line", "line_width", "brush"}]. */
  /* The annotation layers (data/annotations.hh): [{"name", "color", "thickness", "hidden", "locked",
   * "strokes": [{"points": [(x, y)], "start", "end"}]}], and the active one's index. */
  m.def("annotations", []() {
    const Document &doc = *current_context().document;
    py::list list;
    for (const AnnotationLayer &l : doc.annotations) {
      py::dict d;
      d["name"] = l.name;
      d["color"] = py::make_tuple(l.color.r, l.color.g, l.color.b, l.color.a);
      d["thickness"] = l.thickness;
      d["hidden"] = l.hidden;
      d["locked"] = l.locked;
      py::list strokes;
      for (const AnnotationStroke &s : l.strokes) {
        py::dict sd;
        py::list points;
        for (const float2 &p : s.points) {
          points.append(py::make_tuple(p.x, p.y));
        }
        sd["points"] = points;
        sd["start"] = annotation_end_identifier(s.start);
        sd["end"] = annotation_end_identifier(s.end);
        strokes.append(sd);
      }
      d["strokes"] = strokes;
      list.append(d);
    }
    return py::make_tuple(list, doc.annotations.empty() ? -1 : doc.active_annotation);
  });
  /* The drawing guides: [{"type", "points": [(x, y)], "lines", "hidden", "locked"}], and the picked one's
   * index (-1: none). */
  m.def("guides", []() {
    const Document &doc = *current_context().document;
    py::list list;
    for (const DrawingGuide &g : doc.guides) {
      py::dict d;
      d["type"] = guide_type_identifier(g.type);
      py::list points;
      for (const float2 &p : g.points) {
        points.append(py::make_tuple(p.x, p.y));
      }
      d["points"] = points;
      d["lines"] = g.lines;
      d["hidden"] = g.hidden;
      d["locked"] = g.locked;
      list.append(d);
    }
    return py::make_tuple(list, doc.active_guide);
  });
  m.def("guide_settings", []() {
    const GuideSettings &s = current_context().document->guide_settings;
    py::dict d;
    d["show"] = s.show;
    d["hue"] = s.hue;
    d["opacity"] = s.opacity;
    return d;
  });
  m.def("move_options", []() {
    const MoveOptions o = viewport_move_options(*current_context().document);
    py::dict d;
    d["keep_poses"] = o.keep_poses;
    d["only_bones"] = o.only_bones;
    d["without_children"] = o.without_children;
    return d;
  });
  m.def("materials", []() {
    const Document &doc = *current_context().document;
    py::list list;
    for (const Material &mat : doc.materials) {
      py::dict d;
      d["name"] = mat.name;
      d["users"] = material_users(doc, mat.id);
      d["fill"] = py::make_tuple(mat.style.fill.r, mat.style.fill.g, mat.style.fill.b, mat.style.fill.a);
      d["line"] = py::make_tuple(mat.style.line.r, mat.style.line.g, mat.style.line.b, mat.style.line.a);
      d["line_width"] = mat.style.line_width;
      d["brush"] = mat.style.brush.name;
      list.append(d);
    }
    return list;
  });
  /* A text layer's runs: [{text, font, weight, italic, size, has_fill, has_outline, fill, line, line_width,
   * brush, material}]. */
  m.def("layer_text_runs", [layer_or_raise](uint64_t id) {
    const Layer &layer = layer_or_raise(id);
    const Document &doc = *current_context().document;
    py::list list;
    for (const TextRun &run : layer.text.runs) {
      py::dict d;
      d["text"] = run.text;
      d["font"] = run.font;
      d["weight"] = run.weight;
      d["italic"] = run.italic;
      d["size"] = run.size;
      d["has_fill"] = run.has_fill;
      d["has_outline"] = run.has_outline;
      d["fill"] = py::make_tuple(run.style.fill.r, run.style.fill.g, run.style.fill.b, run.style.fill.a);
      d["line"] = py::make_tuple(run.style.line.r, run.style.line.g, run.style.line.b, run.style.line.a);
      d["line_width"] = run.style.line_width;
      d["brush"] = run.style.brush.name;
      const Material *material = material_find(doc, run.material);
      d["material"] = material ? material->name : std::string();
      list.append(d);
    }
    return list;
  });
  /* Replaces a text layer's runs with dicts as layer_text_runs gives (keys left out keep their defaults;
   * material by name, "" for its own look). */
  m.def("layer_set_text_runs", [layer_or_raise](uint64_t id, py::list runs) {
    Layer &layer = layer_or_raise(id);
    if (!layer.is_text()) {
      throw py::value_error("'" + layer.name + "' is not a text layer");
    }
    Document &doc = *current_context().document;
    const auto color = [](py::handle v) {
      const auto t = v.cast<std::vector<float>>();
      return Color{t.at(0), t.at(1), t.at(2), t.size() > 3 ? t[3] : 1.0f};
    };
    std::vector<TextRun> out;
    for (py::handle item : runs) {
      const py::dict d = py::reinterpret_borrow<py::dict>(item);
      TextRun run;
      for (const auto &[k, v] : d) {
        const std::string key = py::str(k);
        if (key == "text") {
          run.text = v.cast<std::string>();
        }
        else if (key == "font") {
          run.font = v.cast<std::string>();
        }
        else if (key == "weight") {
          run.weight = std::clamp(v.cast<int>(), 1, 999);
        }
        else if (key == "italic") {
          run.italic = v.cast<bool>();
        }
        else if (key == "size") {
          run.size = std::max(v.cast<float>(), 1e-4f);
        }
        else if (key == "has_fill") {
          run.has_fill = v.cast<bool>();
        }
        else if (key == "has_outline") {
          run.has_outline = v.cast<bool>();
        }
        else if (key == "fill") {
          run.style.fill = color(v);
        }
        else if (key == "line") {
          run.style.line = color(v);
        }
        else if (key == "line_width") {
          run.style.line_width = std::max(v.cast<float>(), 0.0f);
        }
        else if (key == "brush") {
          run.style.brush.name = v.cast<std::string>();
        }
        else if (key == "material") {
          const std::string name = v.cast<std::string>();
          run.material = 0;
          for (const Material &mat : doc.materials) {
            if (mat.name == name) {
              run.material = mat.id;
            }
          }
          if (!name.empty() && run.material == 0) {
            throw py::value_error("no material '" + name + "'");
          }
        }
        else {
          throw py::key_error("unknown text run setting '" + key + "'");
        }
      }
      out.push_back(std::move(run));
    }
    layer.text.runs = std::move(out);
    text_tidy(layer.text);
    document_sync_text(doc);
    current_context().undo->mark_changed();
  });
  /* The layers drawn at (x, y) (document space), top first, as Select Layer finds them: a fill's hole
   * shows what is under it. */
  m.def("layers_at", [](float x, float y) {
    return layers_at(*current_context().document, {x, y}, LayerPickOptions());
  });
  /* The installed font families; whether one is; a PostScript name's (family, weight, italic) or None. */
  m.def("text_fonts", []() { return text_font_families(); });
  m.def("text_font_installed", [](const std::string &family) { return text_font_installed(family); });
  m.def("text_font_from_postscript", [](const std::string &name) -> py::object {
    std::string family;
    int weight = 400;
    bool italic = false;
    if (!text_font_from_postscript(name, &family, &weight, &italic)) {
      return py::none();
    }
    return py::make_tuple(family, weight, italic);
  });
  /* A text layer's bounds (layer space): ((min x, min y), (max x, max y)). */
  m.def("layer_text_bounds", [layer_or_raise](uint64_t id) {
    const TextBox box = text_bounds(layer_or_raise(id).text);
    return py::make_tuple(py::make_tuple(box.min.x, box.min.y), py::make_tuple(box.max.x, box.max.y));
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
      else if (key == "fill_rule") {
        const std::string rule = v.cast<std::string>();
        if (rule == "LOOPS") {
          s.fill_rule = FillRule::Loops;
        }
        else if (rule == "EVEN_ODD") {
          s.fill_rule = FillRule::EvenOdd;
        }
        else if (rule == "NONZERO") {
          s.fill_rule = FillRule::NonZero;
        }
        else {
          throw py::value_error("fill_rule is LOOPS, EVEN_ODD or NONZERO, not '" + rule + "'");
        }
      }
      else {
        throw py::key_error("unknown shape setting '" + key + "'");
      }
    }
    /* A linked shape's look is its material's: every shape sharing it changes too. */
    document_sync_materials(*current_context().document);
    current_context().undo->mark_changed();
  });
  m.def(
      "layer_add_curve",
      [layer_or_raise](uint64_t id, const std::vector<std::pair<float, float>> &points, bool closed, bool fill) {
        Context &C = current_context();
        Layer &layer = layer_or_raise(id);
        if (!layer.has_drawing() || layer.is_reference()) {
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
  /* The Split tool's click: a new point on segment `segment` of curve `curve`, `t` along it. */
  m.def("layer_split_segment", [layer_or_raise](uint64_t id, int curve, int segment, float t) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference()) {
      throw py::value_error("A reference's drawing is its original's");
    }
    if (curve < 0 || curve >= int(layer.drawing.curves.size()) || segment < 0 ||
        segment >= int(layer.drawing.curves[curve].points.size()))
    {
      throw py::index_error("no segment " + std::to_string(segment) + " on curve " + std::to_string(curve));
    }
    /* At the curve's detail level, as the tool cuts (the segment as it shows). */
    const int point = drawing_detail_split(layer.drawing, {curve, segment}, std::clamp(t, 0.0f, 1.0f),
                                           drawing_knife_level(layer.drawing, curve));
    current_context().undo->mark_changed();
    return point;
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
    Document &doc = *current_context().document;
    const bool self_x = layer_mirror_x_self(doc, layer.id, ts.mirror_x);
    const PartnerMirror partners = layer_mirror_partners_begin(doc, {layer.id}, ts.mirror_x);
    const std::vector<MirrorPair> pairs = drawing_mirror_pairs(drawing, moving, self_x, ts.mirror_y);
    for (const int i : moving) {
      drawing.points[i].pos += float2(dx, dy);
    }
    drawing_apply_mirror(drawing, pairs);
    layer_mirror_partners_end(doc, partners);
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
#define OANIM_BRUSH_DICT(type, field, key) d[key] = b.field;
    OANIM_BRUSH_SETTINGS(OANIM_BRUSH_DICT)
#undef OANIM_BRUSH_DICT
    return d;
  };
  m.def("brushes", []() {
    py::list result;
    for (const Brush &brush : brushes()) {
      py::dict d;
      d["name"] = brush.name;
      d["images"] = int(brush.files.size());
      result.append(d);
    }
    return result;
  });
  m.def("brush_presets", [brush_to_dict](bool library) {
    py::list result;
    const std::vector<BrushPreset> &presets = library ? library_brush_presets()
                                                      : current_context().document->brush_presets;
    for (const BrushPreset &p : presets) {
      py::dict d;
      d["name"] = p.name;
      d["brush"] = brush_to_dict(p.settings);
      d["shape"] = p.settings.name;
      d["shape_missing"] = !p.settings.name.empty() && !brush_find(p.settings.name);
      d["shape_images"] = int(p.shape_data.size());
      d["group"] = p.group;
      d["builtin"] = p.builtin;
      result.append(d);
    }
    return result;
  });
  m.def("brush_categories", []() { return library_brush_categories(); });
  m.def("asset_library", []() { return preferences().asset_library; });
  m.def("drivers", []() {
    Document &doc = *current_context().document;
    const auto ref_dict = [](const DriverRef &r) {
      py::dict d;
      d["layer"] = r.layer;
      d["path"] = r.path;
      d["component"] = r.component;
      return d;
    };
    static const char *const TYPES[] = {"EXPRESSION", "AVERAGE", "SUM", "MIN", "MAX"};
    py::list result;
    for (const Driver &drv : doc.drivers) {
      py::dict d;
      d["target"] = ref_dict(drv.target);
      d["label"] = driver_ref_label(doc, drv.target);
      d["type"] = TYPES[int(drv.type)];
      d["expression"] = drv.expression;
      py::list vars;
      for (const DriverVar &v : drv.vars) {
        py::dict vd;
        vd["name"] = v.name;
        vd["source"] = ref_dict(v.source);
        vars.append(vd);
      }
      d["vars"] = vars;
      d["remap"] = drv.remap;
      d["from"] = py::make_tuple(drv.from_min, drv.from_max);
      d["to"] = py::make_tuple(drv.to_min, drv.to_max);
      d["clamp"] = drv.clamp;
      d["two_way"] = drv.two_way;
      d["factor"] = drv.flip ? -drv.factor : drv.factor;
      d["error"] = drv.error;
      float value = 0.0f;
      d["value"] = driver_ref_read(doc, drv.target, &value) ? py::object(py::float_(value)) : py::object(py::none());
      result.append(d);
    }
    return result;
  });
  m.def("driver_clipboard_size", []() { return int(driver_clipboard().size()); });
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
  /* A curve's handles after the modifiers (layer space), as layer_curve_handles: `shown`, as the viewport shows
   * them while editing (a deform's segments each as one Bezier, without the points Precision adds). */
  m.def(
      "layer_evaluated_handles",
      [layer_or_raise](uint64_t id, int curve_index, bool shown) {
        Document &doc = *current_context().document;
        animation_sync(doc);
        std::optional<VectorDeformWireScope> wire;
        if (shown) {
          wire.emplace();
        }
        const VectorDrawing drawing = layer_evaluated_drawing(doc, layer_or_raise(id), false);
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
      },
      py::arg("id"), py::arg("curve"), py::arg("shown") = false);
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
  m.def("camera_to_frame", [](float x, float y) {
    const float2 p = camera_to_frame(*current_context().document).apply({x, y});
    return py::make_tuple(p.x, p.y);
  });
  m.def("layer_is_selected", [layer_or_raise](uint64_t id) { return layer_or_raise(id).selected; });
  m.def("layer_set_selected", [layer_or_raise](uint64_t id, bool selected) {
    layer_or_raise(id).selected = selected;
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
    /* Frame 0's key (the rest) goes only with the last of the keys, as Alt I and the timeline do. */
    const bool removed = ref.scalar ? channel_delete_key(*ref.scalar, *ref.scalar_value, frame)
                                    : channel_delete_key(*ref.vector, *ref.vector_value, frame);
    Layer *layer = layer_find(doc, id);
    for (Bone &bone : layer->bones) {
      bone_values_from_changes(bone);
    }
    if (!layer->is_reference()) {
      drawing_values_from_changes(layer->drawing);
    }
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
  m.def("layer_bone_pose", [layer_or_raise](uint64_t id, const std::string &name) {
    /* A bone's head and tail as posed now (bone layer space). */
    const Layer &armature = layer_or_raise(id);
    const Bone *b = bone_find_by_name(armature, name);
    if (!b) {
      throw py::value_error("no bone \"" + name + "\"");
    }
    const bool setup = current_context().document->frame_current == 0;
    for (const BonePose &p : armature_pose(armature, setup)) {
      if (p.id == b->id) {
        return py::make_tuple(p.head.x, p.head.y, p.tail.x, p.tail.y);
      }
    }
    throw py::value_error("no pose");
  });
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
      d["depth"] = b.depth;
      d["strength"] = b.strength;
      d["selected"] = b.selected;
      d["select_head"] = bone_head_selected(layer, b);
      d["select_tail"] = bone_tail_selected(layer, b);
      d["connected"] = b.connected;
      d["inherit_rotation"] = b.inherit_rotation;
      /* Its swap: (bone layer, bone) it stands in for, or None. */
      {
        const Document &swap_doc = *current_context().document;
        const Layer *swap_layer = b.swap_layer ? layer_find(swap_doc, b.swap_layer) : nullptr;
        const Bone *swap_bone = swap_layer ? bone_find(*swap_layer, b.swap_bone) : nullptr;
        d["stands_in_for"] = swap_bone ? py::object(py::make_tuple(swap_layer->name, swap_bone->name)) : py::object(py::none());
      }
      d["inherit_scale"] = b.inherit_scale == InheritScale::Full      ? "FULL"
                           : b.inherit_scale == InheritScale::Uniform ? "UNIFORM"
                                                                      : "NONE";
      d["local_location"] = b.local_location;
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
      else if (key == "depth") {
        bone.depth = std::max(1e-4f, value.cast<float>());
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
        const std::string before = bone.name;
        bone.name = value.cast<std::string>();
        armature_bone_renamed(*current_context().document, id, before, bone.name);
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
        d["use_squash_limit"] = s.use_squash_limit;
        d["squash_limit"] = s.squash_limit;
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
      d["follow_move"] = c.follow_move;
      d["chain"] = c.chain;
      d["max_stretch"] = c.max_stretch;
      d["shape_layer"] = c.shape_layer;
      d["shape"] = c.shape_id;
      {
        Layer *owner = nullptr;
        const Modifier *deform = follow_point_modifier(*current_context().document, c, &owner);
        d["follow_layer"] = deform ? owner->name : std::string();
        d["follow_modifier"] = deform ? deform->name : std::string();
        const Layer *follow_shape = deform ? layer_find(*current_context().document, deform->deform_layer) : nullptr;
        d["follow_shape_point"] = follow_shape_point_number(follow_shape, c.follow_point_id);
        d["follow_shape_point2"] = follow_shape_point_number(follow_shape, c.follow_point2_id);
      }
      d["follow_point"] = py::make_tuple(c.follow_point.x, c.follow_point.y);
      d["show_follow_point"] = c.follow_show;
      d["follow_offset"] = py::make_tuple(c.follow_offset.x, c.follow_offset.y);
      d["follow_now"] = py::make_tuple(c.follow_now.x, c.follow_now.y);
      d["follow_error"] = c.follow_error;
      d["follow_second"] = c.follow_two;
      d["follow_shift"] = py::make_tuple(c.follow_shift.x, c.follow_shift.y);
      d["follow_shift2"] = py::make_tuple(c.follow_shift2.x, c.follow_shift2.y);
      d["follow_point2"] = py::make_tuple(c.follow_point2.x, c.follow_point2.y);
      d["follow_rotation"] = c.follow_rotation;
      d["follow_scale"] = c.follow_scale;
      d["follow_turn"] = c.follow_turn;
      d["follow_stretch"] = c.follow_stretch;
      d["follow_now2"] = py::make_tuple(c.follow_now2.x, c.follow_now2.y);
      d["fit"] = c.shape_fit == ShapeFit::Whole ? "WHOLE" : "CENTER";
      d["fit_layer"] = c.fit_layer;
      d["overlap"] = c.overlap;
      d["softness"] = c.softness;
      d["affect_transform"] = c.affect_transform;
      d["child_position"] = c.child_position;
      d["child_rotation"] = c.child_rotation;
      d["child_scale"] = c.child_scale;
      d["range_auto"] = c.range_auto;
      d["look_range"] = py::make_tuple(c.look_range.x, c.look_range.y);
      d["look_auto_range"] = py::make_tuple(c.look_auto_range.x, c.look_auto_range.y);
      d["squash"] = c.squash;
      d["eyeball_auto"] = c.eyeball_auto;
      {
        const Bone *middle = bone_find(layer, c.eyeball_center);
        d["eyeball_center"] = middle ? middle->name : std::string();
      }
      d["eyeball_size"] = py::make_tuple(c.eyeball_size.x, c.eyeball_size.y);
      d["eyeball_auto_size"] = py::make_tuple(c.eyeball_auto_size.x, c.eyeball_auto_size.y);
      d["facing"] = py::make_tuple(c.facing.x, c.facing.y);
      d["max_side"] = c.max_side;
      d["max_up"] = c.max_up;
      d["max_down"] = c.max_down;
      {
        py::list round;
        for (const uint64_t kept : c.keep_round) {
          if (const Layer *l = layer_find(*current_context().document, kept)) {
            round.append(l->name);
          }
        }
        d["keep_round"] = round;
      }
      d["divergence"] = c.divergence;
      d["shape_region"] = c.shape_region.size();
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
        else if (key == "use_squash_limit") {
          s.use_squash_limit = v.cast<bool>();
        }
        else if (key == "squash_limit") {
          s.squash_limit = std::clamp(v.cast<float>(), 1e-4f, 0.9f);
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
      else if (key == "follow_move") {
        c.follow_move = value.cast<bool>();
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
      else if (key == "shape_layer" || key == "fit_layer") {
        /* A layer by id or name ("" or 0: none). */
        uint64_t found = 0;
        if (py::isinstance<py::str>(value)) {
          const std::string wanted = value.cast<std::string>();
          for (const Layer *l : document_layers_flat(*current_context().document)) {
            if (!wanted.empty() && l->name == wanted) {
              found = l->id;
              break;
            }
          }
          if (!wanted.empty() && !found) {
            throw py::key_error("no layer named '" + wanted + "'");
          }
        }
        else {
          found = value.cast<uint64_t>();
        }
        (key == "shape_layer" ? c.shape_layer : c.fit_layer) = found;
      }
      else if (key == "shape") {
        c.shape_id = value.cast<uint64_t>();
      }
      else if (key == "follow_deform") {
        /* (layer name, modifier name), or None: the point starts at the bone's head. */
        Document &doc = *current_context().document;
        Layer *owner = nullptr;
        Modifier *deform = nullptr;
        if (!value.is_none()) {
          const auto names = value.cast<std::pair<std::string, std::string>>();
          deform = follow_point_find(doc, names.first, names.second, &owner);
          if (!deform) {
            throw py::key_error("no deform '" + names.second + "' on a layer named '" + names.first + "'");
          }
        }
        follow_point_aim(doc, layer, bone, c, owner, deform);
      }
      else if (key == "follow_point") {
        const auto point = value.cast<std::pair<float, float>>();
        Layer *owner = nullptr;
        if (follow_point_modifier(*current_context().document, c, &owner)) {
          follow_point_place(*current_context().document, layer, c, *owner, {point.first, point.second});
        }
      }
      else if (key == "show_follow_point") {
        c.follow_show = value.cast<bool>();
      }
      else if (key == "follow_second") {
        follow_point_second(*current_context().document, layer, bone, c, value.cast<bool>());
      }
      else if (key == "follow_point2") {
        const auto point = value.cast<std::pair<float, float>>();
        Layer *owner = nullptr;
        if (follow_point_modifier(*current_context().document, c, &owner)) {
          follow_point_place(*current_context().document, layer, c, *owner, {point.first, point.second}, true);
        }
      }
      else if (key == "follow_shape_point" || key == "follow_shape_point2") {
        Document &doc = *current_context().document;
        Layer *owner = nullptr;
        const Modifier *deform = follow_point_modifier(doc, c, &owner);
        const uint64_t point_id = deform ? follow_shape_point(layer_find(doc, deform->deform_layer), deform->deform_shape, value)
                                         : follow_shape_point(nullptr, 0, value);
        follow_point_pick(doc, layer, c, key == "follow_shape_point2", point_id);
      }
      else if (key == "follow_rotation") {
        c.follow_rotation = value.cast<bool>();
      }
      else if (key == "follow_shift" || key == "follow_shift2") {
        const auto shift = value.cast<std::pair<float, float>>();
        (key == "follow_shift" ? c.follow_shift : c.follow_shift2) = {shift.first, shift.second};
      }
      else if (key == "follow_to" || key == "follow_to2") {
        /* Where it's to show now, in document space: as a drag does. */
        const auto to = value.cast<std::pair<float, float>>();
        const bool second = key == "follow_to2";
        (second ? c.follow_shift2 : c.follow_shift) =
            follow_point_shift_to(*current_context().document, layer, bone, c, second, {to.first, to.second});
      }
      else if (key == "follow_scale") {
        c.follow_scale = value.cast<bool>();
      }
      else if (key == "fit") {
        const std::string fit = value.cast<std::string>();
        if (fit != "CENTER" && fit != "WHOLE") {
          throw py::value_error("fit is CENTER or WHOLE");
        }
        c.shape_fit = fit == "CENTER" ? ShapeFit::Center : ShapeFit::Whole;
      }
      else if (key == "overlap") {
        c.overlap = std::clamp(value.cast<float>(), 0.0f, 1.0f);
      }
      else if (key == "softness") {
        c.softness = std::max(0.0f, value.cast<float>());
      }
      else if (key == "affect_transform") {
        c.affect_transform = value.cast<bool>();
      }
      else if (key == "child_position" || key == "child_rotation" || key == "child_scale") {
        (key == "child_position" ? c.child_position : key == "child_rotation" ? c.child_rotation : c.child_scale) =
            value.cast<bool>();
      }
      else if (key == "range_auto") {
        c.range_auto = value.cast<bool>();
      }
      else if (key == "look_range") {
        if (py::isinstance<py::float_>(value) || py::isinstance<py::int_>(value)) {
          const float v = std::max(0.0f, value.cast<float>());
          c.look_range = {v, v};
        }
        else {
          const auto r = value.cast<std::pair<float, float>>();
          c.look_range = {std::max(0.0f, r.first), std::max(0.0f, r.second)};
        }
      }
      else if (key == "squash") {
        c.squash = std::clamp(value.cast<float>(), 0.0f, 1.0f);
      }
      else if (key == "eyeball_auto") {
        c.eyeball_auto = value.cast<bool>();
      }
      else if (key == "eyeball_center") {
        const std::string t = value.cast<std::string>();
        const Bone *middle = t.empty() ? nullptr : bone_find_by_name(layer, t);
        if (!t.empty() && !middle) {
          throw py::key_error("no bone named '" + t + "'");
        }
        c.eyeball_center = middle ? middle->id : 0;
      }
      else if (key == "eyeball_size") {
        if (py::isinstance<py::float_>(value) || py::isinstance<py::int_>(value)) {
          const float v = std::max(1e-4f, value.cast<float>());
          c.eyeball_size = {v, v};
        }
        else {
          const auto r = value.cast<std::pair<float, float>>();
          c.eyeball_size = {std::max(1e-4f, r.first), std::max(1e-4f, r.second)};
        }
      }
      else if (key == "facing") {
        const auto r = value.cast<std::pair<float, float>>();
        c.facing = {r.first, r.second};
      }
      else if (key == "max_side" || key == "max_up" || key == "max_down") {
        (key == "max_side" ? c.max_side : key == "max_up" ? c.max_up : c.max_down) =
            std::clamp(value.cast<float>(), 0.0f, 3.1415927f);
      }
      else if (key == "keep_round") {
        /* Layers by name or id. */
        c.keep_round.clear();
        for (const py::handle item : value) {
          uint64_t found = 0;
          if (py::isinstance<py::str>(item)) {
            const std::string wanted = item.cast<std::string>();
            for (const Layer *l : document_layers_flat(*current_context().document)) {
              if (l->name == wanted) {
                found = l->id;
                break;
              }
            }
            if (!found) {
              throw py::key_error("no layer named '" + wanted + "'");
            }
          }
          else {
            found = item.cast<uint64_t>();
          }
          c.keep_round.push_back(found);
        }
      }
      else if (key == "divergence") {
        c.divergence = value.cast<float>();
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
  m.def("layer_parent_bone_deform", [layer_or_raise](uint64_t id) {
    return std::string(layer_or_raise(id).parent_bone_points ? "POINTS" : "LAYER");
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
  /* Where the layer's points and handles show, bones and parents included (document space): the
   * points, and each curve's [(point, handle in, handle out, aligned, hidden, level, index)] and whether
   * it closes. `level` < 0: the curves as drawn (every point, detail's handles final); else as they are
   * at that detail level (data/detail.hh): only the points up to it, the handles as that level's curve
   * has them. `index` is the point's place in the whole curve (what place_handle takes). */
  m.def("layer_shown", [layer_or_raise](uint64_t id, int level) {
    const Document &doc = *current_context().document;
    const Layer &layer = layer_or_raise(id);
    const VectorDrawing &stored = layer_drawing(doc, layer);
    VectorDrawing drawing = stored;
    if (level < 0) {
      drawing_detail_final_handles(drawing);
    }
    else if (drawing_has_detail(drawing)) {
      for (Shape &shape : drawing.shapes) {
        shape.detail_level = level;
      }
      /* A curve no shape uses shows every level: take its points above `level` out too. */
      for (Curve &curve : drawing.curves) {
        std::erase_if(curve.points, [&](const CurvePoint &cp) { return drawing.points[cp.point].detail.level > level; });
      }
      drawing_detail_final_handles(drawing);
    }
    std::vector<Transform2D> skins = layer_point_skins(doc, layer);
    skins.resize(drawing.points.size());
    const Transform2D world = layer_world_transform(doc, layer.id);
    const auto shown = [&](int point, float2 p) { return world.apply(skins[size_t(point)].apply(p)); };
    const bool fitted = layer_handles_fitted(doc, layer);
    py::list points;
    for (int i = 0; i < int(drawing.points.size()); i++) {
      const float2 q = shown(i, drawing.points[size_t(i)].pos);
      points.append(py::make_tuple(q.x, q.y));
    }
    py::list curves;
    for (size_t c = 0; c < drawing.curves.size(); c++) {
      const Curve &curve = drawing.curves[c];
      const Curve &whole = stored.curves[c];
      py::list cps;
      for (int i = 0; i < int(curve.points.size()); i++) {
        const CurvePoint &cp = curve.points[size_t(i)];
        /* Fitted by a Resample Self, a handle's end shows where the stack puts it, as in the viewport. */
        const auto end = [&](bool pre) {
          const float2 stored_end = curve_handle(drawing, curve, i, pre);
          return fitted ? world.apply(layer_places_shown(doc, layer, {{cp.point, stored_end}})[0]) : shown(cp.point, stored_end);
        };
        const float2 in = end(true), out = end(false);
        const PointDetail &dt = drawing.points[size_t(cp.point)].detail;
        int index = i;
        for (int k = 0; k < int(whole.points.size()); k++) {
          if (whole.points[size_t(k)].point == cp.point) {
            index = k;
            break;
          }
        }
        cps.append(py::make_tuple(cp.point, py::make_tuple(in.x, in.y), py::make_tuple(out.x, out.y), cp.aligned,
                                  dt.hidden, dt.level, index));
      }
      curves.append(py::make_tuple(cps, curve.closed));
    }
    return py::make_tuple(points, curves);
  }, py::arg("id"), py::arg("level") = -1);
  /* Sets shapes' detail edit level (all of them, or the ones listed), as Detail Level does for the
   * selected shapes, clamped to how high each can go; then places the detail as that level edits it. */
  m.def("layer_detail_level_set", [layer_or_raise](uint64_t id, int level, std::vector<int> shapes) {
    Layer &layer = layer_or_raise(id);
    VectorDrawing &drawing = layer.drawing;
    for (size_t s = 0; s < drawing.shapes.size(); s++) {
      if (!shapes.empty() && std::find(shapes.begin(), shapes.end(), int(s)) == shapes.end()) {
        continue;
      }
      Shape &shape = drawing.shapes[s];
      shape.detail_level = std::clamp(level, 0, shape_detail_level_limit(drawing, shape));
    }
    drawing_detail_update_shown(drawing);
    current_context().undo->mark_changed();
  }, py::arg("id"), py::arg("level"), py::arg("shapes") = std::vector<int>());
  /* Places the layer's detail points on their curves now (as the viewport does each frame): an edited
   * one keeps where it is, the rest ride their coarser curve, by each curve's edit level. */
  m.def("layer_detail_sync", [layer_or_raise](uint64_t id) {
    drawing_detail_sync(layer_or_raise(id).drawing, true);
  });
  /* Moves a point (or one handle) so it shows at a document-space place: the bones' bend undone, as
   * a G drag places it. */
  const auto stored_at = [](const Document &doc, const Layer &layer, int point, std::pair<float, float> at) {
    std::vector<Transform2D> skins = layer_point_skins(doc, layer);
    skins.resize(layer.drawing.points.size());
    const float2 in_layer = layer_world_transform(doc, layer.id).inverted().apply({at.first, at.second});
    return skins[size_t(point)].inverted().apply(in_layer);
  };
  m.def("layer_place_point", [layer_or_raise, stored_at](uint64_t id, int index, std::pair<float, float> at) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference() && !layer.reference.edit_points) {
      throw py::value_error("This copy's points are locked (reference_set edit_points=1 unlocks them)");
    }
    if (index < 0 || index >= int(layer.drawing.points.size())) {
      throw py::index_error("no point " + std::to_string(index));
    }
    const Document &doc = *current_context().document;
    const float2 was = layer.drawing.points[size_t(index)].pos;
    layer.drawing.points[size_t(index)].pos = stored_at(doc, layer, index, at);
    std::vector<Transform2D> skins = layer_point_skins(doc, layer);
    skins.resize(layer.drawing.points.size());
    const float2 in_layer = layer_world_transform(doc, layer.id).inverted().apply({at.first, at.second});
    layer_points_settle(doc, layer, {{index, in_layer, skins[size_t(index)].inverted(),
                                      length(layer.drawing.points[size_t(index)].pos - was)}});
    current_context().undo->mark_changed();
  });
  m.def("layer_place_handle", [layer_or_raise, stored_at](uint64_t id, int curve_index, int index, const std::string &side,
                                                         std::pair<float, float> at, bool keep_other) {
    Layer &layer = layer_or_raise(id);
    if (layer.is_reference() && !layer.reference.edit_points) {
      throw py::value_error("This copy's points are locked (reference_set edit_points=1 unlocks them)");
    }
    VectorDrawing &drawing = layer.drawing;
    if (curve_index < 0 || curve_index >= int(drawing.curves.size()) || index < 0 ||
        index >= int(drawing.curves[size_t(curve_index)].points.size()))
    {
      throw py::index_error("no such curve point");
    }
    if (side != "IN" && side != "OUT") {
      throw py::value_error("side must be 'IN' or 'OUT'");
    }
    Curve &curve = drawing.curves[size_t(curve_index)];
    const Document &doc = *current_context().document;
    const float2 was = curve_handle(drawing, curve, index, side == "IN");
    float2 stored = stored_at(doc, layer, curve.points[size_t(index)].point, at);
    /* Bent by a deform, it shows as no skin takes back: changed by as much as it is moved, as a drag does. */
    const bool fitted = layer_handles_fitted(doc, layer);
    if (fitted) {
      const float2 in_layer = layer_world_transform(doc, layer.id).inverted().apply({at.first, at.second});
      const StackPlace place{curve.points[size_t(index)].point, was};
      stored = was + layer_place_unskin(doc, layer, place).apply_vector(in_layer - layer_places_shown(doc, layer, {place})[0]);
    }
    else if (doc.frame_current != 0 && layer_has_modifier_effect(doc, layer, false)) {
      const VectorDeformWireScope wire;
      const VectorDrawing shown = layer_evaluated_drawing(doc, layer, false);
      if (curve_index < int(shown.curves.size()) && shown.curves[size_t(curve_index)].points.size() == curve.points.size()) {
        std::vector<Transform2D> skins = layer_point_skins(doc, layer);
        skins.resize(drawing.points.size());
        const float2 in_layer = layer_world_transform(doc, layer.id).inverted().apply({at.first, at.second});
        const float2 from = curve_handle(shown, shown.curves[size_t(curve_index)], index, side == "IN");
        stored = curve_handle(drawing, curve, index, side == "IN") +
                 skins[size_t(curve.points[size_t(index)].point)].inverted().apply_vector(in_layer - from);
      }
    }
    curve_point_move_handle(drawing, curve, index, side == "IN", stored, keep_other);
    {
      std::vector<Transform2D> skins = layer_point_skins(doc, layer);
      skins.resize(drawing.points.size());
      const float2 in_layer = layer_world_transform(doc, layer.id).inverted().apply({at.first, at.second});
      const StackPlace place{curve.points[size_t(index)].point, was};
      layer_handles_settle(doc, layer, {{curve_index, index, side == "IN", keep_other, in_layer,
                                         fitted ? layer_place_unskin(doc, layer, place)
                                                : skins[size_t(curve.points[size_t(index)].point)].inverted(),
                                         length(stored - was)}});
    }
    current_context().undo->mark_changed();
  });

  m.def("layer_to_document", [layer_or_raise](uint64_t id, std::pair<float, float> p) {
    const float2 q = layer_world_transform(*current_context().document, layer_or_raise(id).id).apply({p.first, p.second});
    return py::make_tuple(q.x, q.y);
  });

  /* Point groups: [(name, {point index: weight}, locked)], and the active one's index. */
  m.def("layer_point_groups", [layer_or_raise](uint64_t id) {
    const VectorDrawing &d = layer_or_raise(id).drawing;
    py::list groups;
    for (const PointGroup &g : d.groups) {
      py::dict members;
      for (int i = 0; i < int(d.points.size()); i++) {
        if (const auto it = g.weights.find(d.points[size_t(i)].id); it != g.weights.end()) {
          members[py::int_(i)] = it->second;
        }
      }
      groups.append(py::make_tuple(g.name, members, g.locked));
    }
    return py::make_tuple(groups, d.active_group);
  });

  /* Extrude's first step (E before the move): new points past the selected open ends, or branching from other
   * selected points; they become the selection. Returns their indices. */
  m.def("layer_extrude_selected", [layer_or_raise](uint64_t id) {
    Context &C = current_context();
    Layer &layer = layer_or_raise(id);
    if (!layer.has_drawing() || layer.is_reference()) {
      throw py::value_error("Only a vector layer's own points extrude");
    }
    const std::vector<int> added = drawing_extrude_selected(layer.drawing, C.wm->tool_settings.style);
    C.undo->mark_changed();
    return added;
  });
  /* A shape's runs (its connected pieces, each drawn as a line of its own): [[(curve, segment), ...], ...]. */
  m.def("layer_shape_runs", [layer_or_raise](uint64_t id, int shape) {
    const VectorDrawing &d = layer_or_raise(id).drawing;
    if (shape < 0 || shape >= int(d.shapes.size())) {
      throw py::index_error("Shape index out of range");
    }
    py::list runs;
    for (const Shape &run : shape_runs(d, d.shapes[size_t(shape)])) {
      py::list edges;
      for (const ShapeEdge &e : run.edges) {
        edges.append(py::make_tuple(e.curve, e.segment));
      }
      runs.append(edges);
    }
    return runs;
  });
  /* Grids: [(name, columns, rows, [node point index, row by row from the bottom])]. */
  m.def("layer_grids", [layer_or_raise](uint64_t id) {
    const VectorDrawing &d = layer_or_raise(id).drawing;
    py::list grids;
    for (const DrawingGrid &g : d.grids) {
      py::list nodes;
      for (const uint64_t node : g.nodes) {
        nodes.append(drawing_point_index(d, node));
      }
      grids.append(py::make_tuple(g.name, g.columns, g.rows, nodes));
    }
    return grids;
  });
  /* Where a grid takes a place: from where its setup (frame 0) has it to where the grid shows now. */
  m.def("layer_grid_map", [layer_or_raise](uint64_t id, int grid, float x, float y) {
    const Document &doc = *current_context().document;
    const Layer &layer = layer_or_raise(id);
    if (grid < 0 || grid >= int(layer.drawing.grids.size())) {
      throw py::index_error("Grid index out of range");
    }
    const VectorDrawing rest = layer_setup_drawing(doc, layer, false);
    const VectorDrawing bent = layer_evaluated_drawing(doc, layer, false);
    const float2 q = grid_map(rest, bent, layer.drawing.grids[size_t(grid)], {x, y});
    return py::make_tuple(q.x, q.y);
  });

  /* --- Modifiers and the evaluated drawing ------------------------------------------------ */
  m.def("layer_modifiers", [layer_or_raise](uint64_t id) {
    py::list result;
    for (const Modifier &mod : layer_or_raise(id).modifiers) {
      py::dict d;
      d["type"] = mod.type == ModifierType::Armature    ? "ARMATURE"
                  : mod.type == ModifierType::Boolean   ? "BOOLEAN"
                  : mod.type == ModifierType::Bevel     ? "BEVEL"
                  : mod.type == ModifierType::Resample ? "RESAMPLE"
                  : modifier_is_deform(mod.type) ? deform_type_id(mod.type)
                                                        : "MIRROR";
      if (modifier_is_deform(mod.type)) {
        const Document &doc = *current_context().document;
        const Layer *target = layer_find(doc, mod.deform_layer);
        d["layer"] = target ? target->name : std::string();
        std::string shape;
        if (target) {
          if (const int k = drawing_shape_index(target->drawing, mod.deform_shape); k >= 0) {
            shape = target->drawing.shapes[size_t(k)].name;
          }
        }
        d["shape"] = shape;
        d["kind"] = deform_kind_name(vector_deform_kind(doc, layer_or_raise(id), mod));
        d["fits"] = deform_type_id(vector_deform_fitting_type(doc, layer_or_raise(id), mod));
        d["weights"] = mod.deform_custom ? "CUSTOM" : "AUTOMATIC";
        d["group"] = mod.deform_group;
        d["strength"] = mod.deform_strength;
        d["precision"] = mod.deform_precision;
        d["stretch"] = mod.deform_stretch;
        /* Its two ends as point numbers on the shape's layer, None when it has no Part (or they are gone). */
        d["part"] = py::none();
        if (target && mod.deform_part_from != 0) {
          const int from = drawing_point_index(target->drawing, mod.deform_part_from);
          const int to = drawing_point_index(target->drawing, mod.deform_part_to);
          if (from >= 0 && to >= 0) {
            d["part"] = py::make_tuple(from, to);
          }
        }
        d["resample_self"] = mod.deform_resample_self;
        d["cage_method"] = cage_method_id(mod.deform_cage_method);
        d["mesh_method"] = mesh_method_id(mod.deform_mesh_method);
        d["smooth"] = mod.deform_smooth;
        d["blend"] = mod.deform_blend;
        if (mod.type == ModifierType::FollowDeform) {
          const auto pair = [](float2 v) { return py::make_tuple(v.x, v.y); };
          d["point"] = pair(mod.deform_point);
          d["point2"] = pair(mod.deform_point2);
          d["show_point"] = mod.deform_show_point;
          d["second"] = mod.deform_two;
          d["rotation"] = mod.deform_rotation;
          d["scale"] = mod.deform_scale;
          d["shift"] = pair(mod.deform_shift);
          d["shift2"] = pair(mod.deform_shift2);
          /* A point of the shape it sits on, by its number on the shape's layer: None for its own place. */
          d["shape_point"] = follow_shape_point_number(target, mod.deform_point_id);
          d["shape_point2"] = follow_shape_point_number(target, mod.deform_point2_id);
        }
      }
      if (mod.type == ModifierType::Resample) {
        d["split"] = mod.subdiv_by_length ? "LENGTH" : "COUNT";
        d["spacing"] = mod.subdiv_spacing;
        d["render_spacing"] = mod.subdiv_render_spacing;
        d["pieces"] = mod.subdiv_pieces;
        d["max_pieces"] = mod.subdiv_max_pieces;
        d["weights"] = mod.subdiv_from_bones ? "BONES" : "BLEND";
        d["preserve_volume"] = mod.subdiv_preserve_volume;
        d["group"] = mod.subdiv_group;
      }
      if (mod.type == ModifierType::Bevel) {
        d["limit"] = mod.bevel_limit == BevelLimit::Group ? "GROUP" : mod.bevel_limit == BevelLimit::None ? "NONE" : "ANGLE";
        d["width_type"] = mod.bevel_width == BevelWidth::Width   ? "WIDTH"
                          : mod.bevel_width == BevelWidth::Depth   ? "DEPTH"
                          : mod.bevel_width == BevelWidth::Percent ? "PERCENT"
                                                                   : "OFFSET";
        d["angle"] = mod.bevel_angle * 180.0f / 3.1415927f;
        d["group"] = mod.bevel_group;
        d["amount"] = mod.bevel_amount;
        d["profile"] = mod.bevel_profile;
        d["clamp"] = mod.bevel_clamp;
      }
      d["name"] = mod.name;
      d["show_viewport"] = mod.show_viewport;
      d["show_render"] = mod.show_render;
      if (modifier_makes_points(mod)) {
        d["show_points"] = mod.show_points;
      }
      if (mod.type == ModifierType::Mirror) {
        d["axis_x"] = mod.axis_x;
        d["axis_y"] = mod.axis_y;
        d["center"] = py::make_tuple(mod.center.x, mod.center.y);
        d["merge"] = mod.merge;
        d["merge_distance"] = mod.merge_distance;
        d["mirror_bones"] = mod.mirror_bones;
      }
      if (mod.type == ModifierType::Boolean) {
        const Document &doc = *current_context().document;
        const Layer &self = layer_or_raise(id);
        const auto shape_names = [](const Layer *l, const std::vector<uint64_t> &ids) {
          py::list names;
          for (const uint64_t sid : ids) {
            for (const Shape &s : l ? l->drawing.shapes : std::vector<Shape>()) {
              if (s.id == sid) {
                names.append(s.name);
              }
            }
          }
          return names;
        };
        py::list targets;
        py::dict target_shapes;
        for (const MergeTarget &t : mod.merge_targets) {
          const Layer *l = layer_find(doc, t.layer);
          targets.append(l ? l->name : std::string());
          if (l) {
            target_shapes[py::str(l->name)] = shape_names(l, t.shapes);
          }
        }
        d["targets"] = targets;
        d["target_shapes"] = target_shapes;
        d["shapes"] = shape_names(&self, mod.merge_shapes);
        d["operation"] = mod.merge_operation == MergeOperation::Union      ? "UNION"
                         : mod.merge_operation == MergeOperation::Subtract ? "SUBTRACT"
                                                                           : "INTERSECT";
        d["smoothing"] = mod.merge_smoothing;
        d["corners_only"] = mod.merge_corners_only;
      }
      result.append(d);
    }
    return result;
  });
  /* A deform modifier's weight for each of the layer's own points (Custom, else automatic), before Strength;
   * `reset`: fill its Custom group with the automatic weights first (Reset to Automatic). */
  m.def(
      "layer_deform_weights",
      [layer_or_raise](uint64_t id, int index, bool reset) {
        Layer &layer = layer_or_raise(id);
        if (index < 0 || index >= int(layer.modifiers.size()) ||
            !modifier_is_deform(layer.modifiers[size_t(index)].type))
        {
          throw py::index_error("no deform modifier (Curve, Cage or Mesh Deform) " + std::to_string(index));
        }
        Document &doc = *current_context().document;
        if (reset) {
          vector_deform_fill_group(doc, layer, layer.modifiers[size_t(index)]);
          current_context().undo->mark_changed();
        }
        return vector_deform_weights(doc, layer, layer.modifiers[size_t(index)]);
      },
      py::arg("id"), py::arg("index"), py::arg("reset") = false);
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
      else if (key == "show_points" && modifier_makes_points(mod)) {
        mod.show_points = value.cast<bool>();
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
      else if (mod.type == ModifierType::Boolean && key == "smoothing") {
        mod.merge_smoothing = std::max(0.0f, value.cast<float>());
      }
      else if (mod.type == ModifierType::Bevel && key == "limit") {
        const std::string limit = value.cast<std::string>();
        if (limit != "ANGLE" && limit != "GROUP" && limit != "NONE") {
          throw py::value_error("limit is NONE, ANGLE or GROUP");
        }
        mod.bevel_limit = limit == "GROUP" ? BevelLimit::Group : limit == "NONE" ? BevelLimit::None : BevelLimit::Angle;
      }
      else if (mod.type == ModifierType::Bevel && key == "width_type") {
        const std::string width = value.cast<std::string>();
        if (width == "OFFSET") mod.bevel_width = BevelWidth::Offset;
        else if (width == "WIDTH") mod.bevel_width = BevelWidth::Width;
        else if (width == "DEPTH") mod.bevel_width = BevelWidth::Depth;
        else if (width == "PERCENT") mod.bevel_width = BevelWidth::Percent;
        else throw py::value_error("width_type is OFFSET, WIDTH, DEPTH or PERCENT");
      }
      else if (mod.type == ModifierType::Bevel && key == "angle") {
        mod.bevel_angle = std::clamp(value.cast<float>(), 0.0f, 180.0f) * 3.1415927f / 180.0f;
      }
      else if (mod.type == ModifierType::Bevel && key == "group") {
        mod.bevel_group = value.cast<std::string>();
      }
      else if (mod.type == ModifierType::Bevel && key == "amount") {
        mod.bevel_amount = std::max(0.0f, value.cast<float>());
      }
      else if (mod.type == ModifierType::Bevel && key == "profile") {
        mod.bevel_profile = std::clamp(value.cast<float>(), 0.0f, 1.0f);
      }
      else if (mod.type == ModifierType::Bevel && key == "clamp") {
        mod.bevel_clamp = value.cast<bool>();
      }
      else if (mod.type == ModifierType::Resample && key == "split") {
        const std::string split = value.cast<std::string>();
        if (split != "LENGTH" && split != "COUNT") {
          throw py::value_error("split is LENGTH or COUNT");
        }
        mod.subdiv_by_length = split == "LENGTH";
      }
      else if (mod.type == ModifierType::Resample && key == "spacing") {
        mod.subdiv_spacing = std::max(1e-4f, value.cast<float>());
      }
      else if (mod.type == ModifierType::Resample && key == "render_spacing") {
        mod.subdiv_render_spacing = std::max(0.0f, value.cast<float>());
      }
      else if (mod.type == ModifierType::Resample && key == "pieces") {
        mod.subdiv_pieces = std::clamp(value.cast<int>(), 1, 256);
      }
      else if (mod.type == ModifierType::Resample && key == "max_pieces") {
        mod.subdiv_max_pieces = std::clamp(value.cast<int>(), 1, 256);
      }
      else if (mod.type == ModifierType::Resample && key == "weights") {
        const std::string w = value.cast<std::string>();
        if (w != "BLEND" && w != "BONES") {
          throw py::value_error("weights is BLEND or BONES");
        }
        mod.subdiv_from_bones = w == "BONES";
      }
      else if (mod.type == ModifierType::Resample && key == "preserve_volume") {
        mod.subdiv_preserve_volume = value.cast<bool>();
      }
      else if (mod.type == ModifierType::Resample && key == "group") {
        mod.subdiv_group = value.cast<std::string>();
      }
      else if (mod.type == ModifierType::FollowDeform &&
               (key == "point" || key == "point2" || key == "shift" || key == "shift2")) {
        const auto v = value.cast<std::pair<float, float>>();
        (key == "point"    ? mod.deform_point
         : key == "point2" ? mod.deform_point2
         : key == "shift"  ? mod.deform_shift
                           : mod.deform_shift2) = {v.first, v.second};
      }
      else if (mod.type == ModifierType::FollowDeform && (key == "point_to" || key == "point2_to")) {
        /* Where it's to show now, in document space: as a drag does. */
        const auto v = value.cast<std::pair<float, float>>();
        const Document &doc = *current_context().document;
        const Layer &self = layer_or_raise(id);
        const bool second = key == "point2_to";
        const float2 to = layer_world_transform(doc, id).inverted().apply({v.first, v.second});
        (second ? mod.deform_shift2 : mod.deform_shift) = vector_deform_follow_shift(doc, self, mod, second, to);
      }
      else if (mod.type == ModifierType::FollowDeform && (key == "shape_point" || key == "shape_point2")) {
        Document &doc = *current_context().document;
        const uint64_t point_id = follow_shape_point(layer_find(doc, mod.deform_layer), mod.deform_shape, value);
        vector_deform_follow_pick(doc, layer_or_raise(id), mod, key == "shape_point2", point_id);
      }
      else if (mod.type == ModifierType::FollowDeform && key == "show_point") {
        mod.deform_show_point = value.cast<bool>();
      }
      else if (mod.type == ModifierType::FollowDeform && key == "second") {
        vector_deform_follow_two(mod, value.cast<bool>());
      }
      else if (mod.type == ModifierType::FollowDeform && key == "rotation") {
        mod.deform_rotation = value.cast<bool>();
      }
      else if (mod.type == ModifierType::FollowDeform && key == "scale") {
        mod.deform_scale = value.cast<bool>();
      }
      else if (mod.type == ModifierType::FollowDeform && key == "type") {
        throw py::value_error("Follow Deform stays Follow Deform");
      }
      else if (modifier_is_deform(mod.type) && key == "type") {
        /* Change Type: another deform, keeping its settings and painted weights. */
        const std::string type = value.cast<std::string>();
        const ModifierType to = type == "CURVE_DEFORM"  ? ModifierType::CurveDeform
                                : type == "CAGE_DEFORM" ? ModifierType::CageDeform
                                : type == "MESH_DEFORM" ? ModifierType::MeshDeform
                                                        : ModifierType::Mirror;
        if (to == ModifierType::Mirror) {
          throw py::value_error("a deform's type is CURVE_DEFORM, CAGE_DEFORM or MESH_DEFORM");
        }
        if (mod.name.rfind(deform_type_name(mod.type), 0) == 0) {
          mod.name = deform_type_name(to) + mod.name.substr(std::string(deform_type_name(mod.type)).size());
        }
        mod.type = to;
      }
      else if (modifier_is_deform(mod.type) && key == "layer") {
        const std::string name = value.cast<std::string>();
        const Layer *target = nullptr;
        for (const Layer *l : document_layers_flat(*current_context().document)) {
          target = !target && l->name == name ? l : target;
        }
        if (!name.empty() && (!target || !target->has_drawing() || target->id == layer.id)) {
          throw py::value_error("A deform's shape is on another drawing layer: no layer '" + name + "'");
        }
        mod.deform_layer = target ? target->id : 0;
        mod.deform_shape = 0;
        mod.deform_part_from = mod.deform_part_to = 0;
        mod.deform_point_id = mod.deform_point2_id = 0;
      }
      else if (modifier_is_deform(mod.type) && key == "shape") {
        const std::string name = value.cast<std::string>();
        Layer *target = layer_find(*current_context().document, mod.deform_layer);
        /* A shape just drawn from Python has no id yet. */
        if (target) {
          drawing_ensure_ids(target->drawing);
        }
        uint64_t found = 0;
        for (const Shape &s : target ? target->drawing.shapes : std::vector<Shape>()) {
          found = !found && s.name == name ? s.id : found;
        }
        if (!found && !name.empty()) {
          throw py::value_error("no shape '" + name + "' on the deform's layer (set layer first)");
        }
        mod.deform_part_from = found == mod.deform_shape ? mod.deform_part_from : 0;
        mod.deform_part_to = found == mod.deform_shape ? mod.deform_part_to : 0;
        mod.deform_point_id = found == mod.deform_shape ? mod.deform_point_id : 0;
        mod.deform_point2_id = found == mod.deform_shape ? mod.deform_point2_id : 0;
        mod.deform_shape = found;
      }
      else if (modifier_is_deform(mod.type) && key == "weights") {
        const std::string w = value.cast<std::string>();
        if (w != "AUTOMATIC" && w != "CUSTOM") {
          throw py::value_error("weights is AUTOMATIC or CUSTOM");
        }
        const bool was = mod.deform_custom;
        mod.deform_custom = w == "CUSTOM";
        /* Custom starts from the automatic weights, unless its group is already there. */
        if (mod.deform_custom && !was &&
            std::none_of(layer.drawing.groups.begin(), layer.drawing.groups.end(),
                         [&](const PointGroup &g) { return !mod.deform_group.empty() && g.name == mod.deform_group; }))
        {
          vector_deform_fill_group(*current_context().document, layer, mod);
        }
      }
      else if (modifier_is_deform(mod.type) && key == "strength") {
        mod.deform_strength = std::clamp(value.cast<float>(), 0.0f, 1.0f);
      }
      else if (modifier_is_deform(mod.type) && key == "precision") {
        mod.deform_precision = std::max(0.0f, value.cast<float>());
      }
      else if (modifier_is_deform(mod.type) && key == "stretch") {
        mod.deform_stretch = value.cast<bool>();
      }
      else if (mod.type == ModifierType::CurveDeform && key == "part") {
        Document &doc = *current_context().document;
        if (value.is_none()) {
          mod.deform_part_from = mod.deform_part_to = 0;
        }
        else if (py::isinstance<py::str>(value)) {
          if (value.cast<std::string>() != "SELECTED") {
            throw py::value_error("part is (from, to) point numbers, \"SELECTED\" or None");
          }
          if (!vector_deform_part_from_selection(doc, mod)) {
            throw py::value_error("the selected points don't run along the shape end to end");
          }
        }
        else {
          const auto ends = value.cast<std::pair<int, int>>();
          Layer *target = layer_find(doc, mod.deform_layer);
          if (!target || std::min(ends.first, ends.second) < 0 ||
              std::max(ends.first, ends.second) >= int(target->drawing.points.size()))
          {
            throw py::value_error("part's points aren't on the deform's layer (set layer first)");
          }
          drawing_ensure_ids(target->drawing);
          mod.deform_part_from = target->drawing.points[size_t(ends.first)].id;
          mod.deform_part_to = target->drawing.points[size_t(ends.second)].id;
        }
      }
      else if (modifier_is_deform(mod.type) && key == "resample_self") {
        mod.deform_resample_self = value.cast<bool>();
      }
      else if (modifier_is_deform(mod.type) && key == "cage_method") {
        if (!cage_method_from_id(value.cast<std::string>(), mod.deform_cage_method)) {
          throw py::value_error("cage_method is MEAN_VALUE, RIGID or ENVELOPE");
        }
      }
      else if (modifier_is_deform(mod.type) && key == "smooth") {
        mod.deform_smooth = value.cast<bool>();
      }
      else if (modifier_is_deform(mod.type) && key == "mesh_method") {
        if (!mesh_method_from_id(value.cast<std::string>(), mod.deform_mesh_method)) {
          throw py::value_error("mesh_method is CELLS or ENVELOPE");
        }
      }
      else if (modifier_is_deform(mod.type) && key == "blend") {
        mod.deform_blend = std::clamp(value.cast<float>(), 0.0f, 1.0f);
      }
      else {
        throw py::key_error("unknown modifier setting '" + key + "'");
      }
    }
    current_context().undo->mark_changed();
  });
  /* The drawing as it shows: (points [(x, y)], curve count, shapes [(name, closed, connected, merged,
   * area, fill)]). */
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
          const bool connected = shape_outline(d, shape, 16, outline, nullptr, &closed);
          /* The area it fills (0 for an open one). */
          double area = 0.0;
          for (size_t k = 0; closed && k < outline.size(); k++) {
            const float2 a = outline[k], b = outline[(k + 1) % outline.size()];
            area += double(a.x) * b.y - double(b.x) * a.y;
          }
          const char *merged = shape.merged_fill && shape.merged_line ? "ALL"
                               : shape.merged_fill                     ? "FILL"
                               : shape.merged_line                     ? "LINE"
                                                                       : "";
          shapes.append(py::make_tuple(shape.name, closed, connected, merged, std::fabs(area) * 0.5,
                                       py::make_tuple(shape.style.fill.r, shape.style.fill.g, shape.style.fill.b,
                                                      shape.style.fill.a)));
        }
        return py::make_tuple(points, int(d.curves.size()), shapes);
      },
      py::arg("id"), py::arg("render") = false);
  /* A shape's outline as the evaluated drawing draws it: `steps` samples per curve segment (at least). */
  m.def(
      "layer_evaluated_outline",
      [layer_or_raise](uint64_t id, int shape, int steps, bool render) {
        Document &doc = *current_context().document;
        animation_sync(doc);
        const VectorDrawing d = layer_evaluated_drawing(doc, layer_or_raise(id), render);
        if (shape < 0 || shape >= int(d.shapes.size())) {
          throw py::index_error("Shape index out of range");
        }
        std::vector<float2> outline;
        bool closed = false;
        shape_outline(d, d.shapes[size_t(shape)], std::clamp(steps, 1, 256), outline, nullptr, &closed);
        py::list result;
        for (const float2 &q : outline) {
          result.append(py::make_tuple(q.x, q.y));
        }
        return py::make_tuple(result, closed);
      },
      py::arg("id"), py::arg("shape"), py::arg("steps") = 16, py::arg("render") = false);
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
                                               const std::string &mode, bool normalize, bool selected_only,
                                               bool keep_animation) {
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
    /* Keep Animation, as the brush in the viewport: every action's keyed frames show it where they did. */
    const DrawingsShownAll kept = keep_animation ? drawings_shown_capture_all(doc, {layer.id}) : DrawingsShownAll();
    const int changed = layer_weight_paint(doc, layer, brush);
    if (changed > 0 && keep_animation) {
      drawings_shown_keep_all(doc, kept);
    }
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
                       bool fill, int columns, int rows) {
        Context &C = current_context();
        Layer &layer = layer_or_raise(id);
        if (!layer.has_drawing() || layer.is_reference()) {
          throw py::value_error("Shapes can only be added to vector layers that are not references");
        }
        PrimitiveKind kind;
        if (!primitive_from_identifier(kind_name, &kind)) {
          throw py::value_error("add_primitive draws BOX, CIRCLE or GRID; use add_curve for curves");
        }
        std::vector<float2> pts;
        for (const auto &[x, y] : points) {
          pts.push_back({x, y});
        }
        const VectorDrawing shape = primitive_drawing(kind, pts, C.wm->tool_settings.style, fill, columns, rows);
        if (shape.curves.empty()) {
          return false;
        }
        drawing_append_selected(layer.drawing, shape);
        C.undo->mark_changed();
        return true;
      },
      py::arg("id"), py::arg("kind"), py::arg("points"), py::arg("fill") = false, py::arg("columns") = 3,
      py::arg("rows") = 3);
  m.def("layer_shape_outline", [layer_or_raise](uint64_t id, int shape, int steps) {
    /* A shape's outline as drawn: steps samples per curve segment. */
    /* As drawn: detail's handles final. */
    VectorDrawing drawing = layer_drawing(*current_context().document, layer_or_raise(id));
    drawing_detail_final_handles(drawing);
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
  /* Detail levels: each point's level and whether it is hidden, and each shape's edit level and
   * highest level. */
  m.def("layer_detail", [layer_or_raise](uint64_t id) {
    VectorDrawing &drawing = layer_or_raise(id).drawing;
    drawing_detail_sync(drawing, true);
    py::list levels, hidden, shape_levels, shape_max;
    for (const MeshPoint &pt : drawing.points) {
      levels.append(pt.detail.level);
      hidden.append(pt.detail.hidden);
    }
    for (const Shape &shape : drawing.shapes) {
      shape_levels.append(shape.detail_level);
      shape_max.append(shape_detail_max_level(drawing, shape));
    }
    py::dict r;
    r["levels"] = levels;
    r["hidden"] = hidden;
    r["shape_levels"] = shape_levels;
    r["shape_max_levels"] = shape_max;
    /* The points each curve's wire runs through, at its edit level. */
    py::list wires;
    for (const Curve &curve : drawing_detail_shown_curves(drawing).curves) {
      py::list points;
      for (const CurvePoint &cp : curve.points) {
        points.append(cp.point);
      }
      wires.append(points);
    }
    r["wires"] = wires;
    return r;
  });
  /* Media layers' clips, as dicts (oanim.Layer.media_clips). */
  m.def("layer_media_clips", [layer_or_raise](uint64_t id) {
    static const char *const ends[] = {"HOLD", "LOOP", "HIDE"};
    static const char *const timings[] = {"REAL_TIME", "FRAME_BY_FRAME"};
    py::list clips;
    for (const MediaClip &c : layer_or_raise(id).media.clips) {
      py::dict d;
      d["id"] = c.id;
      d["filepath"] = c.filepath;
      d["sequence"] = c.sequence;
      d["packed"] = !c.packed.empty();
      d["start"] = c.start;
      d["length"] = c.length;
      d["in_point"] = c.in;
      d["end"] = ends[int(c.end)];
      d["timing"] = timings[int(c.timing)];
      d["missing"] = media_clip_missing(c);
      d["selected"] = c.selected;
      clips.append(d);
    }
    return clips;
  });
  /* What a Media layer shows on a frame: (clip id, seconds into its file), or None. */
  /* How a property's field shows now (oanim layer.field_state): its key state and whether a smart
   * action plays into it. */
  m.def("layer_field_state", [layer_or_raise](uint64_t id, const std::string &path) {
    Layer &layer = layer_or_raise(id);
    const int key = layer_key_state(layer, {path}, current_context().document->frame_current);
    static const char *const keys[] = {"NONE", "ANIMATED", "KEYED"};
    return py::make_tuple(std::string(keys[std::clamp(key, 0, 2)]), layer_smart_state(layer, {path}) != 0);
  });
  /* A bone layer's actions, and what of them plays now. */
  m.def("layer_actions", [layer_or_raise](uint64_t id) {
    Layer &layer = layer_or_raise(id);
    py::list out;
    for (const std::string &name : layer.actions) {
      py::dict d;
      d["name"] = name;
      d["kind"] = smart_action_bone(layer, name) ? "SMART" : fix_action_find(layer, name) ? "FIX" : "ACTION";
      d["full_turn"] = smart_action_full_turn(layer, name);
      out.append(d);
    }
    return out;
  });
  m.def("layer_action_owners", [layer_or_raise](uint64_t id) {
    layer_or_raise(id);
    py::list out;
    for (const Layer *owner : action_owners(*current_context().document, id)) {
      out.append(owner->id);
    }
    return out;
  });
  m.def("layer_smart_actions_playing", [layer_or_raise](uint64_t id) {
    Layer &layer = layer_or_raise(id);
    py::list out;
    for (const auto &[name, t] : smart_actions_playing(layer)) {
      const float length = smart_action_length(layer, name);
      out.append(py::make_tuple(name, t, length > 0.0f ? std::min(t / length, 1.0f) : 0.0f));
    }
    return out;
  });
  m.def("layer_fix_actions", [layer_or_raise](uint64_t id) {
    Layer &layer = layer_or_raise(id);
    const auto playing = smart_actions_playing(layer);
    py::list out;
    for (const FixAction &f : layer.fix_actions) {
      py::dict d;
      d["name"] = f.name;
      py::list drivers, poses;
      for (const std::string &n : f.drivers) {
        drivers.append(n);
      }
      for (const FixPose &pose : f.poses) {
        py::dict pd;
        py::list at, shares;
        for (const float v : pose.at) {
          at.append(v);
        }
        for (const float v : fix_pose_shares(layer, f, pose)) {
          shares.append(v);
        }
        pd["frame"] = pose.frame;
        pd["at"] = at;
        pd["shares"] = shares;
        pd["label"] = fix_pose_label(layer, f, pose);
        poses.append(pd);
      }
      const FixMix mix = fix_action_mix(layer, f, playing);
      d["drivers"] = drivers;
      d["poses"] = poses;
      d["strength"] = mix.strength;
      d["nearest_pose"] = mix.nearest;
      d["broken"] = fix_action_broken(layer, f);
      out.append(d);
    }
    return out;
  });
  m.def("layer_media_shown", [layer_or_raise](uint64_t id, int frame) -> py::object {
    const MediaShown shown = media_shown_at(*current_context().document, layer_or_raise(id), frame);
    if (!shown.clip) {
      return py::none();
    }
    return py::make_tuple(shown.clip->id, shown.seconds);
  });
  m.def("media_probe", [](const std::string &path, bool sequence) {
    static const char *const kinds[] = {"NONE", "IMAGE", "SEQUENCE", "VIDEO", "AUDIO"};
    const MediaInfo info = media_probe(path, sequence);
    py::dict d;
    d["kind"] = kinds[int(info.kind)];
    d["width"] = info.width;
    d["height"] = info.height;
    d["source_width"] = info.source_width;
    d["source_height"] = info.source_height;
    d["duration"] = info.duration;
    d["fps"] = info.fps;
    d["frame_count"] = info.frame_count;
    d["has_audio"] = info.has_audio;
    d["error"] = info.error;
    return d;
  });
  /* A decoded picture's pixel (waiting for the exact frame): (width, height, (r, g, b, a), time). */
  m.def("media_pixel", [](const std::string &path, bool sequence, double seconds, int x, int y) -> py::object {
    const MediaFramePtr f = media_frame(path, sequence, seconds, true);
    if (!f || f->rgba.empty()) {
      return py::none();
    }
    const int px = std::clamp(x, 0, f->width - 1), py_ = std::clamp(y, 0, f->height - 1);
    const uint8_t *p = &f->rgba[(size_t(py_) * f->width + px) * 4];
    return py::make_tuple(f->width, f->height, py::make_tuple(p[0], p[1], p[2], p[3]), f->time);
  });
  /* The document's sound at `seconds` for `frames` stereo frames (interleaved), once every source
   * has decoded. */
  m.def("media_mix", [](double seconds, int frames) {
    Document &doc = *current_context().document;
    for (Layer *l : media_layers(doc)) {
      for (const MediaClip &c : l->media.clips) {
        if (media_clip_info(c).has_audio) {
          media_sound(media_clip_file(c), true);
        }
      }
    }
    std::vector<float> out(size_t(std::max(frames, 0)) * 2, 0.0f);
    media_mix(doc, seconds, size_t(std::max(frames, 0)), out.data());
    py::list samples;
    for (const float v : out) {
      samples.append(v);
    }
    return samples;
  });
  m.def("media_missing_count", []() { return media_missing_count(*current_context().document); });
  /* A layer's mask rows: [{"mode", "invert", "layers" (names; "" for one that's gone), "enabled", ...}]. */
  m.def("layer_line_shows", [layer_or_raise](uint64_t id, float x, float y, bool render) {
    Layer &layer = layer_or_raise(id);
    const LineCutter cutter = line_cutter(*current_context().document, layer, render);
    return line_cutter_shows(cutter, float2(x, y));
  });
  m.def("layer_masks", [layer_or_raise](uint64_t id) {
    Document &doc = *current_context().document;
    const auto step_dict = [&](auto &&self, const MaskStep &s, py::dict &d) -> void {
      d["verb"] = mask_verb_identifier(mask_step_verb(s));
      d["name"] = s.name;
      d["group"] = s.group;
      d["whole"] = s.whole;
      d["global"] = s.global;
      d["users"] = mask_area_users(doc, s.global);
      py::list kids;
      for (const MaskStep &child : s.children) {
        py::dict cd;
        self(self, child, cd);
        kids.append(cd);
      }
      d["children"] = kids;
      d["mode"] = mask_mode_identifier(s.mode);
      d["invert"] = s.invert;
      d["combine"] = mask_combine_identifier(s.combine);
      d["grow"] = s.grow;
      /* As before rows: Outside was a Subtract, Expand a grow. */
      d["outside"] = s.mode == MaskMode::Subtract;
      d["expand"] = s.grow > 0.0f;
      d["fills_only"] = s.fills_only;
      d["strokes_only"] = s.strokes_only;
      d["inside_stroke"] = s.inside_stroke;
      d["shape"] = s.fills_only ? "FILLS_ONLY" : s.strokes_only ? "STROKES_ONLY" : "AS_DRAWN";
      d["filled"] = s.filled;
      d["extend"] = mask_extend_identifier(s.extend);
      py::list names;
      for (const uint64_t l : s.layers) {
        const Layer *found = layer_find(doc, l);
        names.append(found ? found->name : std::string());
      }
      d["layers"] = names;
    };
    py::list result;
    for (const LayerMask &card : layer_or_raise(id).masks) {
      py::dict d;
      d["name"] = card.name;
      d["start"] = card.start_shown ? "EVERYTHING" : "NOTHING";
      d["enabled"] = card.enabled;
      d["render"] = card.render;
      d["end_lines"] = card.end_lines;
      d["ends"] = card.end_lines_flat ? "FLAT" : "BRUSH";
      d["reach"] = card.line_reach;
      d["cuts"] = mask_cuts_identifier(card.cuts);
      d["lines_only"] = card.cuts == MaskCuts::Lines;
      py::list steps;
      for (const MaskStep &s : card.steps) {
        py::dict sd;
        step_dict(step_dict, s, sd);
        const char *problem = mask_group_problem(card, size_t(&s - card.steps.data()));
        sd["problem"] = problem ? problem : "";
        steps.append(sd);
      }
      d["steps"] = steps;
      /* Its first step's, on the card too (as rows had them). */
      if (!card.steps.empty()) {
        step_dict(step_dict, card.steps.front(), d);
        d["name"] = card.name;
      }
      result.append(d);
    }
    return result;
  });
  m.def("layer_mask_summary", [layer_or_raise](uint64_t id) {
    return mask_summary(*current_context().document, layer_or_raise(id));
  });
  /* The layers whose masks name this one. */
  m.def("layer_masking", [layer_or_raise](uint64_t id) {
    Document &doc = *current_context().document;
    layer_or_raise(id);
    py::list names;
    for (const Layer *l : document_layers_flat(doc)) {
      for (const LayerMask &mask : layer_mask_cards(doc, *l)) {
        const std::vector<uint64_t> named = mask_card_layers(mask);
        if (std::find(named.begin(), named.end(), id) != named.end()) {
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
  /* What a viewport click on `hit` picks for the Draw Order eyedropper moving `moving` (ids). */
  m.def("layer_bone_family", [layer_or_raise](uint64_t id) {
    layer_or_raise(id);
    std::vector<std::string> names;
    for (const Layer *l : armature_family(*current_context().document, id)) {
      names.push_back(l->name);
    }
    return names;
  });
  m.def("layer_switch_link", [layer_or_raise](uint64_t id) {
    /* A linked switch's group: its members' names (the lead first), its names, what it lacks now. */
    const Layer &sw = layer_or_raise(id);
    Document &doc = *current_context().document;
    py::list members, names;
    if (const SwitchLink *link = switch_link_of(doc, sw)) {
      if (const Layer *lead = layer_find(doc, link->lead)) {
        members.append(lead->name);
      }
      for (const Layer *m : switch_link_members(doc, link->id)) {
        if (m->id != link->lead) {
          members.append(m->name);
        }
      }
      for (const SwitchLinkName &n : link->names) {
        names.append(n.name);
      }
    }
    return py::make_tuple(members, names, sw.switch_missing);
  });
  m.def("layer_aliases", [layer_or_raise](uint64_t id) { return layer_or_raise(id).switch_aliases; });
  m.def("switch_link_report", [layer_or_raise](uint64_t id) {
    const SwitchLink *link = switch_link_of(*current_context().document, layer_or_raise(id));
    if (link) {
      ui_switch_link_report_open(link->id);
    }
    return link != nullptr;
  });
  m.def("bone_turn_snap", [](float degrees, bool rotate_key) {
    std::string what;
    const float snapped = viewport_bone_turn_snap(*current_context().document, degrees, rotate_key, &what);
    return py::make_tuple(snapped, what);
  });
  m.def("frame_sync_playing", [](int frame, uint64_t edits) {
    Document &doc = *current_context().document;
    doc.frame_current = frame;
    animation_sync_playing(doc, edits);
  });
  m.def("switch_preview", [](uint64_t sw, uint64_t child) {
    if (sw == 0) {
      switch_preview_clear();
    }
    else {
      switch_preview_set(sw, child);
    }
    Document &doc = *current_context().document;
    switches_resolve(doc, doc.frame_current);
  });
  m.def("layer_switch", [layer_or_raise](uint64_t id) {
    const Layer &sw = layer_or_raise(id);
    if (!sw.is_switch()) {
      throw py::value_error("\"" + sw.name + "\" is not a switch layer");
    }
    const auto name = [&](float value) -> py::object {
      const Layer *child = switch_child(sw, value);
      return child ? py::object(py::str(child->name)) : py::object(py::none());
    };
    return py::make_tuple(name(float(sw.switch_shown)), name(float(switch_default_child(*current_context().document, sw))));
  });
  m.def("draw_order_pick_target", [](uint64_t hit, const std::vector<uint64_t> &moving) {
    return draw_order_pick_target(*current_context().document, hit, moving);
  });
  m.def("draw_order", []() {
    Document &doc = *current_context().document;
    animation_sync(doc);
    py::list names;
    for (const Layer *l : draw_order_layers(doc, [](const Layer &layer) { return layer.shown(); })) {
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

  /* The report the status bar shows: (count shown so far, type, title, message, hint). */
  m.def("banner_report", [] {
    const BannerReport &r = banner_report();
    static const char *const TYPES[] = {"INFO", "OPERATOR", "WARNING", "ERROR"};
    return py::make_tuple(r.serial, TYPES[int(r.type)], r.title, r.message, r.hint);
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

namespace oanim::python {

bool init(const std::filesystem::path &scripts_dir, Context *C)
{
  g_context = C;
  g_scripts_dir = scripts_dir;

  /* Isolated: ignore PYTHONPATH and user site-packages so the app behaves the same
   * on every machine. Home points at the Python installation the build used. */
  PyConfig config;
  PyConfig_InitIsolatedConfig(&config);
  const std::wstring home = std::filesystem::path(OANIM_PYTHON_HOME).wstring();
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
    py::module_ startup = py::module_::import("oanim._startup");
    return startup.attr("load_startup_scripts")(path_to_utf8(scripts_dir / "startup")).cast<bool>();
  }
  catch (py::error_already_set &error) {
    report(ReportType::Error, "Error starting the oanim module:\n" + format_exception(error));
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

uint64_t python_runs()
{
  return g_python_runs;
}

bool run_file(const std::filesystem::path &path)
{
  g_python_runs++;
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
    py::object console = py::module_::import("oanim._console").attr("get")();
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
    return py::module_::import("oanim._console").attr("get")().attr("version").cast<uint64_t>();
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
    py::list items = py::module_::import("oanim._console").attr("get")().attr("lines");
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
    return py::module_::import("oanim._console").attr("get")().attr("history").cast<std::vector<std::string>>();
  }
  catch (py::error_already_set &error) {
    report(ReportType::Error, "Console error:\n" + format_exception(error));
    return {};
  }
}

void console_push(const std::string &line)
{
  g_python_runs++;
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
  g_python_runs++;
  const std::optional<py::object> result = console_call("run_text", name, source);
  return result && result->cast<bool>();
}

std::string remote_run(const std::string &code)
{
  g_python_runs++;
  try {
    const py::object result = py::module_::import("oanim._remote").attr("run")(code);
    return py::module_::import("json").attr("dumps")(result).cast<std::string>();
  }
  catch (py::error_already_set &error) {
    return py::module_::import("json").attr("dumps")(py::dict(py::arg("error") = std::string(error.what())))
        .cast<std::string>();
  }
}

bool run_string(const std::string &code)
{
  if (!g_initialized) {
    report(ReportType::Error, "Python isn't running, so this can't be done");
    return false;
  }
  g_python_runs++;
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

}  // namespace oanim::python

