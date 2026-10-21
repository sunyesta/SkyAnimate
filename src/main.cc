/* Open Animate entry point.
 *
 *   Open Animate                                   open the window
 *   OpenAnimate walk.oanim                          open a file
 *   OpenAnimate --background --python script.py   run a script without a window, then exit
 */

#include <cstdio>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#endif

#include "wm/app.hh"

/* Open Animate is a windowed app, so Windows gives it no console. When output is already
 * redirected (ctest, pipes) the standard handles work as-is; when started from a terminal,
 * attach to that terminal so printing and --background output still appear. */
static void console_attach_to_parent()
{
#ifdef _WIN32
  const HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
  if (out != nullptr && out != INVALID_HANDLE_VALUE) {
    return;
  }
  if (AttachConsole(ATTACH_PARENT_PROCESS)) {
    FILE *stream = nullptr;
    freopen_s(&stream, "CONOUT$", "w", stdout);
    freopen_s(&stream, "CONOUT$", "w", stderr);
  }
#endif
}

static void print_help()
{
  std::printf(
      "Usage: OpenAnimate [options] [file.oanim]\n"
      "\n"
      "  -b, --background       Run without a window and exit when scripts finish.\n"
      "                         The exit code is 1 if any script raised an error.\n"
      "  -P, --python FILE      Run a Python script after startup (repeatable).\n"
      "  --python-expr CODE     Run Python code after startup (repeatable).\n"
      "  -h, --help             Show this help.\n");
}

int main(int argc, char **argv)
{
  console_attach_to_parent();

  oanim::LaunchOptions options;
  for (int i = 1; i < argc; i++) {
    const char *arg = argv[i];
    const bool has_value = i + 1 < argc;
    if (std::strcmp(arg, "-b") == 0 || std::strcmp(arg, "--background") == 0) {
      options.background = true;
    }
    else if ((std::strcmp(arg, "-P") == 0 || std::strcmp(arg, "--python") == 0) && has_value) {
      options.python_files.push_back(argv[++i]);
    }
    else if (std::strcmp(arg, "--python-expr") == 0 && has_value) {
      options.python_exprs.push_back(argv[++i]);
    }
    else if (std::strcmp(arg, "-h") == 0 || std::strcmp(arg, "--help") == 0) {
      print_help();
      return 0;
    }
    else if (arg[0] != '-' && options.open_file.empty()) {
      options.open_file = arg;
    }
    else {
      std::fprintf(stderr, "Unknown or incomplete argument: %s\n\n", arg);
      print_help();
      return 2;
    }
  }

  oanim::App app;
  return app.run(options);
}
