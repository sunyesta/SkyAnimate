"""Auto Focus in the Layers editor (its settings button): folds every group but those holding the selected layers,
opens those, and leaves a selected group as it is.

Run with:  OpenAnimate --background --python tests/test_layers_auto_focus.py
"""
import oanim

layers = oanim.data.layers
ops = oanim.ops.layer


def layer(name, parent=None):
    ops.new()
    new = layers.active
    new.name = name
    assert ops.move_to_group(group=str(parent.id if parent else 0)) == {'FINISHED'}
    return new


def group(name, parent=None):
    ops.new_group()
    new = layers.active
    new.name = name
    assert ops.move_to_group(group=str(parent.id if parent else 0)) == {'FINISHED'}
    return new


head = group("Head")
face = group("Face", head)
eye = layer("Eye", face)
inner = group("Inner", face)
inner_line = layer("InnerLine", inner)
body = group("Body")
arm = layer("Arm", body)
legs = group("Legs")
legs_line = layer("Shin", legs)
groups = [head, face, inner, body, legs]


def picked(*chosen):
    for l in [head, face, eye, inner, inner_line, body, arm, legs, legs_line]:
        l.select = False
    layers.active = chosen[0]
    for l in chosen:
        l.select = True


def fold(*opened):
    for g in groups:
        g.expanded = g in opened


def open_groups():
    return [g.name for g in groups if g.expanded]


# Deep in the tree: its whole path stays open, everything else folds.
picked(eye)
fold(*groups)
assert ops.auto_focus() == {'FINISHED'}
assert open_groups() == ["Head", "Face"], open_groups()

# The path is opened if it was folded.
picked(legs_line)
fold(head)
assert ops.auto_focus() == {'FINISHED'}
assert open_groups() == ["Legs"], open_groups()

# Several selected: the paths of them all.
picked(arm, inner_line)
fold()
assert ops.auto_focus() == {'FINISHED'}
assert open_groups() == ["Head", "Face", "Inner", "Body"], open_groups()

# A selected group isn't folded, nor opened: its layers show as far as it did.
picked(inner)
fold(*groups)
assert ops.auto_focus() == {'FINISHED'}
assert open_groups() == ["Head", "Face", "Inner"], open_groups()
fold()
assert ops.auto_focus() == {'FINISHED'}
assert open_groups() == ["Head", "Face"], open_groups()

print("Layers Auto Focus tests passed")
