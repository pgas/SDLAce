include_directories(${xAce_SOURCE_DIR}/src)

if(USE_SDL)
  # Resolve SDL2 include path once
  find_library(SDL2_LIB SDL2 HINTS /opt/homebrew/lib /usr/local/lib)
  if(NOT SDL2_LIB)
    message(FATAL_ERROR "SDL2 library not found")
  endif()
  # SDL2 headers: check canonical Homebrew location first, then pkg-config
  find_path(SDL2_INCLUDE_DIR SDL2/SDL.h
    HINTS /opt/homebrew/include /usr/local/include)
  if(NOT SDL2_INCLUDE_DIR)
    message(FATAL_ERROR "SDL2 headers not found")
  endif()

  # ---- SDL2 backend — use keyboard_sdl.c for keyboard_test and spooler_test
  set(KEYBOARD_SRC ${xAce_SOURCE_DIR}/src/keyboard_sdl.c)

  add_executable(keyboard_test keyboard_test.c ${KEYBOARD_SRC})
  target_compile_definitions(keyboard_test PRIVATE USE_SDL)
  target_include_directories(keyboard_test PRIVATE ${SDL2_INCLUDE_DIR})
  target_link_libraries(keyboard_test PRIVATE ${SDL2_LIB})

  add_executable(spooler_test spooler_test.c
    ${xAce_SOURCE_DIR}/src/spooler.c
    ${KEYBOARD_SRC}
  )
  target_compile_definitions(spooler_test PRIVATE USE_SDL)
  target_include_directories(spooler_test PRIVATE ${SDL2_INCLUDE_DIR})
  target_link_libraries(spooler_test PRIVATE ${SDL2_LIB})

else()
  # Original X11 backend
  add_executable(keyboard_test keyboard_test.c ${xAce_SOURCE_DIR}/src/keyboard.c)
  target_link_libraries(keyboard_test X11)

  add_executable(spooler_test spooler_test.c ${xAce_SOURCE_DIR}/src/spooler.c)
  target_link_libraries(spooler_test)
endif()

add_executable(tape_test tape_test.c ${xAce_SOURCE_DIR}/src/tape.c)
target_link_libraries(tape_test)

add_test(NAME tape_test     COMMAND tape_test     WORKING_DIRECTORY ${xAce_SOURCE_DIR}/tests)
add_test(NAME keyboard_test COMMAND keyboard_test WORKING_DIRECTORY ${xAce_SOURCE_DIR}/tests)
add_test(NAME spooler_test  COMMAND spooler_test  WORKING_DIRECTORY ${xAce_SOURCE_DIR}/tests)
