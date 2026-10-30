#include "io/pdf_export.hh"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include <miniz.h>

#include "anim/animation.hh"
#include "data/document.hh"
#include "data/layer.hh"
#include "io/vector_export.hh"

namespace oanim {

namespace {

/* A pixel in points (96 pixels to the inch). */
constexpr float POINTS_PER_PIXEL = 0.75f;

/* A text string as UTF-16 with its byte order mark, in hex (layer names in any script). */
std::string pdf_text(const std::string &utf8)
{
  std::string out = "<FEFF";
  const auto unit = [&](uint32_t u) {
    char text[8];
    std::snprintf(text, sizeof(text), "%04X", u);
    out += text;
  };
  for (size_t i = 0; i < utf8.size();) {
    const uint8_t c = uint8_t(utf8[i]);
    uint32_t code = c;
    size_t length = 1;
    if (c >= 0xF0 && i + 3 < utf8.size() + 0) {
      code = (c & 0x07u) << 18 | (uint8_t(utf8[i + 1]) & 0x3Fu) << 12 | (uint8_t(utf8[i + 2]) & 0x3Fu) << 6 |
             (uint8_t(utf8[i + 3]) & 0x3Fu);
      length = 4;
    }
    else if (c >= 0xE0 && i + 2 < utf8.size()) {
      code = (c & 0x0Fu) << 12 | (uint8_t(utf8[i + 1]) & 0x3Fu) << 6 | (uint8_t(utf8[i + 2]) & 0x3Fu);
      length = 3;
    }
    else if (c >= 0xC0 && i + 1 < utf8.size()) {
      code = (c & 0x1Fu) << 6 | (uint8_t(utf8[i + 1]) & 0x3Fu);
      length = 2;
    }
    i += length;
    if (code >= 0x10000) {
      code -= 0x10000;
      unit(0xD800 + (code >> 10));
      unit(0xDC00 + (code & 0x3FF));
    }
    else {
      unit(code);
    }
  }
  return out + ">";
}

std::string deflate(const std::string &data)
{
  mz_ulong size = mz_compressBound(mz_ulong(data.size()));
  std::string out(size_t(size), '\0');
  if (mz_compress2(reinterpret_cast<unsigned char *>(out.data()), &size,
                   reinterpret_cast<const unsigned char *>(data.data()), mz_ulong(data.size()), 6) != MZ_OK)
  {
    return {};
  }
  out.resize(size_t(size));
  return out;
}

class PdfWriter {
 public:
  explicit PdfWriter(const Document &doc) : doc_(doc)
  {
    out_ = "%PDF-1.7\n%\xE2\xE3\xCF\xD3\n";
    catalog_ = reserve();
    pages_ = reserve();
    resources_ = reserve();
    layers();
  }

  /* One page: a frame's scene. */
  void page(const VectorScene &scene)
  {
    scene_ = &scene;
    smasks_.assign(scene.masks.size(), 0);
    images_.assign(scene.pictures.size(), std::string());
    const float w = float(scene.width) * POINTS_PER_PIXEL, h = float(scene.height) * POINTS_PER_PIXEL;
    /* Drawn in pixels, y down. */
    std::string content = num(POINTS_PER_PIXEL) + " 0 0 " + num(-POINTS_PER_PIXEL) + " 0 " + num(h) + " cm\n";
    nodes(scene.nodes, content);
    const int contents = stream_object("", content);
    const int page = reserve();
    begin(page);
    out_ += "<< /Type /Page /Parent " + ref(pages_) + " /MediaBox [0 0 " + num(w) + " " + num(h) +
            "] /Resources " + ref(resources_) + " /Contents " + ref(contents) +
            " /Group << /S /Transparency /CS /DeviceRGB >> >>";
    end();
    page_ids_.push_back(page);
    report.masks += int(scene.masks.size());
    report.brushed += scene.brushed;
    report.pictures += int(scene.pictures.size()) - scene.brushed;
    scene_ = nullptr;
  }

