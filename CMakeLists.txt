cmake_minimum_required(VERSION 3.16)
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
if(NOT DEFINED ENV{VITASDK})
  message(FATAL_ERROR "Set VITASDK first")
endif()
set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "Vita toolchain")
file(STRINGS src/version.h VCO_VERSION_LINE REGEX "^#define VCO_VERSION ")
string(REGEX MATCH "[0-9]+\\.[0-9]+\\.[0-9]+" VCO_VERSION "${VCO_VERSION_LINE}")
project(VitaCheatOverlay VERSION ${VCO_VERSION} LANGUAGES C CXX)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)
set(CMAKE_C_STANDARD 99)
set(CMAKE_CXX_STANDARD 11)
option(VCO_BUILD_SDK_CHECKS "Build optional SDK link probes and legacy adapters" OFF)
option(VCO_RESOURCE_PROFILE "Bounded developer-only user-service resource capture" OFF)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(VCO_UI_RESOURCE "${CMAKE_CURRENT_BINARY_DIR}/vco-base.rco")
add_custom_command(OUTPUT "${VCO_UI_RESOURCE}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/generate_ui_resources.py"
          --build --output "${VCO_UI_RESOURCE}"
  DEPENDS scripts/generate_ui_resources.py scripts/rco.py scripts/ui_assets.py
          resources/base.rco resources/overlay.json src/ui/resource_ids.h
  VERBATIM)
