/* SkyAnimate entry point.
 *
 *   SkyAnimate                                   open the window
 *   SkyAnimate --background --python script.py   run a script without a window, then exit
 */

#include <cstdio>
#include <cstring>

#include "wm/app.hh"

static void print_help()
{
  std::printf(
      "Usage: SkyAnimate [options]\n"
      "\n"
      "  -b, --background       Run without a window and exit when scripts finish.\n"
      "                         The exit code is 1 if any script raised an error.\n"
      "  -P, --python FILE      Run a Python script after startup (repeatable).\n"
      "  --python-expr CODE     Run Python code after startup (repeatable).\n"
      "  -h, --help             Show this help.\n");
}

int main(int argc, char **argv)
{
  sky::LaunchOptions options;
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
    else {
      std::fprintf(stderr, "Unknown or incomplete argument: %s\n\n", arg);
      print_help();
      return 2;
    }
  }

  sky::App app;
  return app.run(options);
}
