include_directories(${CMAKE_SOURCE_DIR}/src)

# Locate SDL2 for tests (no SDL2main needed)
find_library(SDL2_LIB SDL2 HINTS /opt/homebrew/lib /usr/local/lib REQUIRED)
find_path(SDL2_INCLUDE_DIR SDL2/SDL.h
  HINTS /opt/homebrew/include /usr/local/include REQUIRED)

set(KEYBOARD_SRC ${CMAKE_SOURCE_DIR}/src/keyboard_sdl.c)

add_executable(keyboard_test keyboard_test.c ${KEYBOARD_SRC})
target_compile_definitions(keyboard_test PRIVATE USE_SDL)
target_include_directories(keyboard_test PRIVATE ${SDL2_INCLUDE_DIR})
target_link_libraries(keyboard_test PRIVATE ${SDL2_LIB})

add_executable(spooler_test spooler_test.c
  ${CMAKE_SOURCE_DIR}/src/spooler.c
  ${KEYBOARD_SRC}
)
target_compile_definitions(spooler_test PRIVATE USE_SDL)
target_include_directories(spooler_test PRIVATE ${SDL2_INCLUDE_DIR})
target_link_libraries(spooler_test PRIVATE ${SDL2_LIB})

add_executable(tape_test tape_test.c ${CMAKE_SOURCE_DIR}/src/tape.c)
target_link_libraries(tape_test)

add_test(NAME tape_test     COMMAND tape_test     WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME keyboard_test COMMAND keyboard_test WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
add_test(NAME spooler_test  COMMAND spooler_test  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/tests)
