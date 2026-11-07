/* Operators that edit the active vector layer's drawing: selection, delete, shapes, curvature,
 * and the start of modal transforms (G, R, S, E). */

#include "anim/animation.hh"
#include "anim/bone_family.hh"
#include "anim/deform.hh"
#include "ops/edit_ops.hh"

#include <algorithm>
#include <functional>
#include <map>
#include <unordered_set>

#include "core/log.hh"
#include "data/layer_mirror.hh"
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
  if (!layer || !layer->has_drawing() || layer->locked) {
    return nullptr;
  }
  if (r_layer) {
    *r_layer = layer;
  }
  return &layer->drawing;
}

std::vector<Layer *> editable_layers(Context &C)
{
  if (!C.document) {
    return {};
  }
  const bool weights = C.wm && (C.wm->tool_settings.tool == Tool::WeightPaint || C.wm->tool_settings.tool == Tool::WeightSelect);
  return weights ? document_weight_layers(*C.document) : document_editable_layers(*C.document);
}

std::vector<Layer *> document_weight_layers(Document &doc)
{
  std::vector<Layer *> layers;
  Layer *active = document_active_layer(doc);
  if (!active || active->locked) {
    return layers;
  }
  if (active->has_drawing()) {
    layers.push_back(active);
    return layers;
  }
  /* A bone layer's Edit Descendants is off by default, for its bones' sake: a weight tool on it means its drawings. */
  if (!active->is_group() || (!active->edit_descendants && !active->is_bone_layer()) || active->parent_bone != 0 ||
      layer_binding_group(doc, active->id) || (!active->is_bone_layer() && !layer_armature(doc, active->id)))
  {
    return layers;
  }
  const auto take = [&](auto &&self, Layer &group) -> void {
    for (Layer &child : group.children) {
      if (!child.shown() || child.is_bone_layer() || child.parent_bone != 0) {
        continue;
      }
      if (child.type == LayerType::Vector && !child.locked && !(child.is_reference() && child.reference.match_original)) {
        layers.push_back(&child);
      }
      self(self, child);
    }
  };
  take(take, *active);
  return layers;
}

std::shared_ptr<const std::set<uint64_t>> editable_layer_ids(Document &doc)
{
  auto ids = std::make_shared<std::set<uint64_t>>();
  for (const Layer *l : document_editable_layers(doc)) {
    ids->insert(l->id);
  }
  return ids;
}

std::vector<Layer *> document_editable_layers(Document &doc)
{
  std::vector<Layer *> layers;
  /* Each once. A set, as a group or bone layer can bring thousands. */
  std::unordered_set<const Layer *> taken;
  const auto add = [&](Layer *layer) {
    if (taken.insert(layer).second) {
      layers.push_back(layer);
    }
  };
  /* The active layer, when its drawing can be edited (a reference's too; see editable_drawing). */
  Layer *active = document_active_layer(doc);
  if (active && (!active->has_drawing() || active->locked)) {
    active = nullptr;
  }
  if (active) {
    add(active);
  }
  for (const Layer *layer : document_layers_flat(doc)) {
    if (layer->selected && layer != active && layer->shown() && layer->has_drawing() && !layer->locked)
    {
      add(layer_find(doc, layer->id));
    }
  }
  /* A group picked or active with Edit Descendants brings every drawing inside it, as if each were
   * picked (a hidden or locked one, or one inside a hidden group, stays out). */
  const auto take_descendants = [&](auto &&self, Layer &group) -> void {
    for (Layer &child : group.children) {
      if (!child.shown()) {
        continue;
      }
      if (child.has_drawing() && !child.locked) {
        add(&child);
      }
      self(self, child);
    }
  };
  for (const Layer *flat : document_layers_flat(doc)) {
    if ((flat->selected || flat->id == doc.active_layer) && flat->is_group() && flat->edit_descendants &&
        flat->shown())
    {
      take_descendants(take_descendants, *layer_find(doc, flat->id));
    }
  }
  /* A bone layer being worked in brings the drawings in its shown bone collections, and those of the sub bone groups
   * shown inside it in theirs, as their bones are worked with it; a bone layer inside that isn't one keeps its own.
   * Walked from the bone layer itself, as asking each layer of the document which bone layer holds it searches the
   * whole tree each time (the toolbar asks every frame). */
  if (Layer *bones = document_active_layer(doc); bones && bones->is_bone_layer()) {
    const auto take = [&](auto &&self, Layer &parent, const Layer &rig) -> void {
      for (Layer &layer : parent.children) {
        if (layer.edit_with_bones && layer.type == LayerType::Vector && bone_collections_show(rig, layer.collections) &&
            layer.shown() && layer.has_drawing() && !layer.locked)
        {
          add(&layer);
        }
        if (!layer.is_bone_layer()) {
          self(self, layer, rig);
        }
        else if (layer.sub_bone_group && layer.shown()) {
          self(self, layer, layer);
        }
      }
    };
    take(take, *bones, *bones);
  }
  return layers;
}

MirrorSelection mirror_selection_begin(Context &C)
{
  MirrorSelection m;
  if (!C.document || !C.wm || !C.wm->tool_settings.mirror_x) {
    return m;
  }
  Document &doc = *C.document;
  std::vector<uint64_t> ids;
  for (Layer *layer : editable_layers(C)) {
    ids.push_back(layer->id);
  }
  const auto add = [&](Layer &layer, int point) {
    MeshPoint &p = layer.drawing.points[point];
    if (!p.selected && !p.detail.hidden) {
      p.selected = true;
      m.added[layer.id].push_back(p.id);
    }
  };
  for (const uint64_t id : ids) {
    Layer *layer = layer_find(doc, id);
    if (!layer || layer->type != LayerType::Vector) {
      continue;
    }
    drawing_ensure_ids(layer->drawing);
    const std::vector<int> selected = drawing_selected_points(layer->drawing);
    if (selected.empty()) {
      continue;
    }
    Layer *partner = layer_mirror_partner(doc, id);
    if (!partner) {
      for (const int i : selected) {
        const int image = drawing_find_mirror_point(layer->drawing, i, true, false);
        if (image >= 0) {
          add(*layer, image);
        }
      }
      continue;
    }
    if (!partner->shown() || partner->locked || partner->type != LayerType::Vector ||
        (partner->is_reference() && !partner->reference.edit_points))
    {
      continue;
    }
    drawing_ensure_ids(partner->drawing);
    bool any = false;
    for (const int image : layer_mirror_images(doc, *layer, *partner, selected)) {
      if (image >= 0) {
        add(*partner, image);
        any = true;
      }
    }
    /* Picked, it is edited along with this one. */
    if (any && !partner->selected && std::find(ids.begin(), ids.end(), partner->id) == ids.end()) {
      partner->selected = true;
      m.picked.push_back(partner->id);
    }
  }
  return m;
}

void mirror_selection_end(Context &C, const MirrorSelection &mirror)
{
  if (!C.document) {
    return;
  }
  for (const auto &[id, points] : mirror.added) {
    Layer *layer = layer_find(*C.document, id);
    if (!layer) {
      continue;
    }
    /* Copies (Duplicate) still carrying their original's id get their own first, so they keep their
     * selection. */
    drawing_ensure_ids(layer->drawing);
    for (MeshPoint &p : layer->drawing.points) {
      if (p.selected && std::find(points.begin(), points.end(), p.id) != points.end()) {
        p.selected = false;
      }
    }
  }
  for (const uint64_t id : mirror.picked) {
    if (Layer *layer = layer_find(*C.document, id)) {
      layer->selected = false;
    }
  }
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
                            std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec,
                            std::vector<OpProperty> props = {})
{
  OperatorType ot;
  ot.props = std::move(props);
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = flag;
  ot.poll = std::move(poll);
  ot.exec = std::move(exec);
  operators().add(std::move(ot));
}

static constexpr unsigned UNDO = OPTYPE_REGISTER | OPTYPE_UNDO;
/* Done on the selected points' mirror images too while X mirror editing is on. */
static constexpr unsigned MIRRORED = UNDO | OPTYPE_MIRROR;

