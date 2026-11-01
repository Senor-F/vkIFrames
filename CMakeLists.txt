cmake_minimum_required(VERSION 3.21)
project(vkIFrames VERSION 0.1.0 LANGUAGES CXX)

# One release version for the whole project (library log line, Decky plugin, archives). The commit and its commit time come from git (or from
# -DFRAMEGEN_COMMIT=... -DFRAMEGEN_COMMIT_TIME=<unix seconds> when building from a tarball): no build date or time is embedded, so two builds of the
# same commit give the same library.
set(FRAMEGEN_VERSION "0.1.0-alpha.1" CACHE STRING "Release version shown in the library's log line and used in archive names")
if(NOT FRAMEGEN_COMMIT)
    find_package(Git QUIET)
    set(FRAMEGEN_COMMIT "unknown")
    set(FRAMEGEN_COMMIT_TIME "0")
    if(GIT_FOUND)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} rev-parse --short=10 HEAD OUTPUT_VARIABLE _c OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _r1)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} log -1 --format=%ct OUTPUT_VARIABLE _t OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET RESULT_VARIABLE _r2)
        execute_process(COMMAND ${GIT_EXECUTABLE} -C ${CMAKE_SOURCE_DIR} status --porcelain --untracked-files=no OUTPUT_VARIABLE _d OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(_r1 EQUAL 0 AND _r2 EQUAL 0)
            set(FRAMEGEN_COMMIT "${_c}")
            set(FRAMEGEN_COMMIT_TIME "${_t}")
            if(_d)
                set(FRAMEGEN_COMMIT "${_c}-dirty")
            endif()
        endif()
    endif()
endif()
message(STATUS "vkIFrames ${FRAMEGEN_VERSION}, commit ${FRAMEGEN_COMMIT}, commit time ${FRAMEGEN_COMMIT_TIME}")
add_compile_options(-ffile-prefix-map=${CMAKE_SOURCE_DIR}=. -fdebug-prefix-map=${CMAKE_SOURCE_DIR}=. -ffile-prefix-map=${CMAKE_BINARY_DIR}=.)   # no build folder in the binary

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)       # only the FRAMEGEN_EXPORT symbols are public
set(CMAKE_VISIBILITY_INLINES_HIDDEN ON)

# A layer never links the Vulkan loader, so only the headers are needed (no libvulkan dev package).
find_package(VulkanHeaders CONFIG QUIET)                       # provides Vulkan::Headers when Vulkan-Headers is installed with CMake files
if(NOT TARGET Vulkan::Headers)
    find_path(FRAMEGEN_VULKAN_INCLUDE_DIR vulkan/vk_layer.h HINTS ${Vulkan_INCLUDE_DIR} ENV VULKAN_SDK PATH_SUFFIXES include)
    if(NOT FRAMEGEN_VULKAN_INCLUDE_DIR)
        message(FATAL_ERROR "Vulkan headers not found. Install Vulkan-Headers (recent enough for VK_EXT_present_timing) "
                            "or pass -DFRAMEGEN_VULKAN_INCLUDE_DIR=<dir containing vulkan/vk_layer.h>.")
    endif()
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${FRAMEGEN_VULKAN_INCLUDE_DIR}")
endif()

