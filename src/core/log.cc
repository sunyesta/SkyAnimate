#include "core/log.hh"

#include <cstdio>

namespace sky {

static std::vector<Report> g_reports;
static constexpr size_t REPORTS_MAX = 1000;

void report(ReportType type, const std::string &message)
{
  FILE *stream = (type == ReportType::Error || type == ReportType::Warning) ? stderr : stdout;
  std::fprintf(stream, "%s\n", message.c_str());
  std::fflush(stream);

  if (g_reports.size() >= REPORTS_MAX) {
    g_reports.erase(g_reports.begin());
  }
  g_reports.push_back({type, message});
}

const std::vector<Report> &reports()
{
  return g_reports;
}

static MouseReport g_mouse_report;

void report_at_mouse(ReportType type, const std::string &message, const std::string &at_mouse,
                     const std::string &title)
{
  report(type, message);
  g_mouse_report.type = type;
  g_mouse_report.title = title;
  g_mouse_report.message = at_mouse.empty() ? message : at_mouse;
  g_mouse_report.serial++;
}

const MouseReport &mouse_report()
{
  return g_mouse_report;
}

}  // namespace sky
