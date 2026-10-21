/* Operators that edit the active vector layer's drawing: selection, delete, shapes, curvature,
 * and the start of modal transforms (G, R, S, E). */

#include "ops/edit_ops.hh"

#include <algorithm>
#include <functional>
#include <map>

#include "core/log.hh"
#include "data/bones.hh"
#include "data/detail.hh"
#include "data/document.hh"
#include "ops/operator.hh"
#include "ops/undo.hh"
#include "wm/context.hh"

namespace oanim {

VectorDrawing *editable_drawing(Context &C, Layer **r_layer)
{
  Layer *layer = C.document ? document_active_layer(*C.document) : nullptr;
  /* A reference's drawing can be edited too: its looks always, its points with Edit Points on
   * (edits become its overrides; see anim/reference.hh). */
  if (!layer || layer->is_group() || layer->locked) {
    return nullptr;
  }
  if (r_layer) {
    *r_layer = layer;
  }
  return &layer->drawing;
}

std::vector<Layer *> editable_layers(Context &C)
{
  std::vector<Layer *> layers;
  if (!C.document) {
    return layers;
  }
  Document &doc = *C.document;
  Layer *active = nullptr;
  if (editable_drawing(C, &active)) {
    layers.push_back(active);
  }
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->selected && layer != active && layer->visible && !layer->is_group() && !layer->locked)
    {
      layers.push_back(layer_find(doc, layer->id));
    }
  }
  /* A bone layer being worked in brings the drawings in its shown bone collections. */
  if (const Layer *bones = document_active_layer(doc); bones && bones->is_bone_layer()) {
    for (const Layer *layer : document_layers_flat(doc)) {
      const Layer *owner = layer_armature(doc, layer->id);
      if (owner && owner->id == bones->id && layer_edited_with_bones(doc, *layer) && layer->visible && !layer->is_group() &&
          !layer->locked &&
          std::find(layers.begin(), layers.end(), layer) == layers.end())
      {
        layers.push_back(layer_find(doc, layer->id));
      }
    }
  }
  return layers;
}

static bool editable_poll(Context &C)
{
  return !editable_layers(C).empty();
}

/* Calls f for each editable layer with selected shapes (a point of theirs selected), with which;
 * false when there are none. */
static bool for_selected_shapes(Context &C, const std::function<void(Layer &, const std::vector<bool> &)> &f)
{
  bool any = false;
  for (Layer *layer : editable_layers(C)) {
    if (layer->is_reference()) {
      continue;
    }
    const std::vector<bool> shapes = drawing_selected_shapes(layer->drawing);
    if (std::find(shapes.begin(), shapes.end(), true) == shapes.end()) {
      continue;
    }
    any = true;
    f(*layer, shapes);
  }
  return any;
}

/* Selected points, or handles picked on their own (G, R and S move those too). */
static bool has_selection(const VectorDrawing &drawing)
{
  for (const MeshPoint &p : drawing.points) {
    if (p.selected) {
      return true;
    }
  }
  for (const Curve &curve : drawing.curves) {
    for (const CurvePoint &cp : curve.points) {
      if (cp.select_in || cp.select_out) {
        return true;
      }
    }
  }
  return false;
}

static bool editable_with_selection_poll(Context &C)
{
  for (const Layer *layer : editable_layers(C)) {
    if (has_selection(layer->drawing)) {
      return true;
    }
  }
  return false;
}

void drawing_expand_selection_by_mode(Context &C, VectorDrawing &drawing)
{
  const SelectMode mode = C.wm->tool_settings.select_mode;
  if (mode != SelectMode::Stroke) {
    return;
  }
  const std::vector<bool> was_selected = [&] {
    std::vector<bool> s(drawing.points.size());
    for (size_t i = 0; i < drawing.points.size(); i++) {
      s[i] = drawing.points[i].selected;
    }
    return s;
  }();
  for (const Curve &curve : drawing.curves) {
    bool any = false;
    for (const CurvePoint &cp : curve.points) {
      any |= was_selected[cp.point];
    }
    if (any) {
      for (const CurvePoint &cp : curve.points) {
        drawing.points[cp.point].selected = true;
      }
    }
  }
}

static void vector_operator(const char *idname, const char *name, const char *description, unsigned flag,
                            std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = flag;
  ot.poll = std::move(poll);
  ot.exec = std::move(exec);
  operators().add(std::move(ot));
}

