"""Runs code sent through remote control (the MCP bridge in tools/mcp) inside the running app.

As a console: names defined in one call are there in the next, and the value of a last line that
is an expression comes back (as its repr), with whatever was printed and any error's traceback.
"""

import ast
import contextlib
import io
import traceback

_namespace = {"__name__": "__remote__"}


def run(code):
    import sky

    _namespace.setdefault("sky", sky)
    output = io.StringIO()
    result = None
    error = None
    try:
        tree = ast.parse(code, "<remote>", "exec")
        last = tree.body.pop() if tree.body and isinstance(tree.body[-1], ast.Expr) else None
        with contextlib.redirect_stdout(output), contextlib.redirect_stderr(output):
            exec(compile(tree, "<remote>", "exec"), _namespace)
            if last is not None:
                value = eval(compile(ast.Expression(last.value), "<remote>", "eval"), _namespace)
                if value is not None:
                    result = repr(value)
    except BaseException as e:
        # From the code sent on: this file's own frame is no help to whoever reads it.
        tb = e.__traceback__.tb_next if e.__traceback__ is not None else None
        error = "".join(traceback.format_exception(type(e), e, tb))
    return {"output": output.getvalue(), "result": result, "error": error}
