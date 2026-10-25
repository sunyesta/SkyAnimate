#include "text/text_outline.hh"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>

#include <windows.h>

#include <d2d1.h>
#include <dwrite_1.h>
#include <wrl/client.h>

namespace oanim {

using Microsoft::WRL::ComPtr;

/* Text is laid out at this many DirectWrite units (DIPs) to a document unit, so sizes of a tenth of a
 * unit lay out as 100-unit fonts do, with whole-unit hinting nowhere near mattering. */
static constexpr float SCALE = 1000.0f;

static IDWriteFactory *factory()
{
  static ComPtr<IDWriteFactory> f = [] {
    ComPtr<IDWriteFactory> made;
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown **>(made.GetAddressOf()));
    return made;
  }();
  return f.Get();
}

static IDWriteFontCollection *system_fonts()
{
  static ComPtr<IDWriteFontCollection> fonts = [] {
    ComPtr<IDWriteFontCollection> got;
    if (factory()) {
      factory()->GetSystemFontCollection(got.GetAddressOf());
    }
    return got;
  }();
  return fonts.Get();
}

static std::wstring wide(const std::string &s)
{
  const std::u16string u = utf8_to_utf16(s);
  return std::wstring(u.begin(), u.end());
}

static std::string narrow(const wchar_t *s, size_t n)
{
  return utf16_to_utf8(std::u16string(s, s + n));
}

/* A text's layout, kept while the text is unchanged (the same layout answers every caret and hit). */
struct Layout {
  ComPtr<IDWriteTextLayout> layout;
  std::u16string text;
  /* Where each run starts in `text`. */
  std::vector<int> run_start;
  /* Added to every x: point text is aligned about the layer's origin. */
  float shift_x = 0.0f;
};

static std::string layout_key(const TextSettings &text)
{
  std::string key;
  const auto put = [&](const void *data, size_t size) { key.append(static_cast<const char *>(data), size); };
  for (const TextRun &run : text.runs) {
    const size_t n = run.text.size(), f = run.font.size();
    put(&n, sizeof(n));
    key += run.text;
    put(&f, sizeof(f));
    key += run.font;
    put(&run.weight, sizeof(run.weight));
    put(&run.italic, sizeof(run.italic));
    put(&run.size, sizeof(run.size));
  }
  put(&text.align, sizeof(text.align));
  put(&text.line_spacing, sizeof(text.line_spacing));
  put(&text.letter_spacing, sizeof(text.letter_spacing));
  put(&text.word_spacing, sizeof(text.word_spacing));
  put(&text.box_width, sizeof(text.box_width));
  return key;
}

static DWRITE_FONT_WEIGHT font_weight(int weight)
{
  return DWRITE_FONT_WEIGHT(std::clamp(weight, 1, 999));
}

