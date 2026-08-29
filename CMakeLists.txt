cmake_minimum_required(VERSION 3.21)

project(QtRuntimeAgentDemo VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_HOTPATCH_SELFTEST "Build the standalone raw hotpatch self-test" ON)
option(ENABLE_CET "Build x86-64 code with Intel CET/IBT landing pads" OFF)

# Keeping the evidence that says whether replacing a function is safe.
#
# Deciding that needs to know whether callers could see the body, whether the
# entry has one name, and whether the compiler made copies. All three are known
# at build time and unrecoverable from an optimized binary afterwards, so they
# are kept beside the artifacts and the compile database, where nothing has to
# rediscover them from memory later.
#
# Split DWARF keeps the debug information out of the running binary, which stays
# optimized. The clone dumps record inlining and specialization directly, in a
# form that needs no DWARF reader.
option(PATCH_READY "Keep the build evidence a coverage decision needs" OFF)
if(PATCH_READY)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU")
        add_compile_options(-g -gsplit-dwarf -fdump-ipa-clones)
    else()
        message(FATAL_ERROR "PATCH_READY currently requires GCC")
    endif()
endif()

# The manifest a coverage decision is read from, written after the link because
# it names the build id of the binary it describes.
if(PATCH_READY)
    set(RUNTIME_AGENT_COVERAGE_MANIFEST "${CMAKE_BINARY_DIR}/coverage-manifest.json")
endif()

if(ENABLE_CET)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_compile_options(-fcf-protection=full)
        add_link_options(-fcf-protection=full)
    else()
        message(FATAL_ERROR "ENABLE_CET currently requires GCC/Clang on x86-64")
    endif()
endif()

# Preparing a function for entry replacement.
#
# Three separate properties are needed and none of them belongs in the target's
# source, because the tooling has to work on applications that were not written
# for it.
#
#   -fpatchable-function-entry  reserves the NOP area the redirect is written
#                               into, without which the patcher has nowhere to
#                               go that does not require relocating real
#                               instructions.
#   -fno-lto                    keeps this an ordinary object, so callers in
#                               other translation units see only the
#                               declaration. That opacity is what stops a
#                               caller assuming this body leaves a register
#                               untouched, which a replacement is free to
#                               clobber. The LTO presets would otherwise
#                               dissolve the boundary this rests on.
#   -fno-ipa-icf                stops the body being folded together with an
#                               identical one, which would leave two names for
#                               one entry and make "what did I patch" ambiguous.
#
# This is weaker than GCC's noipa attribute, which is per function and covers
# IPA that does not exist yet. It holds here because cube_step.cpp has no
# callers of its own, so the object boundary is the whole story. A target whose
# callers share a translation unit needs a compiler plugin or a much broader
# build profile.
# Stated once. The flag reserves the area and the generated header tells the
# code that resolves the site how much to expect, so the two cannot drift.
# Not a cache entry. A cached value survives a change here, so an existing build
# directory would keep reserving the old amount while the code that resolves the
# site expects the new one, which is the drift stating it once was meant to
# prevent. It is a property of the encoding, not a knob: a gateway is 13 bytes
# and its continuation is 4.
set(RUNTIME_AGENT_PATCH_AREA_BYTES 20)
if(RUNTIME_AGENT_PATCH_AREA_BYTES LESS 17)
    message(FATAL_ERROR
        "a gateway needs 13 bytes and its ENDBR64 continuation needs 4, so the prepared "
        "area cannot be smaller than 17")
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    set_source_files_properties(src/cube_step.cpp PROPERTIES COMPILE_OPTIONS
        "-fpatchable-function-entry=${RUNTIME_AGENT_PATCH_AREA_BYTES},0;-fno-lto;-fno-ipa-icf")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/src/agent/patch_area.h.in"
    "${CMAKE_BINARY_DIR}/generated/agent/patch_area.h"
    @ONLY)

add_library(cube_patch_wobble MODULE patches/wobble_patch.cpp)
target_include_directories(cube_patch_wobble PRIVATE include)
set_target_properties(cube_patch_wobble PROPERTIES PREFIX "" OUTPUT_NAME "cube_patch_wobble")

