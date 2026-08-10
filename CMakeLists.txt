cmake_minimum_required(VERSION 3.14)

project(SDLAce)

# Enable Objective-C for the macOS native UI (macos_ui.m)
if(APPLE)
  enable_language(OBJC)
endif()

enable_testing()

add_subdirectory(src)
add_subdirectory(tests)

if(APPLE)
  set(CPACK_GENERATOR "DragNDrop")
  set(CPACK_PACKAGE_FILE_NAME "sdlace-macos")
  set(CPACK_DMG_VOLUME_NAME "SDLAce")
  include(CPack)
endif()
