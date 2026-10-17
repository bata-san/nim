cmake_minimum_required(VERSION 3.25)
project(nim LANGUAGES CXX VERSION 0.3.1)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# Static CRT so the released exes run without the VC++ redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

if(NOT WIN32)
  message(FATAL_ERROR "nim targets Windows only (D3D12, game ships Win64 packages)")
endif()

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /Zc:__cplusplus /MP)
  add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# ---- nim_core: package access (no GPU, no UI) --------------------------------
add_library(nim_core STATIC
  src/core/mapped_file.cpp
  src/core/oodle.cpp
  src/core/crypto.cpp
  src/core/package.cpp
  src/core/package_manager.cpp
  src/content/strings.cpp
  src/content/dialogue.cpp
  src/content/tfx.cpp
  src/content/tfx_vm.cpp
  src/content/object_channels.cpp
  src/content/map.cpp
  src/content/fonts.cpp
  src/content/animation.cpp
  src/content/weather.cpp
)
target_include_directories(nim_core PUBLIC include)
target_link_libraries(nim_core PUBLIC bcrypt)

# ---- nimcli: headless inspection / extraction --------------------------------
add_executable(nimcli tools/nimcli/main.cpp)
target_link_libraries(nimcli PRIVATE nim_core)

# ---- nim_render: D3D12 renderer running the game's own shaders --------------
add_library(nim_render STATIC
  src/render/gpu.cpp
  src/render/scene.cpp
  src/render/game_lighting.cpp
  src/render/game_post.cpp
  src/render/game_sky_ambient.cpp
)
target_link_libraries(nim_render PUBLIC nim_core d3d12 dxgi d3dcompiler)
target_link_libraries(nimcli PRIVATE nim_render)

# ---- nimshade: headless shading test -----------------------------------------
add_executable(nimshade tools/nimshade/main.cpp)
target_link_libraries(nimshade PRIVATE nim_render windowscodecs ole32)

# ---- nim: GUI viewer (Dear ImGui, docking branch) ---------------------------
set(IMGUI_DIR third_party/imgui)
add_executable(nim WIN32 tools/nim/main.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(nim PRIVATE ${IMGUI_DIR})
file(GLOB IMGUI_SOURCES ${IMGUI_DIR}/*.cpp ${IMGUI_DIR}/backends/*.cpp)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
target_link_libraries(nim PRIVATE nim_render windowscodecs ole32 shell32 dwmapi)

# ---- tests -------------------------------------------------------------------
enable_testing()
add_executable(nim_tests tests/test_main.cpp)
target_link_libraries(nim_tests PRIVATE nim_core)
add_test(NAME nim_tests COMMAND nim_tests)