add_custom_target(vco_ui_resources ALL DEPENDS "${VCO_UI_RESOURCE}")
set(PAF_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.cache/paf-re" CACHE PATH "ScePaf-RE checkout")
if(NOT EXISTS "${PAF_DIR}/include/paf/plugin.h")
  message(FATAL_ERROR "Run scripts/setup.sh to fetch pinned public PAF headers")
endif()
add_compile_options(-Os -Wall -Wextra -ffunction-sections -fdata-sections)
add_link_options(-nostdlib -Wl,--gc-sections -Wl,-e,module_start -Wl,-u,module_stop)

set(TAI_MODULE_STUB "${CMAKE_CURRENT_BINARY_DIR}/tai-stubs/libtaihenModuleUtils_stub.a")
set(TAI_STUB "${CMAKE_CURRENT_BINARY_DIR}/tai-stubs/libtaihenForKernel_stub.a")
set(TAI_USER_STUB "${CMAKE_CURRENT_BINARY_DIR}/tai-stubs/libtaihen_stub.a")
add_custom_command(OUTPUT "${TAI_STUB}" "${TAI_MODULE_STUB}" "${TAI_USER_STUB}"
  COMMAND "$ENV{VITASDK}/bin/vita-libs-gen" "${CMAKE_CURRENT_SOURCE_DIR}/vendor/taihen/imports.yml" "${CMAKE_CURRENT_BINARY_DIR}/tai-stubs"
  COMMAND make -s -C "${CMAKE_CURRENT_BINARY_DIR}/tai-stubs" libtaihenForKernel_stub.a libtaihenModuleUtils_stub.a libtaihen_stub.a
  DEPENDS vendor/taihen/imports.yml VERBATIM)
add_custom_target(vco_tai_stubs DEPENDS "${TAI_STUB}" "${TAI_MODULE_STUB}" "${TAI_USER_STUB}")
# Shared portable parser, compiler and execution/control protocol.
add_subdirectory(src/cheats)
add_subdirectory(src/catalog)
add_subdirectory(src/execution)
add_subdirectory(src/agent)
target_compile_options(vco_game_executor PRIVATE -fshort-wchar)
add_library(vco_game_worker STATIC src/agent/vita_worker.c)
target_compile_options(vco_game_worker PRIVATE -fshort-wchar -Werror)
target_link_libraries(vco_game_worker PUBLIC vco_game_executor SceLibKernel_stub SceKernelThreadMgr_stub gcc)
add_library(vco_game_memory STATIC src/agent/vita_memory.c)
target_include_directories(vco_game_memory PRIVATE src vendor/taihen)
target_compile_options(vco_game_memory PRIVATE -fshort-wchar -Werror -fstack-usage)
target_link_libraries(vco_game_memory PUBLIC vco_game_executor "${TAI_USER_STUB}" SceLibKernel_stub SceKernelThreadMgr_stub gcc)
add_dependencies(vco_game_memory vco_tai_stubs)
add_library(vco_game_allocator STATIC src/agent/vita_allocator.c)
target_include_directories(vco_game_allocator PRIVATE src)
target_compile_options(vco_game_allocator PRIVATE -fshort-wchar -Werror -fstack-usage)
target_link_libraries(vco_game_allocator PUBLIC vco_game_executor SceSysmem_stub gcc)
# Isolated SDK probe uses syscall placeholders; the formal Game target below
# uses the generated resident bridge imports. This probe emits no SELF.
if(VCO_BUILD_SDK_CHECKS)
add_executable(VcoGameMemoryLinkCheck tests/game_memory_linkcheck.c src/platform/clib.c)
target_include_directories(VcoGameMemoryLinkCheck PRIVATE src)
target_link_libraries(VcoGameMemoryLinkCheck PRIVATE vco_game_memory)
target_compile_options(VcoGameMemoryLinkCheck PRIVATE -fshort-wchar)
target_link_options(VcoGameMemoryLinkCheck PRIVATE -Wl,-u,vco_game_native_memory_init
  -Wl,-u,vco_game_native_memory_callbacks -Wl,-u,vco_game_native_memory_prepare -Wl,-u,vco_game_native_memory_finalize)
# Standalone core link check emits no SELF. Game.suprx links the same core with
# real resident imports; device qualification remains separate.
add_executable(VcoGameExecutorLinkCheck tests/vita_backend_linkcheck.c src/platform/clib.c)
target_compile_options(VcoGameExecutorLinkCheck PRIVATE -fshort-wchar)
target_link_libraries(VcoGameExecutorLinkCheck PRIVATE vco_game_worker)
target_link_options(VcoGameExecutorLinkCheck PRIVATE -Wl,-u,vco_game_worker_start -Wl,-u,vco_game_worker_stop
  -Wl,-u,vco_game_executor_init -Wl,-u,vco_game_executor_handle -Wl,-u,vco_broker_init
  -Wl,-u,vco_broker_open -Wl,-u,vco_broker_end -Wl,-u,vco_broker_observe -Wl,-u,vco_broker_attach
  -Wl,-u,vco_broker_load -Wl,-u,vco_broker_clear -Wl,-u,vco_broker_submit -Wl,-u,vco_broker_poll -Wl,-u,vco_broker_publish
  -Wl,-u,vco_broker_receive -Wl,-u,vco_broker_ack -Wl,-u,vco_broker_status)
endif()

# Resident copied broker/current-game calls. The normal kernel and independent
# SDK probe share this implementation; neither owns a game execution thread.
add_library(vco_game_kernel STATIC src/agent/native_service.c src/agent/native_loader.c src/agent/native_access.c src/agent/native_modules.c
  src/platform/vita/shell_owner.c)
target_compile_definitions(vco_game_kernel PRIVATE __VITA_KERNEL__)
target_compile_options(vco_game_kernel PRIVATE -fshort-wchar -Werror -fstack-usage)
target_include_directories(vco_game_kernel PRIVATE src vendor/taihen)
target_link_libraries(vco_game_kernel PUBLIC vco_game_executor "${CMAKE_CURRENT_BINARY_DIR}/tai-stubs/libtaihenForKernel_stub.a"
  "${CMAKE_CURRENT_BINARY_DIR}/tai-stubs/libtaihenModuleUtils_stub.a"
  SceSysmemForDriver_stub SceSysrootForDriver_stub SceThreadmgrForDriver_stub
  SceProcEventForDriver_stub SceModulemgrForDriver_stub SceCtrlForDriver_stub SceSysclibForDriver_stub SceIofilemgrForDriver_stub gcc)
add_dependencies(vco_game_kernel vco_tai_stubs)
if(VCO_BUILD_SDK_CHECKS)
add_executable(VcoGameKernelLinkCheck tests/game_kernel_linkcheck.c)
target_compile_options(VcoGameKernelLinkCheck PRIVATE -fshort-wchar)
target_link_libraries(VcoGameKernelLinkCheck PRIVATE vco_game_kernel)
target_link_options(VcoGameKernelLinkCheck PRIVATE -Wl,-u,vco_agent_loader_start
  -Wl,-u,vcoAgentAttach -Wl,-u,vcoAgentPoll -Wl,-u,vcoAgentPublish -Wl,-u,vcoAgentPermission
  -Wl,-u,vcoAgentRead -Wl,-u,vcoAgentWrite -Wl,-u,vcoAgentModules -Wl,-u,vcoAgentModuleInfo -Wl,-u,vcoAgentPad
  -Wl,-u,vcoServiceObserve -Wl,-u,vcoServiceLoad -Wl,-u,vcoServiceStatus
  -Wl,-u,vcoServiceSubmit -Wl,-u,vcoServiceReceive -Wl,-u,vcoServiceAck -Wl,-u,vcoServiceResume
  -Wl,-u,vcoServiceTrace -Wl,-u,vco_service_authorize_caller)
endif()

target_compile_options(vco_execution PRIVATE -fshort-wchar)
target_compile_options(vco_cheats PRIVATE -fshort-wchar)
target_compile_options(vco_catalog PRIVATE -fshort-wchar)

if(VCO_BUILD_SDK_CHECKS)
# Historical kernel execution adapters remain for regressions and the legacy
# link check. The normal kernel uses vco_game_kernel instead.
add_library(vco_vita_memory STATIC src/platform/vita/memory.c src/platform/vita/patches.c
  src/platform/vita/execution_state.c
  src/platform/vita/control_worker.c src/platform/vita/close_worker.c src/platform/vita/control_rpc.c src/platform/vita/shell_owner.c
  src/platform/vita/modules.c src/platform/vita/pad.c src/platform/vita/authority.c
  src/platform/vita/process_events.c src/platform/vita/process_watch.c)
target_include_directories(vco_vita_memory PRIVATE src vendor/taihen)
target_compile_definitions(vco_vita_memory PRIVATE __VITA_KERNEL__)
target_compile_options(vco_vita_memory PRIVATE -fshort-wchar -Werror)
target_link_libraries(vco_vita_memory PUBLIC vco_execution "${TAI_STUB}" "${TAI_MODULE_STUB}"
  SceSysmemForDriver_stub SceSysrootForDriver_stub SceSysclibForDriver_stub SceThreadmgrForDriver_stub
  SceCtrlForDriver_stub SceProcEventForDriver_stub gcc)
add_dependencies(vco_vita_memory vco_tai_stubs)

# This ELF is a link check only, not a loadable/deployable plugin.
add_executable(VcoNativeLinkCheck tests/vita_backend_linkcheck.c)
target_link_libraries(VcoNativeLinkCheck PRIVATE vco_vita_memory)
target_compile_options(VcoNativeLinkCheck PRIVATE -fshort-wchar)
target_link_options(VcoNativeLinkCheck PRIVATE
  -Wl,-u,vco_vita_memory_init -Wl,-u,vco_vita_memory_bind -Wl,-u,vco_vita_memory_unbind
  -Wl,-u,vco_vita_execution_init -Wl,-u,vco_vita_execution_callbacks
  -Wl,-u,vco_vita_execution_set_features
  -Wl,-u,vco_vita_control_worker_close_submit -Wl,-u,vco_vita_control_worker_close_receive
  -Wl,-u,vco_vita_control_worker_close_release
  -Wl,-u,vco_vita_patch_support_set_trace
  -Wl,-u,vco_vita_execution_rearm
  -Wl,-u,vco_vita_execution_error
  -Wl,-u,vco_vita_patch_support_init -Wl,-u,vco_vita_patch_support_probe
  -Wl,-u,vco_vita_patch_support_dispose -Wl,-u,vco_vita_execution_reap_retired
  -Wl,-u,vco_vita_execution_dispose -Wl,-u,vco_session_observe
  -Wl,-u,vco_control_client_init -Wl,-u,vco_control_client_bind
  -Wl,-u,vco_control_client_query -Wl,-u,vco_control_client_set
  -Wl,-u,vco_control_client_retry -Wl,-u,vco_control_client_accept -Wl,-u,vco_control_dispatch
  -Wl,-u,vco_mailbox_init -Wl,-u,vco_mailbox_submit -Wl,-u,vco_mailbox_resume
  -Wl,-u,vco_mailbox_receive -Wl,-u,vco_mailbox_ack -Wl,-u,vco_mailbox_pump
  -Wl,-u,vco_vita_rpc_submit -Wl,-u,vco_vita_rpc_receive
  -Wl,-u,vco_vita_rpc_ack -Wl,-u,vco_vita_rpc_resume
  -Wl,-u,vco_transport_init -Wl,-u,vco_transport_bind -Wl,-u,vco_transport_reconnect
  -Wl,-u,vco_vita_shell_owner_init -Wl,-u,vco_vita_shell_owner_bind
  -Wl,-u,vco_vita_shell_owner_authorize -Wl,-u,vco_vita_shell_owner_unbind
  -Wl,-u,vco_vita_modules_init -Wl,-u,vco_vita_modules_snapshot
  -Wl,-u,vco_vita_modules_confirm_mapping -Wl,-u,vco_vita_modules_resolve
  -Wl,-u,vco_vita_modules_guard
  -Wl,-u,vco_vita_modules_invalidate -Wl,-u,vco_vita_pad_init -Wl,-u,vco_vita_pad_read
  -Wl,-u,vco_process_events_start -Wl,-u,vco_process_events_stop
  -Wl,-u,vco_process_events_poll -Wl,-u,vco_process_events_state -Wl,-u,vco_process_events_invalidation
  -Wl,-u,vco_vita_process_watch_init -Wl,-u,vco_vita_process_watch_update
  -Wl,-u,vco_vita_process_watch_unchanged
  -Wl,-u,vco_transport_query -Wl,-u,vco_transport_set -Wl,-u,vco_transport_tick
  -Wl,-u,vco_transport_idle -Wl,-u,vco_mailbox_open
  -Wl,-u,vco_mailbox_stop -Wl,-u,vco_mailbox_stop_result
  -Wl,-u,vco_vita_control_worker_init -Wl,-u,vco_vita_control_worker_start
  -Wl,-u,vco_vita_control_worker_stop -Wl,-u,vco_vita_control_worker_submit
  -Wl,-u,vco_vita_control_worker_reap_stopped
  -Wl,-u,vco_vita_control_worker_resume -Wl,-u,vco_vita_control_worker_receive
  -Wl,-u,vco_vita_control_worker_ack -Wl,-u,vco_engine_bind -Wl,-u,vco_engine_submit
  -Wl,-u,vco_engine_step -Wl,-u,vco_engine_unbind)
endif()

# AppMgr candidates are sampled in the Shell; kernel authority independently
# verifies them. AppMgr user imports never enter the kernel plugin.
add_library(vco_foreground_probe STATIC src/platform/foreground_probe.c)
target_include_directories(vco_foreground_probe PRIVATE src)
target_compile_options(vco_foreground_probe PRIVATE -fshort-wchar -Werror)
target_link_libraries(vco_foreground_probe PUBLIC SceAppMgr_stub)
if(VCO_BUILD_SDK_CHECKS)
add_executable(VcoForegroundLinkCheck tests/vita_backend_linkcheck.c src/platform/clib.c)
target_link_libraries(VcoForegroundLinkCheck PRIVATE vco_foreground_probe SceLibKernel_stub gcc)
target_compile_options(VcoForegroundLinkCheck PRIVATE -fshort-wchar)
target_link_options(VcoForegroundLinkCheck PRIVATE -Wl,-u,vco_foreground_sample)
endif()

add_executable(VitaCheatOverlayKernel src/kernel.c src/diagnostics.c src/platform/game_probe_kernel.c src/platform/touch_kernel.c src/input/touch.c)
target_link_options(VitaCheatOverlayKernel PRIVATE -Wl,-u,vcoPoll -Wl,-u,vcoCapture -Wl,-u,vcoProbeGame -Wl,-u,vcoPollTouch
  -Wl,-u,vcoServiceObserve -Wl,-u,vcoServiceLoad -Wl,-u,vcoServiceStatus
  -Wl,-u,vcoServiceSubmit -Wl,-u,vcoServiceReceive -Wl,-u,vcoServiceAck -Wl,-u,vcoServiceResume
  -Wl,-u,vcoServiceTrace
  -Wl,-u,vcoAgentAttach -Wl,-u,vcoAgentPoll -Wl,-u,vcoAgentPublish -Wl,-u,vcoAgentPermission
  -Wl,-u,vcoAgentRead -Wl,-u,vcoAgentWrite -Wl,-u,vcoAgentModules -Wl,-u,vcoAgentModuleInfo
  -Wl,-u,vcoAgentPad -Wl,-u,vcoAgentFault -Wl,-u,vcoAgentTrace)
target_compile_definitions(VitaCheatOverlayKernel PRIVATE __VITA_KERNEL__)
target_compile_options(VitaCheatOverlayKernel PRIVATE -fshort-wchar -Werror)
target_include_directories(VitaCheatOverlayKernel PRIVATE src vendor/taihen)
target_link_libraries(VitaCheatOverlayKernel PRIVATE vco_game_kernel "${TAI_STUB}" "${TAI_MODULE_STUB}"
  SceProcessmgrForDriver_stub
  SceCtrlForDriver_stub SceThreadmgrForDriver_stub SceSysmemForDriver_stub SceIofilemgrForDriver_stub SceTouchForDriver_stub gcc)
vita_create_self(VitaCheatOverlayKernel.skprx VitaCheatOverlayKernel
  CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/kernel-exports.yml" UNSAFE)
vita_create_stubs(bridge-stubs VitaCheatOverlayKernel "${CMAKE_CURRENT_SOURCE_DIR}/kernel-exports.yml" KERNEL)

add_executable(VitaCheatOverlayGame src/agent/game.c src/platform/clib.c)
add_dependencies(VitaCheatOverlayGame bridge-stubs)
target_include_directories(VitaCheatOverlayGame PRIVATE src)
target_compile_options(VitaCheatOverlayGame PRIVATE -fshort-wchar -Werror -fstack-usage)
target_link_libraries(VitaCheatOverlayGame PRIVATE vco_game_worker vco_game_memory vco_game_allocator
  "${CMAKE_CURRENT_BINARY_DIR}/libVitaCheatOverlayKernel_stub.a" SceKernelModulemgr_stub gcc)
vita_create_self(VitaCheatOverlayGame.suprx VitaCheatOverlayGame
  CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/game-exports.yml" UNSAFE)

add_library(shell_objects OBJECT src/shell.cpp src/ui/overlay_view.cpp src/ui/execution_view.c
  src/diagnostics.c
  src/platform/execution_service.c
  src/platform/catalog_service.c src/platform/clib.c
  src/config/config.c src/platform/settings_service.c)
add_dependencies(shell_objects vco_ui_resources)
if(VCO_RESOURCE_PROFILE)
  target_sources(shell_objects PRIVATE src/platform/resource_profile.c)
  target_compile_definitions(shell_objects PRIVATE VCO_RESOURCE_PROFILE=1)
endif()
target_include_directories(shell_objects PRIVATE src src/catalog src/cheats src/ui)
target_include_directories(shell_objects SYSTEM PRIVATE "${PAF_DIR}/include")
target_compile_options(shell_objects PRIVATE -fshort-wchar "$<$<COMPILE_LANGUAGE:CXX>:-fno-rtti;-fno-exceptions>")
set(PAF_STUB "${CMAKE_CURRENT_BINARY_DIR}/paf-stubs/libVcoPaf_stub.a")
add_custom_command(OUTPUT "${PAF_STUB}"
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/scripts/paf_stubs.py"
    "${PAF_DIR}/lib" "${CMAKE_CURRENT_BINARY_DIR}/paf-stubs" $<TARGET_OBJECTS:shell_objects>
  DEPENDS shell_objects $<TARGET_OBJECTS:shell_objects> scripts/paf_stubs.py
  COMMAND_EXPAND_LISTS VERBATIM)
add_executable(VitaCheatOverlayUI $<TARGET_OBJECTS:shell_objects> "${PAF_STUB}")
set_target_properties(VitaCheatOverlayUI PROPERTIES LINKER_LANGUAGE CXX)
add_dependencies(VitaCheatOverlayUI bridge-stubs)
target_link_libraries(VitaCheatOverlayUI PRIVATE "${PAF_STUB}" vco_catalog vco_cheats vco_execution vco_foreground_probe
  "${CMAKE_CURRENT_BINARY_DIR}/libVitaCheatOverlayKernel_stub.a"
  SceLibKernel_stub SceIofilemgr_stub SceKernelThreadMgr_stub SceSysmem_stub SceAppMgr_stub SceRegistryMgr_stub gcc)
vita_create_self(VitaCheatOverlayUI.suprx VitaCheatOverlayUI
  CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/ui-exports.yml" UNSAFE)

add_executable(VitaCheatOverlayShell src/loader.c src/diagnostics.c)
add_dependencies(VitaCheatOverlayShell VitaCheatOverlayUI vco_tai_stubs)
target_include_directories(VitaCheatOverlayShell PRIVATE src vendor/taihen "${CMAKE_CURRENT_BINARY_DIR}/paf-stubs")
target_link_libraries(VitaCheatOverlayShell PRIVATE "${TAI_USER_STUB}"
  SceLibKernel_stub SceIofilemgr_stub SceKernelThreadMgr_stub)
vita_create_self(VitaCheatOverlayShell.suprx VitaCheatOverlayShell
  CONFIG "${CMAKE_CURRENT_SOURCE_DIR}/shell-exports.yml" UNSAFE)
