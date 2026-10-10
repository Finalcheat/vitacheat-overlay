add_library(vco_catalog STATIC catalog.c encoding.c metadata.c source.c sfo.c ../ui/text.c)
target_include_directories(vco_catalog PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/../ui")
target_link_libraries(vco_catalog PUBLIC vco_cheats)
target_compile_options(vco_catalog PRIVATE -Wall -Wextra -Wpedantic -Werror)
