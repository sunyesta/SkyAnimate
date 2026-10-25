/* Material operators (data/material.hh): make a shape's look a shared material, link shapes to one,
 * unlink them (they keep the look as their own), rename, remove, and remove the unused ones. They act
 * on the selected shapes of the layers being edited, as the Shape panel and the brush dialog do. */

#include <string>

#include "core/log.hh"
#include "data/document.hh"
#include "data/material.hh"
#include "ops/edit_ops.hh"
#include "ops/operator.hh"
#include "wm/context.hh"

namespace oanim {

/* The selected shapes of the layers being edited (not a reference's copies: their looks are the
 * copy's own changes), topmost first in each layer: the first one is the one the Shape panel shows. */
static std::vector<Shape *> material_targets(Context &C)
{
  std::vector<Shape *> shapes;
  for (Layer *layer : editable_layers(C)) {
    if (layer->is_reference()) {
      continue;
    }
    VectorDrawing &drawing = layer->drawing;
    const std::vector<bool> selected = drawing_selected_shapes(drawing);
    for (int s = int(drawing.shapes.size()) - 1; s >= 0; s--) {
      if (selected[size_t(s)]) {
        shapes.push_back(&drawing.shapes[size_t(s)]);
      }
    }
  }
  return shapes;
}

static bool any_material(Context &C)
{
  return C.document && !C.document->materials.empty();
}

static bool have_targets(Context &C)
{
  return C.document && !material_targets(C).empty();
}

/* The material named by `material=` (a name or id), or a report. */
static Material *material_arg(Context &C)
{
  Material *m = material_find_name(*C.document, op_arg(C, "material"));
  if (!m) {
    report(ReportType::Warning, "There's no material \"" + op_arg(C, "material") + "\"");
  }
  return m;
}

/* Whether any of the points along the shape's edges is selected. */
static bool shape_touches_selection(const VectorDrawing &drawing, const Shape &shape)
{
  for (const ShapeEdge &e : shape.edges) {
    if (e.curve < 0 || e.curve >= int(drawing.curves.size())) {
      continue;
    }
    const Curve &curve = drawing.curves[size_t(e.curve)];
    const int n = int(curve.points.size());
    if (n == 0 || e.segment < 0 || e.segment >= n) {
      continue;
    }
    for (const int k : {e.segment, (e.segment + 1) % n}) {
      const int p = curve.points[size_t(k)].point;
      if (p >= 0 && p < int(drawing.points.size()) && drawing.points[size_t(p)].selected) {
        return true;
      }
    }
  }
  return false;
}

/* Link Materials' source: the active shape (the last clicked) on a layer being edited, else the shape the
 * Shape panel shows. */
static Shape *link_source(Context &C, uint64_t *r_layer)
{
  Document &doc = *C.document;
  for (Layer *layer : editable_layers(C)) {
    if (layer->id == doc.active_shape_layer && !layer->is_reference()) {
      const int s = drawing_shape_index(layer->drawing, doc.active_shape);
      if (s >= 0) {
        *r_layer = layer->id;
        return &layer->drawing.shapes[size_t(s)];
      }
    }
  }
  if (Shape *shown = shown_shape(C)) {
    for (Layer *layer : editable_layers(C)) {
      for (const Shape &s : layer->drawing.shapes) {
        if (&s == shown) {
          *r_layer = layer->id;
        }
      }
    }
    return shown;
  }
  return nullptr;
}

void material_operators_register()
{
  {
    /* Ctrl L > Link Materials, as Blender's Link/Transfer Data: the active shape's material (or its own
     * look) to every shape with a point selected. */
    OperatorType ot;
    ot.idname = "material.link_selected";
    ot.name = "Link Materials";
    ot.description = "Give every shape with at least one point selected the active shape's material (the shape "
                     "last clicked), on the layers being edited; a shape with a look of its own passes on that "
                     "look instead";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = [](Context &C) { return C.document && !editable_layers(C).empty(); };
    ot.exec = [](Context &C) {
      uint64_t source_layer = 0;
      const Shape *source_ptr = link_source(C, &source_layer);
      if (!source_ptr) {
        report(ReportType::Warning, "Click a shape to make it the active one, then select the shapes to give its "
                                    "material");
        return OpStatus::Cancelled;
      }
      const Shape source = *source_ptr;
      int count = 0;
      for (Layer *layer : editable_layers(C)) {
        if (layer->is_reference()) {
          continue;
        }
        VectorDrawing &drawing = layer->drawing;
        for (Shape &shape : drawing.shapes) {
          if ((layer->id == source_layer && shape.id == source.id) || !shape_touches_selection(drawing, shape)) {
            continue;
          }
          if (shape.material == source.material && shape.style == source.style) {
            continue;
          }
          if (source.material == 0 && shape.material != 0) {
            const Material *was = material_find(*C.document, shape.material);
            shape.material_last = was ? was->name : shape.material_last;
          }
          shape.material = source.material;
          if (source.material != 0) {
            shape.material_last.clear();
          }
          shape.style = source.style;
          count++;
        }
      }
      if (count == 0) {
        report(ReportType::Info, "Select points of the shapes to give the active shape's material");
        return OpStatus::Cancelled;
      }
      const Material *m = material_find(*C.document, source.material);
      C.undo_name = m ? "Link " + m->name : std::string("Link Look");
      report(ReportType::Info, std::string(m ? "Linked " + m->name + " to " : "Gave its look to ") +
                                   std::to_string(count) + (count == 1 ? " shape" : " shapes"));
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "material.new";
    ot.name = "New Material";
    ot.description = "Make the shape's look a shared material (name=, else one after the material it last came from, "
                     "\"Skin.001\", or the shape's name), linking the selected shapes to it: it sets their fill and "
                     "line colours, line width and brush";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_targets;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const std::vector<Shape *> shapes = material_targets(C);
      if (shapes.empty()) {
        return OpStatus::Cancelled;
      }
      Shape &first = *shapes.front();
      std::string name = op_arg(C, "name");
      if (name.empty()) {
        /* After the material it last came from: unique makes it "Skin.001" while Skin is there. */
        name = !first.material_last.empty() ? first.material_last
               : !first.name.empty()        ? first.name
                                            : std::string("Material");
      }
      const Style style = first.style;
      Material &m = material_add(doc, name, style);
      for (Shape *shape : shapes) {
        shape->material = m.id;
        shape->style = style;
      }
      C.undo_name = "New Material " + m.name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "material.assign";
    ot.name = "Link Material";
    ot.description = "Link the selected shapes to a material (material=, a name or id): they take its look";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_MIRROR;
    ot.poll = [](Context &C) { return any_material(C) && have_targets(C); };
    ot.exec = [](Context &C) {
      Material *m = material_arg(C);
      if (!m) {
        return OpStatus::Cancelled;
      }
      int count = 0;
      for (Shape *shape : material_targets(C)) {
        if (shape->material != m->id || !(shape->style == m->style)) {
          shape->material = m->id;
          shape->style = m->style;
          count++;
        }
      }
      if (count == 0) {
        return OpStatus::Cancelled;
      }
      C.undo_name = "Link " + m->name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "material.unlink";
    ot.name = "Unlink Material";
    ot.description = "Give the selected shapes their look as their own: no longer linked to their material, looking "
                     "the same";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = have_targets;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      int count = 0;
      std::string name;
      for (Shape *shape : material_targets(C)) {
        if (const Material *m = material_find(doc, shape->material)) {
          name = m->name;
          shape->material_last = m->name;
          shape->material = 0;
          count++;
        }
      }
      if (count == 0) {
        return OpStatus::Cancelled;
      }
      C.undo_name = "Unlink " + name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "material.rename";
    ot.name = "Rename Material";
    ot.description = "Rename a material (material=, to name=; made unique as Blender's names are)";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = any_material;
    ot.exec = [](Context &C) {
      Material *m = material_arg(C);
      const std::string wanted = op_arg(C, "name");
      if (!m || wanted.empty()) {
        return OpStatus::Cancelled;
      }
      const std::string name = material_unique_name(*C.document, wanted, m->id);
      if (name == m->name) {
        return OpStatus::Cancelled;
      }
      C.undo_name = "Rename " + m->name + " to " + name;
      m->name = name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "material.remove";
    ot.name = "Delete Material";
    ot.description = "Delete a material (material=): the shapes using it keep its look as their own";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = any_material;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      Material *m = material_arg(C);
      if (!m) {
        return OpStatus::Cancelled;
      }
      const uint64_t id = m->id;
      const std::string name = m->name;
      document_for_each_shape(doc, [&](Layer &, VectorDrawing &, Shape &shape) {
        if (shape.material == id) {
          shape.material = 0;
          shape.material_last = name;
        }
      });
      std::erase_if(doc.materials, [&](const Material &x) { return x.id == id; });
      C.undo_name = "Delete Material " + name;
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
  {
    OperatorType ot;
    ot.idname = "material.remove_unused";
    ot.name = "Remove Unused Materials";
    ot.description = "Delete the materials no shape uses";
    ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
    ot.poll = any_material;
    ot.exec = [](Context &C) {
      Document &doc = *C.document;
      const size_t before = doc.materials.size();
      std::erase_if(doc.materials, [&](const Material &m) { return material_users(doc, m.id) == 0; });
      const size_t removed = before - doc.materials.size();
      if (removed == 0) {
        report(ReportType::Info, "Every material is used");
        return OpStatus::Cancelled;
      }
      report(ReportType::Info, "Removed " + std::to_string(removed) + (removed == 1 ? " material" : " materials"));
      C.undo_name = "Remove Unused Materials";
      return OpStatus::Finished;
    };
    operators().add(std::move(ot));
  }
}

}  // namespace oanim
