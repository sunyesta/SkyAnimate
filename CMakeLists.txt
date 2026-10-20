cmake_minimum_required(VERSION 3.28)

project(SkyAnimate VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Put every executable and DLL (ours, SDL3, Python) in one folder so the app runs in place.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

include(cmake/Dependencies.cmake)

set(SKY_SOURCES
  src/main.cc
  src/anim/animation.cc
  src/anim/channel.cc
  src/anim/deform.cc
  src/anim/draw_order.cc
  src/anim/dynamics.cc
  src/anim/reference.cc
  src/assets/asset_library.cc
  src/assets/brushes.cc
  src/core/crash.cc
  src/core/log.cc
  src/data/bones.cc
  src/data/document.cc
  src/data/document_layers.cc
  src/data/layer.cc
  src/data/vector.cc
  src/editors/brush_settings.cc
  src/editors/console.cc
  src/editors/editors.cc
  src/editors/layers_editor.cc
  src/editors/properties.cc
  src/editors/text_editor.cc
  src/editors/timeline.cc
  src/editors/viewport.cc
  src/gpu/brush_stroke.cc
  src/gpu/gl.cc
  src/gpu/scene_renderer.cc
  src/gpu/tessellate.cc
  src/io/sky_file.cc
  src/ops/anim_ops.cc
  src/ops/draw_order_ops.cc
  src/ops/mask_ops.cc
  src/ops/asset_ops.cc
  src/ops/builtin_ops.cc
  src/ops/file_ops.cc
  src/ops/layer_ops.cc
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
  src/wm/keymap.cc
  src/wm/remote.cc
)

add_executable(SkyAnimate ${SKY_SOURCES})

target_include_directories(SkyAnimate PRIVATE src)
# Header-only libraries as system headers, so /W4 doesn't report their warnings.
target_include_directories(SkyAnimate SYSTEM PRIVATE ${earcut_SOURCE_DIR}/include ${json_SOURCE_DIR}/single_include)
target_link_libraries(SkyAnimate PRIVATE sky_imgui SDL3::SDL3 pybind11::embed miniz opengl32 ws2_32 dbghelp)
target_compile_definitions(SkyAnimate PRIVATE
  SKY_SCRIPTS_DIR="${CMAKE_SOURCE_DIR}/scripts"
  SKY_PYTHON_HOME="${SKY_PYTHON_ROOT}"
  NOMINMAX
  WIN32_LEAN_AND_MEAN
)

if(MSVC)
  target_compile_options(SkyAnimate PRIVATE /W4 /utf-8 /permissive-)
endif()

# A windowed app (no console window when opened from a shortcut) that still uses main().
# main.cc reattaches to the terminal's console when run from one.
set_target_properties(SkyAnimate PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(SkyAnimate PRIVATE /ENTRY:mainCRTStartup)

# The embedded interpreter loads python3XX.dll at startup; copy it next to the executable.
add_custom_command(TARGET SkyAnimate POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
    "${SKY_PYTHON_ROOT}/python${Python_VERSION_MAJOR}${Python_VERSION_MINOR}.dll"
    "${SKY_PYTHON_ROOT}/python3.dll"
    $<TARGET_FILE_DIR:SkyAnimate>
)

enable_testing()
add_test(NAME milestone_1_gate
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_milestone_1_gate.py)
add_test(NAME rna_and_undo
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_rna_and_undo.py)
add_test(NAME text_and_console
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_text_and_console.py)
add_test(NAME drawing_and_layers
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_drawing_and_layers.py)
add_test(NAME save_and_open
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_save_and_open.py)
add_test(NAME shape_tool
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_shape_tool.py)
add_test(NAME frames_and_playback
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_frames_and_playback.py)
add_test(NAME brushes
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_brushes.py)
add_test(NAME handles
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_handles.py)
add_test(NAME animation
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_animation.py)
add_test(NAME rigging
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_rigging.py)
add_test(NAME weight_paint
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_weight_paint.py)
add_test(NAME multi_layer
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_multi_layer.py)
add_test(NAME bone_editing
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_editing.py)
add_test(NAME actions
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_actions.py)
add_test(NAME smart_actions
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_smart_actions.py)
add_test(NAME bone_constraints
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_constraints.py)
add_test(NAME live_dynamics
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_live_dynamics.py)
add_test(NAME timeline_pins
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_timeline_pins.py)
add_test(NAME remote_control
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_remote_control.py)
add_test(NAME proportional_editing
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_proportional_editing.py)
add_test(NAME references
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_references.py)
add_test(NAME bone_shapes
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_shapes.py)
add_test(NAME mirror_points
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_mirror_points.py)
add_test(NAME keyed_here
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_keyed_here.py)
add_test(NAME interpolation
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_interpolation.py)
add_test(NAME reference_edges
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_reference_edges.py)
add_test(NAME draw_order
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_draw_order.py)
add_test(NAME masks
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_masks.py)
add_test(NAME bone_collections
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_collections.py)
add_test(NAME bone_transform
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_bone_transform.py)
