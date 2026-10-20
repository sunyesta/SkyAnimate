#pragma once

/* Reports: messages shown in the Info editor and printed to the console,
 * like Blender's report system. */

#include <cstdint>
#include <string>
#include <vector>

namespace sky {

enum class ReportType { Info, Operator, Warning, Error };

struct Report {
  ReportType type;
  std::string message;
};

void report(ReportType type, const std::string &message);
const std::vector<Report> &reports();

/* Something not allowed, shown at the mouse as well as reported (as Blender shows an operator's
 * warning beside the cursor): `message` goes to the Info editor, `at_mouse` (or `message`) beside
 * the cursor. The latest one; `serial` counts them, so the UI sees a new one. */
struct MouseReport {
  ReportType type = ReportType::Warning;
  /* A bold first line, if any, then the message. */
  std::string title;
  std::string message;
  uint64_t serial = 0;
};
void report_at_mouse(ReportType type, const std::string &message, const std::string &at_mouse = "",
                     const std::string &title = "");
const MouseReport &mouse_report();

}  // namespace sky
