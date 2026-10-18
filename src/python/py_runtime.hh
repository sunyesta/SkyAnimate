#pragma once

/* The embedded Python interpreter and the `_sky` module that scripts/modules/sky wraps. */

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace sky {
struct Context;
}

namespace sky::python {

/* Starts the interpreter, adds scripts/modules to sys.path and runs every script in
 * scripts/startup. Returns false if Python could not start or a startup script failed. */
bool init(const std::filesystem::path &scripts_dir, Context *C);
/* Removes Python-defined operators and stops the interpreter. */
void shutdown();

/* Run a script file or a code string as __main__. Errors are reported with a
 * traceback; returns false on an uncaught exception. */
bool run_file(const std::filesystem::path &path);
bool run_string(const std::string &code);

/* The scripts folder Python was started with. */
const std::filesystem::path &scripts_dir();

/* The interactive console (scripts/modules/sky/_console.py). */
struct ConsoleLine {
  /* "INPUT", "OUTPUT", "ERROR" or "INFO". */
  std::string kind;
  std::string text;
};
/* Changes whenever the scrollback does, so editors can skip re-reading it. */
uint64_t console_version();
std::vector<ConsoleLine> console_lines();
std::string console_prompt();
std::vector<std::string> console_history();
void console_push(const std::string &line);
/* Returns the line with the name at its end completed. */
std::string console_complete(const std::string &line);
void console_clear();

/* Runs a text block's code as __main__, printing into the console. False if it raised. */
bool text_run(const std::string &name, const std::string &source);

}  // namespace sky::python
