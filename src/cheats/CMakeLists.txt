add_library(vco_cheats STATIC parser.c compiler.c runtime.c)
target_include_directories(vco_cheats PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_compile_features(vco_cheats PUBLIC c_std_99)
target_compile_options(vco_cheats PRIVATE -Wall -Wextra -Wpedantic -Werror)
