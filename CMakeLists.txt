cmake_minimum_required(VERSION 3.24)
project(DockX LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00)
add_compile_options(/W4 /permissive- /utf-8 /EHsc /bigobj /Zc:__cplusplus /MP)

option(DOCKX_ASAN "Build with AddressSanitizer" OFF)
if(DOCKX_ASAN)
  add_compile_options(/fsanitize=address)
endif()

# Pure logic (no UI): config, JSON, layout math. Unit-tested.
add_library(dockcore STATIC
  src/core/json.cpp
  src/core/config.cpp
  src/core/strings.cpp
  src/core/log.cpp
  src/ui/layout.cpp)
target_include_directories(dockcore PUBLIC src)
target_link_libraries(dockcore PUBLIC shell32 ole32)

add_executable(DockX WIN32
  src/main.cpp
  src/render/graphics.cpp
  src/shell/apps.cpp
  src/shell/icons.cpp
  src/shell/screen.cpp
  src/shell/droptarget.cpp
  src/ui/dock.cpp
  src/ui/popup.cpp
  src/ui/menu.cpp
  src/ui/expose.cpp
  src/app.manifest)
target_link_libraries(DockX PRIVATE dockcore
  d3d11 dxgi d2d1 dwrite dxguid windowscodecs dwmapi shlwapi shell32 ole32 oleaut32
  uuid propsys windowsapp CoreMessaging version shcore)

add_executable(dock_tests tests/tests.cpp)
target_link_libraries(dock_tests PRIVATE dockcore)

enable_testing()
add_test(NAME dock_tests COMMAND dock_tests)
