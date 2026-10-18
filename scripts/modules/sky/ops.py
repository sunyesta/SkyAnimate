"""Run operators by name, like bpy.ops: sky.ops.ed.undo() returns {'FINISHED'} or {'CANCELLED'}."""

import _sky


class _Operator:
    def __init__(self, idname):
        self.idname = idname

    def __call__(self):
        return {_sky.ops_call(self.idname)}

    def poll(self):
        return _sky.ops_poll(self.idname)

    def __repr__(self):
        return f"sky.ops.{self.idname}"


class _Category:
    def __init__(self, name):
        self._name = name

    def __getattr__(self, name):
        if name.startswith("_"):
            raise AttributeError(name)
        return _Operator(f"{self._name}.{name}")

    def __dir__(self):
        prefix = self._name + "."
        return sorted(op["idname"][len(prefix):] for op in _sky.ops_list() if op["idname"].startswith(prefix))

    def __repr__(self):
        return f"sky.ops.{self._name}"


def __getattr__(name):
    if name.startswith("_"):
        raise AttributeError(name)
    return _Category(name)


def __dir__():
    return sorted({op["idname"].split(".")[0] for op in _sky.ops_list()})
