#pragma once

/* The embedded Python interpreter and the `_sky` module that scripts/modules/sky wraps. */

#include <filesystem>
#include <string>

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

}  // namespace sky::python