void vector_operators_register()
{
  /* --- Handles --------------------------------------------------------------- */
  for (const bool align : {true, false}) {
    vector_operator(align ? "mesh.handles_align" : "mesh.handles_unalign", align ? "Align Handles" : "Unalign Handles",
                    align ? "Line up the selected points' handles and keep them in line (with one handle picked, "
                            "that one turns; a picked point turns both, halfway each); moving one then turns the other"
                          : "Let the selected points' handles move separately again",
                    UNDO, editable_with_selection_poll, [align](Context &C) {
                      int changed = 0;
                      const ToolSettings &ts = C.wm->tool_settings;
                      std::vector<uint64_t> ids;
                      for (const Layer *layer : editable_layers(C)) {
                        ids.push_back(layer->id);
                      }
                      const PartnerMirror partners = layer_mirror_partners_begin(*C.document, ids, ts.mirror_x);
                      std::vector<std::pair<uint64_t, std::vector<std::tuple<int, int, AlignFollow>>>> aligned;
                      for (Layer *layer : editable_layers(C)) {
                        aligned.emplace_back(layer->id, std::vector<std::tuple<int, int, AlignFollow>>());
                        changed += drawing_align_selected_handles(layer->drawing, align, &aligned.back().second);
                        drawing_mirror_selected_handles(
                            layer->drawing, layer_mirror_x_self(*C.document, layer->id, ts.mirror_x), ts.mirror_y);
                      }
                      layer_mirror_partners_end(*C.document, partners);
                      if (align) {
                        /* Kept in line on every frame: keys (an action's too) that turn the two apart would still
                         * turn one by itself. The line made here is recorded first, then the keys agree with it. */
                        animation_record(*C.document);
                        bool rekeyed = false;
                        for (auto &[id, points] : aligned) {
                          Layer *layer = layer_find(*C.document, id);
                          if (!layer || layer->is_reference()) {
                            continue;
                          }
                          VectorDrawing &d = layer->drawing;
                          for (const auto &[c, i, follow] : points) {
                            CurvePoint &cp = d.curves[size_t(c)].points[size_t(i)];
                            if (d.points[size_t(cp.point)].detail.level == 0) {
                              rekeyed |= curve_point_align_keys(cp, follow);
                            }
                          }
                        }
                        if (rekeyed) {
                          animation_evaluate(*C.document, C.document->frame_current);
                        }
                      }
                      return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                    });
  }

  /* Away from frame 0, Blender's Clear Location (Alt G) and Clear Rotation (Alt R) for points: the
   * selected points, or their handles (or a handle picked on its own), back to where the setup pose
   * and the action bone actions put them, their keys holding no change of their own. Picked bones take the
   * keys first. */
  for (const bool positions : {true, false}) {
    vector_operator(
        positions ? "mesh.clear_location" : "mesh.clear_handles", positions ? "Clear Location" : "Clear Handles",
        positions ? "Take the selected points' own change on this frame away: back to the setup pose, as action bone "
                    "actions move it (Alt G, away from frame 0)"
                  : "Take the selected points' handles' own change on this frame away (or a handle picked on its "
                    "own): back to their setup shape, as action bone actions move it (Alt R, away from frame 0)",
        MIRRORED,
        [](Context &C) { return C.document && C.document->frame_current != 0 && editable_with_selection_poll(C); },
        [positions](Context &C) {
          /* Amount: part of the way back; X and Y (Clear Location): only along those. */
          const float amount = std::clamp(op_arg_float(C, "amount", 1.0f), 0.0f, 1.0f);
          const bool along_x = op_arg(C, "x", "1") == "1", along_y = op_arg(C, "y", "1") == "1";
          const auto blend = [&](float2 from, float2 to, bool axes) {
            return float2(!axes || along_x ? from.x + (to.x - from.x) * amount : from.x,
                          !axes || along_y ? from.y + (to.y - from.y) * amount : from.y);
          };
          int changed = 0;
          for (Layer *layer : editable_layers(C)) {
            /* A copy's points are its original's: its own changes go with Reset to Original. */
            if (!layer->is_reference()) {
              const VectorDrawing was = layer->drawing;
              changed += drawing_clear_selected_pose(layer->drawing, positions, !positions);
              VectorDrawing &d = layer->drawing;
              for (size_t i = 0; i < d.points.size() && i < was.points.size(); i++) {
                d.points[i].pos = blend(was.points[i].pos, d.points[i].pos, true);
              }
              for (size_t c = 0; c < d.curves.size() && c < was.curves.size(); c++) {
                for (size_t i = 0; i < d.curves[c].points.size() && i < was.curves[c].points.size(); i++) {
                  CurvePoint &cp = d.curves[c].points[i];
                  const CurvePoint &old = was.curves[c].points[i];
                  cp.handle_in = blend(old.handle_in, cp.handle_in, false);
                  cp.handle_out = blend(old.handle_out, cp.handle_out, false);
                  /* One handle of an aligned point cleared by itself: the other turns to stay in line, as a handle
                   * dragged by itself turns it (curve_point_move_handle). */
                  const bool point = d.points[size_t(cp.point)].selected;
                  if (!positions && cp.aligned && !point && cp.select_in != cp.select_out &&
                      d.points[size_t(cp.point)].detail.level == 0)
                  {
                    const float2 &moved = cp.select_in ? cp.handle_in : cp.handle_out;
                    float2 &other = cp.select_in ? cp.handle_out : cp.handle_in;
                    if (moved != (cp.select_in ? old.handle_in : old.handle_out) && length(moved) > 1e-9f) {
                      other = -normalize(moved) * length(other);
                    }
                  }
                }
              }
            }
          }
          return changed > 0 ? OpStatus::Finished : OpStatus::Cancelled;
        },
        positions ? std::vector<OpProperty>{op_float_property("amount", "Amount",
                                                              "How much of the change from the setup pose goes: 1 all "
                                                              "of it",
                                                              "1", 0.0f, 1.0f, 0.01f),
                                            op_bool_property("x", "X", "Clear across (x)", "1"),
                                            op_bool_property("y", "Y", "Clear up (y)", "1")}
                  : std::vector<OpProperty>{op_float_property("amount", "Amount",
                                                              "How much of the change from the setup shape goes: 1 "
                                                              "all of it",
                                                              "1", 0.0f, 1.0f, 0.01f)});
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
    threshold.description = "How near the axis a point is to count as on it, unless its partner's mirror image is as "
                            "near it: then the two stay a pair";
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
    const auto matching = [](const OpArgs &args) {
      const auto it = args.find("match_shape");
      return it != args.end() && (it->second == "1" || it->second == "True" || it->second == "true");
    };
    /* Weights hold on every frame, and a shape matched here has other partners than the setup's. */
    weights.visible = [matching](const OpArgs &args) { return !matching(args); };
    OpProperty match_shape = op_bool_property(
        "match_shape", "Match Current Shape",
        "Go by the shape as it shows on this frame, through its bones: sides and pairs from where the points are now, "
        "each copy on the exact mirror image of its partner. Otherwise the pose is mirrored from the setup's pairs",
        "1");
    match_shape.enabled = [](Context &C) { return C.document->frame_current != 0; };
    match_shape.disabled_why = "On frame 0 the points already pair where they are";
    OpProperty axis;
    axis.name = "axis";
    axis.label = "Axis";
    axis.description = "The vertical line to mirror across: the layer's own, or the middle of the selected points (of "
                       "both layers, on a sided pair), for a shape that has moved off it";
    axis.items = {{"LAYER", "Layer Axis"}, {"SELECTION", "Middle of Selection"}};
    axis.fallback = "LAYER";
    axis.visible = matching;
    ot.props = {direction, threshold, weights, match_shape, axis};
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const bool right_to_left = op_arg(C, "direction", "RIGHT_TO_LEFT") != "LEFT_TO_RIGHT";
      const float threshold = op_arg_float(C, "threshold", 0.01f);
      const bool setup = doc.frame_current == 0;
      const bool told = op_has_arg(C, "match_shape");
      /* Weights hold on every frame, so a script asking for them means the setup's pairs. */
      bool match_shape = !setup && (told ? op_arg_bool(C, "match_shape") : !op_arg_bool(C, "weights"));
      bool middle = op_arg(C, "axis", "LAYER") == "SELECTION";
      const auto match_of = [&](const Layer &layer) {
        SymmetrizeMatch match;
        match.skins = layer_point_skins(doc, layer);
        match.middle = middle;
        return match;
      };
      /* What rides or hangs on the points only lands once they have moved: a second run puts it right. */
      const auto runs = [&](const Layer &layer) {
        const auto detail = [](const MeshPoint &p) { return p.detail.level > 0; };
        return match_shape && std::any_of(layer.drawing.points.begin(), layer.drawing.points.end(), detail) ? 2 : 1;
      };
      std::vector<Layer *> layers;
      for (Layer *layer : editable_layers(C)) {
        if (!layer->is_reference() && has_selection(layer->drawing)) {
          layers.push_back(layer);
        }
      }
      /* A sided layer (Ear_L) symmetrizes with its partner (Ear_R) across the pair's axis; a pair
       * once, whichever of them is edited. */
      std::map<uint64_t, Layer *> partner_of;
      for (auto it = layers.begin(); it != layers.end();) {
        Layer *partner = layer_mirror_partner(doc, (*it)->id);
        if (partner && partner->type == LayerType::Vector &&
            !(partner->is_reference() && !partner->reference.edit_points))
        {
          if (!partner_of.count(partner->id)) {
            partner_of[(*it)->id] = partner;
          }
          it = layers.erase(it);
          continue;
        }
        ++it;
      }
      /* Whether everything pairs up, by the setup's sides or by the shape (tried on copies). */
      const auto pairs_up = [&](bool matching) {
        for (auto &[id, partner] : partner_of) {
          Layer trial = *layer_find(doc, id), trial_partner = *partner;
          const SymmetrizeMatch match = matching ? match_of(trial) : SymmetrizeMatch();
          const SymmetrizeMatch partner_match = matching ? match_of(trial_partner) : SymmetrizeMatch();
          if (!layer_symmetrize_with_partner(doc, trial, trial_partner, right_to_left, threshold, setup, nullptr, nullptr,
                                             matching ? &match : nullptr, matching ? &partner_match : nullptr)
                   .done)
          {
            return false;
          }
        }
        for (Layer *layer : layers) {
          VectorDrawing trial = layer->drawing;
          const SymmetrizeMatch match = matching ? match_of(*layer) : SymmetrizeMatch();
          if (!drawing_symmetrize(trial, right_to_left, threshold, setup, nullptr, matching ? &match : nullptr).done) {
            return false;
          }
        }
        return true;
      };
      /* Not told which way (the menu): points the shape can't pair go by the setup's pairs, else across the middle of
       * the selection, as a run that stops here would open no panel to pick those in. The panel shows what ran. */
      if (!told && match_shape && !pairs_up(true)) {
        if (pairs_up(false)) {
          match_shape = false;
          report(ReportType::Info, "Symmetrize went by the setup's pairs (these points aren't even on this frame)");
        }
        else if (!op_has_arg(C, "axis")) {
          middle = true;
          if (pairs_up(true)) {
            report(ReportType::Info, "Symmetrize went across the middle of the selection (these points aren't even "
                                     "across the layer's axis)");
          }
          else {
            middle = false;
          }
        }
      }
      if (!told) {
        op_arg_used(C, "match_shape", match_shape ? "1" : "0");
        op_arg_used(C, "axis", middle ? "SELECTION" : "LAYER");
      }
      const bool weights = !match_shape && op_arg_bool(C, "weights");
      /* Each layer has to pair up before any changes. */
      for (auto &[id, partner] : partner_of) {
        Layer trial = *layer_find(doc, id), trial_partner = *partner;
        std::vector<int> unmatched;
        const SymmetrizeMatch match = match_shape ? match_of(trial) : SymmetrizeMatch();
        const SymmetrizeMatch partner_match = match_shape ? match_of(trial_partner) : SymmetrizeMatch();
        const SymmetrizeResult r = layer_symmetrize_with_partner(doc, trial, trial_partner, right_to_left, threshold,
                                                                 setup, nullptr, &unmatched,
                                                                 match_shape ? &match : nullptr,
                                                                 match_shape ? &partner_match : nullptr);
        if (!r.done) {
          Layer *layer = layer_find(doc, id);
          for (MeshPoint &p : layer->drawing.points) {
            p.selected = false;
          }
          for (const int i : unmatched) {
            layer->drawing.points[i].selected = true;
          }
          C.undo->mark_changed();
          report_banner(ReportType::Warning,
                        "Symmetrize: \"" + layer->name + "\" has more selected points than \"" + partner->name +
                            "\" has points to pair them with",
                        "Select as many points as its partner layer has on the other side", "Uneven sides");
          return OpStatus::Cancelled;
        }
      }
      for (Layer *layer : layers) {
        VectorDrawing trial = layer->drawing;
        const SymmetrizeMatch match = match_shape ? match_of(*layer) : SymmetrizeMatch();
        const SymmetrizeResult r = drawing_symmetrize(trial, right_to_left, threshold, setup, nullptr,
                                                      match_shape ? &match : nullptr);
        if (!r.done) {
          for (MeshPoint &p : layer->drawing.points) {
            p.selected = false;
          }
          for (const int i : r.unmatched) {
            layer->drawing.points[i].selected = true;
          }
          C.undo->mark_changed();
          /* Off frame 0 the sides counted may not be the ones on view. */
          const std::string side_counts = std::to_string(r.right) + " on the right, " + std::to_string(r.left) +
                                          " on the left" + (setup ? "" : match_shape ? " on this frame" : " in the setup pose");
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
        for (int run = runs(*layer); run > 0; run--) {
          const SymmetrizeMatch match = match_shape ? match_of(*layer) : SymmetrizeMatch();
          drawing_symmetrize(layer->drawing, right_to_left, threshold, setup, weights ? &flip : nullptr,
                             match_shape ? &match : nullptr);
          if (match_shape) {
            drawing_detail_sync(layer->drawing, true);
          }
        }
      }
      for (auto &[id, partner] : partner_of) {
        Layer *layer = layer_find(doc, id);
        std::map<uint64_t, uint64_t> flip;
        if (weights) {
          if (const Layer *armature = layer_armature(doc, layer->id)) {
            for (const Bone &b : armature->bones) {
              const Bone *other = bone_find_by_name(*armature, bone_mirror_name(b.name));
              flip[b.id] = other ? other->id : b.id;
            }
          }
        }
        for (int run = std::max(runs(*layer), runs(*partner)); run > 0; run--) {
          const SymmetrizeMatch match = match_shape ? match_of(*layer) : SymmetrizeMatch();
          const SymmetrizeMatch partner_match = match_shape ? match_of(*partner) : SymmetrizeMatch();
          layer_symmetrize_with_partner(doc, *layer, *partner, right_to_left, threshold, setup,
                                        weights ? &flip : nullptr, nullptr, match_shape ? &match : nullptr,
                                        match_shape ? &partner_match : nullptr);
          if (match_shape) {
            drawing_detail_sync(layer->drawing, true);
            drawing_detail_sync(partner->drawing, true);
          }
        }
      }
      return layers.empty() && partner_of.empty() ? OpStatus::Cancelled : OpStatus::Finished;
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
                  MIRRORED, editable_with_selection_poll, [](Context &C) {
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

  /* --- Point groups (Blender's vertex groups), on the active layer's drawing ------------------- */
  const auto group_drawing = [](Context &C) -> VectorDrawing * {
    Layer *layer = nullptr;
    VectorDrawing *drawing = editable_drawing(C, &layer);
    return drawing && layer->type == LayerType::Vector && !layer->is_reference() ? drawing : nullptr;
  };
  const auto has_group = [group_drawing](Context &C) {
    const VectorDrawing *d = group_drawing(C);
    return d && !d->groups.empty();
  };
  /* The group named by index= (a number) or name=, or the active one. */
  const auto group_of = [](Context &C, VectorDrawing &d) -> int {
    if (op_has_arg(C, "index")) {
      const int i = op_arg_int(C, "index", -1);
      return i >= 0 && i < int(d.groups.size()) ? i : -1;
    }
    if (op_has_arg(C, "group")) {
      const std::string name = op_arg(C, "group");
      for (int i = 0; i < int(d.groups.size()); i++) {
        if (d.groups[size_t(i)].name == name) {
          return i;
        }
      }
      return -1;
    }
    return d.groups.empty() ? -1 : std::clamp(d.active_group, 0, int(d.groups.size()) - 1);
  };
  vector_operator("mesh.point_group_add", "Add Point Group",
                  "A new point group on the active layer (name=, \"Group\" by default), made the active one",
                  UNDO, [group_drawing](Context &C) { return group_drawing(C) != nullptr; },
                  [group_drawing](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    PointGroup g;
                    g.name = drawing_group_unique_name(d, op_arg(C, "name", "Group"));
                    d.groups.push_back(std::move(g));
                    d.active_group = int(d.groups.size()) - 1;
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_remove", "Remove Point Group",
                  "Remove a point group (the active one, or index= / group=); its points stay",
                  UNDO, has_group, [group_drawing, group_of](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    const int i = group_of(C, d);
                    if (i < 0) {
                      return OpStatus::Cancelled;
                    }
                    d.groups.erase(d.groups.begin() + i);
                    d.active_group = std::clamp(d.active_group > i ? d.active_group - 1 : d.active_group, 0,
                                                std::max(0, int(d.groups.size()) - 1));
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_set", "Point Group Settings",
                  "Rename a point group (name=), the active one or index= / group=; active=1 makes it the active one; "
                  "lock=1 or 0 locks it (Assign and Remove leave it alone) or unlocks it",
                  UNDO, has_group, [group_drawing, group_of](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    const int i = group_of(C, d);
                    if (i < 0) {
                      return OpStatus::Cancelled;
                    }
                    if (op_has_arg(C, "name")) {
                      const std::string name = op_arg(C, "name");
                      if (name.empty()) {
                        report(ReportType::Warning, "A point group needs a name");
                        return OpStatus::Cancelled;
                      }
                      if (name != d.groups[size_t(i)].name) {
                        d.groups[size_t(i)].name = drawing_group_unique_name(d, name);
                      }
                    }
                    if (op_arg_bool(C, "active")) {
                      d.active_group = i;
                    }
                    if (op_has_arg(C, "lock")) {
                      d.groups[size_t(i)].locked = op_arg_bool(C, "lock");
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_assign", "Assign to Point Group",
                  "Put the selected points in the active point group, with weight= (0-1, 1 by default)",
                  MIRRORED, has_group, [group_drawing, group_of](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    const int i = group_of(C, d);
                    if (d.groups[size_t(i)].locked) {
                      report(ReportType::Warning, "\"" + d.groups[size_t(i)].name + "\" is locked");
                      return OpStatus::Cancelled;
                    }
                    drawing_ensure_ids(d);
                    const float w = std::clamp(op_arg_float(C, "weight", 1.0f), 0.0f, 1.0f);
                    int n = 0;
                    for (const MeshPoint &p : d.points) {
                      if (p.selected) {
                        d.groups[size_t(i)].weights[p.id] = w;
                        n++;
                      }
                    }
                    if (n == 0) {
                      report(ReportType::Warning, "Select the points to put in the group");
                      return OpStatus::Cancelled;
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_unassign", "Remove from Point Group",
                  "Take the selected points out of the active point group",
                  MIRRORED, has_group, [group_drawing, group_of](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    PointGroup &g = d.groups[size_t(group_of(C, d))];
                    if (g.locked) {
                      report(ReportType::Warning, "\"" + g.name + "\" is locked");
                      return OpStatus::Cancelled;
                    }
                    int n = 0;
                    for (const MeshPoint &p : d.points) {
                      if (p.selected) {
                        n += int(g.weights.erase(p.id));
                      }
                    }
                    return n ? OpStatus::Finished : OpStatus::Cancelled;
                  });
  vector_operator("mesh.point_group_move", "Move Point Group",
                  "Move the active point group up (direction=-1) or down (direction=1) the list",
                  UNDO, has_group, [group_drawing, group_of](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    const int i = group_of(C, d);
                    const int to = i + (op_arg_int(C, "direction", 1) < 0 ? -1 : 1);
                    if (to < 0 || to >= int(d.groups.size())) {
                      return OpStatus::Cancelled;
                    }
                    std::swap(d.groups[size_t(i)], d.groups[size_t(to)]);
                    d.active_group = to;
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_copy", "Copy Point Group",
                  "A copy of the active point group (\"Group_copy\"), its points and weights, made active",
                  UNDO, has_group, [group_drawing, group_of](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    PointGroup copy = d.groups[size_t(group_of(C, d))];
                    copy.name = drawing_group_unique_name(d, copy.name + "_copy");
                    copy.locked = false;
                    d.groups.insert(d.groups.begin() + group_of(C, d) + 1, std::move(copy));
                    d.active_group = group_of(C, d) + 1;
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_sort", "Sort Point Groups by Name", "Put the point groups in order of their names",
                  UNDO, has_group, [group_drawing](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    const std::string active = d.groups[size_t(std::clamp(d.active_group, 0, int(d.groups.size()) - 1))].name;
                    std::stable_sort(d.groups.begin(), d.groups.end(),
                                     [](const PointGroup &a, const PointGroup &b) { return a.name < b.name; });
                    for (int i = 0; i < int(d.groups.size()); i++) {
                      if (d.groups[size_t(i)].name == active) {
                        d.active_group = i;
                      }
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_unassign_all", "Remove from All Point Groups",
                  "Take the selected points out of every point group that isn't locked",
                  UNDO, has_group, [group_drawing](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    int n = 0;
                    for (PointGroup &g : d.groups) {
                      for (const MeshPoint &p : d.points) {
                        if (p.selected && !g.locked) {
                          n += int(g.weights.erase(p.id));
                        }
                      }
                    }
                    return n ? OpStatus::Finished : OpStatus::Cancelled;
                  });
  vector_operator("mesh.point_group_remove_all", "Delete All Point Groups",
                  "Remove every point group of the active layer (unlocked=1: only the ones not locked); the points stay",
                  UNDO, has_group, [group_drawing](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    const bool unlocked = op_arg_bool(C, "unlocked");
                    std::erase_if(d.groups, [&](const PointGroup &g) { return !unlocked || !g.locked; });
                    d.active_group = 0;
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.point_group_lock_all", "Lock All Point Groups",
                  "Lock every point group (lock=1, the default) or unlock them all (lock=0)",
                  UNDO, has_group, [group_drawing](Context &C) {
                    VectorDrawing &d = *group_drawing(C);
                    const bool lock = !op_has_arg(C, "lock") || op_arg_bool(C, "lock");
                    for (PointGroup &g : d.groups) {
                      g.locked = lock;
                    }
                    return OpStatus::Finished;
                  });
  for (const bool select : {true, false}) {
    vector_operator(select ? "mesh.point_group_select" : "mesh.point_group_deselect",
                    select ? "Select Point Group" : "Deselect Point Group",
                    select ? "Select the points in the active point group (adding to the selection)"
                           : "Deselect the points in the active point group",
                    UNDO, has_group, [group_drawing, group_of, select](Context &C) {
                      VectorDrawing &d = *group_drawing(C);
                      const PointGroup &g = d.groups[size_t(group_of(C, d))];
                      for (MeshPoint &p : d.points) {
                        if (g.weights.contains(p.id)) {
                          p.selected = select;
                        }
                      }
                      return OpStatus::Finished;
                    });
  }

  /* What the X menu offers, as Blender's Delete menu: each whatever the selection mode is. */
  vector_operator("mesh.delete_points", "Delete Points", "Delete the selected points and the curves left too short",
                  MIRRORED, editable_with_selection_poll, [](Context &C) {
                    for (Layer *layer : editable_layers(C)) {
                      std::vector<bool> remove(layer->drawing.points.size());
                      for (size_t i = 0; i < layer->drawing.points.size(); i++) {
                        remove[i] = layer->drawing.points[i].selected;
                      }
                      drawing_remove_points(layer->drawing, remove);
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.make_segment", "Make Segment",
                  "Join the two selected points with a segment (F, as Blender's): two curve ends make one curve of "
                  "two, the two ends of one curve close it, and the shapes along them run on; any other two points "
                  "get a segment of their own, welded to both, drawn as the line through them is. With whole loops "
                  "selected instead, F joins their fills (mesh.join_fills)",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    int made = 0;
                    for (Layer *layer : editable_layers(C)) {
                      const std::vector<int> picked = drawing_selected_points(layer->drawing);
                      if (picked.size() == 2 && drawing_make_segment(layer->drawing, picked[0], picked[1])) {
                        made++;
                      }
                    }
                    if (!made) {
                      /* Whole loops picked: one fill of them, as Blender's F makes a face of a loop. */
                      for (Layer *layer : editable_layers(C)) {
                        made += drawing_join_fills(layer->drawing) > 0;
                      }
                      if (made) {
                        C.undo_name = "Join Fills";
                        return OpStatus::Finished;
                      }
                      report(ReportType::Warning, "Select two points not already joined by a segment, or two or more "
                                                  "loops to fill together");
                      return OpStatus::Cancelled;
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.join_fills", "Join Fills",
                  "Make one shape of the selected shapes that go round (every point of each selected): their loops "
                  "fill together, a loop inside another cutting a hole (Even-Odd), with the first one's look",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    int joined = 0;
                    for (Layer *layer : editable_layers(C)) {
                      joined += drawing_join_fills(layer->drawing) > 0;
                    }
                    if (!joined) {
                      report(ReportType::Warning, "Select two or more whole loops to fill together");
                      return OpStatus::Cancelled;
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.separate_fills", "Separate Fills",
                  "Split each selected shape of several loops into a shape per loop, each with its look",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    int made = 0;
                    for (Layer *layer : editable_layers(C)) {
                      made += drawing_separate_fills(layer->drawing);
                    }
                    if (!made) {
                      report(ReportType::Warning, "Select a shape of several loops to separate");
                      return OpStatus::Cancelled;
                    }
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.delete_edges", "Delete Edges",
                  "Delete the segments between selected points, keeping the points: curves split there",
                  MIRRORED, editable_with_selection_poll, [](Context &C) {
                    int deleted = 0;
                    for (Layer *layer : editable_layers(C)) {
                      deleted += drawing_delete_edges(layer->drawing, drawing_selected_edges(layer->drawing));
                    }
                    return deleted ? OpStatus::Finished : OpStatus::Cancelled;
                  });
  vector_operator("mesh.dissolve_points", "Dissolve Points",
                  "Remove the selected points, joining the curve across each gap so lines and shapes stay whole",
                  MIRRORED, editable_with_selection_poll, [](Context &C) {
                    for (Layer *layer : editable_layers(C)) {
                      VectorDrawing &drawing = layer->drawing;
                      /* All at once, so a run of neighbouring points is fitted as one span. */
                      std::vector<int> selected;
                      for (int i = 0; i < int(drawing.points.size()); i++) {
                        if (drawing.points[i].selected) {
                          selected.push_back(i);
                        }
                      }
                      drawing_dissolve_points(drawing, selected);
                    }
                    return OpStatus::Finished;
                  });
  {
    /* Blender's Subdivide (right-click on points): each segment between two selected points cut into
     * number_cuts + 1 pieces, keeping its shape exactly (each cut an exact split, as the knife's). The cuts are
     * spaced evenly along the segment's length, not its Bezier parameter, so they don't bunch up where its
     * handles are long. At a shape's detail level above 0 they are detail of that level, as the knife's are; the
     * new points join the selection. */
    OpProperty cuts;
    cuts.name = "number_cuts";
    cuts.label = "Number of Cuts";
    cuts.description = "How many points each segment between two selected points gets";
    cuts.kind = OpProperty::Kind::Int;
    cuts.fallback = "1";
    cuts.min = 1.0f;
    cuts.max = 100.0f;
    cuts.step = 1.0f;
    vector_operator(
        "mesh.subdivide", "Subdivide",
        "Cut each segment between two selected points into number_cuts + 1 pieces, keeping the curve's shape: the "
        "new points spaced evenly along it and selected (at a shape's detail level, as its detail)",
        MIRRORED, editable_with_selection_poll,
        [](Context &C) {
          const int number = std::clamp(int(op_arg_float(C, "number_cuts", 1.0f)), 1, 100);
          int made = 0;
          for (Layer *layer : editable_layers(C)) {
            if (layer->is_reference()) {
              continue;
            }
            VectorDrawing &drawing = layer->drawing;
            const auto shown_selected = [&](int p) {
              return drawing.points[size_t(p)].selected && !drawing.points[size_t(p)].detail.hidden;
            };
            std::vector<int> added;
            for (int c = 0; c < int(drawing.curves.size()); c++) {
              const int level = drawing_knife_level(drawing, c);
              /* From the last segment back, so the ones before keep their places as points go in. */
              for (int s = drawing.curves[size_t(c)].segment_count() - 1; s >= 0; s--) {
                const Curve &curve = drawing.curves[size_t(c)];
                int a, b;
                curve_segment_points(curve, s, &a, &b);
                if (!shown_selected(a) || !shown_selected(b)) {
                  continue;
                }
                /* Where the cuts go along its length, as parameters of the whole segment. */
                float2 ctrl[4];
                curve_segment_bezier(drawing, curve, s, ctrl);
                constexpr int STEPS = 128;
                std::vector<float> along(STEPS + 1, 0.0f);
                float2 prev = ctrl[0];
                for (int k = 1; k <= STEPS; k++) {
                  const float2 now = bezier_eval(ctrl, float(k) / STEPS);
                  along[size_t(k)] = along[size_t(k - 1)] + distance(prev, now);
                  prev = now;
                }
                std::vector<float> ts;
                for (int k = 1; k <= number; k++) {
                  const float want = along[STEPS] * float(k) / float(number + 1);
                  const auto it = std::lower_bound(along.begin(), along.end(), want);
                  const int hi = std::clamp(int(it - along.begin()), 1, STEPS);
                  const float span = along[size_t(hi)] - along[size_t(hi - 1)];
                  const float f = span > 1e-12f ? (want - along[size_t(hi - 1)]) / span : 0.0f;
                  ts.push_back((float(hi - 1) + f) / STEPS);
                }
                /* The last cut first: the part before it is then the segment again, its parameters scaled. */
                float end = 1.0f;
                for (int k = number - 1; k >= 0; k--) {
                  const float t = std::clamp(ts[size_t(k)] / end, 1e-4f, 1.0f - 1e-4f);
                  added.push_back(drawing_detail_split(drawing, {c, s}, t, level));
                  end = ts[size_t(k)];
                  made++;
                }
              }
            }
            for (const int p : added) {
              drawing.points[size_t(p)].selected = true;
            }
          }
          if (made == 0) {
            report(ReportType::Warning, "Select two neighbouring points of a curve to subdivide the segment between them");
            return OpStatus::Cancelled;
          }
          return OpStatus::Finished;
        },
        {cuts});
  }
  {
    /* X opens the Delete menu at the mouse, as in Blender. */
    OperatorType ot;
    ot.idname = "wm.link_data_menu";
    ot.name = "Link/Transfer Data";
    ot.description = "Blender's Link/Transfer Data menu (Ctrl L): with points selected, Link Materials; on a bone "
                     "layer, Copy Constraints; with layers picked, Copy Masks";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = [](Context &C) {
      if (!C.document || !C.wm) {
        return false;
      }
      const Layer *active = document_active_layer(*C.document);
      return (active && active->is_bone_layer()) || !editable_layers(C).empty();
    };
    ot.exec = [](Context &C) {
      C.wm->menu_request = "link_data";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  /* M: Blender's Merge menu, for points. */
  {
    OperatorType ot;
    ot.idname = "mesh.merge_menu";
    ot.name = "Merge";
    ot.description = "Choose how to merge the selected points: all into one, or those near one another";
    ot.flag = OPTYPE_REGISTER;
    ot.poll = editable_with_selection_poll;
    ot.exec = [](Context &C) {
      C.wm->menu_request = "mesh_merge";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  vector_operator("mesh.merge", "Merge",
                  "Merge the selected points into one, at their middle (Blender's Merge > At Center): curves through "
                  "them run through it, and shapes that met only there become one",
                  UNDO, editable_with_selection_poll, [](Context &C) {
                    /* With X mirror editing the images merge among themselves, each side at its own middle: all
                     * together, a line's two ends would meet. */
                    const MirrorSelection mirrored = mirror_selection_begin(C);
                    int merged = 0;
                    for (Layer *layer : editable_layers(C)) {
                      if (layer->is_reference() && !layer->reference.edit_points) {
                        continue;
                      }
                      std::vector<int> picked, images;
                      const auto added = mirrored.added.find(layer->id);
                      for (const int p : drawing_selected_points(layer->drawing)) {
                        const uint64_t id = layer->drawing.points[size_t(p)].id;
                        const bool image = added != mirrored.added.end() &&
                                           std::find(added->second.begin(), added->second.end(), id) != added->second.end();
                        (image ? images : picked).push_back(p);
                      }
                      merged += drawing_merge_points(layer->drawing, {picked, images});
                    }
                    mirror_selection_end(C, mirrored);
                    if (merged == 0) {
                      report(ReportType::Warning, "Select at least two points to merge");
                      return OpStatus::Cancelled;
                    }
                    report(ReportType::Info, "Removed " + std::to_string(merged) + (merged == 1 ? " point" : " points"));
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.merge_by_distance", "Merge by Distance",
                  "Merge the selected points that lie within distance= of one another (by way of each other), each "
                  "group at its middle (Blender's Merge > By Distance)",
                  MIRRORED, editable_with_selection_poll, [](Context &C) {
                    const float distance = std::max(op_arg_float(C, "distance", 0.001f), 0.0f);
                    int merged = 0;
                    for (Layer *layer : editable_layers(C)) {
                      if (layer->is_reference() && !layer->reference.edit_points) {
                        continue;
                      }
                      merged += drawing_merge_points(layer->drawing, drawing_points_within(layer->drawing, distance));
                    }
                    report(ReportType::Info, "Removed " + std::to_string(merged) + (merged == 1 ? " point" : " points"));
                    return OpStatus::Finished;
                  },
                  {op_float_property("distance", "Merge Distance", "Points this close (or closer) become one", "0.001",
                                     0.0f, 10.0f, 0.0005f)});
  {
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
                        drawing_mirror_selected_handles(drawing,
                                                        layer_mirror_x_self(*C.document, layer->id,
                                                                            C.wm->tool_settings.mirror_x),
                                                        C.wm->tool_settings.mirror_y);
                      }
                      return OpStatus::Finished;
                    });
  }

  /* P: Blender's Separate > Selection. What Duplicate would copy (the runs of selected points and the
   * shapes they fully cover) goes to a new layer above, in the same group, with the layer's settings,
   * bones and keys; the original loses those segments (points other shapes still use stay). The new layer
   * becomes the active one, as Layer Duplicate's copy does. */
  vector_operator(
      "mesh.separate", "Separate",
      "Move the selected points, the curve pieces between them and the shapes they cover into a new layer of their "
      "own, above this one (P, as Blender's Separate > Selection)",
      MIRRORED, editable_with_selection_poll, [](Context &C) {
        Document &doc = *C.document;
        std::vector<uint64_t> ids;
        for (Layer *layer : editable_layers(C)) {
          if (layer->type == LayerType::Vector && !layer->is_reference()) {
            ids.push_back(layer->id);
          }
        }
        std::vector<std::string> made;
        std::vector<uint64_t> copies;
        uint64_t active_copy = 0;
        for (const uint64_t id : ids) {
          Layer *layer = layer_find(doc, id);
          VectorDrawing piece = layer->drawing;
          if (!drawing_duplicate_selected(piece)) {
            continue;
          }
          /* Only the copy is selected now: everything else goes. */
          std::vector<bool> others(piece.points.size());
          for (size_t i = 0; i < piece.points.size(); i++) {
            others[i] = !piece.points[i].selected;
          }
          drawing_remove_points(piece, others);
          for (MeshPoint &p : piece.points) {
            p.selected = false;
          }
          drawing_delete_edges(layer->drawing, drawing_selected_edges(layer->drawing));
          drawing_remove_unused_points(layer->drawing);
          for (MeshPoint &p : layer->drawing.points) {
            p.selected = false;
          }
          Layer copy = layer_copy_with_new_ids(doc, *layer);
          copy.drawing = std::move(piece);
          copy.name = document_layer_unique_name(doc, layer->name);
          copy.selected = false;
          int index = 0;
          std::vector<Layer> *container = layer_container(doc, id, &index);
          if (id == doc.active_layer) {
            active_copy = copy.id;
          }
          copies.push_back(copy.id);
          made.push_back(copy.name);
          container->insert(container->begin() + index, std::move(copy));
        }
        if (made.empty()) {
          report(ReportType::Warning, "Select at least two connected points (a shape, or a piece of a line) to separate");
          return OpStatus::Cancelled;
        }
        for (const Layer *l : document_layers_flat(doc)) {
          layer_find(doc, l->id)->selected = false;
        }
        for (const uint64_t id : copies) {
          if (id != active_copy) {
            layer_find(doc, id)->selected = true;
          }
        }
        doc.active_layer = active_copy ? active_copy : copies.front();
        report(ReportType::Info, "Separated into " + made.front() +
                                     (made.size() > 1 ? " and " + std::to_string(made.size() - 1) + " more" : ""));
        return OpStatus::Finished;
      });

  vector_operator("mesh.delete_shape", "Delete Shape", "Delete the fills and outlines that use selected points",
                  MIRRORED, editable_with_selection_poll, [](Context &C) {
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

  {
    /* Shapes by name (the Shapes list's right-click Delete): only those fills and outlines go; their
     * curves and points stay for the shapes that share them. */
    OperatorType ot;
    ot.idname = "mesh.delete_shapes";
    ot.name = "Delete Shapes";
    ot.description = "Delete shapes by name or id (shapes=, | between them) from a layer (layer=, else the active "
                     "one), keeping their curves and points for any other shape on them";
    ot.flag = MIRRORED;
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      Layer *layer = op_has_arg(C, "layer") ? layer_by_name_or_id(doc, op_arg(C, "layer")) : document_active_layer(doc);
      if (!layer || !layer->has_drawing()) {
        report(ReportType::Error, "No layer with a drawing to delete shapes from");
        return OpStatus::Cancelled;
      }
      if (layer->is_reference()) {
        report(ReportType::Warning, "A copy (reference layer) keeps its original's shapes: hide them in the Reference "
                                    "tab, or Break Reference");
        return OpStatus::Cancelled;
      }
      VectorDrawing &drawing = layer->drawing;
      std::vector<bool> doomed(drawing.shapes.size(), false);
      const std::string list = op_arg(C, "shapes");
      for (size_t at = 0; at <= list.size();) {
        const size_t bar = std::min(list.find('|', at), list.size());
        const std::string name = list.substr(at, bar - at);
        at = bar + 1;
        if (name.empty()) {
          continue;
        }
        bool found = false;
        for (size_t s = 0; s < drawing.shapes.size(); s++) {
          if (drawing.shapes[s].name == name || std::to_string(drawing.shapes[s].id) == name) {
            doomed[s] = found = true;
          }
        }
        if (!found) {
          report(ReportType::Error, layer->name + " has no shape \"" + name + "\"");
          return OpStatus::Cancelled;
        }
      }
      const auto count = std::count(doomed.begin(), doomed.end(), true);
      if (count == 0) {
        return OpStatus::Cancelled;
      }
      size_t s = 0;
      std::erase_if(drawing.shapes, [&](const Shape &) { return doomed[s++]; });
      C.undo_name = count == 1 ? "Delete Shape" : "Delete Shapes";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }

  /* --- Modal transforms -----------------------------------------------------------
   * These only ask the viewport to start; it makes the undo step when the move ends. */
  const auto transform_op = [](const char *idname, const char *name, const char *description, char kind) {
    OperatorType ot;
    ot.idname = idname;
    ot.name = name;
    ot.description = description;
    /* Points (and handles) of the edited layers, or the bone layer's selected bones. */
    ot.poll = [kind](Context &C) {
      if (!C.wm->mouse_in_viewport) {
        return false;
      }
      const Layer *active = C.document ? document_active_layer(*C.document) : nullptr;
      /* A Media or text layer moves, turns and scales as a whole, and so does any layer with the
       * Transform Layer tool. */
      if (active && (active->is_media() || active->is_text() ||
                     (kind != 'E' && C.wm->tool_settings.tool == Tool::TransformLayer)))
      {
        return !active->locked;
      }
      if (active && active->is_bone_layer()) {
        /* A bone picked by its body or by either ball, here or in a sub bone group inside, or points of
         * its layers marked Editable in Bone Group. */
        for (const Layer *l : armature_and_inside(*C.document, active->id)) {
          if (armature_any_picked(*l)) {
            return true;
          }
        }
        return editable_with_selection_poll(C);
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
    ot.exec = [tool](Context &C) {
      if (tool_setup_only(tool) && C.document && C.document->frame_current != 0) {
        report_banner(ReportType::Warning, "Setup frame only: press Tab to go to frame 0");
        return OpStatus::Cancelled;
      }
      C.wm->tool_settings.tool = tool;
      /* The Motion Path tool edits paths: chosen with them off, it shows them (not an undo step, as a tool isn't). */
      if (tool == Tool::MotionPath && C.document && !C.document->motion_paths) {
        C.document->motion_paths = true;
        if (C.undo) {
          C.undo->mark_changed();
        }
      }
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
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      /* A setup-only tool: away from frame 0 it says what makes it work. */
      if (C.document->frame_current != 0) {
        report_banner(ReportType::Warning, "Setup frame only: press Tab to go to frame 0");
        return OpStatus::Cancelled;
      }
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
  tool_op("wm.tool_select_layer", "Select Layer Tool", Tool::SelectLayer);
  tool_op("wm.tool_cursor", "Cursor Tool", Tool::Cursor);
  {
    /* oanim.ops.wm.tool_draw(mode="PEN"): the Draw tool, optionally picking freehand or the pen. */
    OperatorType ot;
    ot.idname = "wm.tool_draw";
    ot.name = "Draw Tool";
    ot.description = "Switch to the Draw tool: freehand strokes, or mode= PEN (the Bezier Pen), FILL_PEN, FILL_ERASER, "
                     "STROKE_ERASER or TRIMMER";
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      /* A setup-only tool: away from frame 0 it says what makes it work. */
      if (C.document->frame_current != 0) {
        report_banner(ReportType::Warning, "Setup frame only: press Tab to go to frame 0");
        return OpStatus::Cancelled;
      }
      const std::string mode = op_arg(C, "mode");
      if (mode == "PEN") {
        draw_mode_set(C.wm->tool_settings, DrawMode::Pen);
      }
      else if (mode == "FREEHAND") {
        draw_mode_set(C.wm->tool_settings, DrawMode::Freehand);
      }
      else if (mode == "FILL_PEN") {
        draw_mode_set(C.wm->tool_settings, DrawMode::FillPen);
      }
      else if (mode == "FILL_ERASER") {
        draw_mode_set(C.wm->tool_settings, DrawMode::FillEraser);
      }
      else if (mode == "STROKE_ERASER") {
        draw_mode_set(C.wm->tool_settings, DrawMode::StrokeEraser);
      }
      else if (mode == "TRIMMER") {
        draw_mode_set(C.wm->tool_settings, DrawMode::Trimmer);
      }
      else if (!mode.empty()) {
        report(ReportType::Error, "Unknown draw mode '" + mode + "': use FREEHAND, PEN, FILL_PEN, FILL_ERASER, STROKE_ERASER or TRIMMER");
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
  tool_op("wm.tool_sculpt", "Sculpt Tool", Tool::Sculpt);
  {
    /* Ctrl Tab: into Sculpt, and back to the tool it came from, as Blender toggles sculpt mode. */
    OperatorType ot;
    ot.idname = "wm.tool_sculpt_toggle";
    ot.name = "Toggle Sculpt";
    ot.description = "Switch to the Sculpt tool, or from it back to the tool used before";
    ot.exec = [](Context &C) {
      ToolSettings &ts = C.wm->tool_settings;
      if (ts.tool == Tool::Sculpt) {
        ts.tool = ts.tool_before_sculpt == Tool::Sculpt ? Tool::Select : ts.tool_before_sculpt;
      }
      else {
        ts.tool_before_sculpt = ts.tool_temporary ? ts.tool_return : ts.tool;
        ts.tool = Tool::Sculpt;
        ts.tool_temporary = false;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  tool_op("wm.tool_transform_bones", "Transform Bones Tool", Tool::TransformBones);
  tool_op("wm.tool_transform_points", "Transform Points Tool", Tool::TransformPoints);
  tool_op("wm.tool_transform_layer", "Transform Layer Tool", Tool::TransformLayer);
  tool_op("wm.tool_edit_text", "Edit Text Tool", Tool::EditText);
  tool_op("wm.tool_motion_path", "Motion Path Tool", Tool::MotionPath);
  {
    /* oanim.ops.wm.tool_shape(shape="CIRCLE"): the Shape tool, optionally picking the shape. */
    OperatorType ot;
    ot.idname = "wm.tool_shape";
    ot.name = "Shape Tool";
    ot.description = "Switch to the Shape tool: shape=BOX, CIRCLE or GRID (columns=, rows= its cells)";
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      /* A setup-only tool: away from frame 0 it says what makes it work. */
      if (C.document->frame_current != 0) {
        report_banner(ReportType::Warning, "Setup frame only: press Tab to go to frame 0");
        return OpStatus::Cancelled;
      }
      const std::string shape = op_arg(C, "shape");
      if (!shape.empty() && !primitive_from_identifier(shape, &C.wm->tool_settings.shape)) {
        report(ReportType::Error, "Unknown shape '" + shape + "': use BOX, CIRCLE or GRID");
        return OpStatus::Cancelled;
      }
      if (op_has_arg(C, "columns")) {
        C.wm->tool_settings.grid_columns = std::clamp(op_arg_int(C, "columns", 3), 1, GRID_CELLS_MAX);
      }
      if (op_has_arg(C, "rows")) {
        C.wm->tool_settings.grid_rows = std::clamp(op_arg_int(C, "rows", 3), 1, GRID_CELLS_MAX);
      }
      C.wm->tool_settings.tool = Tool::Shape;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* oanim.ops.wm.tool_annotate(mode="GUIDE_LINE"): the Annotate tool, optionally picking its variant. */
    OperatorType ot;
    ot.idname = "wm.tool_annotate";
    ot.name = "Annotate Tool";
    ot.description = "Switch to the Annotate tool: notes and drawing guides over the drawing; mode= DRAW, LINE, "
                     "ERASER, GUIDE_LINE, GUIDE_PERSPECTIVE or GUIDE_GRID";
    ot.poll = [](Context &C) { return C.document != nullptr; };
    ot.exec = [](Context &C) {
      static const std::pair<const char *, AnnotateMode> MODES[] = {
          {"DRAW", AnnotateMode::Draw},           {"LINE", AnnotateMode::Line},
          {"ERASER", AnnotateMode::Eraser},       {"GUIDE_LINE", AnnotateMode::GuideLine},
          {"GUIDE_PERSPECTIVE", AnnotateMode::GuidePerspective}, {"GUIDE_GRID", AnnotateMode::GuideGrid}};
      const std::string mode = op_arg(C, "mode");
      if (!mode.empty()) {
        const auto it = std::find_if(std::begin(MODES), std::end(MODES), [&](const auto &m) { return mode == m.first; });
        if (it == std::end(MODES)) {
          report(ReportType::Error, "Unknown annotate mode '" + mode +
                                        "': use DRAW, LINE, ERASER, GUIDE_LINE, GUIDE_PERSPECTIVE or GUIDE_GRID");
          return OpStatus::Cancelled;
        }
        C.wm->tool_settings.annotate_mode = it->second;
      }
      C.wm->tool_settings.tool = Tool::Annotate;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    /* Kept for scripts: the pen is now a mode of the Draw tool. */
    OperatorType ot;
    ot.idname = "wm.tool_pen";
    ot.name = "Bezier Pen Tool";
    ot.description = "Switch to the Draw tool's Bezier Pen: click to add points along a curve";
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
  {
    /* Alt S: Blender Grease Pencil's Shrink/Fatten. */
    OperatorType ot;
    ot.idname = "transform.radius";
    ot.name = "Scale Radius";
    ot.description = "Scale the line's width at the selected points with the mouse (Alt S, Blender's Shrink/Fatten): "
                     "away from their middle fattens, towards it thins; a typed number sets the factor, Ctrl snaps "
                     "to tenths";
    ot.poll = [](Context &C) {
      const Layer *active = C.document ? document_active_layer(*C.document) : nullptr;
      return C.wm->mouse_in_viewport && active && !active->is_media() && !active->is_bone_layer() &&
             editable_with_selection_poll(C);
    };
    ot.exec = [](Context &C) {
      C.wm->transform_request = 'W';
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
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
      /* The Motion Path tool's Edit slides path dots proportionally too. */
      if (C.wm->tool_settings.tool == Tool::MotionPath && C.wm->tool_settings.path_brush == PathBrush::Edit) {
        return true;
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
    vector_operator(idname, name, description, MIRRORED, editable_with_selection_poll, [arrange](Context &C) {
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
                  "Remove the selected shapes' detail above the level they are edited at", MIRRORED, editable_poll,
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
                    report(ReportType::Info, "Removed " + std::to_string(removed) + (removed == 1 ? " detail point" : " detail points"));
                    return OpStatus::Finished;
                  });
  /* Smooth to Lower Level (right-click on detail points): back onto the curve of the level below, by
   * Influence; adjustable in the ViewportActionPanel. Away from frame 0 it poses them (auto-key keys it). */
  {
    OperatorType ot;
    ot.idname = "mesh.detail_smooth";
    ot.name = "Smooth to Lower Level";
    ot.description = "Move the selected detail points (and handles of detail points picked on their own) onto the "
                     "curve of the level below, so they keep none of their own offset (influence 1: all the way)";
    ot.flag = MIRRORED;
    ot.poll = editable_with_selection_poll;
    OpProperty influence;
    influence.name = "influence";
    influence.label = "Influence";
    influence.description = "How far onto the curve of the level below (1: all the way, 0: unchanged)";
    influence.kind = OpProperty::Kind::Float;
    influence.fallback = "1";
    influence.min = 0.0f;
    influence.max = 1.0f;
    influence.step = 0.01f;
    ot.props = {influence};
    ot.exec = [](Context &C) {
      const float amount = std::clamp(op_arg_float(C, "influence", 1.0f), 0.0f, 1.0f);
      int changed = 0;
      for (Layer *layer : editable_layers(C)) {
        if (!layer->is_reference()) {
          changed += drawing_detail_smooth_selected(layer->drawing, amount);
        }
      }
      if (changed == 0) {
        report(ReportType::Warning, "Select detail points (level 1 and up) to smooth them to the level below");
        return OpStatus::Cancelled;
      }
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  /* Apply Detail and Rebuild Levels reshape the setup: frame 0 only. */
  const auto setup_frame = [](Context &C, const char *what) {
    if (C.document->frame_current == 0) {
      return true;
    }
    report(ReportType::Warning, std::string(what) + " works on the setup frame (0)");
    return false;
  };
  vector_operator("mesh.detail_raise", "Move to Higher Level",
                  "Move the selected points one detail level up, keeping the curve's look: they then ride along "
                  "the coarser points around them and show from that level (setup frame)",
                  MIRRORED, editable_with_selection_poll, [setup_frame](Context &C) {
                    if (!setup_frame(C, "Move to Higher Level")) {
                      return OpStatus::Cancelled;
                    }
                    int raised = 0, skipped = 0;
                    bool dropped = false;
                    for (Layer *layer : editable_layers(C)) {
                      if (layer->is_reference()) {
                        continue;
                      }
                      int s = 0;
                      bool keys = false;
                      raised += drawing_detail_raise_selected(layer->drawing, &s, &keys);
                      skipped += s;
                      dropped |= keys;
                    }
                    if (raised == 0) {
                      report(ReportType::Warning,
                             skipped > 0 ? "These points can't go up a level: an open curve's ends, points on more "
                                           "than one curve and points at the top level stay where they are"
                                         : "Select points to move up a detail level");
                      return OpStatus::Cancelled;
                    }
                    std::string text = "Moved " + std::to_string(raised) + (raised == 1 ? " point" : " points") +
                                       " up a detail level";
                    if (skipped > 0) {
                      text += "; " + std::to_string(skipped) + " couldn't (curve ends, or on more than one curve)";
                    }
                    if (dropped) {
                      text += " (their keys were removed)";
                    }
                    report(dropped || skipped > 0 ? ReportType::Warning : ReportType::Info, text);
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.detail_lower", "Move to Lower Level",
                  "Move the selected detail points one detail level down (from level 1 they become base points), "
                  "keeping the curve's look: the finer detail around them then rides them (setup frame)",
                  MIRRORED, editable_with_selection_poll, [setup_frame](Context &C) {
                    if (!setup_frame(C, "Move to Lower Level")) {
                      return OpStatus::Cancelled;
                    }
                    int lowered = 0, skipped = 0;
                    bool dropped = false;
                    for (Layer *layer : editable_layers(C)) {
                      if (layer->is_reference()) {
                        continue;
                      }
                      int s = 0;
                      bool keys = false;
                      lowered += drawing_detail_lower_selected(layer->drawing, &s, &keys);
                      skipped += s;
                      dropped |= keys;
                    }
                    if (lowered == 0) {
                      report(ReportType::Warning, skipped > 0 ? "These are base points (level 0): they can't go lower"
                                                              : "Select detail points to move down a level");
                      return OpStatus::Cancelled;
                    }
                    std::string text = "Moved " + std::to_string(lowered) + (lowered == 1 ? " point" : " points") +
                                       " down a detail level";
                    if (skipped > 0) {
                      text += "; " + std::to_string(skipped) + (skipped == 1 ? " is a base point" : " are base points");
                    }
                    if (dropped) {
                      text += " (their keys were removed)";
                    }
                    report(dropped ? ReportType::Warning : ReportType::Info, text);
                    return OpStatus::Finished;
                  });
  vector_operator("mesh.detail_apply", "Apply Detail",
                  "Make the selected shapes' detail points ordinary points, as they show (setup frame)", MIRRORED,
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
                           "Applied " + std::to_string(changed) + (changed == 1 ? " detail point" : " detail points") +
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
                    if (made == 0) {
                      report(ReportType::Info, "No point is close enough to the curve without it to be detail: "
                                               "raise the Tolerance, or add detail with the knife (K)");
                      return OpStatus::Cancelled;
                    }
                    report(dropped ? ReportType::Warning : ReportType::Info,
                           std::to_string(made) + (made == 1 ? " point became detail" : " points became detail") +
                               (dropped ? " (their keys were removed)" : ""));
                    return made > 0 ? OpStatus::Finished : OpStatus::Cancelled;
                  });

  /* Blender's Duplicate (Shift D): copy, then move the copy. Cancelling the move leaves the
   * copy where it was made, as in Blender. */
  vector_operator("mesh.duplicate_move", "Duplicate", "Copy the selected points, curves and shapes, then move the copy",
                  MIRRORED, editable_with_selection_poll, [](Context &C) {
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
