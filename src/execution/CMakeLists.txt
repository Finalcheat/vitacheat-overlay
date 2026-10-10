add_library(vco_execution STATIC admission.c session.c memory.c close_key.c close_queue.c engine.c control.c client.c mailbox.c transport.c upload.c trace.c)
target_include_directories(vco_execution PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(vco_execution PUBLIC vco_cheats)
target_compile_options(vco_execution PRIVATE -Wall -Wextra -Wpedantic -Werror)
