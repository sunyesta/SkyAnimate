cmake_minimum_required(VERSION 3.28)

project(OpenAnimate VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put every executable and DLL (ours, SDL3, Python) in one folder so the app runs in place.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/Dependencies.cmake)

set(OANIM_SOURCES
  src/main.cc
  src/anim/animation.cc
  src/anim/drivers.cc
  src/anim/expression.cc
  src/anim/channel.cc
  src/anim/deform.cc
  src/anim/bevel.cc
  src/anim/keep_poses.cc
  src/anim/merge.cc
  src/anim/draw_order.cc
  src/anim/layer_pick.cc
  src/anim/dynamics.cc
  src/anim/reference.cc
  src/assets/asset_library.cc
  src/assets/brushes.cc
  src/assets/brush_presets.cc
  src/core/crash.cc
  src/core/log.cc
  src/data/bones.cc
  src/data/camera.cc
  src/data/detail.cc
  src/data/document.cc
  src/data/document_layers.cc
  src/data/layer.cc
  src/data/material.cc
  src/data/media_layer.cc
  src/data/vector.cc
  src/editors/brush_editor.cc
  src/editors/console.cc
  src/editors/driver_ui.cc
  src/editors/editors.cc
  src/editors/layers_editor.cc
  src/editors/properties.cc
  src/editors/text_editor.cc
  src/editors/timeline.cc
  src/editors/timeline_media.cc
  src/editors/viewport.cc
  src/gpu/brush_stroke.cc
  src/gpu/gl.cc
  src/gpu/scene_renderer.cc
  src/gpu/tessellate.cc
  src/io/oanim_file.cc
  src/media/audio.cc
  src/media/media.cc
  src/ops/anim_ops.cc
  src/ops/draw_order_ops.cc
  src/ops/mask_ops.cc
  src/ops/material_ops.cc
  src/ops/media_ops.cc
  src/ops/asset_ops.cc
  src/ops/driver_ops.cc
  src/ops/builtin_ops.cc
  src/ops/file_ops.cc
  src/ops/layer_ops.cc
  src/ops/layer_clipboard.cc
  src/ops/operator.cc
  src/ops/rig_ops.cc
  src/ops/undo.cc
  src/ops/vector_ops.cc
  src/python/py_runtime.cc
  src/rna/rna.cc
  src/ui/theme.cc
  src/wm/app.cc
  src/wm/event_system.cc
  src/wm/file_dialog.cc
  src/wm/file_drop.cc
  src/wm/keymap.cc
  src/wm/remote.cc
)

add_executable(OpenAnimate ${OANIM_SOURCES})
# Clipper2's inline code, optimized in here, has unreachable code of its own (C4702 comes from the
# optimizer, past any pragma).
set_source_files_properties(src/anim/merge.cc PROPERTIES COMPILE_OPTIONS "/wd4702")

target_include_directories(OpenAnimate PRIVATE src)
# Header-only libraries as system headers, so /W4 doesn't report their warnings.
target_include_directories(OpenAnimate SYSTEM PRIVATE ${earcut_SOURCE_DIR}/include ${json_SOURCE_DIR}/single_include)
target_link_libraries(OpenAnimate PRIVATE oanim_imgui SDL3::SDL3 pybind11::embed miniz Clipper2 opengl32 ws2_32 dbghelp
  mfplat mfreadwrite mfuuid windowscodecs ole32 propsys shell32)
