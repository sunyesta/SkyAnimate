#pragma once

/* Reports: messages shown in the Info editor and printed to the console,
 * like Blender's report system. */

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

}  // namespace sky
