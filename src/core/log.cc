#include "core/log.hh"

#include <cstdio>

namespace oanim {

static std::vector<Report> g_reports;
static constexpr size_t REPORTS_MAX = 1000;

static BannerReport g_banner;

void report(ReportType type, const std::string &message, bool banner)
{
  FILE *stream = (type == ReportType::Error || type == ReportType::Warning) ? stderr : stdout;
  std::fprintf(stream, "%s\n", message.c_str());
  std::fflush(stream);

  if (g_reports.size() >= REPORTS_MAX) {
    g_reports.erase(g_reports.begin());
  }
  g_reports.push_back({type, message});
  if (banner && type != ReportType::Operator) {
    g_banner.type = type;
    g_banner.title.clear();
    g_banner.message = message;
    g_banner.hint.clear();
    g_banner.serial++;
  }
}

const std::vector<Report> &reports()
{
  return g_reports;
}

void report_banner(ReportType type, const std::string &message, const std::string &hint, const std::string &title)
{
  report(type, message);
  g_banner.title = title;
  g_banner.hint = hint;
}

const BannerReport &banner_report()
{
  return g_banner;
}

}  // namespace oanim
