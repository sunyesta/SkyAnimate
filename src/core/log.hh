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

/* `banner`: also shown in the status bar's reports banner (Info, Warning and Error ones), as Blender
 * shows an operator's reports there; false for routine lines (undo steps, startup) that only the
 * Info editor needs. */
void report(ReportType type, const std::string &message, bool banner = true);
const std::vector<Report> &reports();

/* The report the status bar shows: the latest Info, Warning or Error. `title` is a bold lead-in
 * (what went wrong) and `hint` what to do, when the report has them; `serial` counts them, so the
 * status bar sees a new one. */
struct BannerReport {
  ReportType type = ReportType::Info;
  std::string title;
  std::string message;
  std::string hint;
  uint64_t serial = 0;
};
/* A report with a bold title and a hint for the status bar (the Info editor gets `message`):
 * something not allowed, and how to do it instead. */
void report_banner(ReportType type, const std::string &message, const std::string &hint = "",
                   const std::string &title = "");
const BannerReport &banner_report();

}  // namespace sky