static constexpr unsigned UNDO = OPTYPE_REGISTER | OPTYPE_UNDO;

void vector_operators_register()
{
  /* --- Handles --------------------------------------------------------------- */
  for (const bool align : {true, false}) {
    vector_operator(align ? "mesh.handles_align" : "mesh.handles_unalign", align ? "Align Handles" : "Unalign Handles",
                    align ? "Line up the selected points' handles and keep them in line (with one handle picked, "
                            "that one turns); moving one then turns the other"
                          : "Let the selected points' handles move separately again",
                    UNDO, editable_with_selection_poll, [align](Context &C) {
                      int changed = 0;
                      for (Layer *layer : editable_layers(C)) {
                        changed += drawing_align_selected_handles(layer->drawing, align);
                        drawing_mirror_selected_handles(layer->drawing, C.wm->tool_settings.mirror_x,
                                                        C.wm->tool_settings.mirror_y);
                      }
                      return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                    });
  }

  /* Away from frame 0, Blender's Clear Location (Alt G) and Clear Rotation (Alt R) for points: the
   * selected points, or their handles (or a handle picked on its own), back to the setup pose, where
   * their keys hold no change. Picked bones take the keys first. */
  for (const bool positions : {true, false}) {
    vector_operator(
        positions ? "mesh.clear_location" : "mesh.clear_handles", positions ? "Clear Location" : "Clear Handles",
        positions ? "Put the selected points back where they are in the setup pose (Alt G, away from frame 0)"
                  : "Give the selected points' handles (or a handle picked on its own) back their setup shape (Alt R, "
                    "away from frame 0)",
        UNDO,
        [](Context &C) { return C.document && C.document->frame_current != 0 && editable_with_selection_poll(C); },
        [positions](Context &C) {
          int changed = 0;
          for (Layer *layer : editable_layers(C)) {
            /* A copy's points are its original's: its own changes go with Reset to Original. */
            if (!layer->is_reference()) {
              changed += drawing_clear_selected_pose(layer->drawing, positions, !positions);
            }
          }
          return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
        });
  }

  /* Symmetrize (right-click on points): one side's points copied onto their partners on the other,
   * across the layer's vertical axis; adjustable in the ViewportActionPanel. */
  {
    OperatorType ot;
    ot.idname = "mesh.symmetrize";
    ot.name = "Symmetrize";
    ot.description = "Make the selected points mirror images across the layer's vertical axis, copying one side onto "
                     "the other; points on the axis get even handles";
    ot.flag = UNDO;
    ot.poll = editable_with_selection_poll;
    OpProperty direction;
    direction.name = "direction";
    direction.label = "Direction";
    direction.description = "Which side is copied onto the other";
    direction.items = {{"RIGHT_TO_LEFT", "Right to Left"}, {"LEFT_TO_RIGHT", "Left to Right"}};
    direction.fallback = "RIGHT_TO_LEFT";
    OpProperty threshold;
    threshold.name = "threshold";
    threshold.label = "Threshold";
    threshold.description = "How near the axis a point is to count as on it";
    threshold.kind = OpProperty::Kind::Float;
    threshold.fallback = "0.01";
    threshold.min = 0.0f;
    threshold.max = 10.0f;
    threshold.step = 0.001f;
    OpProperty weights;
    weights.name = "weights";
    weights.label = "Weights";
    weights.description = "Copy the bone weights too, to the other side's bones (_L and _R)";
    weights.kind = OpProperty::Kind::Bool;
    weights.fallback = "0";
    ot.props = {direction, threshold, weights};
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const bool right_to_left = op_arg(C, "direction", "RIGHT_TO_LEFT") != "LEFT_TO_RIGHT";
      const float threshold = op_arg_float(C, "threshold", 0.01f);
      const bool weights = op_arg_bool(C, "weights");
      const bool setup = doc.frame_current == 0;
      std::vector<Layer *> layers;
      for (Layer *layer : editable_layers(C)) {
        if (!layer->is_reference() && has_selection(layer->drawing)) {
          layers.push_back(layer);
        }
      }
      /* Each layer has to pair up before any changes. */
      for (Layer *layer : layers) {
        VectorDrawing trial = layer->drawing;
        const SymmetrizeResult r = drawing_symmetrize(trial, right_to_left, threshold, setup, nullptr);
        if (!r.done) {
          for (MeshPoint &p : layer->drawing.points) {
            p.selected = false;
          }
          for (const int i : r.unmatched) {
            layer->drawing.points[i].selected = true;
          }
          C.undo->mark_changed();
          const std::string side_counts = std::to_string(r.right) + " on the right, " + std::to_string(r.left) +
                                          " on the left";
          report_banner(ReportType::Warning,
                        "Symmetrize: \"" + layer->name + "\" has " + side_counts +
                            " (off the axis); each point needs a partner on the other side",
                        side_counts + ": select the same number on each side (the ones without a partner are "
                                      "selected now)",
                        "Uneven sides");
          return OpStatus::Cancelled;
        }
      }
      for (Layer *layer : layers) {
        std::map<uint64_t, uint64_t> flip;
        if (weights) {
          if (const Layer *armature = layer_armature(doc, layer->id)) {
            for (const Bone &b : armature->bones) {
              const Bone *other = bone_find_by_name(*armature, bone_mirror_name(b.name));
              flip[b.id] = other ? other->id : b.id;
            }
          }
        }
        drawing_symmetrize(layer->drawing, right_to_left, threshold, setup, weights ? &flip : nullptr);
      }
      return layers.empty() ? OpStatus::Cancelled : OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  /* --- Selection ------------------------------------------------------------- */
  vector_operator("mesh.select_all", "Select All", "Select every point in the active layer", UNDO, editable_poll,
                  [](Context &C) {
                    for (Layer *layer : editable_layers(C)) {
                      for (MeshPoint &p : layer->drawing.points) {
                        p.selected = !p.detail.hidden;
                      }
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.select_none", "Deselect All", "Deselect every point in the active layer", UNDO,
                  editable_poll, [](Context &C) {
                    for (Layer *layer : editable_layers(C)) {
                      for (MeshPoint &p : layer->drawing.points) {
                        p.selected = false;
                      }
                      drawing_deselect_handles(layer->drawing);
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.select_invert", "Invert Selection", "Select the unselected points and deselect the rest",
                  UNDO, editable_poll, [](Context &C) {
                    for (Layer *layer : editable_layers(C)) {
                      for (MeshPoint &p : layer->drawing.points) {
                        p.selected = !p.selected && !p.detail.hidden;
                      }
                    }
                    return OpStatus::Finished;
                  });

  for (const bool deselect : {false, true}) {
    vector_operator(
        deselect ? "mesh.deselect_linked_pick" : "mesh.select_linked_pick",
        deselect ? "Deselect Linked" : "Select Linked",
        deselect ? "Deselect the shape or curve under the mouse" : "Select the whole shape or curve under the mouse",
        UNDO,
        [](Context &C) { return editable_poll(C) && C.wm->mouse_in_viewport; },
        [deselect](Context &C) {
          /* The first editable layer with a shape or curve under the mouse, active one first. */
          for (Layer *layer : editable_layers(C)) {
            VectorDrawing &drawing = layer->drawing;
            const Transform2D to_layer = layer_world_transform(*C.document, layer->id).inverted();
            const float2 mouse = to_layer.apply(C.wm->mouse_document);
            const float radius = 0.03f * to_layer.scale();
            std::vector<int> points;
            if (const int s = drawing_pick_shape(drawing, mouse, radius); s >= 0) {
              for (const ShapeEdge &e : drawing.shapes[s].edges) {
                int a, b;
                curve_segment_points(drawing.curves[e.curve], e.segment, &a, &b);
                points.push_back(a);
                points.push_back(b);
              }
            }
            else {
              ShapeEdge edge;
              float t;
              if (!drawing_pick_segment(drawing, mouse, radius, &edge, &t)) {
                continue;
              }
              for (const CurvePoint &cp : drawing.curves[edge.curve].points) {
                points.push_back(cp.point);
              }
            }
            for (int p : points) {
              drawing.points[p].selected = !deselect;
            }
            return OpStatus::Finished;
          }
          return OpStatus::Cancelled;
        });
  }

  const auto select_mode_op = [](const char *idname, const char *name, SelectMode mode) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = "Change what clicking selects: points, whole strokes, or segments between junctions";
    ot.exec = [mode](Context &C) {
      C.wm->tool_settings.select_mode = mode;
      for (Layer *layer : editable_layers(C)) {
        drawing_expand_selection_by_mode(C, layer->drawing);
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  };
  select_mode_op("mesh.select_mode_point", "Point Selection Mode", SelectMode::Point);
  select_mode_op("mesh.select_mode_edge", "Edge Selection Mode", SelectMode::Edge);
  select_mode_op("mesh.select_mode_stroke", "Stroke Selection Mode", SelectMode::Stroke);

  /* --- Editing ----------------------------------------------------------------- */
  vector_operator("mesh.delete", "Delete Points",
                  "Delete the selected points and the curves left too short; in Edges mode, delete the selected "
                  "edges and keep their points",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    const bool edges = C.wm->tool_settings.select_mode == SelectMode::Edge;
                    for (Layer *layer : editable_layers(C)) {
                      VectorDrawing &drawing = layer->drawing;
                      if (edges) {
                        drawing_delete_edges(drawing, drawing_selected_edges(drawing));
                        continue;
                      }
                      std::vector<bool> remove(drawing.points.size());
                      for (size_t i = 0; i < drawing.points.size(); i++) {
                        remove[i] = drawing.points[i].selected;
                      }
                      drawing_remove_points(drawing, remove);
                    }
                    return OpStatus::Finished;
                  });

  /* What the X menu offers, as Blender's Delete menu: each whatever the selection mode is. */
  vector_operator("mesh.delete_points", "Delete Points", "Delete the selected points and the curves left too short",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    for (Layer *layer : editable_layers(C)) {
                      std::vector<bool> remove(layer->drawing.points.size());
                      for (size_t i = 0; i < layer->drawing.points.size(); i++) {
                        remove[i] = layer->drawing.points[i].selected;
                      }
                      drawing_remove_points(layer->drawing, remove);
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.delete_edges", "Delete Edges",
                  "Delete the segments between selected points, keeping the points: curves split there",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    int deleted = 0;
                    for (Layer *layer : editable_layers(C)) {
                      deleted += drawing_delete_edges(layer->drawing, drawing_selected_edges(layer->drawing));
                    }
                    return deleted ? OpStatus::Finished : OpStatus::Cancelled;
                  });
  vector_operator("mesh.dissolve_points", "Dissolve Points",
                  "Remove the selected points, joining the curve across each gap so lines and shapes stay whole",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    for (Layer *layer : editable_layers(C)) {
                      VectorDrawing &drawing = layer->drawing;
                      /* One at a time: each dissolve renumbers the points (it also clears out
                       * points nothing uses any more). */
                      for (;;) {
                        int last = -1;
                        for (int i = int(drawing.points.size()) - 1; i >= 0 && last < 0; i--) {
                          last = drawing.points[i].selected ? i : -1;
                        }
                        const size_t before = drawing.points.size();
                        if (last < 0) {
                          break;
                        }
                        drawing_dissolve_point(drawing, last);
                        if (drawing.points.size() >= before) {
                          drawing.points[last].selected = false;
                        }
                      }
                    }
                    return OpStatus::Finished;
                  });
  {
    /* X opens the Delete menu at the mouse, as in Blender. */
    OperatorType ot;
    ot.idname = "mesh.delete_menu";
    ot.name = "Delete";
    ot.description = "Choose what to delete: the selected points, the edges between them, or dissolve the points";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = editable_with_selection_poll;
    ot.exec = [](Context &C) {
      C.wm->menu_request = "mesh_delete";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  vector_operator("mesh.select_point_layers", "Select Layers of Selected Points",
                  "Make the layer whose points (or handles) are selected the active layer, and pick the others "
                  "that have some, keeping the points selected: to edit them on their own layer",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    Document &doc = *C.document;
                    std::vector<uint64_t> owners;
                    for (Layer *layer : editable_layers(C)) {
                      if (has_selection(layer->drawing)) {
                        owners.push_back(layer->id);
                      }
                    }
                    if (owners.empty()) {
                      return OpStatus::Cancelled;
                    }
                    for (const Layer *l : document_layers_flat(doc)) {
                      layer_find(doc, l->id)->selected = false;
                    }
                    /* The first in the Layers editor's order is active; the others are picked. */
                    bool first = true;
                    for (const Layer *l : document_layers_flat(doc)) {
                      if (std::find(owners.begin(), owners.end(), l->id) == owners.end()) {
                        continue;
                      }
                      if (first) {
                        doc.active_layer = l->id;
                        first = false;
                      }
                      else {
                        layer_find(doc, l->id)->selected = true;
                      }
                    }
                    return OpStatus::Finished;
                  });

  for (const bool peak : {true, false}) {
    vector_operator(peak ? "mesh.peak" : "mesh.smooth", peak ? "Peak" : "Smooth",
                    peak ? "Make the selected points sharp corners" : "Make the selected points smooth curves", UNDO,
                    editable_with_selection_poll, [peak](Context &C) {
                      for (Layer *layer : editable_layers(C)) {
                        VectorDrawing &drawing = layer->drawing;
                        /* Peak: handles straight toward the neighbours; Smooth: smooth, aligned handles. Once. */
                        drawing_set_selected_handles_shape(drawing, peak);
                        drawing_mirror_selected_handles(drawing, C.wm->tool_settings.mirror_x,
                                                        C.wm->tool_settings.mirror_y);
                      }
                      return OpStatus::Finished;
                    });
  }

  vector_operator("mesh.delete_shape", "Delete Shape", "Delete the fills and outlines that use selected points",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    bool deleted = false;
                    for (Layer *layer : editable_layers(C)) {
                      VectorDrawing &drawing = layer->drawing;
                      const size_t before = drawing.shapes.size();
                      std::erase_if(drawing.shapes, [&](const Shape &shape) {
                        for (const ShapeEdge &e : shape.edges) {
                          int a, b;
                          curve_segment_points(drawing.curves[e.curve], e.segment, &a, &b);
                          if (drawing.points[a].selected && drawing.points[b].selected) {
                            return true;
                          }
                        }
                        return false;
                      });
                      deleted |= drawing.shapes.size() != before;
                    }
                    return deleted ? OpStatus::Finished : OpStatus::Cancelled;
                  });

  /* --- Modal transforms -----------------------------------------------------------
   * These only ask the viewport to start; it makes the undo step when the move ends. */
  const auto transform_op = [](const char *idname, const char *name, const char *description, char kind) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    /* Points (and handles) of the edited layers, or the bone layer's selected bones. */
    ot.poll = [](Context &C) {
      if (!C.wm->mouse_in_viewport) {
        return false;
      }
      const Layer *active = C.document ? document_active_layer(*C.document) : nullptr;
      if (active && active->is_bone_layer()) {
        /* A bone picked by its body or by either ball, or points of its layers marked Editable in
         * Bone Group. */
        return armature_any_picked(*active) || editable_with_selection_poll(C);
      }
      return editable_with_selection_poll(C);
    };
    ot.exec = [kind](Context &C) {
      C.wm->transform_request = kind;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  };
  /* --- Tools ------------------------------------------------------------------------ */
  const auto tool_op = [](const char *idname, const char *name, Tool tool) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = "Switch the viewport tool";
    if (tool_setup_only(tool)) {
      ot.poll = [](Context &C) { return C.document && C.document->frame_current == 0; };
    }
    ot.exec = [tool](Context &C) {
      C.wm->tool_settings.tool = tool;
      C.wm->tool_settings.tool_switched_for_posing = false;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  };
  {
    /* K: Split Curve for one split, then back to the tool before. */
    OperatorType ot;
    ot.idname = "wm.tool_split_curve_temporary";
    ot.name = "Split Curve (Once)";
    ot.description = "Split a line once with the Split Curve tool, then go back to the current tool";
    ot.poll = [](Context &C) { return C.document && C.document->frame_current == 0; };
    ot.exec = [](Context &C) {
      ToolSettings &ts = C.wm->tool_settings;
      if (!ts.tool_temporary) {
        ts.tool_return = ts.tool;
      }
      ts.tool = Tool::SplitCurve;
      ts.tool_temporary = ts.tool_return != Tool::SplitCurve;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  tool_op("wm.tool_select", "Select Box Tool", Tool::Select);
  tool_op("wm.tool_cursor", "Cursor Tool", Tool::Cursor);
  {
    /* oanim.ops.wm.tool_draw(mode="PEN"): the Draw tool, optionally picking freehand or the pen. */
    OperatorType ot;
    ot.idname = "wm.tool_draw";
    ot.name = "Draw Tool";
    ot.description = "Switch to the Draw tool: freehand strokes, or the pen (mode=\"PEN\")";
    ot.poll = [](Context &C) { return C.document && C.document->frame_current == 0; };
    ot.exec = [](Context &C) {
      const std::string mode = op_arg(C, "mode");
      if (mode == "PEN") {
        C.wm->tool_settings.draw_mode = DrawMode::Pen;
      }
      else if (mode == "FREEHAND") {
        C.wm->tool_settings.draw_mode = DrawMode::Freehand;
      }
      else if (!mode.empty()) {
        report(ReportType::Error, "Unknown draw mode '" + mode + "': use FREEHAND or PEN");
        return OpStatus::Cancelled;
      }
      C.wm->tool_settings.tool = Tool::Draw;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  tool_op("wm.tool_split_curve", "Split Curve Tool", Tool::SplitCurve);
  tool_op("wm.tool_weight_select", "Weight Select Tool", Tool::WeightSelect);
  tool_op("wm.tool_weight_paint", "Weight Paint Tool", Tool::WeightPaint);
  tool_op("wm.tool_transform_bones", "Transform Bones Tool", Tool::TransformBones);
  tool_op("wm.tool_transform_points", "Transform Points Tool", Tool::TransformPoints);
  {
    /* oanim.ops.wm.tool_shape(shape="CIRCLE"): the Shape tool, optionally picking the shape. */
    OperatorType ot;
    ot.idname = "wm.tool_shape";
    ot.name = "Shape Tool";
    ot.description = "Switch to the Shape tool: box or circle";
    ot.poll = [](Context &C) { return C.document && C.document->frame_current == 0; };
    ot.exec = [](Context &C) {
      const std::string shape = op_arg(C, "shape");
      if (!shape.empty() && !primitive_from_identifier(shape, &C.wm->tool_settings.shape)) {
        report(ReportType::Error, "Unknown shape '" + shape + "': use BOX or CIRCLE");
        return OpStatus::Cancelled;
      }
      C.wm->tool_settings.tool = Tool::Shape;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* Kept for scripts: the pen is now a mode of the Draw tool. */
    OperatorType ot;
    ot.idname = "wm.tool_pen";
    ot.name = "Pen Tool";
    ot.description = "Switch to the Draw tool's pen: click to add points along a curve";
    ot.exec = [](Context &C) {
      C.wm->tool_settings.tool = Tool::Draw;
      C.wm->tool_settings.draw_mode = DrawMode::Pen;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  transform_op("transform.translate", "Move", "Move the selected points with the mouse", 'G');
  transform_op("transform.rotate", "Rotate", "Rotate the selected points with the mouse", 'R');
  transform_op("transform.resize", "Scale", "Scale the selected points with the mouse", 'S');
  transform_op("mesh.extrude", "Extrude", "Add a new point beyond each selected end point and move it", 'E');
  /* Proportional editing: O, Shift O and Alt O, as in Blender. Only for points: they need an
   * editable layer with points. */
  const auto proportional_op = [](const char *idname, const char *name, const char *description,
                                  void (*change)(ToolSettings &ts)) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    ot.poll = [](Context &C) {
      if (!C.document || !C.wm) {
        return false;
      }
      for (const Layer *layer : editable_layers(C)) {
        if (!layer->drawing.points.empty()) {
          return true;
        }
      }
      return false;
    };
    ot.exec = [change](Context &C) {
      change(C.wm->tool_settings);
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  };
  proportional_op("transform.proportional_toggle", "Proportional Editing",
                  "Turn proportional editing on or off: moving points drags the points around them too (O)",
                  [](ToolSettings &ts) { ts.proportional = !ts.proportional; });
  proportional_op("transform.proportional_falloff_cycle", "Proportional Falloff",
                  "Switch proportional editing to the next falloff (Shift O)", [](ToolSettings &ts) {
                    ts.proportional_falloff =
                        ProportionalFalloff((int(ts.proportional_falloff) + 1) % PROPORTIONAL_FALLOFF_COUNT);
                    report(ReportType::Info,
                           std::string("Proportional falloff: ") + proportional_falloff_name(ts.proportional_falloff));
                  });
  proportional_op("transform.proportional_connected_toggle", "Proportional Connected Only",
                  "Proportional editing only along the curves, or straight across (Alt O)",
                  [](ToolSettings &ts) { ts.proportional_connected = !ts.proportional_connected; });

  /* Blender's Arrange (Page Up/Down) for shapes, Moho's Raise and Lower Shape. */
  const auto arrange_op = [](const char *idname, const char *name, const char *description, ShapeArrange arrange) {
    vector_operator(idname, name, description, UNDO, editable_with_selection_poll, [arrange](Context &C) {
      /* Shapes move within their own layer's stacking order. */
      bool any_selected = false, moved = false;
      for (Layer *layer : editable_layers(C)) {
        const std::vector<bool> selected = drawing_selected_shapes(layer->drawing);
        if (std::find(selected.begin(), selected.end(), true) != selected.end()) {
          any_selected = true;
          moved |= drawing_arrange_shapes(layer->drawing, arrange);
        }
      }
      if (!any_selected) {
        report(ReportType::Warning, "Select a point of a shape to arrange it");
        return OpStatus::Cancelled;
      }
      /* Already at the top or bottom: nothing to undo. */
      return moved ? OpStatus::Finished : OpStatus::Cancelled;
    });
  };
  arrange_op("mesh.shape_raise", "Bring Forward", "Move the selected shapes one step up the stacking order",
             ShapeArrange::Forward);
  arrange_op("mesh.shape_lower", "Send Backward", "Move the selected shapes one step down the stacking order",
             ShapeArrange::Backward);
  arrange_op("mesh.shape_raise_to_front", "Bring to Front", "Move the selected shapes above all others",
             ShapeArrange::ToFront);
  arrange_op("mesh.shape_lower_to_back", "Send to Back", "Move the selected shapes below all others",
             ShapeArrange::ToBack);

  /* --- Detail levels (data/detail.hh) ------------------------------------------------ */
  vector_operator(
      "mesh.detail_level_set", "Detail Level",
      "Set the selected shapes' detail level (level: 0 is their own points), or step it (delta: 1 or -1): a "
      "shape's curves show their points up to it and the knife adds detail points at it; one past a shape's "
      "highest level is an empty level to add detail at",
      UNDO, editable_poll, [](Context &C) {
        const int level = op_arg_int(C, "level", -1), delta = op_arg_int(C, "delta", 0);
        bool changed = false;
        const bool any = for_selected_shapes(C, [&](Layer &layer, const std::vector<bool> &shapes) {
          VectorDrawing &drawing = layer.drawing;
          for (size_t s = 0; s < drawing.shapes.size(); s++) {
            if (!shapes[s]) {
              continue;
            }
            Shape &shape = drawing.shapes[s];
            const int to = std::clamp(level >= 0 ? level : shape.detail_level + delta, 0,
                                      shape_detail_level_limit(drawing, shape));
            changed |= to != shape.detail_level;
            shape.detail_level = to;
          }
          drawing_detail_update_shown(drawing);
        });
        if (!any) {
          report(ReportType::Warning, "Select a shape first");
          return OpStatus::Cancelled;
        }
        return changed ? OpStatus::Finished : OpStatus::Cancelled;
      });
  /* Its shortcuts (Ctrl 0 to Ctrl 4 set it, Alt Page Up and Page Down step it), as Blender's Ctrl 1
   * to Ctrl 5 and Page Up and Down set and step multiresolution levels. */
  for (int level = 0; level <= DETAIL_LEVEL_MAX; level++) {
    static std::string names[DETAIL_LEVEL_MAX + 1][2];
    names[level][0] = "mesh.detail_level_" + std::to_string(level);
    names[level][1] = "Detail Level " + std::to_string(level);
    vector_operator(names[level][0].c_str(), names[level][1].c_str(), "Set the selected shapes' detail level",
                    OPTYPE_REGISTER, editable_poll, [level](Context &C) {
                      return operator_call(C, "mesh.detail_level_set", {{"level", std::to_string(level)}});
                    });
  }
  for (const int delta : {1, -1}) {
    vector_operator(delta > 0 ? "mesh.detail_level_up" : "mesh.detail_level_down",
                    delta > 0 ? "Detail Level Up" : "Detail Level Down",
                    delta > 0 ? "Edit the selected shapes one detail level higher"
                              : "Edit the selected shapes one detail level lower",
                    OPTYPE_REGISTER, editable_poll, [delta](Context &C) {
                      return operator_call(C, "mesh.detail_level_set", {{"delta", std::to_string(delta)}});
                    });
  }
  vector_operator("mesh.detail_delete_higher", "Delete Higher",
                  "Remove the selected shapes' detail above the level they are edited at", UNDO, editable_poll,
                  [](Context &C) {
                    int removed = 0;
                    const bool any = for_selected_shapes(C, [&](Layer &layer, const std::vector<bool> &shapes) {
                      removed += drawing_detail_delete_higher(layer.drawing, shapes);
                    });
                    if (!any) {
                      report(ReportType::Warning, "Select a shape first");
                      return OpStatus::Cancelled;
                    }
                    if (removed == 0) {
                      report(ReportType::Info, "No detail above this level");
                      return OpStatus::Cancelled;
                    }
                    report(ReportType::Info, "Removed " + std::to_string(removed) + " detail points");
                    return OpStatus::Finished;
                  });
  /* Apply Detail and Rebuild Levels reshape the setup: frame 0 only. */
  const auto setup_frame = [](Context &C, const char *what) {
    if (C.document->frame_current == 0) {
      return true;
    }
    report(ReportType::Warning, std::string(what) + " works on the setup frame (0)");
    return false;
  };
  vector_operator("mesh.detail_apply", "Apply Detail",
                  "Make the selected shapes' detail points ordinary points, as they show (setup frame)", UNDO,
                  editable_poll, [setup_frame](Context &C) {
                    if (!setup_frame(C, "Apply Detail")) {
                      return OpStatus::Cancelled;
                    }
                    int changed = 0;
                    bool dropped = false;
                    const bool any = for_selected_shapes(C, [&](Layer &layer, const std::vector<bool> &shapes) {
                      bool keys = false;
                      changed += drawing_detail_apply(layer.drawing, shapes, &keys);
                      dropped |= keys;
                    });
                    if (!any) {
                      report(ReportType::Warning, "Select a shape first");
                      return OpStatus::Cancelled;
                    }
                    if (changed == 0) {
                      report(ReportType::Info, "No detail to apply");
                      return OpStatus::Cancelled;
                    }
                    report(dropped ? ReportType::Warning : ReportType::Info,
                           "Applied " + std::to_string(changed) + " detail points" +
                               (dropped ? " (their keys were removed)" : ""));
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.detail_rebuild", "Rebuild Levels",
                  "Keep the points that matter for the selected shapes' outline (within tolerance) as their own and "
                  "make the rest level 1 detail, as they show (setup frame)",
                  UNDO, editable_poll, [setup_frame](Context &C) {
                    if (!setup_frame(C, "Rebuild Levels")) {
                      return OpStatus::Cancelled;
                    }
                    const float tolerance = std::max(op_arg_float(C, "tolerance", 0.02f), 0.0f);
                    int made = 0;
                    bool dropped = false;
                    const bool any = for_selected_shapes(C, [&](Layer &layer, const std::vector<bool> &shapes) {
                      bool keys = false;
                      made += drawing_detail_rebuild(layer.drawing, shapes, tolerance, &keys);
                      dropped |= keys;
                    });
                    if (!any) {
                      report(ReportType::Warning, "Select a shape first");
                      return OpStatus::Cancelled;
                    }
                    report(dropped ? ReportType::Warning : ReportType::Info,
                           std::to_string(made) + " points became detail" +
                               (dropped ? " (their keys were removed)" : ""));
                    return made > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                  });

  /* Blender's Duplicate (Shift D): copy, then move the copy. Cancelling the move leaves the
   * copy where it was made, as in Blender. */
  vector_operator("mesh.duplicate_move", "Duplicate", "Copy the selected points, curves and shapes, then move the copy",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    bool copied = false;
                    for (Layer *layer : editable_layers(C)) {
                      if (has_selection(layer->drawing)) {
                        copied |= drawing_duplicate_selected(layer->drawing);
                      }
                    }
                    if (!copied) {
                      report(ReportType::Warning, "Select at least two connected points to duplicate");
                      return OpStatus::Cancelled;
                    }
                    if (C.wm->mouse_in_viewport) {
                      C.wm->transform_request = 'G';
                    }
                    return OpStatus::Finished;
                  });
}

}  // namespace oanim
