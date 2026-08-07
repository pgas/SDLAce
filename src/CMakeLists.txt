
cmake_minimum_required(VERSION 3.10)

project("SDLAce")

add_definitions(-DSCALE=2 -DWHITE_ON_BLACK -DXACE_VERSION=\"0.5\")

# ---- SDL2 build (only backend) ----------------------------------------

# Locate SDL2 headers and library directly (avoids SDL2main confusion)
find_library(SDL2_LIB SDL2
  HINTS /opt/homebrew/lib /usr/local/lib
  REQUIRED)
find_path(SDL2_INCLUDE_DIR SDL2/SDL.h
  HINTS /opt/homebrew/include /usr/local/include
  REQUIRED)

# Locate SDL2_ttf for bezel text rendering
find_library(SDL2_TTF_LIB SDL2_ttf
  HINTS /opt/homebrew/lib /usr/local/lib
  REQUIRED)
find_path(SDL2_TTF_INCLUDE_DIR SDL2/SDL_ttf.h
  HINTS /opt/homebrew/include /usr/local/include
  REQUIRED)

message(STATUS "SDL2     lib: ${SDL2_LIB}")
message(STATUS "SDL2_ttf lib: ${SDL2_TTF_LIB}")

if(APPLE)
  set(PLATFORM_SOURCES macos_ui.m)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)
  set(PLATFORM_SOURCES linux_ui.c)
endif()

add_executable(sdlace
  sdlmain.c
  z80.c
  tape.c
  keyboard_sdl.c
  spooler.c
  ${PLATFORM_SOURCES}
)

target_compile_definitions(sdlace PRIVATE USE_SDL SDL_MAIN_HANDLED)
target_include_directories(sdlace PRIVATE
  ${SDL2_INCLUDE_DIR}
  ${SDL2_TTF_INCLUDE_DIR}
)
target_link_libraries(sdlace PRIVATE ${SDL2_LIB} ${SDL2_TTF_LIB})

if(NOT APPLE)
  target_link_libraries(sdlace PRIVATE PkgConfig::GTK3)
endif()
if(APPLE)
  set_source_files_properties(macos_ui.m PROPERTIES LANGUAGE OBJC)
  target_link_libraries(sdlace PRIVATE "-framework Cocoa")

  set_target_properties(sdlace PROPERTIES
    MACOSX_BUNDLE TRUE
    MACOSX_BUNDLE_BUNDLE_NAME "SDLAce"
    MACOSX_BUNDLE_GUI_IDENTIFIER "org.sdlace.emulator"
    MACOSX_BUNDLE_BUNDLE_VERSION "0.5"
    MACOSX_BUNDLE_SHORT_VERSION_STRING "0.5"
    OUTPUT_NAME "SDLAce"
  )

  # Bundle the ROM
  set(ROM_SRC "${CMAKE_SOURCE_DIR}/ace.rom")
  set_source_files_properties(${ROM_SRC} PROPERTIES
    MACOSX_PACKAGE_LOCATION "Resources"
  )
  target_sources(sdlace PRIVATE ${ROM_SRC})
endif()

install(TARGETS sdlace DESTINATION bin)
