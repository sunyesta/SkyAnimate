import oanim

# Bind a key to an operator. A keymap is "Window" for shortcuts that work everywhere,
# or an editor name such as "Viewport" or "Text Editor".
# Key names follow Blender: "A", "F3", "SPACE", "ESC", "RET", "LEFT_ARROW", ...

oanim.keymap.add("Window", "screen.animation_play", "F5")

for keymap, idname, key in oanim.keymap.items():
    print(f"{keymap:12} {key:16} {idname}")
