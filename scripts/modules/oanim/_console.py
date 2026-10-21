"""The interactive Python console behind the Console editor, like Blender's console_python.

Lines are kept as (kind, text) pairs, kind being "INPUT", "OUTPUT", "ERROR" or "INFO".
Scripts run from the text editor print into the same scrollback.
"""

import code
import io
import re
import rlcompleter
import sys
import traceback

import _oanim

import oanim

MAX_LINES = 5000

_PROMPT = ">>> "
_PROMPT_MORE = "... "


class _Console:
    def __init__(self):
        self.namespace = {
            "__name__": "__console__",
            "__builtins__": __builtins__,
            "oanim": oanim,
            "C": _oanim.context(),
            "D": oanim.data,
        }
        self.interpreter = code.InteractiveConsole(self.namespace)
        self.more = False
        self.lines = []
        self.history = []
        self.version = 0
        self._banner()

    def _banner(self):
        self.write("INFO", f"PYTHON INTERACTIVE CONSOLE {sys.version.split()[0]}")
        self.write("INFO", "")
        self.write("INFO", "Builtin Modules:       oanim, oanim.data, oanim.ops, oanim.types, oanim.context, oanim.utils, oanim.keymap, oanim.wm")
        self.write("INFO", "Convenience Variables: C = oanim.context, D = oanim.data")
        self.write("INFO", "")

    def write(self, kind, text):
        for line in text.rstrip("\n").split("\n"):
            self.lines.append((kind, line.expandtabs(4)))
        del self.lines[:-MAX_LINES]
        self.version += 1

    def prompt(self):
        return _PROMPT_MORE if self.more else _PROMPT

    def _run_captured(self, func):
        """Run func() with stdout and stderr captured into the scrollback."""
        out, err = io.StringIO(), io.StringIO()
        saved = sys.stdout, sys.stderr
        sys.stdout, sys.stderr = out, err
        try:
            return func()
        finally:
            sys.stdout, sys.stderr = saved
            if out.getvalue():
                self.write("OUTPUT", out.getvalue())
            if err.getvalue():
                self.write("ERROR", err.getvalue())

    def push(self, line):
        self.write("INPUT", self.prompt() + line)
        if line.strip() and (not self.history or self.history[-1] != line):
            self.history.append(line)
        self.more = self._run_captured(lambda: self.interpreter.push(line))

    def complete(self, line):
        """Complete the name at the end of line. Lists the choices when there are several."""
        match = re.search(r"[\w.]+$", line)
        if not match:
            return line
        word = match.group(0)
        completer = rlcompleter.Completer(self.namespace)
        options = []
        state = 0
        while len(options) < 500:
            option = completer.complete(word, state)
            if option is None:
                break
            options.append(option)
            state += 1
        if not options:
            return line
        prefix = options[0]
        for option in options[1:]:
            while not option.startswith(prefix):
                prefix = prefix[:-1]
        if len(options) > 1:
            self.write("INPUT", self.prompt() + line)
            self.write("INFO", "  ".join(option.rsplit(".", 1)[-1] for option in options))
        return line[: match.start()] + prefix

    def run_text(self, name, source):
        """Run a text block's code as __main__. Returns False if it raised."""
        namespace = {"__name__": "__main__", "__file__": name, "__builtins__": __builtins__}

        def run():
            exec(compile(source, name, "exec"), namespace)

        try:
            self._run_captured(run)
            return True
        except SystemExit:
            return True
        except Exception:
            message = traceback.format_exc()
            self.write("ERROR", message)
            _oanim.report("ERROR", f"Error in text '{name}':\n{message}")
            return False

    def clear(self):
        self.lines.clear()
        self.version += 1


_console = None


def get():
    global _console
    if _console is None:
        _console = _Console()
    return _console
