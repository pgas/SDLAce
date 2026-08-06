add_definitions(-DSCALE=2 -DWHITE_ON_BLACK -DXACE_VERSION=\"0.5\")

if(USE_SDL)
  # ---- SDL2 / macOS native build ----------------------------------------
  find_package(SDL2 REQUIRED CONFIG QUIET)

  add_executable(xace
    sdlmain.c
    z80.c
    tape.c
    keyboard_sdl.c
    spooler.c
  )

  target_compile_definitions(xace PRIVATE USE_SDL)

  # SDL2 imported targets differ slightly between cmake configs
  if(TARGET SDL2::SDL2)
    target_link_libraries(xace PRIVATE SDL2::SDL2 SDL2::SDL2main)
  else()
    # Fallback: use sdl2-config variables
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL2 REQUIRED sdl2)
    target_include_directories(xace PRIVATE ${SDL2_INCLUDE_DIRS})
    target_link_libraries(xace PRIVATE ${SDL2_LIBRARIES})
  endif()

  if(APPLE)
    # Silence SDL's redefine of main on macOS
    target_compile_definitions(xace PRIVATE SDL_MAIN_HANDLED)

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