static std::shared_ptr<Layout> make_layout(const TextSettings &text)
{
  auto out = std::make_shared<Layout>();
  IDWriteFactory *f = factory();
  if (!f) {
    return out;
  }
  const TextRun first = text.runs.empty() ? TextRun() : text.runs.front();
  for (const TextRun &run : text.runs) {
    out->run_start.push_back(int(out->text.size()));
    out->text += utf8_to_utf16(run.text);
  }
  ComPtr<IDWriteTextFormat> format;
  if (FAILED(f->CreateTextFormat(wide(first.font).c_str(), nullptr, font_weight(first.weight),
                                 first.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL,
                                 DWRITE_FONT_STRETCH_NORMAL, std::max(first.size, 1e-4f) * SCALE, L"en-us",
                                 format.GetAddressOf())))
  {
    return out;
  }
  const bool boxed = text.box_width > 0.0f;
  format->SetWordWrapping(boxed ? DWRITE_WORD_WRAPPING_WRAP : DWRITE_WORD_WRAPPING_NO_WRAP);
  /* Proportional (Windows 10 on): the font's own line height, times the spacing; the first line stays
   * where it was. */
  format->SetLineSpacing(DWRITE_LINE_SPACING_METHOD(2), std::max(text.line_spacing, 0.01f), 1.0f);
  const float width = boxed ? text.box_width * SCALE : 1e7f;
  const wchar_t *chars = reinterpret_cast<const wchar_t *>(out->text.c_str());
  if (FAILED(f->CreateTextLayout(chars, UINT32(out->text.size()), format.Get(), width, 1e7f,
                                 out->layout.GetAddressOf())))
  {
    return out;
  }
  IDWriteTextLayout *layout = out->layout.Get();
  ComPtr<IDWriteTextLayout1> layout1;
  out->layout.As(&layout1);
  for (size_t r = 0; r < text.runs.size(); r++) {
    const TextRun &run = text.runs[r];
    const int start = out->run_start[r];
    const int length = int(out->text.size()) - start -
                       (r + 1 < text.runs.size() ? int(out->text.size()) - out->run_start[r + 1] : 0);
    if (length <= 0) {
      continue;
    }
    const DWRITE_TEXT_RANGE range{UINT32(start), UINT32(length)};
    layout->SetFontFamilyName(wide(run.font).c_str(), range);
    layout->SetFontWeight(font_weight(run.weight), range);
    layout->SetFontStyle(run.italic ? DWRITE_FONT_STYLE_ITALIC : DWRITE_FONT_STYLE_NORMAL, range);
    layout->SetFontSize(std::max(run.size, 1e-4f) * SCALE, range);
    if (layout1 && (text.letter_spacing != 0.0f || text.word_spacing != 0.0f)) {
      const float em = std::max(run.size, 1e-4f) * SCALE;
      layout1->SetCharacterSpacing(0.0f, text.letter_spacing * em, 0.0f, range);
      if (text.word_spacing != 0.0f) {
        for (int k = start; k < start + length; k++) {
          if (out->text[size_t(k)] == u' ') {
            layout1->SetCharacterSpacing(0.0f, (text.letter_spacing + text.word_spacing) * em, 0.0f,
                                         DWRITE_TEXT_RANGE{UINT32(k), 1});
          }
        }
      }
    }
  }
  static const DWRITE_TEXT_ALIGNMENT ALIGN[] = {DWRITE_TEXT_ALIGNMENT_LEADING, DWRITE_TEXT_ALIGNMENT_CENTER,
                                                DWRITE_TEXT_ALIGNMENT_TRAILING, DWRITE_TEXT_ALIGNMENT_JUSTIFIED};
  if (boxed) {
    layout->SetTextAlignment(ALIGN[int(text.align)]);
  }
  else {
    /* Point text: as wide as its longest line, aligned in that, the whole put about the origin. */
    DWRITE_TEXT_METRICS metrics{};
    layout->GetMetrics(&metrics);
    const float w = std::max(metrics.widthIncludingTrailingWhitespace, 1e-3f);
    layout->SetMaxWidth(w);
    if (text.align == TextAlign::Center || text.align == TextAlign::Right) {
      layout->SetTextAlignment(ALIGN[int(text.align)]);
      out->shift_x = text.align == TextAlign::Center ? -w * 0.5f : -w;
    }
  }
  return out;
}

static std::shared_ptr<Layout> layout_of(const TextSettings &text)
{
  static std::unordered_map<std::string, std::shared_ptr<Layout>> cache;
  const std::string key = layout_key(text);
  if (auto found = cache.find(key); found != cache.end()) {
    return found->second;
  }
  if (cache.size() > 64) {
    cache.clear();
  }
  return cache.emplace(key, make_layout(text)).first->second;
}

static float2 to_layer(const Layout &l, float x, float y)
{
  return {(x + l.shift_x) / SCALE, -y / SCALE};
}

/* --- Outlines ---------------------------------------------------------------------------------- */

