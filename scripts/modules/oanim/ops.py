"""Run operators by name, like bpy.ops: oanim.ops.ed.undo() returns {'FINISHED'} or {'CANCELLED'}.

Keyword arguments become the operator's arguments: oanim.ops.wm.open_mainfile(filepath="walk.oanim").
"""

import _oanim


def _arg_string(value):
    if isinstance(value, bool):
        return "1" if value else "0"
    return str(value)


class _Operator:
    def __init__(self, idname):
        self.idname = idname

    def __call__(self, **kwargs):
        args = {name: _arg_string(value) for name, value in kwargs.items()}
        return {_oanim.ops_call(self.idname, args)}

    def poll(self):
        return _oanim.ops_poll(self.idname)

    def __repr__(self):
        return f"oanim.ops.{self.idname}"


class _Category:
    def __init__(self, name):
        self._name = name

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return _Operator(f"{self._name}.{name}")

    def __dir__(self):
        prefix = self._name + "."
        return sorted(op["idname"][len(prefix):] for op in _oanim.ops_list() if op["idname"].startswith(prefix))

    def __repr__(self):
        return f"oanim.ops.{self._name}"


def __getattr__(name):
    if name.startswith("_"):
        raise AttributeError(name)
    return _Category(name)


def __dir__():
    return sorted({op["idname"].split(".")[0] for op in _oanim.ops_list()})
