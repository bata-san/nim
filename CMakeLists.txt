cmake_minimum_required(VERSION 3.25)
project(nim LANGUAGES CXX VERSION 0.1.0)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

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
)
target_include_directories(nim_core PUBLIC include)
target_link_libraries(nim_core PUBLIC bcrypt)

# ---- nimcli: headless inspection / extraction --------------------------------
add_executable(nimcli tools/nimcli/main.cpp)
target_link_libraries(nimcli PRIVATE nim_core)

# ---- tests -------------------------------------------------------------------
enable_testing()
add_executable(nim_tests tests/test_main.cpp)
target_link_libraries(nim_tests PRIVATE nim_core)
add_test(NAME nim_tests COMMAND nim_tests)
