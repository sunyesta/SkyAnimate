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

}  // namespace sky