add_library(cube_patch_reverse MODULE patches/reverse_patch.cpp)
target_include_directories(cube_patch_reverse PRIVATE include)
set_target_properties(cube_patch_reverse PROPERTIES PREFIX "" OUTPUT_NAME "cube_patch_reverse")

if(BUILD_HOTPATCH_SELFTEST)
    add_executable(hotpatch_selftest
        tests/hotpatch_selftest.cpp
        src/cube_step.cpp
        src/agent/entry_hotpatch.cpp
        src/agent/patch_site.cpp
        src/agent/patch_manager.cpp
        src/agent/patch_registry.cpp
    src/agent/write_admission.cpp
        src/agent/write_admission.cpp
        src/agent/quiescence_providers.cpp
    )
    target_include_directories(hotpatch_selftest PRIVATE include src ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(hotpatch_selftest PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(hotpatch_selftest PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O3>
    )
    add_test(NAME raw-entry-hotpatch-wobble
             COMMAND hotpatch_selftest $<TARGET_FILE:cube_patch_wobble>)
    add_test(NAME raw-entry-hotpatch-reverse
             COMMAND hotpatch_selftest $<TARGET_FILE:cube_patch_reverse>)
endif()

find_package(Qt6 6.4 REQUIRED COMPONENTS
    Core Gui Widgets Network Concurrent OpenGL OpenGLWidgets)

qt_standard_project_setup()

qt_add_executable(qt_runtime_cube
    src/main.cpp
    src/main_window.h
    src/main_window.cpp
    src/cube_widget.h
    src/cube_widget.cpp
    src/cube_step.cpp
    src/agent/entry_hotpatch.h
    src/agent/entry_hotpatch.cpp
    src/agent/patch_site.h
    src/agent/patch_site.cpp
    src/agent/gateway_record.h
    src/agent/quiescence.h
    src/agent/quiescence_providers.h
    src/agent/quiescence_providers.cpp
    src/agent/patch_manager.h
    src/agent/patch_manager.cpp
    src/agent/patch_registry.cpp
    src/agent/object_registry.h
    src/agent/object_registry.cpp
    src/agent/module_manager.h
    src/agent/module_manager.cpp
    src/agent/coverage_manifest.h
    src/agent/coverage_manifest.cpp
    src/agent/build_id.h
    src/agent/build_id.cpp
    src/agent/runtime_agent.h
    src/agent/runtime_agent.cpp
)

target_include_directories(qt_runtime_cube PRIVATE include src ${CMAKE_BINARY_DIR}/generated)
target_link_libraries(qt_runtime_cube PRIVATE
    Qt6::Core
    Qt6::Gui
    Qt6::Widgets
    Qt6::Network
    Qt6::Concurrent
    Qt6::OpenGL
    Qt6::OpenGLWidgets
    ${CMAKE_DL_LIBS}
)
target_compile_options(qt_runtime_cube PRIVATE
    -Wall -Wextra -Wpedantic
    $<$<CONFIG:Release>:-O3>
)
# Make application-owned C++ symbols available to project-native snippets.
# The stable Host API remains the preferred route; this is the sharp-edged one.
if(UNIX AND NOT APPLE)
    target_link_options(qt_runtime_cube PRIVATE -Wl,--export-dynamic)
endif()

target_compile_definitions(qt_runtime_cube PRIVATE
    DEMO_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    DEMO_BUILD_DIR="${CMAKE_BINARY_DIR}"
    DEMO_BUILD_TYPE="$<CONFIG>"
)

# Stamping modules with the build they were compiled against.
#
# The build id is a property of the linked executable, so the header that
# carries it is written after the link and the modules are compiled with it
# force-included, never named in a source, so a module is stamped whatever order
# its own includes are in and without any snippet source mentioning it.
#
# The dependency runs one way. Modules depend on the executable, and the
# executable depends on no module.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(RUNTIME_AGENT_BUILD_ID_HEADER "${CMAKE_BINARY_DIR}/generated/runtime_agent_build_id.h")
if(Python3_Interpreter_FOUND)
    add_custom_command(
        OUTPUT ${RUNTIME_AGENT_BUILD_ID_HEADER}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/read_build_id.py
                $<TARGET_FILE:qt_runtime_cube>
                --header ${RUNTIME_AGENT_BUILD_ID_HEADER}
        DEPENDS qt_runtime_cube ${CMAKE_SOURCE_DIR}/tools/read_build_id.py
        COMMENT "Reading the host build id that modules are stamped with"
        VERBATIM
    )
    add_custom_target(runtime_agent_build_id DEPENDS ${RUNTIME_AGENT_BUILD_ID_HEADER})

    if(PATCH_READY)
        add_custom_command(
            OUTPUT ${RUNTIME_AGENT_COVERAGE_MANIFEST}
            COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tools/analyze_coverage.py
                    --binary $<TARGET_FILE:qt_runtime_cube>
                    --build-dir ${CMAKE_BINARY_DIR}
                    --caller-domain "the GUI thread only: the sole caller is\
 CubeWidget::advanceAnimation, connected to a QTimer owned by the widget, and the\
 agent's own request handler runs on that same thread"
                    --caller-domain-strength declared
                    --caller-domain-caveat "reentrancy that leaves this target on\
 the stack. A nested event loop only matters if a patch request can be admitted while a\
 frame of the target, or of something holding the patch region, is still active. This\
 target is arithmetic that returns before any modal path runs, so a dialog does not\
 reach it; a target that pumps events would need this restated"
                    --output ${RUNTIME_AGENT_COVERAGE_MANIFEST}
            DEPENDS qt_runtime_cube ${CMAKE_SOURCE_DIR}/tools/analyze_coverage.py
            COMMENT "Deciding whether replacing the target can be complete"
            VERBATIM)
        add_custom_target(runtime_agent_coverage ALL
            DEPENDS ${RUNTIME_AGENT_COVERAGE_MANIFEST})
    endif()
    set(RUNTIME_AGENT_CAN_STAMP TRUE)
else()
    message(STATUS
        "Python3 interpreter not found; modules will be built without a host build id "
        "and the agent will report them as unstamped")
    set(RUNTIME_AGENT_CAN_STAMP FALSE)
endif()

# Modules that reach into application types need this. A module built
# without it loads and is reported as unstamped.
function(stamp_with_host_build_id target)
    if(NOT RUNTIME_AGENT_CAN_STAMP)
        return()
    endif()
    add_dependencies(${target} runtime_agent_build_id)
    target_include_directories(${target} PRIVATE ${CMAKE_BINARY_DIR}/generated)
    target_compile_options(${target} PRIVATE -include runtime_agent_build_id.h)
endfunction()

add_library(agent_snippet_inspect MODULE snippets/inspect_cube.cpp)
target_include_directories(agent_snippet_inspect PRIVATE include src)
target_link_libraries(agent_snippet_inspect PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_inspect PRIVATE -fno-access-control)
set_target_properties(agent_snippet_inspect PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_inspect")
stamp_with_host_build_id(agent_snippet_inspect)

add_library(agent_snippet_observer MODULE snippets/install_observer.cpp)
target_include_directories(agent_snippet_observer PRIVATE include src)
target_link_libraries(agent_snippet_observer PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_observer PRIVATE -fno-access-control)
set_target_properties(agent_snippet_observer PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_observer")
stamp_with_host_build_id(agent_snippet_observer)

add_library(agent_snippet_render_probe MODULE snippets/render_probe.cpp)
target_include_directories(agent_snippet_render_probe PRIVATE include src)
target_link_libraries(agent_snippet_render_probe PRIVATE
    Qt6::Core Qt6::Gui Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_render_probe PRIVATE -fno-access-control)
set_target_properties(agent_snippet_render_probe PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_render_probe")
stamp_with_host_build_id(agent_snippet_render_probe)

# Scene modifications. Each one installs a persistent draw hook on
# CubeWidget::frameRendered and accepts {"restore": true} to take itself
# back off. They read private CubeWidget state for the frame's angle,
# scale, tint, elapsed time and projection, hence -fno-access-control.
add_library(agent_snippet_orbit_sphere MODULE snippets/orbit_sphere.cpp)
target_include_directories(agent_snippet_orbit_sphere PRIVATE include src)
target_link_libraries(agent_snippet_orbit_sphere PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_orbit_sphere PRIVATE -fno-access-control)
set_target_properties(agent_snippet_orbit_sphere PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_orbit_sphere")
stamp_with_host_build_id(agent_snippet_orbit_sphere)

add_library(agent_snippet_catmull_clark MODULE snippets/catmull_clark_cube.cpp)
target_include_directories(agent_snippet_catmull_clark PRIVATE include src)
target_link_libraries(agent_snippet_catmull_clark PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_catmull_clark PRIVATE -fno-access-control)
set_target_properties(agent_snippet_catmull_clark PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_catmull_clark")
stamp_with_host_build_id(agent_snippet_catmull_clark)

add_library(agent_snippet_mobius_ring MODULE snippets/mobius_ring.cpp)
target_include_directories(agent_snippet_mobius_ring PRIVATE include src)
target_link_libraries(agent_snippet_mobius_ring PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_mobius_ring PRIVATE -fno-access-control)
set_target_properties(agent_snippet_mobius_ring PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_mobius_ring")
stamp_with_host_build_id(agent_snippet_mobius_ring)

add_library(agent_snippet_pillow_cube MODULE snippets/pillow_cube.cpp)
target_include_directories(agent_snippet_pillow_cube PRIVATE include src)
target_link_libraries(agent_snippet_pillow_cube PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_pillow_cube PRIVATE -fno-access-control)
set_target_properties(agent_snippet_pillow_cube PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_pillow_cube")
stamp_with_host_build_id(agent_snippet_pillow_cube)

# Reads the system output mix through PulseAudio, so it is built only where
# that library is present. Everything else here needs no audio stack, and a
# missing one should cost this one target and not the configure step.
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(PULSE_SIMPLE QUIET libpulse-simple)
endif()
if(PULSE_SIMPLE_FOUND)
    add_library(agent_snippet_audio_pulse MODULE snippets/audio_pulse.cpp)
    target_include_directories(agent_snippet_audio_pulse PRIVATE
        include src ${PULSE_SIMPLE_INCLUDE_DIRS})
    target_link_libraries(agent_snippet_audio_pulse PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets
        ${PULSE_SIMPLE_LIBRARIES})
    target_compile_options(agent_snippet_audio_pulse PRIVATE -fno-access-control)
    set_target_properties(agent_snippet_audio_pulse PROPERTIES
        PREFIX "" OUTPUT_NAME "agent_snippet_audio_pulse")
    stamp_with_host_build_id(agent_snippet_audio_pulse)
else()
    message(STATUS "libpulse-simple not found; skipping agent_snippet_audio_pulse")
endif()

add_library(agent_snippet_about_signature MODULE snippets/about_signature.cpp)
target_include_directories(agent_snippet_about_signature PRIVATE include src)
target_link_libraries(agent_snippet_about_signature PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_about_signature PRIVATE -fno-access-control)
set_target_properties(agent_snippet_about_signature PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_about_signature")
stamp_with_host_build_id(agent_snippet_about_signature)

add_library(agent_snippet_jack_in_the_box MODULE snippets/jack_in_the_box.cpp)
target_include_directories(agent_snippet_jack_in_the_box PRIVATE include src)
target_link_libraries(agent_snippet_jack_in_the_box PRIVATE
    Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets)
target_compile_options(agent_snippet_jack_in_the_box PRIVATE -fno-access-control)
set_target_properties(agent_snippet_jack_in_the_box PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_jack_in_the_box")
stamp_with_host_build_id(agent_snippet_jack_in_the_box)

if(BUILD_TESTING)
    # The admission rule reads a manifest and answers a question about it, so it
    # needs Qt's JSON and nothing else: no window, no display, no build artifact.
    add_executable(coverage_admission_selftest
        tests/coverage_admission_selftest.cpp
        src/agent/coverage_manifest.cpp
    )
    target_include_directories(coverage_admission_selftest PRIVATE include src)
    target_link_libraries(coverage_admission_selftest PRIVATE Qt6::Core)
    target_compile_options(coverage_admission_selftest PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME coverage-admission-order COMMAND coverage_admission_selftest)
endif()

if(BUILD_TESTING)
    find_package(Python3 COMPONENTS Interpreter QUIET)
    if(Python3_Interpreter_FOUND)
        add_test(NAME python-tool-tests
                 COMMAND ${Python3_EXECUTABLE} -m unittest discover
                         -s ${CMAKE_SOURCE_DIR}/tests -p "test_*.py" -v)
        set_tests_properties(python-tool-tests PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/tools")
    endif()
endif()
