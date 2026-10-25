#include "ops/text_ops.hh"

#include <algorithm>
#include <functional>

#include "core/log.hh"
#include "data/document.hh"
#include "data/text.hh"
#include "ops/operator.hh"
#include "text/text_layer.hh"
#include "text/text_outline.hh"
#include "wm/context.hh"

namespace oanim {

static void add_operator(const char *idname, const char *name, const char *description,
                         std::function<bool(Context &)> poll, std::function<OpStatus(Context &)> exec)
{
  OperatorType ot;
  ot.idname = idname;
  ot.name = name;
  ot.description = description;
  ot.flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  ot.poll = std::move(poll);
  ot.exec = std::move(exec);
  operators().add(std::move(ot));
}

static Layer *active_text_layer(Context &C)
{
  Layer *layer = C.document ? document_active_layer(*C.document) : nullptr;
  return layer && layer->is_text() ? layer : nullptr;
}

void text_operators_register()
{
  add_operator(
      "layer.new_text", "New Text",
      "Add a text layer above the active layer saying text= (\"Text\"), in font= (\"Segoe UI\") at size= (0.12: "
      "document units, the frame 2 tall), its first line's top at x=, y= (document space; the middle of the frame)",
      [](Context &C) { return C.document != nullptr; },
      [](Context &C) {
        const std::string font = op_arg(C, "font", "Segoe UI");
        if (!text_font_installed(font)) {
          report(ReportType::Warning, "Font not installed: " + font + " (another is drawn in its place)");
        }
        document_add_text_layer(*C.document, op_arg(C, "text", "Text"),
                                {op_arg_float(C, "x", 0.0f), op_arg_float(C, "y", 0.0f)}, font,
                                op_arg_float(C, "size", 0.12f));
        return OpStatus::Finished;
      });

  add_operator(
      "text.convert_to_vectors", "Convert to Vectors",
      "Turn the active text layer into an ordinary vector layer of its letters, to edit their points (one way, as "
      "Blender's Convert to Curve); split=LETTERS or WORDS makes a vector layer of each in a group instead",
      [](Context &C) { return active_text_layer(C) != nullptr; },
      [](Context &C) {
        Layer *layer = active_text_layer(C);
        const std::string split = op_arg(C, "split", "NONE");
        if (!text_layer_convert_to_vectors(*C.document, layer->id, split)) {
          return OpStatus::Cancelled;
        }
        C.undo_name = split == "LETTERS" ? "Convert to Letters" : split == "WORDS" ? "Convert to Words"
                                                                                   : "Convert to Vectors";
        return OpStatus::Finished;
      });

  add_operator(
      "text.style_set", "Set Text Style",
      "Style positions start= to end= (UTF-16; the whole text when left out) of the active text layer: font=, "
      "size=, weight= (100-900), italic=, bold= (toggles 400/700 when 'TOGGLE'), fill=, outline=",
      [](Context &C) { return active_text_layer(C) != nullptr; },
      [](Context &C) {
        Layer &layer = *active_text_layer(C);
        const int length = text_length(layer.text);
        const int start = std::clamp(op_arg_int(C, "start", 0), 0, length);
        const int end = std::clamp(op_arg_int(C, "end", length), 0, length);
        std::vector<TextRun *> runs = text_runs_between(layer.text, start, end);
        /* Toggles go by the first run: all of it bold unless it already was. */
        const bool bold_now = !runs.empty() && runs.front()->weight >= 600;
        const bool italic_now = !runs.empty() && runs.front()->italic;
        for (TextRun *run : runs) {
          if (op_has_arg(C, "font")) {
            run->font = op_arg(C, "font");
          }
          if (op_has_arg(C, "size")) {
            run->size = std::max(op_arg_float(C, "size", run->size), 1e-4f);
          }
          if (op_has_arg(C, "weight")) {
            run->weight = std::clamp(op_arg_int(C, "weight", run->weight), 1, 999);
          }
          if (op_has_arg(C, "bold")) {
            run->weight = op_arg(C, "bold") == "TOGGLE" ? (bold_now ? 400 : 700) : (op_arg_bool(C, "bold") ? 700 : 400);
          }
          if (op_has_arg(C, "italic")) {
            run->italic = op_arg(C, "italic") == "TOGGLE" ? !italic_now : op_arg_bool(C, "italic");
          }
          if (op_has_arg(C, "fill")) {
            run->has_fill = op_arg_bool(C, "fill");
          }
          if (op_has_arg(C, "outline")) {
            run->has_outline = op_arg_bool(C, "outline");
          }
        }
        text_tidy(layer.text);
        return OpStatus::Finished;
      });
}

}  // namespace oanim
