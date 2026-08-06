cmake_minimum_required(VERSION 3.14)

project(SDLAce)

# Enable Objective-C for the macOS native UI (macos_ui.m)
if(APPLE)
  enable_language(OBJC)
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)