  std::string finish()
  {
    begin(resources_);
    out_ += "<< /ExtGState <<" + ext_states_ + " >> /XObject <<" + xobjects_ + " >> /Properties <<" + properties_ +
            " >> >>";
    end();
    begin(pages_);
    out_ += "<< /Type /Pages /Count " + std::to_string(page_ids_.size()) + " /Kids [";
    for (const int id : page_ids_) {
      out_ += " " + ref(id);
    }
    out_ += " ] >>";
    end();
    begin(catalog_);
    out_ += "<< /Type /Catalog /Pages " + ref(pages_);
    if (!ocgs_.empty()) {
      std::string all, off;
      for (const auto &[id, ocg] : ocgs_) {
        all += " " + ref(ocg);
        const Layer *l = layer_find(doc_, id);
        if (l && !(l->visible && l->render_visible)) {
          off += " " + ref(ocg);
        }
      }
      out_ += " /OCProperties << /OCGs [" + all + " ] /D << /Name " + pdf_text("Layers") + " /Order [" + order_ +
              " ] /OFF [" + off + " ] /ListMode /AllPages >> >> /PageMode /UseOC";
    }
    out_ += " >>";
    end();
    const size_t xref = out_.size();
    out_ += "xref\n0 " + std::to_string(offsets_.size() + 1) + "\n0000000000 65535 f \n";
    for (const size_t offset : offsets_) {
      char line[24];
      std::snprintf(line, sizeof(line), "%010zu 00000 n \n", offset);
      out_ += line;
    }
    out_ += "trailer\n<< /Size " + std::to_string(offsets_.size() + 1) + " /Root " + ref(catalog_) +
            " >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    report.pages = int(page_ids_.size());
    report.layers = int(ocgs_.size());
    report.bytes = out_.size();
    return std::move(out_);
  }

  PdfExportReport report;

 private:
  const Document &doc_;
  std::string out_;
  std::vector<size_t> offsets_;
  int catalog_ = 0, pages_ = 0, resources_ = 0;
  std::vector<int> page_ids_;
  /* The resources every content stream shares, by name. */
  std::string ext_states_, xobjects_, properties_;
  int next_name_ = 1;
  std::map<std::pair<int, int>, std::string> states_;
  /* Each layer's optional content group, and each drawn layer's membership (it and its groups all on). */
  std::map<uint64_t, int> ocgs_;
  std::map<uint64_t, std::string> memberships_;
  std::string order_;
  /* This page's scene, its masks' soft mask objects (0: not written yet) and pictures' image names. */
  const VectorScene *scene_ = nullptr;
  std::vector<int> smasks_;
  std::vector<std::string> images_;

  static std::string num(float v) { return vector_number(v); }
  static std::string ref(int id) { return std::to_string(id) + " 0 R"; }

  int reserve()
  {
    offsets_.push_back(0);
    return int(offsets_.size());
  }
  void begin(int id)
  {
    offsets_[size_t(id - 1)] = out_.size();
    out_ += std::to_string(id) + " 0 obj\n";
  }
  void end() { out_ += "\nendobj\n"; }

  /* A compressed stream object with `dict` entries. */
  int stream_object(const std::string &dict, const std::string &data)
  {
    const std::string packed = deflate(data);
    const int id = reserve();
    begin(id);
    out_ += "<<" + dict + " /Length " + std::to_string(packed.size()) + " /Filter /FlateDecode >>\nstream\n";
    out_ += packed;
    out_ += "\nendstream";
    end();
    return id;
  }

  std::string new_name(const char *prefix) { return prefix + std::to_string(next_name_++); }

  /* --- Layers --------------------------------------------------------------------------------- */

  /* An optional content group for every layer, and the layer list as the Layers editor nests it. */
  void layers()
  {
    std::function<std::string(const std::vector<Layer> &)> list = [&](const std::vector<Layer> &scope) {
      std::string order;
      for (const Layer &l : scope) {
        const int ocg = reserve();
        begin(ocg);
        out_ += "<< /Type /OCG /Name " + pdf_text(l.name) + " >>";
        end();
        ocgs_[l.id] = ocg;
        order += " " + ref(ocg);
        if (l.is_group() && !l.children.empty()) {
          order += " [" + list(l.children) + " ]";
        }
      }
      return order;
    };
    order_ = list(doc_.layers);
  }