# ---- shaders: shaders/<name>.comp -> generated/<name>_spv.h (uint32_t k<Camel>Spv[]) ----
# Needs glslangValidator (Ubuntu/Debian: glslang-tools). -DFRAMEGEN_SKIP_SHADERS=ON writes stubs instead (mock tests, no GPU):
# the layer then reports "shader not built" and never makes synthetic frames.
option(FRAMEGEN_SKIP_SHADERS "Do not compile shaders; embed empty stubs" OFF)
set(FRAMEGEN_GENERATED_DIR ${CMAKE_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${FRAMEGEN_GENERATED_DIR})
# blend: Stage 3 placeholder; flow_*: Stage 4 optical flow (luma pyramid, coarse search, per-level refinement, occlusion, debug view)
set(FRAMEGEN_SHADERS blend:kBlendSpv flow_luma:kFlowLumaSpv flow_down:kFlowDownSpv flow_coarse:kFlowCoarseSpv
                     flow_refine:kFlowRefineSpv flow_occlusion:kFlowOcclusionSpv flow_vis:kFlowVisSpv flow_interp:kFlowInterpSpv)
if(NOT FRAMEGEN_SKIP_SHADERS)
    find_program(FRAMEGEN_GLSLANG glslangValidator HINTS ENV VULKAN_SDK PATH_SUFFIXES bin)
    if(NOT FRAMEGEN_GLSLANG)
        message(FATAL_ERROR "glslangValidator not found. Install it (Ubuntu/Debian: sudo apt install -y glslang-tools) "
                            "or configure with -DFRAMEGEN_SKIP_SHADERS=ON to build without synthetic frames.")
    endif()
endif()
set(FRAMEGEN_SHADER_HEADERS "")
foreach(entry ${FRAMEGEN_SHADERS})
    string(REPLACE ":" ";" parts ${entry})
    list(GET parts 0 sname)
    list(GET parts 1 symbol)
    set(hdr ${FRAMEGEN_GENERATED_DIR}/${sname}_spv.h)
    if(FRAMEGEN_SKIP_SHADERS)
        string(REPEAT "0u," 130 FRAMEGEN_STUB_WORDS)      # not valid SPIR-V: mock drivers accept it, a real driver rejects it (synthetic frames then stay off)
        file(WRITE ${hdr} "#pragma once\n#include <cstdint>\nstatic const uint32_t ${symbol}[] = {0x07230203u,${FRAMEGEN_STUB_WORDS}0u};   // stub (FRAMEGEN_SKIP_SHADERS)\n")
    else()
        add_custom_command(
            OUTPUT ${hdr}
            COMMAND ${FRAMEGEN_GLSLANG} -V -S comp --vn ${symbol} -o ${hdr} ${CMAKE_SOURCE_DIR}/shaders/${sname}.comp
            DEPENDS ${CMAKE_SOURCE_DIR}/shaders/${sname}.comp
            COMMENT "Compiling shaders/${sname}.comp"
            VERBATIM)
        list(APPEND FRAMEGEN_SHADER_HEADERS ${hdr})
    endif()
endforeach()
add_custom_target(layer_shaders DEPENDS ${FRAMEGEN_SHADER_HEADERS})

find_package(Threads REQUIRED)
add_library(vkIFrames SHARED
    src/layer_main.cpp
    src/layer_dispatch.cpp
    src/present_timing.cpp
    src/gpu_frames.cpp
    src/gpu_images.cpp
    src/optical_flow.cpp
    src/dump_queue.cpp
    src/dump_pool.cpp
    src/live_config.cpp
    src/stats.cpp
)
target_include_directories(vkIFrames PRIVATE include ${FRAMEGEN_GENERATED_DIR})
target_compile_definitions(vkIFrames PRIVATE FRAMEGEN_VERSION="${FRAMEGEN_VERSION}" FRAMEGEN_COMMIT="${FRAMEGEN_COMMIT}" FRAMEGEN_COMMIT_TIME="${FRAMEGEN_COMMIT_TIME}")
add_dependencies(vkIFrames layer_shaders)
target_link_libraries(vkIFrames PRIVATE Vulkan::Headers Threads::Threads)
target_link_options(vkIFrames PRIVATE -Wl,--no-undefined -Wl,--version-script=${CMAKE_SOURCE_DIR}/scripts/layer.map -Wl,--exclude-libs,ALL)   # only the three entry points are exported
# The C++ runtime is linked statically (release): the layer then needs no libstdc++/libgcc_s from the host or the game's container, and its runtime
# symbols stay hidden. Only glibc is a dependency (floor GLIBC_2.34). Turn it off for sanitizer builds (ASan wants the shared runtime).
option(FRAMEGEN_STATIC_RUNTIME "Link libstdc++ and libgcc statically" ON)
if(FRAMEGEN_STATIC_RUNTIME)
    target_link_options(vkIFrames PRIVATE -static-libstdc++ -static-libgcc)
endif()

# Manifest next to the library. The path is relative to the manifest file.
# (configure_file does not evaluate generator expressions, so use a plain name.)
set(FRAMEGEN_LIBRARY_PATH "./libvkIFrames.so")
configure_file(
    manifest/VkLayer_frame_gen.json.in
    ${CMAKE_BINARY_DIR}/VkLayer_frame_gen.json
    @ONLY
)

# Mock-loader test (no GPU needed): ctest --test-dir build
option(FRAMEGEN_BUILD_TESTS "Build the mock-loader test" ON)
if(FRAMEGEN_BUILD_TESTS)
    enable_testing()
    add_executable(test_chain tests/test_chain.cpp)
    target_link_libraries(test_chain PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_test(NAME mock_loader COMMAND test_chain $<TARGET_FILE:vkIFrames>)
    add_executable(test_timing tests/test_timing.cpp)
    target_link_libraries(test_timing PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_test(NAME mock_timing COMMAND test_timing $<TARGET_FILE:vkIFrames>)
    add_executable(test_hold tests/test_hold.cpp)
    target_link_libraries(test_hold PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_test(NAME mock_hold COMMAND test_hold $<TARGET_FILE:vkIFrames>)
    set_tests_properties(mock_hold PROPERTIES ENVIRONMENT "VKIFRAME_SYNTH=0;VKIFRAME_PRESENTER=0")          # 3b sequencing: held frames, no synthetic ones
    add_executable(test_synth tests/test_synth.cpp)
    target_link_libraries(test_synth PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_test(NAME mock_synth COMMAND test_synth $<TARGET_FILE:vkIFrames>)
    add_executable(test_cmdreuse tests/test_cmdreuse.cpp)
    target_link_libraries(test_cmdreuse PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_test(NAME mock_cmdreuse COMMAND test_cmdreuse $<TARGET_FILE:vkIFrames>)
    add_executable(test_detach tests/test_detach.cpp)
    target_link_libraries(test_detach PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS} Threads::Threads)
    add_test(NAME mock_detach COMMAND test_detach $<TARGET_FILE:vkIFrames>)
    add_executable(test_instance_ext tests/test_instance_ext.cpp)
    target_link_libraries(test_instance_ext PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_test(NAME mock_instance_ext COMMAND test_instance_ext $<TARGET_FILE:vkIFrames>)
    add_executable(test_extrap tests/test_extrap.cpp)
    target_link_libraries(test_extrap PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS})
    add_test(NAME mock_extrap COMMAND test_extrap $<TARGET_FILE:vkIFrames>)
    set_tests_properties(mock_synth PROPERTIES ENVIRONMENT "VKIFRAME_PRESENTER=0")        # 3c-i: synthetic frames emitted from the game thread
    add_executable(test_pacer tests/test_pacer.cpp)
    target_link_libraries(test_pacer PRIVATE Vulkan::Headers ${CMAKE_DL_LIBS} Threads::Threads)
    add_test(NAME mock_pacer COMMAND test_pacer $<TARGET_FILE:vkIFrames>)
    add_executable(test_flow_ref tests/test_flow_ref.cpp)
    target_include_directories(test_flow_ref PRIVATE include)
    add_test(NAME flow_reference COMMAND test_flow_ref)
    add_executable(test_scheduler tests/test_scheduler.cpp)
    target_include_directories(test_scheduler PRIVATE include)
    add_test(NAME scheduler_vs_sim COMMAND test_scheduler ${CMAKE_SOURCE_DIR}/tests/data)
    add_executable(test_live tests/test_live.cpp src/live_config.cpp)
    target_include_directories(test_live PRIVATE include ${FRAMEGEN_GENERATED_DIR})
    target_link_libraries(test_live PRIVATE Vulkan::Headers)
    add_test(NAME live_config COMMAND test_live)
    # defaults: the Decky panel's schema against the layer, the safety defaults, the manifest's opt-in
    set(FRAMEGEN_SCHEMA_TABLE ${FRAMEGEN_GENERATED_DIR}/schema_table.h)
    add_custom_command(OUTPUT ${FRAMEGEN_SCHEMA_TABLE}
        COMMAND ${CMAKE_COMMAND} -DSCHEMA=${CMAKE_SOURCE_DIR}/decky/settings_schema.json -DOUT=${FRAMEGEN_SCHEMA_TABLE} -P ${CMAKE_SOURCE_DIR}/cmake/GenSchemaTable.cmake
        DEPENDS ${CMAKE_SOURCE_DIR}/decky/settings_schema.json ${CMAKE_SOURCE_DIR}/cmake/GenSchemaTable.cmake)
    add_executable(test_defaults tests/test_defaults.cpp src/live_config.cpp ${FRAMEGEN_SCHEMA_TABLE})
    target_include_directories(test_defaults PRIVATE include ${FRAMEGEN_GENERATED_DIR})
    target_link_libraries(test_defaults PRIVATE Vulkan::Headers)
    target_compile_definitions(test_defaults PRIVATE FRAMEGEN_MANIFEST_IN="${CMAKE_SOURCE_DIR}/manifest/VkLayer_frame_gen.json.in")
    add_test(NAME defaults COMMAND test_defaults)
    add_executable(test_logcap tests/test_logcap.cpp src/live_config.cpp)
    target_include_directories(test_logcap PRIVATE include ${FRAMEGEN_GENERATED_DIR})
    target_link_libraries(test_logcap PRIVATE Vulkan::Headers)
    add_test(NAME logcap COMMAND test_logcap)
    add_test(NAME logcap_nofile COMMAND test_logcap nofile)
    add_executable(test_stats tests/test_stats.cpp src/stats.cpp src/live_config.cpp)
    target_include_directories(test_stats PRIVATE include ${FRAMEGEN_GENERATED_DIR})
    target_link_libraries(test_stats PRIVATE Vulkan::Headers)
    add_test(NAME stats_bench COMMAND test_stats)
    add_executable(test_flow_images tests/test_flow_images.cpp src/gpu_images.cpp)
    target_include_directories(test_flow_images PRIVATE include ${FRAMEGEN_GENERATED_DIR})
    target_link_libraries(test_flow_images PRIVATE Vulkan::Headers)
    add_test(NAME flow_images COMMAND test_flow_images)
    add_executable(test_dump tests/test_dump.cpp src/dump_queue.cpp)
    target_include_directories(test_dump PRIVATE include ${FRAMEGEN_GENERATED_DIR})
    target_link_libraries(test_dump PRIVATE Vulkan::Headers)
    add_test(NAME dump_queue COMMAND test_dump)
    add_executable(test_dump_pool tests/test_dump_pool.cpp src/dump_pool.cpp)
    target_include_directories(test_dump_pool PRIVATE include ${FRAMEGEN_GENERATED_DIR})
    target_link_libraries(test_dump_pool PRIVATE Vulkan::Headers)
    add_test(NAME dump_pool COMMAND test_dump_pool)
    if(FRAMEGEN_STATIC_RUNTIME)
        add_test(NAME so_checks COMMAND ${CMAKE_SOURCE_DIR}/scripts/check_so.sh $<TARGET_FILE:vkIFrames>)
        # The host process already has its own C++ runtime (here a library that replaces operator new/delete and counts the calls, linked before libstdc++):
        # the layer's allocations must not reach it.
        add_library(hoststd SHARED tests/hoststd.cpp)
        set_target_properties(hoststd PROPERTIES CXX_VISIBILITY_PRESET default)
        add_executable(test_hostlib tests/test_hostlib.cpp)
        target_link_libraries(test_hostlib PRIVATE hoststd Vulkan::Headers ${CMAKE_DL_LIBS})
        add_test(NAME static_runtime_isolated COMMAND test_hostlib $<TARGET_FILE:vkIFrames>)
    endif()
    # keep the tests out of the user's ~/.config/vkiframes/live.log
    set_tests_properties(live_config mock_loader mock_timing mock_hold mock_synth mock_extrap mock_pacer PROPERTIES ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/xdg")
    set_tests_properties(stats_bench PROPERTIES ENVIRONMENT "XDG_CONFIG_HOME=${CMAKE_BINARY_DIR}/xdg-stats")
endif()

# offline scoring of interpolation methods on captured frames (tools/interp_eval); not part of the layer
add_executable(interp_eval tools/interp_eval/interp_eval.cpp)
target_include_directories(interp_eval PRIVATE ${CMAKE_SOURCE_DIR}/include)
