#pragma once

/* Crash reports: when SkyAnimate crashes it writes, in the crashes folder, a minidump
 * (crash_<time>.dmp: open it in Visual Studio or WinDbg with the build's SkyAnimate.pdb) and a
 * readable report (crash_<time>.txt: what went wrong, where, and the call stack with function names
 * and source lines). The next start says so in the Info editor. Not installed in background runs
 * (tests), which never write to the user's folder. */

#include <string>

namespace sky {

/* Writes crash reports into `dir` from now on (unhandled exceptions, std::terminate, abort, and the
 * C runtime's pure-call and invalid-parameter errors). */
void crash_handler_install(const std::string &dir);

/* A crash report written since the last start that nobody has been told about yet (its .txt path),
 * or "". Marks it as told. */
std::string crash_report_unseen(const std::string &dir);

/* Crashes on purpose, to try the handler (sky.wm.debug_crash()). */
[[noreturn]] void crash_now();

}  // namespace sky
