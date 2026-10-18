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
  src/core/log.cc
  src/data/document.cc
  src/editors/editors.cc
  src/editors/timeline.cc
  src/ops/builtin_ops.cc
  src/ops/operator.cc
  src/ops/undo.cc
  src/python/py_runtime.cc
  src/rna/rna.cc
  src/ui/theme.cc
  src/wm/app.cc
  src/wm/event_system.cc
  src/wm/keymap.cc
)

add_executable(SkyAnimate ${SKY_SOURCES})

target_include_directories(SkyAnimate PRIVATE src)
target_link_libraries(SkyAnimate PRIVATE sky_imgui SDL3::SDL3 pybind11::embed opengl32)
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
add_test(NAME frames_and_playback
  COMMAND SkyAnimate --background --python ${CMAKE_SOURCE_DIR}/tests/test_frames_and_playback.py)
