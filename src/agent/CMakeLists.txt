add_library(vco_game_executor STATIC broker.c executor.c)
target_include_directories(vco_game_executor PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vco_game_executor PUBLIC vco_execution)
target_compile_options(vco_game_executor PRIVATE -Wall -Wextra -Wpedantic -Werror)
