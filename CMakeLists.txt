cmake_minimum_required(VERSION 3.14)

project(xAce)

enable_testing()

# Build backend selection: SDL (default on macOS) or X11
if(APPLE)
  option(USE_SDL "Build with SDL2 backend (macOS native)" ON)
else()
  option(USE_SDL "Build with SDL2 backend" OFF)
endif()

add_subdirectory(src)
add_subdirectory(tests)
