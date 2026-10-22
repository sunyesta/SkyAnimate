# Third-party libraries, fetched at configure time and pinned to exact tags.

include(FetchContent)
set(FETCHCONTENT_QUIET OFF)

# --- SDL3: window, input, pen tablets ---------------------------------------
FetchContent_Declare(SDL3
  GIT_REPOSITORY https://github.com/libsdl-org/SDL.git
  GIT_TAG release-3.4.16
  GIT_SHALLOW TRUE
)
set(SDL_SHARED ON CACHE BOOL "" FORCE)
set(SDL_STATIC OFF CACHE BOOL "" FORCE)
set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
set(SDL_TESTS OFF CACHE BOOL "" FORCE)
set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)

# --- Dear ImGui (docking branch): UI toolkit ---------------------------------
FetchContent_Declare(imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.92.9-docking
  GIT_SHALLOW TRUE
)

# --- pybind11: embedded Python and the `oanim` module --------------------------
FetchContent_Declare(pybind11
  GIT_REPOSITORY https://github.com/pybind/pybind11.git
  GIT_TAG v3.0.4
  GIT_SHALLOW TRUE
)

# --- earcut: triangulates concave fills -----------------------------------------
# Header only; SOURCE_SUBDIR points nowhere so its own tests aren't built.
FetchContent_Declare(earcut
  GIT_REPOSITORY https://github.com/mapbox/earcut.hpp.git
  GIT_TAG v3.2.3
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-build
)

# --- miniz: zip archives (.oanim files are zips) -----------------------------------
FetchContent_Declare(miniz
  GIT_REPOSITORY https://github.com/richgel999/miniz.git
  GIT_TAG 3.1.1
  GIT_SHALLOW TRUE
)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_FUZZERS OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(INSTALL_PROJECT OFF CACHE BOOL "" FORCE)

# --- nlohmann/json: the document inside a .oanim file ----------------------------
# Header only, used from single_include; SOURCE_SUBDIR points nowhere so nothing is built.
FetchContent_Declare(json
  GIT_REPOSITORY https://github.com/nlohmann/json.git
  GIT_TAG v3.12.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR do-not-build
)

# --- Clipper2: polygon booleans and offsets (the Merge modifier) --------------------------
FetchContent_Declare(Clipper2
  GIT_REPOSITORY https://github.com/AngusJohnson/Clipper2.git
  GIT_TAG Clipper2_1.5.4
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR CPP
)
set(CLIPPER2_UTILS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_EXAMPLES OFF CACHE BOOL "" FORCE)
set(CLIPPER2_TESTS OFF CACHE BOOL "" FORCE)
set(CLIPPER2_USINGZ "OFF" CACHE STRING "" FORCE)

# The Python installation to embed. Its DLL is copied next to the executable.
set(OANIM_PYTHON_ROOT "C:/Python314" CACHE PATH "Root of the Python installation to embed")
set(Python_ROOT_DIR ${OANIM_PYTHON_ROOT})
find_package(Python 3.14 EXACT REQUIRED COMPONENTS Interpreter Development.Embed)
set(PYBIND11_FINDPYTHON ON)

FetchContent_MakeAvailable(SDL3 imgui pybind11 earcut miniz json Clipper2)

# Blender's number fields: Shift while dragging is precise (a tenth of the speed). ImGui makes
# Shift ten times faster and has no setting for it, so its fetched copy is changed here (the same
# every configure; Alt stays a hundredth).
set(_oanim_widgets "${imgui_SOURCE_DIR}/imgui_widgets.cpp")
file(READ "${_oanim_widgets}" _oanim_text)
set(_oanim_fast "if (g.IO.KeyShift && !(flags & ImGuiSliderFlags_NoSpeedTweaks))\n            adjust_delta *= 10.0f;")
set(_oanim_precise "if (g.IO.KeyShift && !(flags & ImGuiSliderFlags_NoSpeedTweaks))\n            adjust_delta *= 1.0f / 10.0f; // Open Animate: Shift is precise, as in Blender")
string(FIND "${_oanim_text}" "${_oanim_fast}" _oanim_at)
if(NOT _oanim_at EQUAL -1)
  string(REPLACE "${_oanim_fast}" "${_oanim_precise}" _oanim_text "${_oanim_text}")
  file(WRITE "${_oanim_widgets}" "${_oanim_text}")
elseif(NOT _oanim_text MATCHES "Open Animate: Shift is precise")
  message(WARNING "ImGui's drag code changed: Shift no longer made precise")
endif()

# ImGui has no CMake build of its own; compile the core plus the SDL3 and OpenGL 3 backends.
add_library(oanim_imgui STATIC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_demo.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
  ${imgui_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp
)
target_include_directories(oanim_imgui PUBLIC
  ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends ${imgui_SOURCE_DIR}/misc/cpp)
target_link_libraries(oanim_imgui PUBLIC SDL3::SDL3)
