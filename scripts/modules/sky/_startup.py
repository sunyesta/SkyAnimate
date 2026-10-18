"""Loads scripts/startup at launch, like Blender's bl_ui and startup folders."""

import importlib.util
import pathlib
import sys
import traceback

import _sky


def load_startup_scripts(directory):
    """Import every .py file in directory in name order and call its register().

    Returns False if any script failed; the others still load.
    """
    ok = True
    for path in sorted(pathlib.Path(directory).glob("*.py")):
        try:
            spec = importlib.util.spec_from_file_location(path.stem, path)
            module = importlib.util.module_from_spec(spec)
            sys.modules[path.stem] = module
            spec.loader.exec_module(module)
            if hasattr(module, "register"):
                module.register()
        except Exception:
            ok = False
            _sky.report("ERROR", f"Error loading startup script {path.name}:\n{traceback.format_exc()}")
    return ok
