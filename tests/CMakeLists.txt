cmake_minimum_required(VERSION 3.16)
project(VcoHostTests LANGUAGES C CXX)
set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(VCO_SANITIZE "Address and undefined behavior sanitizers" ON)
enable_testing()
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  # UBSan normally reports and continues, which can leave CTest falsely green.
  # Fail at the first finding even without a caller-provided environment flag.
  add_compile_options(-fno-sanitize-recover=all)
endif()
add_subdirectory(../src/cheats cheats-core)
add_subdirectory(../src/catalog catalog-core)
add_compile_options(-Wall -Wextra -Wpedantic -Werror -UNDEBUG)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(vco_cheats PUBLIC -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(vco_cheats PUBLIC -fsanitize=address,undefined)
endif()
add_executable(cheat_tests cheats/cheat_test.c)
target_link_libraries(cheat_tests PRIVATE vco_cheats)
add_executable(cheat_diff_driver cheats/diff_driver.c)
target_link_libraries(cheat_diff_driver PRIVATE vco_cheats)
foreach(case parser write pointer dormant_pointer copy_headers access_session patch unaligned_patch control failure widths boundaries capacity mutations compatibility conditional_patch module_combinations offsets depth)
  add_test(NAME cheats_${case} COMMAND cheat_tests ${case})
endforeach()


add_executable(source_test source_test.c)
target_include_directories(source_test PRIVATE ../src)
target_link_libraries(source_test PRIVATE vco_catalog)
foreach(case hash changes missing failures bounds)
  add_test(NAME catalog_source_${case} COMMAND source_test ${case})
endforeach()
add_executable(input_test input_test.c)
target_include_directories(input_test PRIVATE ../src)
add_test(NAME input_state COMMAND input_test)
add_executable(regional_input_test regional_input_test.c)
target_include_directories(regional_input_test PRIVATE ../src)
foreach(case asian western)
  add_test(NAME input_regional_${case} COMMAND regional_input_test ${case})
endforeach()

add_executable(catalog_tests catalog_test.c)
target_link_libraries(catalog_tests PRIVATE vco_catalog)
add_test(NAME catalog_pipeline COMMAND catalog_tests)

add_executable(encoding_test encoding_test.c)
target_link_libraries(encoding_test PRIVATE vco_catalog)
foreach(case utf8 gbk invalid limits maximum mutations)
  add_test(NAME catalog_encoding_${case} COMMAND encoding_test ${case})
endforeach()

add_executable(settings_test settings_test.c)
target_include_directories(settings_test PRIVATE ../src)
add_test(NAME settings_draft COMMAND settings_test)

add_executable(bindings_test bindings_test.c)
target_include_directories(bindings_test PRIVATE ../src)
foreach(case validation capture names)
  add_test(NAME bindings_${case} COMMAND bindings_test ${case})
endforeach()

add_executable(shell_binding_test shell_binding_test.cpp ../src/input/touch.c ../src/ui/execution_view.c)
target_include_directories(shell_binding_test PRIVATE native_shell_binding ../src ../src/catalog ../src/cheats)
set_target_properties(shell_binding_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
foreach(case safety capture touch apply startup mapping opacity screenshot language)
  add_test(NAME shell_bindings_${case} COMMAND shell_binding_test ${case})
endforeach()

add_executable(config_test config_test.c ../src/config/config.c)
target_include_directories(config_test PRIVATE ../src)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  foreach(target config_test settings_test input_test regional_input_test bindings_test shell_binding_test)
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined)
  endforeach()
endif()
add_test(NAME settings_storage COMMAND config_test)

add_executable(overlay_view_test overlay_view_test.cpp ../src/ui/execution_view.c)
target_include_directories(overlay_view_test PRIVATE native_overlay_view ../src ../src/cheats)
target_link_libraries(overlay_view_test PRIVATE vco_catalog)
set_target_properties(overlay_view_test PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_options(overlay_view_test PRIVATE $<$<COMPILE_LANGUAGE:CXX>:-fshort-wchar>)
foreach(case appearance allocation switches regional localization)
  add_test(NAME overlay_view_${case} COMMAND overlay_view_test ${case})
endforeach()

add_subdirectory(../src/execution execution-core)
add_subdirectory(../src/agent game-executor)
add_executable(game_executor_test game_executor_test.c)
target_include_directories(game_executor_test PRIVATE ../src)
target_link_libraries(game_executor_test PRIVATE vco_game_executor)
foreach(case data patches pause retry replacement failures termination termination_patch identity packets replies ownership capacity fixture
             module_patch module_join module_timeout module_failure module_owner module_terminal module_limits module_replacement
             stopped_receipt stopped_module stopped_owner stopped_restore_failure stopped_stale)
  add_test(NAME game_executor_${case} COMMAND game_executor_test ${case}
    "${CMAKE_CURRENT_SOURCE_DIR}/cheats/fixtures/device/PCSB00031-20261007.psv")
endforeach()
add_executable(game_worker_test game_worker_test.c ../src/agent/vita_worker.c)
target_include_directories(game_worker_test PRIVATE native_game_worker ../src)
target_link_libraries(game_worker_test PRIVATE vco_game_executor)
foreach(case lifecycle execution stale transient receipt faults concurrent stopped_retry stopped_failure stopped_timeout)
  add_test(NAME game_worker_${case} COMMAND game_worker_test ${case})
endforeach()
set_tests_properties(game_worker_concurrent PROPERTIES TIMEOUT 15)
add_executable(game_kernel_test game_kernel_test.c ../src/agent/native_service.c ../src/agent/native_loader.c ../src/agent/native_access.c ../src/agent/native_modules.c
  ../src/platform/vita/shell_owner.c)
target_include_directories(game_kernel_test PRIVATE native_game_kernel native_service native_events native_owner native_pad ../src)
target_compile_options(game_kernel_test PRIVATE -Wno-pedantic)
target_link_libraries(game_kernel_test PRIVATE vco_game_executor)
foreach(case pipeline authorization lease terminal races copies processed_failure owner_restart owner_same_pid
             loader loader_fallback loader_failure loader_start_failure loader_context loader_failure_passthrough loader_start_failure_passthrough
             loader_process_success loader_process_load_failure loader_process_start_failure
             loader_process_permission_failure loader_process_pid_failure loader_process_already_kernel
             loader_stages_success loader_stages_load_failure loader_stages_start_failure
             loader_stages_entry_failure loader_stages_cleanup_failure
             loader_exports_missing loader_exports_fallback
             loader_file_fallback loader_file_original loader_file_failure loader_file_scope loader_file_flags loader_file_hook_failure loader_file_retry loader_file_nested
             loader_gate loader_binding_retry loader_binding_exhaustion launch_receipt stale_failure missing_title register_failure hook_failure concurrent
             syscall_packets_headers syscall_packets_addresses syscall_packets_copies syscall_packets_session syscall_packets_snapshot
             trace_authorization trace_packet trace_copy trace_limit
             access access_faults access_races modules module_fields pad access_missing access_fallback
             game_fault stale_game_fault module_boundary module_negative module_nested module_timeout module_exit module_concurrent
             module_restore_failure module_gate_failure
             stopped_native stopped_busy stopped_copy stopped_stale stopped_module_race
             module_hook_fail_0 module_hook_fail_1 module_hook_fail_2 module_hook_fail_3 module_hook_fail_4
             module_hook_fail_5 module_hook_fail_6 module_hook_fail_7)
  add_test(NAME game_kernel_${case} COMMAND game_kernel_test ${case})
endforeach()
set_tests_properties(game_kernel_concurrent PROPERTIES TIMEOUT 15)
set_tests_properties(game_kernel_module_concurrent PROPERTIES TIMEOUT 15)
add_executable(game_memory_test game_memory_test.c ../src/agent/vita_memory.c)
target_include_directories(game_memory_test PRIVATE native_game_memory native_game_worker ../src)
target_link_libraries(game_memory_test PRIVATE vco_game_executor)
foreach(case data modules patches absolute private release_failure release_positive diagnostic_budget
             partial_patch_failure partial_restore_failure revoked capacity pad neutral snapshot_failure executor fixture
             busy_init busy_restore busy_snapshot busy_exhaustion busy_revoke busy_write trace_recovery)
  add_test(NAME game_memory_${case} COMMAND game_memory_test ${case}
    "${CMAKE_CURRENT_SOURCE_DIR}/cheats/fixtures/device/PCSB00031-20261007.psv")
endforeach()
add_test(NAME game_memory_energy COMMAND game_memory_test energy
  "${CMAKE_CURRENT_SOURCE_DIR}/cheats/fixtures/device/PCSB00031-energy-20261009.psv")
foreach(case trajectory_pause trajectory_guard trajectory_changed trajectory_release)
  add_test(NAME game_memory_${case} COMMAND game_memory_test ${case}
    "${CMAKE_CURRENT_SOURCE_DIR}/cheats/fixtures/device/PCSB00031-trajectory-20261009.psv")
endforeach()
add_executable(game_entry_test game_entry_test.c ../src/agent/game.c ../src/agent/vita_worker.c
  ../src/agent/vita_memory.c ../src/agent/vita_allocator.c ../src/agent/native_modules.c)
target_compile_options(game_entry_test PRIVATE -Wno-pedantic)
target_include_directories(game_entry_test PRIVATE native_game_entry native_game_worker native_game_memory ../src)
target_link_libraries(game_entry_test PRIVATE vco_game_executor)
foreach(case pipeline enabled_stop invalid attach attach_busy attach_busy_exhaustion alloc base create start delete wait revoked fault restore_failure
             pool_capacity pool_faults pool_free_failure stopped_failure boundary_stop boundary_stop_unload boundary_restore_failure)
  add_test(NAME game_entry_${case} COMMAND game_entry_test ${case})
endforeach()
set_tests_properties(game_entry_boundary_stop game_entry_boundary_stop_unload game_entry_boundary_restore_failure PROPERTIES TIMEOUT 15)
add_executable(execution_test execution_test.c)
target_include_directories(execution_test PRIVATE ../src)
target_link_libraries(execution_test PRIVATE vco_execution)
foreach(case session memory engine patches authority_pause patch_authority_pause paused_capacity
             dynamic_claims claim_pressure guarded_claims guarded_data_claims idle_prepare budget)
  add_test(NAME execution_${case} COMMAND execution_test ${case})
endforeach()

add_executable(engine_close_test engine_close_test.c)
target_include_directories(engine_close_test PRIVATE ../src)
target_link_libraries(engine_close_test PRIVATE vco_execution)
foreach(case cycle identities failures transactions)
  add_test(NAME engine_close_${case} COMMAND engine_close_test ${case})
endforeach()

add_executable(touch_test touch_test.c ../src/input/touch.c)
target_include_directories(touch_test PRIVATE ../src)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(touch_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(touch_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case mapping gestures release slider slider_cancel)
  add_test(NAME touch_${case} COMMAND touch_test ${case})
endforeach()

# Build the actual kernel adapter against fake SDK/taiHEN calls, not a rewrite
# of its filtering logic. GNU function-pointer API follows the native SDK.
add_executable(touch_kernel_test touch_kernel_test.c ../src/platform/touch_kernel.c ../src/input/touch.c)
target_include_directories(touch_kernel_test PRIVATE native_touch ../src)
target_compile_options(touch_kernel_test PRIVATE -Wno-pedantic)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(touch_kernel_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(touch_kernel_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME touch_kernel_adapter COMMAND touch_kernel_test)
foreach(case fail0 fail1 fail2 fail3 no_panel)
  add_test(NAME touch_kernel_${case} COMMAND touch_kernel_test ${case})
endforeach()

# Firmware-specific diagnostic exports must not become mandatory imports.
add_executable(probe_kernel_test probe_kernel_test.c ../src/platform/game_probe_kernel.c)
target_include_directories(probe_kernel_test PRIVATE native_probe ../src)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(probe_kernel_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(probe_kernel_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME probe_firmware_compatibility COMMAND probe_kernel_test)

# Compile the native implementation itself against fault-injectable SDK calls.
add_executable(vita_memory_test vita_memory_test.c ../src/platform/vita/memory.c ../src/platform/vita/patches.c ../src/platform/vita/execution_state.c)
target_include_directories(vita_memory_test PRIVATE native_memory ../src)
target_link_libraries(vita_memory_test PRIVATE vco_execution)
foreach(case data_admission patch_admission transfer memblock_contract memblock_spans patch_spans memory_trace denied lifecycle faults patches pipeline scoped_close features patch_probe patch_lifecycle patch_faults patch_mapping patch_modules patch_rearm patch_authority_cleanup patch_authority_failures patch_authority_engine patch_trace patch_failure_stage patch_preacquisition_authority patch_confirmation patch_closed_confirmation patch_restore_confirmation patch_preflight patch_publication patch_native_state patch_capacity authority_pause guarded_claims)
  add_test(NAME vita_memory_${case} COMMAND vita_memory_test ${case})
endforeach()
add_test(NAME vita_memory_pcsb00031 COMMAND vita_memory_test pcsb00031
  "${CMAKE_CURRENT_SOURCE_DIR}/cheats/fixtures/device/PCSB00031.psv")
add_test(NAME vita_memory_pcsb00031_patches COMMAND vita_memory_test pcsb00031_patches
  "${CMAKE_CURRENT_SOURCE_DIR}/cheats/fixtures/device/PCSB00031-patches.psv")
add_test(NAME vita_memory_pcsb00031_trajectory COMMAND vita_memory_test pcsb00031_trajectory
  "${CMAKE_CURRENT_SOURCE_DIR}/cheats/fixtures/device/PCSB00031-20261007.psv")

add_executable(control_test control_test.c)
target_link_libraries(control_test PRIVATE vco_execution)
target_include_directories(control_test PRIVATE ../src)
foreach(case packets commands queue client)
  add_test(NAME control_${case} COMMAND control_test ${case})
endforeach()

find_package(Threads REQUIRED)
target_link_libraries(game_worker_test PRIVATE Threads::Threads)
target_link_libraries(game_kernel_test PRIVATE Threads::Threads)
add_executable(close_queue_test close_queue_test.c)
target_link_libraries(close_queue_test PRIVATE vco_execution Threads::Threads)
target_include_directories(close_queue_test PRIVATE ../src)
foreach(case delivery bounds shutdown concurrent completion completion_stop completion_concurrent)
  add_test(NAME close_queue_${case} COMMAND close_queue_test ${case})
endforeach()
set_tests_properties(close_queue_concurrent close_queue_completion_concurrent PROPERTIES TIMEOUT 15)
add_executable(mailbox_test mailbox_test.c)
target_link_libraries(mailbox_test PRIVATE vco_execution Threads::Threads)
target_include_directories(mailbox_test PRIVATE ../src)
foreach(case delivery shutdown bounds concurrent reconnect)
  add_test(NAME mailbox_${case} COMMAND mailbox_test ${case})
endforeach()

add_executable(control_worker_test control_worker_test.c ../src/platform/vita/control_worker.c ../src/platform/vita/close_worker.c)
target_include_directories(control_worker_test PRIVATE native_worker ../src)
target_link_libraries(control_worker_test PRIVATE vco_execution Threads::Threads)
foreach(case lifecycle failures transport reap_stopped finalization async_stop shutdown_failures shutdown_concurrent shutdown_restart scoped_close scoped_fault scoped_racing_update scoped_concurrent scoped_finish scoped_finish_stop)
  add_test(NAME control_worker_${case} COMMAND control_worker_test ${case})
endforeach()
set_tests_properties(control_worker_shutdown_concurrent control_worker_scoped_concurrent PROPERTIES TIMEOUT 15)

add_executable(control_rpc_test control_rpc_test.c ../src/platform/vita/control_rpc.c)
target_include_directories(control_rpc_test PRIVATE native_rpc ../src)
target_link_libraries(control_rpc_test PRIVATE vco_execution)
foreach(case authorization buffers copy_failures pipeline)
  add_test(NAME control_rpc_${case} COMMAND control_rpc_test ${case})
endforeach()

add_executable(transport_test transport_test.c)
target_link_libraries(transport_test PRIVATE vco_execution)
target_include_directories(transport_test PRIVATE ../src)
foreach(case delivery retries recovery rebind invalid bounded)
  add_test(NAME transport_${case} COMMAND transport_test ${case})
endforeach()

add_executable(shell_owner_test shell_owner_test.c ../src/platform/vita/shell_owner.c)
target_include_directories(shell_owner_test PRIVATE native_owner ../src)
target_link_libraries(shell_owner_test PRIVATE vco_execution)
foreach(case binding authorization faults)
  add_test(NAME shell_owner_${case} COMMAND shell_owner_test ${case})
endforeach()

add_executable(foreground_probe_test foreground_probe_test.c ../src/platform/foreground_probe.c)
target_include_directories(foreground_probe_test PRIVATE native_foreground ../src)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(foreground_probe_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(foreground_probe_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case transitions faults races)
  add_test(NAME foreground_${case} COMMAND foreground_probe_test ${case})
endforeach()

add_executable(process_watch_test process_watch_test.c ../src/platform/vita/process_watch.c)
target_include_directories(process_watch_test PRIVATE ../src)
target_link_libraries(process_watch_test PRIVATE vco_execution)
foreach(case transitions faults bounded pipeline)
  add_test(NAME process_watch_${case} COMMAND process_watch_test ${case})
endforeach()

add_executable(process_events_test process_events_test.c ../src/platform/vita/process_events.c)
target_include_directories(process_events_test PRIVATE native_events ../src)
target_compile_definitions(process_events_test PRIVATE VCO_PROCESS_EVENTS_TESTING)
target_link_libraries(process_events_test PRIVATE vco_execution Threads::Threads)
foreach(case callbacks lifecycle register_failure unregister_failure overflow contention exhaustion
             invalid_metadata transition_callback lifecycle_races concurrent terminal_watch)
  add_test(NAME process_events_${case} COMMAND process_events_test ${case})
endforeach()

add_executable(vita_pad_test vita_pad_test.c ../src/platform/vita/pad.c)
target_include_directories(vita_pad_test PRIVATE native_pad ../src)
target_link_libraries(vita_pad_test PRIVATE vco_execution)
foreach(case mapping capture lifecycle failures pipeline controllers)
  add_test(NAME vita_pad_${case} COMMAND vita_pad_test ${case})
endforeach()

add_executable(vita_modules_test vita_modules_test.c ../src/platform/vita/modules.c)
target_include_directories(vita_modules_test PRIVATE native_modules ../src)
target_link_libraries(vita_modules_test PRIVATE vco_execution)
foreach(case firmware snapshots mapping boundaries lifecycle faults pipeline auto_mapping auto_pipeline background_pipeline enable_growth enable_growth_failure patch_guards negative_guards ownership_segments diagnostics empty_segments segment_diagnostics)
  add_test(NAME vita_modules_${case} COMMAND vita_modules_test ${case})
endforeach()

add_executable(vita_authority_test vita_authority_test.c ../src/platform/vita/authority.c)
target_include_directories(vita_authority_test PRIVATE native_authority ../src)
target_link_libraries(vita_authority_test PRIVATE vco_execution)
foreach(case binding transitions leases packets events replacements faults terminal_latch)
  add_test(NAME vita_authority_${case} COMMAND vita_authority_test ${case})
endforeach()
add_executable(vita_terminal_integration_test vita_terminal_integration_test.c
  ../src/platform/vita/authority.c ../src/platform/vita/process_events.c)
target_include_directories(vita_terminal_integration_test PRIVATE native_authority native_events ../src)
# The two independent SDK fixtures both declare the identical SceUID typedef.
set_property(TARGET vita_terminal_integration_test PROPERTY C_STANDARD 11)
target_link_libraries(vita_terminal_integration_test PRIVATE vco_execution)
foreach(case prepare queued during overflow retirement)
  add_test(NAME vita_terminal_${case} COMMAND vita_terminal_integration_test ${case})
endforeach()
add_executable(execution_view_test execution_view_test.c ../src/ui/execution_view.c)
target_include_directories(execution_view_test PRIVATE ../src ../src/cheats)
target_link_libraries(execution_view_test PRIVATE vco_execution)
foreach(case rows pending binding wrapping localization)
  add_test(NAME execution_view_${case} COMMAND execution_view_test ${case})
endforeach()
add_executable(upload_test upload_test.c)
target_include_directories(upload_test PRIVATE ../src)
target_link_libraries(upload_test PRIVATE vco_execution)
foreach(case roundtrip corrupt limits pipeline)
  add_test(NAME upload_${case} COMMAND upload_test ${case})
endforeach()

find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME syscall_abi_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/../scripts/check_syscall_abi.py")
add_test(NAME deployment_transaction COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/deploy_test.py")
add_executable(loader_test loader_test.c ../src/loader.c ../src/diagnostics.c)
target_include_directories(loader_test PRIVATE native_loader ../src)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(loader_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(loader_test PRIVATE -fsanitize=address,undefined)
endif()
add_test(NAME loader_readiness COMMAND loader_test)

add_executable(harness_test harness_test.c ../tools/device-harness/codes.c)
target_link_libraries(harness_test PRIVATE vco_catalog)
add_test(NAME device_harness_vectors COMMAND harness_test)
add_test(NAME device_harness_bounds COMMAND harness_test bounds)
add_test(NAME device_harness_observations COMMAND harness_test observations)

add_executable(kernel_service_test kernel_service_test.c native_service/fakes.c ../src/platform/vita/execution_state.c
  ../src/platform/vita/service.c ../src/platform/vita/close_worker.c ../src/platform/vita/authority.c
  ../src/platform/vita/shell_owner.c ../src/platform/vita/control_rpc.c)
target_include_directories(kernel_service_test PRIVATE native_service ../src)
target_link_libraries(kernel_service_test PRIVATE vco_execution Threads::Threads)
foreach(case authorization buffers pipeline receipts lifecycle owner_departure bind_failures coexistence fault_exit_cleanup independent_unbind
             quarantine retired_cleanup retired_poison support_failure startup_alloc startup_base startup_events startup_worker shell_restart shell_replacement rpc_restart upload_restart permanent_loss concurrent_scopes quarantine_chain rearm native_state upload_instance runtime_restart runtime_restore runtime_same_owner input_disabled trace_transport trace_freeze authority_pause idle_modules idle_module_residue long_foreground long_background long_unknown long_expired long_silent
             owner_exit_cleanup owner_replaced_target owner_pending_retire owner_retire_poison owner_unknown_target owner_restart_retire
             owner_reference_before owner_reference_after
             terminal_clean terminal_pending terminal_replaced terminal_unknown terminal_native_poison
             terminal_free_before terminal_free_after terminal_reference_before terminal_reference_after
             terminal_memory_failure terminal_public_failure terminal_copy_retry terminal_handoff terminal_reentrant
             terminal_probe_before terminal_probe_after terminal_queued_upload terminal_empty scoped_reload scoped_failure
             native_close native_module_close native_close_failure native_module_close_failure
             module_finish_snapshot module_finish_map module_finish_rearm module_finish_identity
             module_finish_racing_identity module_finish_exit module_finish_lost)
  add_test(NAME kernel_service_${case} COMMAND kernel_service_test ${case})
endforeach()
add_executable(execution_service_test execution_service_test.c ../src/platform/execution_service.c)
target_include_directories(execution_service_test PRIVATE native_execution_service ../src)
target_link_libraries(execution_service_test PRIVATE vco_execution vco_catalog)
foreach(case pipeline copies reload lifecycle failures readiness pagination recovery instance trace trace_invalid trace_failure
             trace_writer_busy trace_health trace_health_failure)
  add_test(NAME execution_service_${case} COMMAND execution_service_test ${case})
endforeach()

# Opt-in sampling must remain on the control worker after all service locks
# are released. The observer's deadline must not inherit diagnostics I/O.
add_executable(execution_service_profile_test execution_service_test.c ../src/platform/execution_service.c)
target_compile_definitions(execution_service_profile_test PRIVATE VCO_RESOURCE_PROFILE=1)
target_include_directories(execution_service_profile_test PRIVATE native_execution_service ../src)
target_link_libraries(execution_service_profile_test PRIVATE vco_execution vco_catalog)
add_test(NAME resource_profile_control_integration COMMAND execution_service_profile_test pipeline)

add_executable(resource_profile_test resource_profile_test.c ../src/platform/resource_profile.c)
target_compile_definitions(resource_profile_test PRIVATE VCO_RESOURCE_PROFILE=1)
target_include_directories(resource_profile_test PRIVATE native_execution_service ../src ../src/cheats)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(resource_profile_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(resource_profile_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case snapshot cadence window clock_start clock_reverse clock_error clock_finish faults invalid visibility wait_for_game end_busy_once end_busy_exhausted end_io_error clock_end_busy)
  add_test(NAME resource_profile_${case} COMMAND resource_profile_test ${case})
endforeach()
foreach(position RANGE 1 6)
  foreach(error -1 -2)
    add_test(NAME resource_profile_io_${position}_${error} COMMAND resource_profile_test io ${position} ${error})
  endforeach()
endforeach()
foreach(count 1 7 8 100)
  add_test(NAME resource_profile_startup_busy_${count} COMMAND resource_profile_test startup_busy ${count})
endforeach()
add_test(NAME resource_profile_analysis COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/resource_profile_test.py" "$<TARGET_FILE:resource_profile_test>")
add_test(NAME resource_budget_inputs COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/resource_budget_test.py")
add_test(NAME resource_package_inputs COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/resource_package_test.py")

add_executable(catalog_service_test catalog_service_test.c ../src/platform/catalog_service.c ../src/diagnostics.c)
target_include_directories(catalog_service_test PRIVATE native_execution_service ../src)
target_link_libraries(catalog_service_test PRIVATE vco_catalog)
add_test(NAME catalog_service_lifecycle COMMAND catalog_service_test)
foreach(case registry traditional japanese korean recovery)
  add_test(NAME catalog_language_${case} COMMAND catalog_service_test language_${case})
endforeach()

add_executable(input_bridge_test input_bridge_test.c ../src/input/touch.c)
target_include_directories(input_bridge_test PRIVATE ../src)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(input_bridge_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(input_bridge_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case busy release lease)
  add_test(NAME input_bridge_${case} COMMAND input_bridge_test ${case})
endforeach()

add_test(NAME package_build_provenance COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/build_stamp_test.py")
add_test(NAME release_tag_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/release_test.py")
add_test(NAME ui_resource_contract COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/ui_resource_test.py")
add_test(NAME catalog_encoding_oracle COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/encoding_oracle.py" "$<TARGET_FILE:encoding_test>")

add_executable(metadata_test metadata_test.c)
target_include_directories(metadata_test PRIVATE ../src)
target_link_libraries(metadata_test PRIVATE vco_catalog vco_execution)
foreach(case versions headers compact conflicts bounds catalog)
  add_test(NAME catalog_metadata_${case} COMMAND metadata_test ${case})
endforeach()

add_executable(ui_lifecycle_test ui_lifecycle_test.c)
target_include_directories(ui_lifecycle_test PRIVATE ../src)
target_link_libraries(ui_lifecycle_test PRIVATE vco_execution)
foreach(case identity release)
  add_test(NAME ui_lifecycle_${case} COMMAND ui_lifecycle_test ${case})
endforeach()

foreach(mode user kernel)
  add_executable(diagnostics_${mode}_test diagnostics_test.c native_diagnostics/caller.c ../src/diagnostics.c)
  target_include_directories(diagnostics_${mode}_test PRIVATE native_diagnostics ../src)
  target_link_libraries(diagnostics_${mode}_test PRIVATE Threads::Threads)
  if(mode STREQUAL "kernel")
    target_compile_definitions(diagnostics_${mode}_test PRIVATE __VITA_KERNEL__)
  endif()
  if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(diagnostics_${mode}_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(diagnostics_${mode}_test PRIVATE -fsanitize=address,undefined)
  endif()
  foreach(case format bounds short_writes failures shared close_consumed close_retained close_rotation)
    add_test(NAME diagnostics_${mode}_${case} COMMAND diagnostics_${mode}_test ${case})
  endforeach()
endforeach()

# Startup failure must keep installed hook code and references resident while
# rejecting capture and execution admission. Compile both real native adapters.
add_executable(kernel_startup_test kernel_startup_test.c ../src/kernel.c
  ../src/platform/touch_kernel.c ../src/input/touch.c)
target_include_directories(kernel_startup_test PRIVATE native_kernel native_touch native_pad
  native_service ../src ../src/cheats)
target_compile_definitions(kernel_startup_test PRIVATE __VITA_KERNEL__)
target_compile_options(kernel_startup_test PRIVATE -Wno-pedantic)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(kernel_startup_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(kernel_startup_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case ready optional ctrl0 ctrl1 ctrl2 ctrl3 ctrl4 ctrl5 ctrl6 ctrl7
             touch0 touch1 touch2 touch3 inflight)
  add_test(NAME kernel_startup_${case} COMMAND kernel_startup_test ${case})
endforeach()

add_executable(trace_test trace_test.c)
target_include_directories(trace_test PRIVATE ../src)
target_link_libraries(trace_test PRIVATE vco_execution Threads::Threads)
foreach(case snapshot wrap limits contention format concurrent freeze)
  add_test(NAME trace_${case} COMMAND trace_test ${case})
endforeach()

add_executable(harness_fixture_test harness_fixture_test.c ../tools/device-harness/fixture.c)
add_executable(harness_observations_test harness_observations_test.c ../tools/device-harness/observations.c)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(harness_observations_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(harness_observations_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case bounds io)
  add_test(NAME harness_observations_${case} COMMAND harness_observations_test ${case})
endforeach()
add_test(NAME harness_observations_analysis COMMAND "${Python3_EXECUTABLE}"
  "${CMAKE_CURRENT_SOURCE_DIR}/harness_observations_test.py" "$<TARGET_FILE:harness_observations_test>")
target_link_libraries(harness_fixture_test PRIVATE vco_catalog)
foreach(case lifecycle contract ranges faults cycles codes bounds)
  add_test(NAME harness_fixture_${case} COMMAND harness_fixture_test ${case})
endforeach()

add_executable(harness_observer_test harness_observer_test.c ../tools/device-harness/fixture_observer.c)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(harness_observer_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(harness_observer_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case initial newer reuse latch invalid limits)
  add_test(NAME harness_observer_${case} COMMAND harness_observer_test ${case})
endforeach()
foreach(variant 0 1)
  add_executable(harness_fixture_entry_${variant}_test harness_fixture_entry_test.c
    ../tools/device-harness/fixture_module.c)
  target_include_directories(harness_fixture_entry_${variant}_test PRIVATE native_harness)
  target_compile_definitions(harness_fixture_entry_${variant}_test PRIVATE VCO_FIXTURE_VARIANT=${variant})
  if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(harness_fixture_entry_${variant}_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(harness_fixture_entry_${variant}_test PRIVATE -fsanitize=address,undefined)
  endif()
  add_test(NAME harness_fixture_entry_${variant} COMMAND harness_fixture_entry_${variant}_test)
endforeach()

add_executable(admission_test admission_test.c)
target_include_directories(admission_test PRIVATE ../src)
target_link_libraries(admission_test PRIVATE vco_execution Threads::Threads)
add_test(NAME execution_admission COMMAND admission_test)
set_tests_properties(execution_admission PROPERTIES TIMEOUT 30)

# Compile the real settings I/O service: migration must not overwrite legacy
# settings, and every save goes to prefixed files in the shared directory.
add_executable(settings_storage_test settings_storage_test.c ../src/platform/settings_service.c ../src/config/config.c)
target_include_directories(settings_storage_test PRIVATE native_settings ../src)
if(VCO_SANITIZE AND CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(settings_storage_test PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(settings_storage_test PRIVATE -fsanitize=address,undefined)
endif()
foreach(case migration preferred recovery corrupt denied defaults regional_migration japanese korean)
  add_test(NAME settings_storage_${case} COMMAND settings_storage_test ${case})
endforeach()
