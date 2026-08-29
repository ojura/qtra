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

# Branch protection, stated in both directions.
#
# Distributions ship GCC configured with -fcf-protection=full already, so a
# build that says nothing about it emits landing pads and carries the IBT
# property note. A preset meant as the control for the CET one has to ask for
# none explicitly, or the two produce the same binary and comparing them proves
# nothing: measured here, both carried 2612 endbr64 instructions and the same
# note until this asked.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
    if(ENABLE_CET)
        add_compile_options(-fcf-protection=full)
        add_link_options(-fcf-protection=full)
    else()
        add_compile_options(-fcf-protection=none)
        add_link_options(-fcf-protection=none)
    endif()
elseif(ENABLE_CET)
    message(FATAL_ERROR "ENABLE_CET currently requires GCC/Clang on x86-64")
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

# Loaded at run time, so its code sits where the loader puts it, which is far
# enough from the executable that a jump naming a thirty-two bit distance cannot
# reach it. That distance is the whole point of the file.
add_library(far_replacement MODULE tests/far_replacement.cpp)
set_target_properties(far_replacement PROPERTIES PREFIX "" OUTPUT_NAME "far_replacement")

add_library(cube_patch_wobble MODULE patches/wobble_patch.cpp)
target_include_directories(cube_patch_wobble PRIVATE include)
set_target_properties(cube_patch_wobble PROPERTIES PREFIX "" OUTPUT_NAME "cube_patch_wobble")

add_library(cube_patch_reverse MODULE patches/reverse_patch.cpp)
target_include_directories(cube_patch_reverse PRIVATE include)
set_target_properties(cube_patch_reverse PROPERTIES PREFIX "" OUTPUT_NAME "cube_patch_reverse")

