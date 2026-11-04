"""The default keymap, like Blender's blender_default.py.

Each entry is (keymap, operator idname, key, modifiers). Editor keymaps such as
"Viewport" are tried before "Window" when the mouse is over that editor.
"""

import oanim

KEYMAP = (
    ("Window", "ed.undo", "Z", {"ctrl": True}),
    # Up the layer tree: the group holding the active layer.
    ("Window", "layer.select_parent", "EQUAL", {}),
    ("Window", "ed.redo", "Z", {"ctrl": True, "shift": True}),
    ("Window", "wm.quit", "Q", {"ctrl": True}),
    ("Window", "render.render", "R", {"ctrl": True}),
    ("Window", "render.render", "F12", {}),
    # Files, as in Blender.
    ("Window", "wm.read_homefile", "N", {"ctrl": True}),
    ("Window", "wm.open_mainfile", "O", {"ctrl": True}),
    ("Window", "wm.save_mainfile", "S", {"ctrl": True}),
    ("Window", "wm.save_as_mainfile", "S", {"ctrl": True, "shift": True}),
    ("Window", "wm.save_incremental", "S", {"ctrl": True, "alt": True}),
    # Frames and playback, as in Blender's "Frames" keymap.
    ("Window", "screen.animation_play", "SPACE", {}),
    ("Window", "screen.animation_play_reverse", "SPACE", {"ctrl": True, "shift": True}),
    ("Window", "screen.animation_play_path_range", "SPACE", {"alt": True}),
    ("Window", "screen.animation_cancel", "ESC", {}),
    ("Window", "screen.frame_offset_backward", "LEFT_ARROW", {}),
    ("Window", "screen.frame_offset_forward", "RIGHT_ARROW", {}),
    ("Window", "screen.frame_jump_start", "LEFT_ARROW", {"shift": True}),
    ("Window", "screen.frame_setup_toggle", "TAB", {}),
    # Into Sculpt, and back to the tool before (beside Tab, anywhere).
    ("Window", "wm.tool_sculpt_toggle", "TAB", {"ctrl": True}),
    ("Window", "screen.frame_jump_end", "RIGHT_ARROW", {"shift": True}),
    ("Window", "screen.keyframe_jump_prev", "DOWN_ARROW", {}),
    ("Window", "screen.keyframe_jump_next", "UP_ARROW", {}),
    ("Window", "screen.workspace_cycle_next", "PAGE_DOWN", {"ctrl": True}),
    ("Window", "screen.workspace_cycle_prev", "PAGE_UP", {"ctrl": True}),
    # Text editor, as in Blender. These work while typing in the text.
    ("Text Editor", "text.run_script", "P", {"alt": True}),
    ("Text Editor", "text.save", "S", {"alt": True}),
    ("Text Editor", "text.new", "N", {"alt": True}),
    # Viewport: editing the active vector layer, as in Blender's Grease Pencil edit mode.
    ("Viewport", "view.overlays_toggle", "Z", {"shift": True}),
    ("Viewport", "mesh.make_segment", "F", {}),
    # Blender's Local View: the picked layers alone, and back.
    ("Viewport", "view.local_view", "SLASH", {}),
    ("Viewport", "view.local_view", "NUMPAD_SLASH", {}),
    ("Viewport", "view.media_view", "ACCENT_GRAVE", {}),
    ("Viewport", "screen.redo_last", "F9", {}),
    ("Viewport", "transform.translate", "G", {}),
    # Draw order, as Photoshop and Figma arrange: Ctrl ] / Ctrl [ one step, with Shift all the way.
    # Harmony's [ and ]: the previous or next child of the switch holding the active layer, keyed.
    ("Window", "layer.switch_previous", "LEFT_BRACKET", {}),
    ("Window", "layer.switch_next", "RIGHT_BRACKET", {}),
    ("Window", "layer.draw_order_forward", "RIGHT_BRACKET", {"ctrl": True}),
    ("Window", "layer.draw_order_backward", "LEFT_BRACKET", {"ctrl": True}),
    ("Window", "layer.draw_order_front", "RIGHT_BRACKET", {"ctrl": True, "shift": True}),
    ("Window", "layer.draw_order_back", "LEFT_BRACKET", {"ctrl": True, "shift": True}),
    # Photoshop's clipping mask: show the layer only inside the one below.
    ("Window", "mask.clip_to_below", "G", {"ctrl": True, "alt": True}),
    ("Viewport", "transform.rotate", "R", {}),
    ("Viewport", "transform.resize", "S", {}),
    # Blender's Snap pie: the 2D cursor and the selection to each other, the grid and the active one.
    ("Viewport", "view.snap_pie", "S", {"shift": True}),
    ("Viewport", "transform.proportional_toggle", "O", {}),
    ("Viewport", "transform.proportional_falloff_cycle", "O", {"shift": True}),
    ("Viewport", "transform.proportional_connected_toggle", "O", {"alt": True}),
    # Bones first: each works only on a bone layer, so on a drawing the mesh operator takes the key.
    ("Viewport", "armature.extrude", "E", {}),
    ("Viewport", "armature.duplicate_move", "D", {"shift": True}),
    ("Viewport", "armature.bone_primitive_add", "A", {"shift": True}),
    # Blender's Add menu on a drawing: a point, line, square or circle at the 2D cursor.
    ("Viewport", "mesh.add_menu", "A", {"shift": True}),
    ("Viewport", "armature.parent_set_menu", "P", {"ctrl": True}),
    ("Viewport", "armature.apply_menu", "A", {"ctrl": True}),
    ("Viewport", "armature.parent_clear_menu", "P", {"alt": True}),
    ("Viewport", "armature.move_to_collection_menu", "M", {}),
    ("Viewport", "armature.assign_to_collection_menu", "M", {"shift": True}),
    # An active Media layer: the layer itself (as Blender's Clear Location / Rotation / Scale).
    ("Viewport", "layer.clear_location", "G", {"alt": True}),
    ("Viewport", "layer.clear_rotation", "R", {"alt": True}),
    ("Viewport", "layer.clear_scale", "S", {"alt": True}),
    ("Viewport", "armature.clear_location", "G", {"alt": True}),
    # Selected points, when no bone is picked (the first operator that can run takes the key).
    ("Viewport", "mesh.clear_location", "G", {"alt": True}),
    ("Viewport", "armature.clear_rotation", "R", {"alt": True}),
    ("Viewport", "mesh.clear_handles", "R", {"alt": True}),
    ("Viewport", "armature.clear_scale", "S", {"alt": True}),
    # Points: Blender Grease Pencil's Shrink/Fatten (the line's width at them).
    ("Viewport", "transform.radius", "S", {"alt": True}),
    ("Viewport", "mesh.extrude", "E", {}),
    ("Viewport", "mesh.duplicate_move", "D", {"shift": True}),
    # Blender's Separate > Selection: the selected pieces into a layer of their own.
    ("Viewport", "mesh.separate", "P", {}),
    ("Viewport", "anim.keyframe_insert", "I", {}),
    # A guide tool's picked guide goes first.
    ("Viewport", "guide.delete_picked", "X", {}),
    ("Viewport", "guide.delete_picked", "DEL", {}),
    ("Viewport", "armature.bone_delete", "X", {}),
    ("Viewport", "armature.select_all", "A", {}),
    ("Viewport", "armature.select_none", "A", {"alt": True}),
    ("Viewport", "armature.select_invert", "I", {"ctrl": True}),
    ("Viewport", "armature.bone_delete", "DEL", {}),
    ("Viewport", "anim.keyframe_delete", "I", {"alt": True}),
    # Shape stacking order, as Blender's Arrange for Grease Pencil strokes.
    ("Viewport", "mesh.shape_raise", "PAGE_UP", {}),
    ("Viewport", "mesh.shape_lower", "PAGE_DOWN", {}),
    ("Viewport", "mesh.shape_raise_to_front", "PAGE_UP", {"shift": True}),
    ("Viewport", "mesh.shape_lower_to_back", "PAGE_DOWN", {"shift": True}),
    ("Viewport", "mesh.copy", "C", {"ctrl": True}),
    ("Viewport", "mesh.paste", "V", {"ctrl": True}),
    ("Viewport", "mesh.select_all", "A", {}),
    ("Viewport", "mesh.select_none", "A", {"alt": True}),
    ("Viewport", "mesh.select_invert", "I", {"ctrl": True}),
    ("Viewport", "mesh.select_linked_pick", "L", {}),
    # Blender's Link/Transfer Data.
    ("Viewport", "wm.link_data_menu", "L", {"ctrl": True}),
    ("Viewport", "mesh.deselect_linked_pick", "L", {"shift": True}),
    ("Viewport", "mesh.select_mode_point", "1", {}),
    ("Viewport", "mesh.select_mode_edge", "2", {}),
    ("Viewport", "mesh.select_mode_stroke", "3", {}),
    ("Viewport", "mesh.delete_menu", "X", {}),
    # Blender's Merge menu (M), for points; on a bone layer, Move to Bone Collection takes it first.
    ("Viewport", "mesh.merge_menu", "M", {}),
    ("Viewport", "mesh.delete", "DEL", {}),
    ("Viewport", "wm.tool_split_curve_temporary", "K", {}),
    # Back to the Select Box tool from any other.
    ("Viewport", "wm.tool_select", "T", {}),
    # Detail levels of the selected shapes, as Blender's multiresolution levels.
    ("Viewport", "mesh.detail_level_0", "0", {"ctrl": True}),
    ("Viewport", "mesh.detail_level_1", "1", {"ctrl": True}),
    ("Viewport", "mesh.detail_level_2", "2", {"ctrl": True}),
    ("Viewport", "mesh.detail_level_3", "3", {"ctrl": True}),
    ("Viewport", "mesh.detail_level_4", "4", {"ctrl": True}),
    ("Viewport", "mesh.detail_level_up", "PAGE_UP", {"alt": True}),
    ("Viewport", "mesh.detail_level_down", "PAGE_DOWN", {"alt": True}),
    # Timeline: keys, as in Blender's.
    ("Timeline", "anim.keys_transform_move", "G", {}),
    ("Timeline", "anim.keys_transform_scale", "S", {}),
    ("Timeline", "anim.keys_duplicate_move", "D", {"shift": True}),
    # Clips picked in a Media layer's row go first; otherwise the selected keys.
    ("Timeline", "media.clips_delete_selected", "X", {}),
    ("Timeline", "media.clips_delete_selected", "DEL", {}),
    ("Timeline", "anim.keys_delete", "X", {}),
    ("Timeline", "anim.keys_delete", "DEL", {}),
    ("Timeline", "anim.keys_select_all", "A", {}),
    ("Timeline", "anim.keys_copy", "C", {"ctrl": True}),
    ("Timeline", "anim.keys_paste", "V", {"ctrl": True}),
    ("Timeline", "anim.keys_deselect_all", "A", {"alt": True}),
    ("Timeline", "anim.interpolation_menu", "T", {}),
    # Properties: a masking area under the mouse.
    ("Properties", "mask.area_delete_hovered", "X", {}),
    ("Properties", "mask.area_delete_hovered", "DEL", {}),
    ("Properties", "mask.areas_group", "G", {"ctrl": True}),
    # Layers editor.
    ("Layers", "layer.delete", "X", {}),
    # Blender's Outliner Show Active.
    ("Layers", "layer.show_active", "PERIOD", {}),
    ("Layers", "layer.show_active", "NUMPAD_PERIOD", {}),
    ("Layers", "layer.delete", "DEL", {}),
    ("Layers", "layer.copy", "C", {"ctrl": True}),
    ("Layers", "layer.paste", "V", {"ctrl": True}),
    ("Layers", "layer.duplicate", "D", {"shift": True}),
    # Blender's Duplicate Linked: a live copy.
    ("Layers", "layer.make_reference", "D", {"alt": True}),
    # Group With Selection: the picked layers into a new group (a folder), as Ctrl G groups in Figma and Photoshop.
    ("Layers", "layer.group", "G", {"ctrl": True}),
    ("Layers", "armature.move_to_collection_menu", "M", {}),
    ("Layers", "armature.assign_to_collection_menu", "M", {"shift": True}),
    # The Sculpt tool, over the viewport: its brushes, and sizing the brush (Blender's F and Shift F).
    ("Sculpt", "sculpt.brush_grab", "G", {}),
    ("Sculpt", "sculpt.brush_push", "P", {}),
    ("Sculpt", "sculpt.brush_smooth", "S", {}),
    ("Sculpt", "sculpt.brush_thickness", "T", {}),
    ("Sculpt", "sculpt.brush_pinch", "I", {}),
    ("Sculpt", "sculpt.brush_twist", "W", {}),
    ("Sculpt", "sculpt.radius_adjust", "F", {}),
    ("Sculpt", "sculpt.strength_adjust", "F", {"shift": True}),
    ("Motion Path", "anim.motion_path_radius_adjust", "F", {}),
    ("Motion Path", "anim.motion_path_strength_adjust", "F", {"shift": True}),
    ("Motion Path", "anim.motion_path_scale_spacing_modal", "S", {}),
    ("Draw", "draw.width_adjust", "F", {}),
    ("Draw", "draw.opacity_adjust", "F", {"shift": True}),
    ("Draw", "draw.presets_popup", "SPACE", {"shift": True}),
)


def register():
    for keymap, idname, key, modifiers in KEYMAP:
        oanim.keymap.add(keymap, idname, key, **modifiers)
