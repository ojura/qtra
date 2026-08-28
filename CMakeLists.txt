cmake_minimum_required(VERSION 3.21)

project(QtRuntimeAgentDemo VERSION 0.1.0 LANGUAGES CXX)

include(CTest)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_QT_APP "Build the Qt 6/OpenGL application and Qt-native snippets" ON)
option(BUILD_HOTPATCH_SELFTEST "Build the standalone raw hotpatch self-test" ON)
option(ENABLE_CET "Build x86-64 code with Intel CET/IBT landing pads" OFF)

if(ENABLE_CET)
    if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang"
       AND CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64")
        add_compile_options(-fcf-protection=full)
        add_link_options(-fcf-protection=full)
    else()
        message(FATAL_ERROR "ENABLE_CET currently requires GCC/Clang on x86-64")
    endif()
endif()

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
    )
    target_include_directories(hotpatch_selftest PRIVATE include src)
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

if(BUILD_QT_APP)
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
        src/agent/object_registry.h
        src/agent/object_registry.cpp
        src/agent/module_manager.h
        src/agent/module_manager.cpp
        src/agent/build_id.h
        src/agent/build_id.cpp
        src/agent/runtime_agent.h
        src/agent/runtime_agent.cpp
    )

    target_include_directories(qt_runtime_cube PRIVATE include src)
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
    # force-included. Force-included rather than included by name, so a module
    # is stamped whatever order its own includes are in and without any snippet
    # source mentioning it.
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
    # missing one should cost this one target rather than the configure step.
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