  /* What marks a layer's content: shown while it and every group holding it are on. */
  std::string membership(uint64_t layer)
  {
    auto it = memberships_.find(layer);
    if (it != memberships_.end()) {
      return it->second;
    }
    std::string chain;
    for (const Layer *l = layer_find(doc_, layer); l; l = layer_parent(const_cast<Document &>(doc_), l->id)) {
      auto ocg = ocgs_.find(l->id);
      if (ocg != ocgs_.end()) {
        chain = " " + ref(ocg->second) + chain;
      }
    }
    std::string name;
    if (!chain.empty()) {
      const int id = reserve();
      begin(id);
      out_ += "<< /Type /OCMD /OCGs [" + chain + " ] /P /AllOn >>";
      end();
      name = new_name("L");
      properties_ += " /" + name + " " + ref(id);
    }
    memberships_.emplace(layer, name);
    return name;
  }

  /* --- Content -------------------------------------------------------------------------------- */

  /* A graphics state fading by `alpha` and masked by `mask` (-1: none). */
  std::string state(float alpha, int mask)
  {
    const int a = int(std::lround(std::clamp(alpha, 0.0f, 1.0f) * 1000.0f));
    const int smask = mask >= 0 ? soft_mask(mask) : 0;
    const auto key = std::make_pair(a, smask);
    auto it = states_.find(key);
    if (it != states_.end()) {
      return it->second;
    }
    const std::string name = new_name("G");
    const int id = reserve();
    begin(id);
    out_ += "<< /Type /ExtGState /ca " + num(float(a) / 1000.0f) + " /CA " + num(float(a) / 1000.0f) +
            " /SMask " + (smask ? ref(smask) : std::string("/None")) + " >>";
    end();
    ext_states_ += " /" + name + " " + ref(id);
    states_.emplace(key, name);
    return name;
  }

  /* A form drawing `content` (pixels) as a transparency group: its object, and by name for drawing it. */
  int form_object(const std::string &content, bool luminosity)
  {
    const float w = float(scene_->width), h = float(scene_->height);
    const int id = stream_object(" /Type /XObject /Subtype /Form /BBox [" + num(-w) + " " + num(-h) + " " +
                                     num(2.0f * w) + " " + num(2.0f * h) + "] /Resources " + ref(resources_) +
                                     " /Group << /S /Transparency" +
                                     (luminosity ? std::string(" /CS /DeviceRGB") : std::string()) + " >>",
                                 content);
    return id;
  }
  std::string form(const std::string &content, bool luminosity)
  {
    const std::string name = new_name("F");
    xobjects_ += " /" + name + " " + ref(form_object(content, luminosity));
    return name;
  }

  /* A mask as a soft mask: its content's luminance on black. */
  int soft_mask(int mask)
  {
    if (smasks_[size_t(mask)]) {
      return smasks_[size_t(mask)];
    }
    std::string content;
    nodes(scene_->masks[size_t(mask)].content, content);
    const int form_id = form_object(content, true);
    const int id = reserve();
    begin(id);
    out_ += "<< /Type /Mask /S /Luminosity /G " + ref(form_id) + " /BC [0 0 0] >>";
    end();
    smasks_[size_t(mask)] = id;
    return id;
  }

  std::string image(int picture)
  {
    if (!images_[size_t(picture)].empty()) {
      return images_[size_t(picture)];
    }
    const VectorPicture &p = scene_->pictures[size_t(picture)];
    std::string rgb, alpha;
    rgb.reserve(size_t(p.width) * size_t(p.height) * 3);
    alpha.reserve(size_t(p.width) * size_t(p.height));
    bool opaque = true;
    for (size_t i = 0; i + 3 < p.rgba.size(); i += 4) {
      rgb.append(reinterpret_cast<const char *>(&p.rgba[i]), 3);
      alpha += char(p.rgba[i + 3]);
      opaque = opaque && p.rgba[i + 3] == 255;
    }
    const std::string size = " /Width " + std::to_string(p.width) + " /Height " + std::to_string(p.height) +
                             " /BitsPerComponent 8";
    std::string smask;
    if (!opaque) {
      smask = " /SMask " + ref(stream_object(" /Type /XObject /Subtype /Image /ColorSpace /DeviceGray" + size, alpha));
    }
    const int id = stream_object(" /Type /XObject /Subtype /Image /ColorSpace /DeviceRGB" + size + smask, rgb);
    const std::string name = new_name("I");
    xobjects_ += " /" + name + " " + ref(id);
    images_[size_t(picture)] = name;
    return name;
  }