target_compile_definitions(OpenAnimate PRIVATE
  OANIM_SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/scripts"
  OANIM_PYTHON_HOME="${OANIM_PYTHON_ROOT}"
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

if(MSVC)
  target_compile_options(OpenAnimate PRIVATE /W4 /utf-8 /permissive-)
endif()

# A windowed app (no console window when opened from a shortcut) that still uses main().
# main.cc reattaches to the terminal's console when run from one.
set_target_properties(OpenAnimate PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(OpenAnimate PRIVATE /ENTRY:mainCRTStartup)

# The embedded interpreter loads python3XX.dll at startup; copy it next to the executable.
add_custom_command(TARGET OpenAnimate POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${OANIM_PYTHON_ROOT}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll"
    "${OANIM_PYTHON_ROOT}/python3.dll"
    $<TARGET_FILE_DIR:OpenAnimate>
)

enable_testing()
add_test(NAME milestone_1_gate
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_milestone_1_gate.py)
add_test(NAME rna_and_undo
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_rna_and_undo.py)
add_test(NAME text_and_console
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_text_and_console.py)
add_test(NAME drawing_and_layers
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_drawing_and_layers.py)
add_test(NAME save_and_open
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_save_and_open.py)
add_test(NAME shape_tool
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_shape_tool.py)
add_test(NAME frames_and_playback
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_frames_and_playback.py)
add_test(NAME brushes
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_brushes.py)
add_test(NAME brush_presets
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_brush_presets.py)
add_test(NAME brush_settings
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_brush_settings.py)
add_test(NAME drivers
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_drivers.py)
add_test(NAME drivers_expressions
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_drivers_expressions.py)
add_test(NAME drivers_structure
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_drivers_structure.py)
add_test(NAME drivers_rig
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_drivers_rig.py)
add_test(NAME mcp_projects
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_mcp_projects.py)
add_test(NAME mask_line_ends
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_mask_line_ends.py)
add_test(NAME delete_shapes
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_delete_shapes.py)
add_test(NAME detail_raise
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_detail_raise.py)
add_test(NAME parent_bone_deform
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_parent_bone_deform.py)
add_test(NAME resolution_link
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_resolution_link.py)
add_test(NAME flip_name
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_flip_name.py)
add_test(NAME layer_symmetrize
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_layer_symmetrize.py)
add_test(NAME make_segment
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_make_segment.py)
add_test(NAME boolean_modifier
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_boolean_modifier.py)
add_test(NAME modifier_apply
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_modifier_apply.py)
add_test(NAME point_groups
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_point_groups.py)
add_test(NAME bevel_modifier
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bevel_modifier.py)
add_test(NAME row_keys
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_row_keys.py)
add_test(NAME keep_poses
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_keep_poses.py)
add_test(NAME transform_panel
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_transform_panel.py)
add_test(NAME layer_clipboard
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_layer_clipboard.py)
add_test(NAME mask_cards
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_mask_cards.py)
add_test(NAME mirror
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_mirror.py)
add_test(NAME mask_cuts
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_mask_cuts.py)
add_test(NAME handles
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_handles.py)
add_test(NAME animation
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_animation.py)
add_test(NAME rigging
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_rigging.py)
add_test(NAME weight_paint
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_weight_paint.py)
add_test(NAME multi_layer
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_multi_layer.py)
add_test(NAME bone_editing
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_editing.py)
add_test(NAME actions
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_actions.py)
add_test(NAME smart_actions
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_smart_actions.py)
add_test(NAME bone_constraints
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_constraints.py)
add_test(NAME live_dynamics
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_live_dynamics.py)
add_test(NAME timeline_pins
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_timeline_pins.py)
add_test(NAME remote_control
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_remote_control.py)
add_test(NAME proportional_editing
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_proportional_editing.py)
add_test(NAME references
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_references.py)
add_test(NAME bone_shapes
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_shapes.py)
add_test(NAME mirror_points
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_mirror_points.py)
add_test(NAME keyed_here
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_keyed_here.py)
add_test(NAME interpolation
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_interpolation.py)
add_test(NAME reference_edges
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_reference_edges.py)
add_test(NAME draw_order
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_draw_order.py)
add_test(NAME masks
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_masks.py)
add_test(NAME materials
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_materials.py)
add_test(NAME layer_pick
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_layer_pick.py)
add_test(NAME bone_collections
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_collections.py)
add_test(NAME bone_transform
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_transform.py)
add_test(NAME point_transform
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_point_transform.py)
add_test(NAME symmetrize
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_symmetrize.py)
add_test(NAME view_settings
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_view_settings.py)
add_test(NAME selection_saved
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_selection_saved.py)
add_test(NAME camera
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_camera.py)
add_test(NAME detail_levels
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_detail_levels.py)
add_test(NAME transform_points
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_transform_points.py)
add_test(NAME media
  COMMAND OpenAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_media.py)
