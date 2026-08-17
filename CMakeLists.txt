cmake_minimum_required(VERSION 3.21)

project(QtRuntimeAgentDemo VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_QT_APP "Build the Qt 6/OpenGL application and Qt-native snippets" ON)
option(BUILD_HOTPATCH_SELFTEST "Build the standalone raw hotpatch self-test" ON)

add_library(cube_patch_wobble MODULE patches/wobble_patch.cpp)
target_include_directories(cube_patch_wobble PRIVATE include)
set_target_properties(cube_patch_wobble PROPERTIES PREFIX "" OUTPUT_NAME "cube_patch_wobble")

add_library(cube_patch_reverse MODULE patches/reverse_patch.cpp)
target_include_directories(cube_patch_reverse PRIVATE include)
set_target_properties(cube_patch_reverse PROPERTIES PREFIX "" OUTPUT_NAME "cube_patch_reverse")

if(BUILD_HOTPATCH_SELFTEST)
    enable_testing()
    add_executable(hotpatch_selftest
        tests/hotpatch_selftest.cpp
        src/cube_step.cpp
        src/agent/entry_hotpatch.cpp
    )
    target_include_directories(hotpatch_selftest PRIVATE include src)
    target_link_libraries(hotpatch_selftest PRIVATE ${CMAKE_DL_LIBS})
    target_compile_options(hotpatch_selftest PRIVATE -O2 -Wall -Wextra -Wpedantic)
    add_test(NAME raw-entry-hotpatch
             COMMAND hotpatch_selftest $<TARGET_FILE:cube_patch_wobble>)
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

    add_library(agent_snippet_inspect MODULE snippets/inspect_cube.cpp)
    target_include_directories(agent_snippet_inspect PRIVATE include src)
    target_link_libraries(agent_snippet_inspect PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    target_compile_options(agent_snippet_inspect PRIVATE -fno-access-control)
    set_target_properties(agent_snippet_inspect PROPERTIES
        PREFIX "" OUTPUT_NAME "agent_snippet_inspect")

    add_library(agent_snippet_observer MODULE snippets/install_observer.cpp)
    target_include_directories(agent_snippet_observer PRIVATE include src)
    target_link_libraries(agent_snippet_observer PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets)
    target_compile_options(agent_snippet_observer PRIVATE -fno-access-control)
    set_target_properties(agent_snippet_observer PROPERTIES
        PREFIX "" OUTPUT_NAME "agent_snippet_observer")
endif()
