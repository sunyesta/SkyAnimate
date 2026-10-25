#include "data/text.hh"

#include <algorithm>

namespace oanim {

std::u16string utf8_to_utf16(const std::string &s)
{
  std::u16string out;
  out.reserve(s.size());
  size_t i = 0;
  while (i < s.size()) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    uint32_t cp = 0xFFFD;
    int extra = 0;
    if (c < 0x80) {
      cp = c;
    }
    else if ((c & 0xE0) == 0xC0) {
      cp = c & 0x1F;
      extra = 1;
    }
    else if ((c & 0xF0) == 0xE0) {
      cp = c & 0x0F;
      extra = 2;
    }
    else if ((c & 0xF8) == 0xF0) {
      cp = c & 0x07;
      extra = 3;
    }
    i++;
    bool ok = c < 0x80 || extra > 0;
    for (int k = 0; k < extra; k++, i++) {
      if (i >= s.size() || (static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
        ok = false;
        break;
      }
      cp = (cp << 6) | (static_cast<unsigned char>(s[i]) & 0x3F);
    }
    if (!ok || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
      cp = 0xFFFD;
    }
    if (cp >= 0x10000) {
      cp -= 0x10000;
      out.push_back(char16_t(0xD800 + (cp >> 10)));
      out.push_back(char16_t(0xDC00 + (cp & 0x3FF)));
    }
    else {
      out.push_back(char16_t(cp));
    }
  }
  return out;
}

std::string utf16_to_utf8(const std::u16string &s)
{
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size(); i++) {
    uint32_t cp = s[i];
    if (cp >= 0xD800 && cp <= 0xDBFF && i + 1 < s.size() && s[i + 1] >= 0xDC00 && s[i + 1] <= 0xDFFF) {
      cp = 0x10000 + ((cp - 0xD800) << 10) + (s[i + 1] - 0xDC00);
      i++;
    }
    else if (cp >= 0xD800 && cp <= 0xDFFF) {
      cp = 0xFFFD;
    }
    if (cp < 0x80) {
      out.push_back(char(cp));
    }
    else if (cp < 0x800) {
      out.push_back(char(0xC0 | (cp >> 6)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else if (cp < 0x10000) {
      out.push_back(char(0xE0 | (cp >> 12)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
    else {
      out.push_back(char(0xF0 | (cp >> 18)));
      out.push_back(char(0x80 | ((cp >> 12) & 0x3F)));
      out.push_back(char(0x80 | ((cp >> 6) & 0x3F)));
      out.push_back(char(0x80 | (cp & 0x3F)));
    }
  }
  return out;
}

std::string text_string(const TextSettings &text)
{
  std::string out;
  for (const TextRun &run : text.runs) {
    out += run.text;
  }
  return out;
}

int text_length(const TextSettings &text)
{
  int n = 0;
  for (const TextRun &run : text.runs) {
    n += int(utf8_to_utf16(run.text).size());
  }
  return n;
}

int text_run_at(const TextSettings &text, int pos)
{
  int at = 0;
  for (int r = 0; r < int(text.runs.size()); r++) {
    const int n = int(utf8_to_utf16(text.runs[r].text).size());
    if (pos <= at + n) {
      return r;
    }
    at += n;
  }
  return std::max(int(text.runs.size()) - 1, 0);
}

/* Splits the run holding `pos` there (UTF-16), so a run starts at it. */
static void split_at(TextSettings &text, int pos)
{
  int at = 0;
  for (size_t r = 0; r < text.runs.size(); r++) {
    const std::u16string s = utf8_to_utf16(text.runs[r].text);
    const int n = int(s.size());
    if (pos > at && pos < at + n) {
      TextRun tail = text.runs[r];
      tail.text = utf16_to_utf8(s.substr(size_t(pos - at)));
      text.runs[r].text = utf16_to_utf8(s.substr(0, size_t(pos - at)));
      text.runs.insert(text.runs.begin() + std::ptrdiff_t(r) + 1, std::move(tail));
      return;
    }
    at += n;
  }
}

void text_tidy(TextSettings &text)
{
  if (text.runs.empty()) {
    text.runs.emplace_back();
    return;
  }
  std::vector<TextRun> out;
  for (TextRun &run : text.runs) {
    if (!out.empty()) {
      TextRun a = out.back(), b = run;
      a.text.clear();
      b.text.clear();
      if (a == b) {
        out.back().text += run.text;
        continue;
      }
    }
    out.push_back(std::move(run));
  }
  /* Empty runs go, but the text keeps one (its style is what typing into it uses). */
  std::vector<TextRun> kept;
  for (TextRun &run : out) {
    if (!run.text.empty()) {
      kept.push_back(std::move(run));
    }
  }
  if (kept.empty()) {
    kept.push_back(std::move(out.front()));
  }
  text.runs = std::move(kept);
}

std::vector<TextRun *> text_runs_between(TextSettings &text, int from, int to)
{
  if (text.runs.empty()) {
    text.runs.emplace_back();
  }
  if (from > to) {
    std::swap(from, to);
  }
  if (from == to) {
    return {&text.runs[size_t(text_run_at(text, from))]};
  }
  split_at(text, from);
  split_at(text, to);
  std::vector<TextRun *> out;
  int at = 0;
  for (TextRun &run : text.runs) {
    const int n = int(utf8_to_utf16(run.text).size());
    if (at >= from && at + n <= to && n > 0) {
      out.push_back(&run);
    }
    at += n;
  }
  return out;
}

void text_replace(TextSettings &text, int from, int to, const std::string &with)
{
  if (text.runs.empty()) {
    text.runs.emplace_back();
  }
  if (from > to) {
    std::swap(from, to);
  }
  const int length = text_length(text);
  from = std::clamp(from, 0, length);
  to = std::clamp(to, 0, length);
  /* The style it goes in: the run the start lands in (the one before, at a join). */
  const int style_run = text_run_at(text, from);
  std::u16string added = utf8_to_utf16(with);
  int at = 0;
  bool placed = false;
  for (int r = 0; r < int(text.runs.size()); r++) {
    TextRun &run = text.runs[size_t(r)];
    std::u16string s = utf8_to_utf16(run.text);
    const int n = int(s.size());
    const int a = std::clamp(from - at, 0, n), b = std::clamp(to - at, 0, n);
    if (b > a) {
      s.erase(size_t(a), size_t(b - a));
    }
    if (!placed && r == style_run) {
      s.insert(size_t(a), added);
      placed = true;
    }
    run.text = utf16_to_utf8(s);
    at += n;
  }
  text_tidy(text);
}

void text_set_string(TextSettings &text, const std::string &with)
{
  const std::u16string old_text = utf8_to_utf16(text_string(text)), new_text = utf8_to_utf16(with);
  size_t head = 0;
  while (head < old_text.size() && head < new_text.size() && old_text[head] == new_text[head]) {
    head++;
  }
  size_t tail = 0;
  while (tail < old_text.size() - head && tail < new_text.size() - head &&
         old_text[old_text.size() - 1 - tail] == new_text[new_text.size() - 1 - tail])
  {
    tail++;
  }
  text_replace(text, int(head), int(old_text.size() - tail),
               utf16_to_utf8(new_text.substr(head, new_text.size() - tail - head)));
}

}  // namespace oanim
