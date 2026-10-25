"""Mirror editing (the viewport header's X and Y) is saved with the file: opening it turns them on or off
as they were saved; a new file, or one saved before this, starts with both off.

Run with:  OpenAnimate --background --python tests/test_mirror_saved.py
"""

import json
import pathlib
import tempfile
import zipfile

import oanim

ops = oanim.ops
wm = oanim.wm
folder = pathlib.Path(tempfile.mkdtemp(prefix="oanim_mirror_"))

assert wm.mirror_editing() == (False, False)
wm.set_mirror_editing(x=True)
on = str(folder / "on.oanim")
assert ops.wm.save_as_mainfile(filepath=on) == {'FINISHED'}

# A new file: both off.
assert ops.wm.read_homefile(confirmed="1") == {'FINISHED'}
assert wm.mirror_editing() == (False, False)
off = str(folder / "off.oanim")
assert ops.wm.save_as_mainfile(filepath=off) == {'FINISHED'}

# Opening each brings its own back.
assert ops.wm.open_mainfile(filepath=on, confirmed="1") == {'FINISHED'}
assert wm.mirror_editing() == (True, False)
assert ops.wm.open_mainfile(filepath=off, confirmed="1") == {'FINISHED'}
assert wm.mirror_editing() == (False, False)

# Y too, and toggling counts as a change to save.
assert ops.wm.open_mainfile(filepath=on, confirmed="1") == {'FINISHED'}
wm.set_mirror_editing(x=True, y=True)
assert ops.wm.save_mainfile() == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=off, confirmed="1") == {'FINISHED'}
assert ops.wm.open_mainfile(filepath=on, confirmed="1") == {'FINISHED'}
assert wm.mirror_editing() == (True, True)

# A file from before this was saved (no mirror_editing in it): both off.
old = folder / "old.oanim"
with zipfile.ZipFile(on) as src, zipfile.ZipFile(old, "w") as dst:
    for item in src.infolist():
        data = src.read(item.filename)
        if item.filename.endswith(".json"):
            j = json.loads(data)
            j.get("document", {}).pop("mirror_editing")
            data = json.dumps(j).encode()
        dst.writestr(item, data)
assert ops.wm.open_mainfile(filepath=str(old), confirmed="1") == {'FINISHED'}
assert wm.mirror_editing() == (False, False)

print("mirror_saved: ok")
