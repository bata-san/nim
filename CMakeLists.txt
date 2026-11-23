cmake_minimum_required(VERSION 3.25)
project(nim LANGUAGES CXX VERSION 0.3.8)

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
  src/content/audio.cpp
  src/content/map_audio.cpp
  src/content/wwise.cpp
  src/content/entity_audio.cpp
  src/content/usm.cpp
  src/content/mpeg1.cpp
  src/content/hca.cpp
  src/content/movies.cpp
  src/content/codex.cpp
  src/sim/ai.cpp
)
target_include_directories(nim_core PUBLIC include)
target_link_libraries(nim_core PUBLIC bcrypt)

# ---- nimcli: headless inspection / extraction --------------------------------
add_executable(nimcli tools/nimcli/main.cpp tools/nimcli/probe_search.cpp tools/nimcli/environment.cpp tools/nim/media.cpp)
target_include_directories(nimcli PRIVATE tools/nim)
target_link_libraries(nimcli PRIVATE nim_core xaudio2)

# ---- nim_render: D3D12 renderer running the game's own shaders --------------
add_library(nim_render STATIC
  src/render/gpu.cpp
  src/render/scene.cpp
  src/render/game_lighting.cpp
  src/render/game_post.cpp
  src/render/dlss.cpp
  src/render/game_sky_ambient.cpp
  src/render/game_froxels.cpp
  src/render/game_ssr.cpp
  src/render/particles.cpp
  src/render/game_light_grid.cpp
  src/render/ai_actors.cpp
)
target_link_libraries(nim_render PUBLIC nim_core d3d12 dxgi d3dcompiler)
target_link_libraries(nimcli PRIVATE nim_render)

set(NIM_DLSS_SDK "" CACHE PATH "Optional NVIDIA DLSS SDK root")
if(NIM_DLSS_SDK)
  set(NIM_NGX_LIBS "${NIM_DLSS_SDK}/lib/Windows_x86_64")
  if(NOT EXISTS "${NIM_DLSS_SDK}/include/nvsdk_ngx_helpers_d3d.h" OR
     NOT EXISTS "${NIM_NGX_LIBS}/x64/nvsdk_ngx_s.lib" OR
     NOT EXISTS "${NIM_NGX_LIBS}/rel/nvngx_dlss.dll")
    message(FATAL_ERROR "NIM_DLSS_SDK must point to the NVIDIA DLSS SDK with Windows x64 libraries")
  endif()
  target_compile_definitions(nim_render PRIVATE NIM_WITH_DLSS=1)
  target_include_directories(nim_render SYSTEM PRIVATE "${NIM_DLSS_SDK}/include")
  target_link_libraries(nim_render PRIVATE
    optimized "${NIM_NGX_LIBS}/x64/nvsdk_ngx_s.lib"
    debug "${NIM_NGX_LIBS}/x64/nvsdk_ngx_s_dbg.lib")
else()
  target_compile_definitions(nim_render PRIVATE NIM_WITH_DLSS=0)
endif()

# ---- nimshade: headless shading test -----------------------------------------
add_executable(nimshade tools/nimshade/main.cpp)
target_link_libraries(nimshade PRIVATE nim_render windowscodecs ole32 dbghelp winmm)

# ---- nimfbx_extract: dynamic model + clip -> asset.json for tools/fbx -----------
add_executable(nimfbx_extract tools/fbx/extract.cpp)
target_link_libraries(nimfbx_extract PRIVATE nim_core)

# ---- nim: GUI viewer (Dear ImGui, docking branch) ---------------------------
set(IMGUI_DIR third_party/imgui)
add_executable(nim WIN32 tools/nim/main.cpp tools/nim/media.cpp tools/nim/ui_text.cpp tools/nim/frame_generation.cpp
    ${IMGUI_DIR}/imgui.cpp ${IMGUI_DIR}/imgui_draw.cpp ${IMGUI_DIR}/imgui_tables.cpp
    ${IMGUI_DIR}/imgui_widgets.cpp ${IMGUI_DIR}/imgui_demo.cpp
    ${IMGUI_DIR}/backends/imgui_impl_win32.cpp ${IMGUI_DIR}/backends/imgui_impl_dx12.cpp)
target_include_directories(nim PRIVATE ${IMGUI_DIR})
file(GLOB IMGUI_SOURCES ${IMGUI_DIR}/*.cpp ${IMGUI_DIR}/backends/*.cpp)
set_source_files_properties(${IMGUI_SOURCES} PROPERTIES COMPILE_OPTIONS "/w")
target_link_libraries(nim PRIVATE nim_render windowscodecs ole32 shell32 dwmapi xaudio2)
target_compile_definitions(nim PRIVATE NIM_VERSION="${PROJECT_VERSION}")
add_custom_command(TARGET nim POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_CURRENT_SOURCE_DIR}/tools/nim/nim-update.ps1"
          "$<TARGET_FILE_DIR:nim>/nim-update.ps1"
  VERBATIM)
if(NIM_DLSS_SDK)
  foreach(viewer IN ITEMS nim nimshade)
    add_custom_command(TARGET ${viewer} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NIM_NGX_LIBS}/rel/nvngx_dlss.dll"
              "$<TARGET_FILE_DIR:${viewer}>/nvngx_dlss.dll"
      VERBATIM)
  endforeach()
endif()

set(NIM_STREAMLINE_SDK "" CACHE PATH "Optional NVIDIA Streamline Windows x64 SDK root")
if(NIM_STREAMLINE_SDK)
  set(NIM_SL_BIN "${NIM_STREAMLINE_SDK}/bin/x64")
  set(NIM_SL_DLLS sl.interposer.dll sl.common.dll sl.dlss_g.dll sl.reflex.dll sl.pcl.dll nvngx_dlssg.dll)
  foreach(dll IN LISTS NIM_SL_DLLS)
    if(NOT EXISTS "${NIM_SL_BIN}/${dll}")
      message(FATAL_ERROR "Streamline SDK missing bin/x64/${dll}")
    endif()
    add_custom_command(TARGET nim POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different "${NIM_SL_BIN}/${dll}" "$<TARGET_FILE_DIR:nim>/${dll}"
      VERBATIM)
  endforeach()
  target_compile_definitions(nim PRIVATE NIM_WITH_STREAMLINE=1)
  target_include_directories(nim SYSTEM PRIVATE "${NIM_STREAMLINE_SDK}/include")
  target_link_libraries(nim PRIVATE wintrust crypt32)
else()
  target_compile_definitions(nim PRIVATE NIM_WITH_STREAMLINE=0)
endif()

# ---- tests -------------------------------------------------------------------
enable_testing()
add_executable(nim_tests tests/test_main.cpp tools/nim/ui_text.cpp)
target_include_directories(nim_tests PRIVATE tools/nim)
target_link_libraries(nim_tests PRIVATE nim_core)
add_test(NAME nim_tests COMMAND nim_tests)

# Frame-input replacement is CPU-only, even though its types belong to nim_render.
add_executable(nim_render_tests tests/render_inputs.cpp)
target_link_libraries(nim_render_tests PRIVATE nim_render)
add_test(NAME nim_render_tests COMMAND nim_render_tests)