  void path(const VectorPath &path, std::string &out)
  {
    size_t p = 0;
    const auto point = [&]() {
      const float2 v = path.points[p++];
      return num(v.x) + " " + num(v.y);
    };
    for (const VectorPath::Op op : path.ops) {
      switch (op) {
        case VectorPath::Move:
          out += point() + " m\n";
          break;
        case VectorPath::Line:
          out += point() + " l\n";
          break;
        case VectorPath::Cubic: {
          const std::string a = point(), b = point(), c = point();
          out += a + " " + b + " " + c + " c\n";
          break;
        }
        case VectorPath::Close:
          out += "h\n";
          break;
      }
    }
  }

  void nodes(const std::vector<VectorNode> &list, std::string &out)
  {
    for (const VectorNode &n : list) {
      node(n, out);
    }
  }

  void node(const VectorNode &n, std::string &out)
  {
    switch (n.kind) {
      case VectorNode::Path: {
        if (n.path.empty()) {
          return;
        }
        const float alpha = n.color.a * n.alpha;
        out += "q\n";
        if (alpha < 0.9995f || n.mask >= 0) {
          out += "/" + state(alpha, n.mask) + " gs\n";
        }
        out += num(n.color.r) + " " + num(n.color.g) + " " + num(n.color.b) + " rg\n";
        path(n.path, out);
        out += n.even_odd ? "f*\nQ\n" : "f\nQ\n";
        return;
      }
      case VectorNode::Picture: {
        const std::string name = image(n.picture);
        /* An image fills the unit square, its first row at the top (1); the node's square has it at 0. */
        const Transform2D &t = n.place;
        out += "q\n";
        if (n.alpha < 0.9995f || n.mask >= 0) {
          out += "/" + state(n.alpha, n.mask) + " gs\n";
        }
        out += num(t.a) + " " + num(t.b) + " " + num(-t.c) + " " + num(-t.d) + " " + num(t.tx + t.c) + " " +
               num(t.ty + t.d) + " cm\n/" + name + " Do\nQ\n";
        return;
      }
      case VectorNode::Group: {
        const std::string marked = n.layer ? membership(n.layer) : std::string();
        if (!marked.empty()) {
          out += "/OC /" + marked + " BDC\n";
        }
        if (n.mask >= 0 || n.opacity < 0.9995f) {
          /* Faded or masked as one: a transparency group. */
          std::string inner;
          nodes(n.children, inner);
          const std::string name = form(inner, false);
          out += "q\n/" + state(n.opacity, n.mask) + " gs\n/" + name + " Do\nQ\n";
        }
        else {
          nodes(n.children, out);
        }
        if (!marked.empty()) {
          out += "EMC\n";
        }
        return;
      }
    }
  }
};

}  // namespace

bool pdf_export(const Document &doc, const std::string &path, const PdfExport &options, std::string *r_error,
                PdfExportReport *r_report)
{
  VectorSceneOptions scene_options;
  scene_options.width = options.width;
  scene_options.height = options.height;
  scene_options.transparent = options.transparent;
  scene_options.hidden_layers = true;
  PdfWriter writer(doc);
  if (options.frame_first > options.frame_last) {
    writer.page(vector_scene(doc, scene_options));
  }
  else {
    /* Each frame worked out on a copy, so the document stays at its own frame. */
    auto copy = std::make_unique<Document>(doc);
    for (int frame = options.frame_first; frame <= options.frame_last; frame++) {
      copy->frame_current = frame;
      animation_sync(*copy);
      writer.page(vector_scene(*copy, scene_options));
    }
  }
  const std::string text = writer.finish();
  std::ofstream out(std::filesystem::path(std::u8string(path.begin(), path.end())),
                    std::ios::binary | std::ios::trunc);
  out.write(text.data(), std::streamsize(text.size()));
  out.close();
  if (!out) {
    if (r_error) {
      *r_error = "Could not write " + path;
    }
    return false;
  }
  if (r_report) {
    *r_report = writer.report;
  }
  return true;
}

}  // namespace oanim