if(BUILD_HOTPATCH_SELFTEST)
    add_executable(hotpatch_selftest
        tests/hotpatch_selftest.cpp
        tests/prologue_fixtures.cpp
        src/cube_step.cpp
        src/agent/entry_hotpatch.cpp
        src/agent/patch_site.cpp
        src/agent/got_site.cpp
        src/agent/prologue_relocation.cpp
        src/agent/patch_manager.cpp
        src/agent/patch_registry.cpp
        src/agent/write_admission.cpp
        src/agent/quiescence_providers.cpp
        src/agent/stop_the_world.cpp
    )
    target_include_directories(hotpatch_selftest
        PRIVATE include src ${CMAKE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(hotpatch_selftest PRIVATE ${CMAKE_DL_LIBS})
    # Relocating a prologue needs the function's extent, which comes from the
    # dynamic symbol table. Without exported symbols the loader cannot name
    # anything in this binary and every plan refuses for a reason that is about
    # the link and not about the function.
    target_link_options(hotpatch_selftest PRIVATE -Wl,--export-dynamic)
    target_compile_options(hotpatch_selftest PRIVATE
        -Wall -Wextra -Wpedantic
        $<$<CONFIG:Release>:-O3>
    )
    add_executable(thread_accounting_selftest
        tests/thread_accounting_selftest.cpp
        src/agent/stop_the_world.cpp
    )
    target_include_directories(thread_accounting_selftest PRIVATE include src)
    target_link_libraries(thread_accounting_selftest PRIVATE pthread)
    target_compile_options(thread_accounting_selftest PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME thread-accounting COMMAND thread_accounting_selftest)

    # Its own process: it installs a gateway, and gateways are permanent, so it
    # cannot share the one prepared entry with tests that need it untouched.
    add_executable(writable_page_selftest
        tests/writable_page_selftest.cpp
        src/cube_step.cpp
        src/agent/entry_hotpatch.cpp
        src/agent/patch_site.cpp
        src/agent/patch_manager.cpp
        src/agent/prologue_relocation.cpp
        src/agent/patch_registry.cpp
        src/agent/write_admission.cpp
        src/agent/quiescence_providers.cpp
        src/agent/stop_the_world.cpp
    )
    target_include_directories(writable_page_selftest
        PRIVATE include src ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(writable_page_selftest PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(writable_page_selftest PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME writable-page COMMAND writable_page_selftest)

    add_dependencies(hotpatch_selftest far_replacement)
    target_compile_definitions(hotpatch_selftest PRIVATE
        FAR_REPLACEMENT_MODULE="$<TARGET_FILE:far_replacement>")

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
    src/cube_protocol.h
    src/cube_protocol.cpp
    src/cube_step.cpp
    src/agent/errno_text.h
    src/agent/page_span.h
    src/agent/entry_hotpatch.h
    src/agent/entry_hotpatch.cpp
    src/agent/patch_site.h
    src/agent/patch_site.cpp
    src/agent/got_site.h
    src/agent/got_site.cpp
    src/agent/prologue_relocation.h
    src/agent/prologue_relocation.cpp
    src/agent/gateway_record.h
    src/agent/quiescence.h
    src/agent/quiescence_providers.h
    src/agent/quiescence_providers.cpp
    src/agent/patch_manager.h
    src/agent/patch_manager.cpp
    src/agent/patch_registry.h
    src/agent/patch_registry.cpp
    src/agent/write_admission.h
    src/agent/write_admission.cpp
    src/agent/object_registry.h
    src/agent/object_registry.cpp
    src/agent/module_registry.h
    src/agent/module_registry.cpp
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

# Binds through the host ABI and needs nothing beyond it, so the mixed-stack
# behaviour can be tested on a machine without an audio stack.
add_library(agent_snippet_bind_probe MODULE snippets/bind_probe.cpp)
target_include_directories(agent_snippet_bind_probe PRIVATE include src)
target_link_libraries(agent_snippet_bind_probe PRIVATE Qt6::Core)
set_target_properties(agent_snippet_bind_probe PROPERTIES
    PREFIX "" OUTPUT_NAME "agent_snippet_bind_probe")
stamp_with_host_build_id(agent_snippet_bind_probe)

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
    add_library(teardown_module MODULE tests/teardown_module.cpp)
    target_include_directories(teardown_module PRIVATE include)
    set_target_properties(teardown_module PROPERTIES PREFIX "" OUTPUT_NAME "teardown_module")

    # Links the adapter and both registries, so destroying an adapter is a real
    # destruction and not a stand-in for one. The widget is never constructed:
    # nothing these ask about involves it.
    qt_add_executable(module_teardown_selftest
        tests/module_teardown_selftest.cpp
        src/cube_step.cpp
        src/cube_widget.h
        src/cube_widget.cpp
        src/agent/entry_hotpatch.cpp
        src/agent/patch_site.cpp
        src/agent/got_site.cpp
        src/agent/patch_manager.cpp
        src/agent/prologue_relocation.cpp
        src/agent/patch_registry.cpp
        src/agent/write_admission.cpp
        src/agent/quiescence_providers.cpp
        src/agent/stop_the_world.cpp
        src/agent/module_registry.cpp
        src/agent/module_manager.cpp
        src/agent/coverage_manifest.cpp
        src/agent/build_id.cpp
    )
    target_include_directories(module_teardown_selftest
        PRIVATE include src ${CMAKE_BINARY_DIR}/generated)
    target_link_libraries(module_teardown_selftest PRIVATE
        Qt6::Core Qt6::Gui Qt6::Widgets Qt6::OpenGL Qt6::OpenGLWidgets ${CMAKE_DL_LIBS})
    target_compile_definitions(module_teardown_selftest PRIVATE
        DEMO_BUILD_DIR="${CMAKE_BINARY_DIR}" DEMO_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
    # The probe module calls cube_step_builtin, which lives here, so this has to
    # offer its own symbols the way the application does.
    target_link_options(module_teardown_selftest PRIVATE -Wl,--export-dynamic)
    add_test(NAME module-teardown
             COMMAND module_teardown_selftest $<TARGET_FILE:teardown_module>)

    # A caller whose calls go straight through a slot with no stub in between,
    # so the relocations that fill them are in the object's ordinary data
    # relocations and not in the table kept for stubs. Reading only one of those
    # tables makes this object's calls invisible, which is what the test checks.
    add_library(no_plt_caller SHARED tests/no_plt_caller.cpp)
    target_include_directories(no_plt_caller PRIVATE tests)
    target_compile_options(no_plt_caller PRIVATE -fno-plt -Wall -Wextra -Wpedantic)

    # A second one, named so that both end in the same characters. Asking for a
    # caller by suffix then matches two loaded objects, which is the case where
    # taking the first would be a choice nobody made.
    add_library(second_no_plt_caller SHARED tests/no_plt_caller.cpp)
    target_include_directories(second_no_plt_caller PRIVATE tests)
    target_compile_options(second_no_plt_caller PRIVATE -fno-plt -Wall -Wextra -Wpedantic)
    set_target_properties(second_no_plt_caller PROPERTIES
        OUTPUT_NAME "second_no_plt_caller")

    # Links Qt so the redirect can be pointed at Qt's own calls, which is the
    # case the unprepared backends exist for. No display and no window.
    add_executable(got_reach_selftest
        tests/got_reach_selftest.cpp
        src/agent/got_site.cpp
        src/agent/got_registry.cpp
    )
    target_include_directories(got_reach_selftest PRIVATE include src tests)
    target_link_libraries(got_reach_selftest PRIVATE
        Qt6::Core no_plt_caller second_no_plt_caller)
    # Built so its own functions carry the marker an indirect branch may land
    # on, because a slot's destination is always reached by one. The test needs
    # replacements that satisfy the default rule and one that deliberately does
    # not, which is what the nocf_check attribute is for.
    target_compile_options(got_reach_selftest PRIVATE
        -Wall -Wextra -Wpedantic -fcf-protection=branch)
    # And at link time, so the choice survives LTO. With link-time code
    # generation the options given to the link decide what is emitted, and the
    # non-CET presets ask for none there, which would strip the markers this
    # target compiles itself to have and refuse every replacement it offers.
    target_link_options(got_reach_selftest PRIVATE -fcf-protection=branch)
    # One object holding a lazy slot and a bound slot for the same symbol, which
    # is the mix a redirect has to refuse. Lazy binding is what makes the first
    # of those lazy, so this one object opts out of binding at load time.
    add_library(mixed_slot_caller SHARED tests/mixed_slot_caller.cpp)
    target_compile_options(mixed_slot_caller PRIVATE -fplt)
    target_link_options(mixed_slot_caller PRIVATE -Wl,-z,lazy)
    target_link_libraries(got_reach_selftest PRIVATE mixed_slot_caller)
    # Both no-stub objects stay loaded even though the test calls into only one.
    # The linker drops a library nothing references, and the second one exists
    # to be a second match for a suffix, which is a reason the linker cannot
    # see.
    target_link_options(got_reach_selftest PRIVATE -Wl,--no-as-needed)
    add_test(NAME got-reach COMMAND got_reach_selftest)

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
        # The build directory as well as the tools, so the live socket test
        # drives this configuration's own binary. Without it every
        # configuration would run that test against whatever build/release
        # happened to hold.
        set_tests_properties(python-tool-tests PROPERTIES
            ENVIRONMENT "PYTHONPATH=${CMAKE_SOURCE_DIR}/tools;RUNTIME_AGENT_TEST_BUILD_DIR=${CMAKE_BINARY_DIR}")
    endif()
endif()