namespace {

struct Segment {
  float2 a, c1, c2, b;
};
using Figure = std::vector<Segment>;

/* Collects a glyph's figures (DirectWrite hands outlines to a Direct2D geometry sink). */
class OutlineSink : public ID2D1SimplifiedGeometrySink {
 public:
  std::vector<Figure> figures;

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
  {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(ID2D1SimplifiedGeometrySink)) {
      *object = this;
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  /* Lives on the stack for one call: nothing keeps it. */
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  void STDMETHODCALLTYPE SetFillMode(D2D1_FILL_MODE) override {}
  void STDMETHODCALLTYPE SetSegmentFlags(D2D1_PATH_SEGMENT) override {}
  void STDMETHODCALLTYPE BeginFigure(D2D1_POINT_2F start, D2D1_FIGURE_BEGIN) override
  {
    figures.emplace_back();
    at_ = {start.x, start.y};
    start_ = at_;
  }
  void STDMETHODCALLTYPE AddLines(const D2D1_POINT_2F *points, UINT32 count) override
  {
    for (UINT32 i = 0; i < count; i++) {
      line_to({points[i].x, points[i].y});
    }
  }
  void STDMETHODCALLTYPE AddBeziers(const D2D1_BEZIER_SEGMENT *beziers, UINT32 count) override
  {
    for (UINT32 i = 0; i < count; i++) {
      const float2 b(beziers[i].point3.x, beziers[i].point3.y);
      figures.back().push_back(
          {at_, {beziers[i].point1.x, beziers[i].point1.y}, {beziers[i].point2.x, beziers[i].point2.y}, b});
      at_ = b;
    }
  }
  void STDMETHODCALLTYPE EndFigure(D2D1_FIGURE_END) override
  {
    /* Every figure of a glyph closes: straight back to its start, if it isn't there. */
    if (distance(at_, start_) > 1e-4f) {
      line_to(start_);
    }
    if (figures.back().size() < 2) {
      figures.pop_back();
    }
  }
  HRESULT STDMETHODCALLTYPE Close() override { return S_OK; }

 private:
  void line_to(float2 b)
  {
    if (distance(at_, b) < 1e-5f) {
      return;
    }
    figures.back().push_back({at_, at_ + (b - at_) / 3.0f, at_ + (b - at_) * (2.0f / 3.0f), b});
    at_ = b;
  }
  float2 at_, start_;
};

/* Walks a layout's glyph runs, making a shape of each glyph. */
class OutlineRenderer : public IDWriteTextRenderer {
 public:
  OutlineRenderer(const TextSettings &text, const Layout &layout, VectorDrawing &drawing, std::vector<int> *chars)
      : text_(text), layout_(layout), drawing_(drawing), chars_(chars)
  {
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **object) override
  {
    if (riid == __uuidof(IUnknown) || riid == __uuidof(IDWritePixelSnapping) ||
        riid == __uuidof(IDWriteTextRenderer))
    {
      *object = this;
      return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
  }
  ULONG STDMETHODCALLTYPE AddRef() override { return 1; }
  ULONG STDMETHODCALLTYPE Release() override { return 1; }
  HRESULT STDMETHODCALLTYPE IsPixelSnappingDisabled(void *, BOOL *disabled) override
  {
    *disabled = TRUE;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetCurrentTransform(void *, DWRITE_MATRIX *m) override
  {
    *m = {1, 0, 0, 1, 0, 0};
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE GetPixelsPerDip(void *, FLOAT *ppd) override
  {
    *ppd = 1.0f;
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawUnderline(void *, FLOAT, FLOAT, const DWRITE_UNDERLINE *, IUnknown *) override
  {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawStrikethrough(void *, FLOAT, FLOAT, const DWRITE_STRIKETHROUGH *,
                                              IUnknown *) override
  {
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE DrawInlineObject(void *, FLOAT, FLOAT, IDWriteInlineObject *, BOOL, BOOL,
                                             IUnknown *) override
  {
    return S_OK;
  }

  HRESULT STDMETHODCALLTYPE DrawGlyphRun(void *, FLOAT x, FLOAT y, DWRITE_MEASURING_MODE, const DWRITE_GLYPH_RUN *run,
                                         const DWRITE_GLYPH_RUN_DESCRIPTION *description, IUnknown *) override
  {
    const bool rtl = (run->bidiLevel & 1) != 0;
    float pen = x;
    for (UINT32 g = 0; g < run->glyphCount; g++) {
      const float advance = run->glyphAdvances ? run->glyphAdvances[g] : 0.0f;
      const DWRITE_GLYPH_OFFSET offset = run->glyphOffsets ? run->glyphOffsets[g] : DWRITE_GLYPH_OFFSET{0, 0};
      OutlineSink sink;
      const UINT16 index = run->glyphIndices[g];
      const FLOAT zero = 0.0f;
      if (SUCCEEDED(run->fontFace->GetGlyphRunOutline(run->fontEmSize, &index, &zero, &offset, 1, run->isSideways,
                                                      rtl, &sink)) &&
          !sink.figures.empty())
      {
        add_glyph(sink.figures, pen, y, glyph_char(description, g));
      }
      pen += rtl ? -advance : advance;
    }
    return S_OK;
  }

 private:
  /* The text position (UTF-16, whole text) of the character a glyph shows. */
  static int glyph_char(const DWRITE_GLYPH_RUN_DESCRIPTION *description, UINT32 glyph)
  {
    if (!description || !description->clusterMap) {
      return description ? int(description->textPosition) : 0;
    }
    int found = 0;
    for (UINT32 c = 0; c < description->stringLength; c++) {
      if (description->clusterMap[c] <= glyph) {
        found = int(c);
      }
    }
    return int(description->textPosition) + found;
  }

  void add_glyph(const std::vector<Figure> &figures, float origin_x, float baseline, int char_pos)
  {
    int run_index = 0;
    for (size_t r = 0; r < layout_.run_start.size(); r++) {
      if (layout_.run_start[r] <= char_pos) {
        run_index = int(r);
      }
    }
    const TextRun &run = text_.runs.empty() ? fallback_ : text_.runs[size_t(run_index)];
    const auto place = [&](float2 p) {
      p.x += origin_x;
      p.y += baseline;
      /* The slant: across by its height above the baseline (y runs down here). */
      p.x += text_.shear * (baseline - p.y);
      return to_layer(layout_, p.x, p.y);
    };
    Shape shape;
    shape.id = drawing_.next_id++;
    shape.fill_rule = FillRule::NonZero;
    shape.has_fill = run.has_fill;
    shape.has_outline = run.has_outline;
    shape.style = run.style;
    shape.material = run.material;
    shape.material_last = run.material_last;
    /* Named after its letter (a surrogate pair whole). */
    size_t n = 1;
    if (char_pos + 1 < int(layout_.text.size()) && layout_.text[size_t(char_pos)] >= 0xD800 &&
        layout_.text[size_t(char_pos)] <= 0xDBFF)
    {
      n = 2;
    }
    shape.name = char_pos < int(layout_.text.size()) ? utf16_to_utf8(layout_.text.substr(size_t(char_pos), n)) : "";
    for (const Figure &figure : figures) {
      Curve curve;
      curve.closed = true;
      const int count = int(figure.size());
      for (int k = 0; k < count; k++) {
        const Segment &seg = figure[size_t(k)];
        const Segment &prev = figure[size_t((k + count - 1) % count)];
        const float2 p = place(seg.a);
        MeshPoint point;
        point.pos = p;
        point.setup_pos = p;
        point.id = drawing_.next_id++;
        drawing_.points.push_back(point);
        CurvePoint cp;
        cp.point = int(drawing_.points.size()) - 1;
        cp.handles_set = true;
        cp.handle_out = place(seg.c1) - p;
        cp.handle_in = place(prev.c2) - p;
        cp.setup_in = cp.handle_in;
        cp.setup_out = cp.handle_out;
        /* Smooth where the curve runs through in a line, sharp at corners (for how a line turns there). */
        const float li = length(cp.handle_in), lo = length(cp.handle_out);
        const bool smooth = li > 1e-7f && lo > 1e-7f && dot(cp.handle_in / li, cp.handle_out / lo) < -0.995f;
        cp.curvature = smooth ? CURVATURE_SMOOTH : CURVATURE_PEAKED;
        curve.points.push_back(cp);
      }
      drawing_.curves.push_back(std::move(curve));
      const int c = int(drawing_.curves.size()) - 1;
      for (int s = 0; s < count; s++) {
        shape.edges.push_back({c, s});
      }
    }
    drawing_.shapes.push_back(std::move(shape));
    if (chars_) {
      chars_->push_back(char_pos);
    }
  }

  const TextSettings &text_;
  const Layout &layout_;
  VectorDrawing &drawing_;
  std::vector<int> *chars_;
  TextRun fallback_;
};

}  // namespace

VectorDrawing text_outline_drawing(const TextSettings &text, std::vector<int> *r_chars)
{
  VectorDrawing drawing;
  const std::shared_ptr<Layout> l = layout_of(text);
  if (!l->layout) {
    return drawing;
  }
  OutlineRenderer renderer(text, *l, drawing, r_chars);
  l->layout->Draw(nullptr, &renderer, 0.0f, 0.0f);
  return drawing;
}

/* --- Carets and hits ------------------------------------------------------------------------- */

TextCaret text_caret(const TextSettings &text, int pos)
{
  const std::shared_ptr<Layout> l = layout_of(text);
  if (!l->layout) {
    return {{0.0f, 0.0f}, {0.0f, -std::max(text.runs.empty() ? 0.12f : text.runs.front().size, 1e-3f)}};
  }
  FLOAT x = 0.0f, y = 0.0f;
  DWRITE_HIT_TEST_METRICS m{};
  l->layout->HitTestTextPosition(UINT32(std::clamp(pos, 0, int(l->text.size()))), FALSE, &x, &y, &m);
  return {to_layer(*l, x, m.top), to_layer(*l, x, m.top + m.height)};
}

int text_hit(const TextSettings &text, float2 p)
{
  const std::shared_ptr<Layout> l = layout_of(text);
  if (!l->layout) {
    return 0;
  }
  BOOL trailing = FALSE, inside = FALSE;
  DWRITE_HIT_TEST_METRICS m{};
  l->layout->HitTestPoint(p.x * SCALE - l->shift_x, -p.y * SCALE, &trailing, &inside, &m);
  return int(m.textPosition) + (trailing ? int(m.length) : 0);
}

std::vector<TextBox> text_selection(const TextSettings &text, int from, int to)
{
  std::vector<TextBox> out;
  const std::shared_ptr<Layout> l = layout_of(text);
  if (from > to) {
    std::swap(from, to);
  }
  if (!l->layout || to <= from) {
    return out;
  }
  UINT32 count = 0;
  l->layout->HitTestTextRange(UINT32(from), UINT32(to - from), 0.0f, 0.0f, nullptr, 0, &count);
  std::vector<DWRITE_HIT_TEST_METRICS> metrics(count);
  if (count == 0 ||
      FAILED(l->layout->HitTestTextRange(UINT32(from), UINT32(to - from), 0.0f, 0.0f, metrics.data(), count, &count)))
  {
    return out;
  }
  for (const DWRITE_HIT_TEST_METRICS &m : metrics) {
    const float2 a = to_layer(*l, m.left, m.top), b = to_layer(*l, m.left + m.width, m.top + m.height);
    out.push_back({{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}});
  }
  return out;
}

static std::vector<DWRITE_LINE_METRICS> line_metrics(const Layout &l)
{
  UINT32 count = 0;
  l.layout->GetLineMetrics(nullptr, 0, &count);
  std::vector<DWRITE_LINE_METRICS> lines(count);
  if (count > 0) {
    l.layout->GetLineMetrics(lines.data(), count, &count);
  }
  return lines;
}

/* The line `pos` is on, and where that line starts. */
static int line_of(const std::vector<DWRITE_LINE_METRICS> &lines, int pos, int *r_start)
{
  int start = 0;
  for (int i = 0; i < int(lines.size()); i++) {
    /* A line holds its positions up to its end (after a new line, the next line begins). */
    const int length = int(lines[size_t(i)].length);
    if (pos < start + length || i + 1 == int(lines.size())) {
      *r_start = start;
      return i;
    }
    start += length;
  }
  *r_start = start - (lines.empty() ? 0 : int(lines.back().length));
  return std::max(int(lines.size()) - 1, 0);
}

int text_line_start(const TextSettings &text, int pos)
{
  const std::shared_ptr<Layout> l = layout_of(text);
  if (!l->layout) {
    return 0;
  }
  int start = 0;
  line_of(line_metrics(*l), pos, &start);
  return start;
}

int text_line_end(const TextSettings &text, int pos)
{
  const std::shared_ptr<Layout> l = layout_of(text);
  if (!l->layout) {
    return 0;
  }
  const std::vector<DWRITE_LINE_METRICS> lines = line_metrics(*l);
  int start = 0;
  const int line = line_of(lines, pos, &start);
  if (lines.empty()) {
    return 0;
  }
  const DWRITE_LINE_METRICS &m = lines[size_t(line)];
  int end = start + int(m.length) - int(m.newlineLength);
  /* A wrapped line ends before its trailing space, so the caret stays on it. */
  if (m.newlineLength == 0 && line + 1 < int(lines.size()) && end > start && l->text[size_t(end - 1)] == u' ') {
    end--;
  }
  return end;
}

int text_line_step(const TextSettings &text, int pos, int lines_by)
{
  const std::shared_ptr<Layout> l = layout_of(text);
  if (!l->layout) {
    return pos;
  }
  const std::vector<DWRITE_LINE_METRICS> lines = line_metrics(*l);
  int start = 0;
  const int line = line_of(lines, pos, &start);
  const int target = line + lines_by;
  if (target < 0) {
    return 0;
  }
  if (target >= int(lines.size())) {
    return int(l->text.size());
  }
  FLOAT x = 0.0f, y = 0.0f;
  DWRITE_HIT_TEST_METRICS m{};
  l->layout->HitTestTextPosition(UINT32(pos), FALSE, &x, &y, &m);
  float top = 0.0f;
  for (int i = 0; i < target; i++) {
    top += lines[size_t(i)].height;
  }
  BOOL trailing = FALSE, inside = FALSE;
  DWRITE_HIT_TEST_METRICS hit{};
  l->layout->HitTestPoint(x, top + lines[size_t(target)].height * 0.5f, &trailing, &inside, &hit);
  return int(hit.textPosition) + (trailing ? int(hit.length) : 0);
}

TextBox text_bounds(const TextSettings &text)
{
  const std::shared_ptr<Layout> l = layout_of(text);
  if (!l->layout) {
    return {};
  }
  DWRITE_TEXT_METRICS m{};
  l->layout->GetMetrics(&m);
  const float right = text.box_width > 0.0f ? text.box_width * SCALE : m.left + m.widthIncludingTrailingWhitespace;
  const float left = text.box_width > 0.0f ? 0.0f : m.left;
  const float2 a = to_layer(*l, left, m.top), b = to_layer(*l, right, m.top + m.height);
  return {{std::min(a.x, b.x), std::min(a.y, b.y)}, {std::max(a.x, b.x), std::max(a.y, b.y)}};
}

/* --- Fonts ------------------------------------------------------------------------------------- */

static std::string family_name(IDWriteFontFamily *family)
{
  ComPtr<IDWriteLocalizedStrings> names;
  if (FAILED(family->GetFamilyNames(names.GetAddressOf()))) {
    return {};
  }
  UINT32 index = 0;
  BOOL exists = FALSE;
  names->FindLocaleName(L"en-us", &index, &exists);
  if (!exists) {
    index = 0;
  }
  UINT32 length = 0;
  names->GetStringLength(index, &length);
  std::wstring name(length + 1, L'\0');
  names->GetString(index, name.data(), length + 1);
  return narrow(name.c_str(), length);
}

const std::vector<std::string> &text_font_families()
{
  static const std::vector<std::string> families = [] {
    std::vector<std::string> out;
    if (IDWriteFontCollection *fonts = system_fonts()) {
      for (UINT32 i = 0; i < fonts->GetFontFamilyCount(); i++) {
        ComPtr<IDWriteFontFamily> family;
        if (SUCCEEDED(fonts->GetFontFamily(i, family.GetAddressOf()))) {
          if (std::string name = family_name(family.Get()); !name.empty()) {
            out.push_back(std::move(name));
          }
        }
      }
    }
    std::sort(out.begin(), out.end(), [](const std::string &a, const std::string &b) {
      return _stricmp(a.c_str(), b.c_str()) < 0;
    });
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
  }();
  return families;
}

bool text_font_installed(const std::string &family)
{
  IDWriteFontCollection *fonts = system_fonts();
  if (!fonts) {
    return false;
  }
  UINT32 index = 0;
  BOOL exists = FALSE;
  fonts->FindFamilyName(wide(family).c_str(), &index, &exists);
  return exists != FALSE;
}

std::vector<std::string> text_missing_fonts(const TextSettings &text)
{
  std::vector<std::string> out;
  for (const TextRun &run : text.runs) {
    if (std::find(out.begin(), out.end(), run.font) == out.end() && !text_font_installed(run.font)) {
      out.push_back(run.font);
    }
  }
  return out;
}

bool text_font_from_postscript(const std::string &postscript, std::string *r_family, int *r_weight, bool *r_italic)
{
  struct Face {
    std::string family;
    int weight = 400;
    bool italic = false;
  };
  static const std::map<std::string, Face> faces = [] {
    std::map<std::string, Face> out;
    IDWriteFontCollection *fonts = system_fonts();
    if (!fonts) {
      return out;
    }
    for (UINT32 i = 0; i < fonts->GetFontFamilyCount(); i++) {
      ComPtr<IDWriteFontFamily> family;
      if (FAILED(fonts->GetFontFamily(i, family.GetAddressOf()))) {
        continue;
      }
      const std::string name = family_name(family.Get());
      for (UINT32 k = 0; k < family->GetFontCount(); k++) {
        ComPtr<IDWriteFont> font;
        if (FAILED(family->GetFont(k, font.GetAddressOf()))) {
          continue;
        }
        ComPtr<IDWriteLocalizedStrings> strings;
        BOOL exists = FALSE;
        if (FAILED(font->GetInformationalStrings(DWRITE_INFORMATIONAL_STRING_POSTSCRIPT_NAME, strings.GetAddressOf(),
                                                 &exists)) ||
            !exists)
        {
          continue;
        }
        UINT32 length = 0;
        strings->GetStringLength(0, &length);
        std::wstring ps(length + 1, L'\0');
        strings->GetString(0, ps.data(), length + 1);
        std::string key = narrow(ps.c_str(), length);
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
        out.emplace(key, Face{name, int(font->GetWeight()), font->GetStyle() != DWRITE_FONT_STYLE_NORMAL});
      }
    }
    return out;
  }();
  std::string key = postscript;
  std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return char(std::tolower(c)); });
  const auto it = faces.find(key);
  if (it == faces.end()) {
    return false;
  }
  *r_family = it->second.family;
  *r_weight = it->second.weight;
  *r_italic = it->second.italic;
  return true;
}

}  // namespace oanim
