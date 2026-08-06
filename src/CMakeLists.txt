add_definitions(-DSCALE=2 -DWHITE_ON_BLACK -DXACE_VERSION=\"0.5\")

if(USE_SDL)
  # ---- SDL2 / macOS native build ----------------------------------------

  # Locate SDL2 headers and library directly (avoids SDL2main confusion)
  find_library(SDL2_LIB SDL2
    HINTS /opt/homebrew/lib /usr/local/lib
    REQUIRED)
  find_path(SDL2_INCLUDE_DIR SDL2/SDL.h
    HINTS /opt/homebrew/include /usr/local/include
    REQUIRED)
  message(STATUS "SDL2 lib:     ${SDL2_LIB}")
  message(STATUS "SDL2 include: ${SDL2_INCLUDE_DIR}")

  if(APPLE)
    set(PLATFORM_SOURCES macos_ui.m)
  else()
    set(PLATFORM_SOURCES "")
  endif()

  add_executable(xace
    sdlmain.c
    z80.c
    tape.c
    keyboard_sdl.c
    spooler.c
    ${PLATFORM_SOURCES}
  )

  target_compile_definitions(xace PRIVATE USE_SDL SDL_MAIN_HANDLED)
  target_include_directories(xace PRIVATE ${SDL2_INCLUDE_DIR})
  target_link_libraries(xace PRIVATE ${SDL2_LIB})

  if(APPLE)
    # Compile the Objective-C file correctly
    set_source_files_properties(macos_ui.m PROPERTIES LANGUAGE OBJC)

    # Cocoa for NSMenu, NSOpenPanel, dispatch queues
    target_link_libraries(xace PRIVATE "-framework Cocoa")

    # Create a proper macOS .app bundle
    set_target_properties(xace PROPERTIES
      MACOSX_BUNDLE TRUE
      MACOSX_BUNDLE_BUNDLE_NAME "xAce"
      MACOSX_BUNDLE_GUI_IDENTIFIER "org.xace.emulator"
      MACOSX_BUNDLE_BUNDLE_VERSION "0.5"
      MACOSX_BUNDLE_SHORT_VERSION_STRING "0.5"
      OUTPUT_NAME "xAce"
    )

    # Copy the ROM into the bundle's Resources directory
    set(ROM_SRC "${CMAKE_SOURCE_DIR}/ace.rom")
    set_source_files_properties(${ROM_SRC} PROPERTIES
      MACOSX_PACKAGE_LOCATION "Resources"
    )
    target_sources(xace PRIVATE ${ROM_SRC})
  endif()

else()
  # ---- Original X11 build -----------------------------------------------
  add_executable(xace
    xmain.c
    z80.c
    tape.c
    keyboard.c
    spooler.c
  )
  target_link_libraries(xace PRIVATE X11 Xext)
endif()

install(TARGETS xace DESTINATION bin)
