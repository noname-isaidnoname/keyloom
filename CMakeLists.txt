cmake_minimum_required(VERSION 3.25)

project(keyloom
    VERSION 0.2.0
    DESCRIPTION "A Linux-first desktop automation language and runtime"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

option(KEYLOOM_USE_MOLD "Link with mold when the linker is installed" ON)

# ccache: honour an explicit -DCMAKE_CXX_COMPILER_LAUNCHER, otherwise use ccache
# when it exists. CMakePresets.json sets the same thing for preset builds.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(KEYLOOM_CCACHE ccache)
    if(KEYLOOM_CCACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${KEYLOOM_CCACHE}")
    endif()
endif()

if(KEYLOOM_USE_MOLD AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    find_program(KEYLOOM_MOLD mold)
    if(KEYLOOM_MOLD)
        add_link_options(-fuse-ld=mold)
        message(STATUS "keyloom: linking with mold (${KEYLOOM_MOLD})")
    endif()
endif()

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(LIBPNG REQUIRED IMPORTED_TARGET libpng)

add_library(keyloom_options INTERFACE)
target_compile_options(keyloom_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual)

add_library(keyloom_core STATIC
    src/value.cpp
    src/lexer.cpp
    src/parser.cpp
    src/checker.cpp
    src/keys.cpp
    src/docs.cpp
    src/subprocess.cpp
    src/uinput.cpp
    src/backends.cpp
    src/scheduler.cpp
    src/input.cpp
    src/runtime.cpp
    src/mousepos.cpp
    src/picker_xcb.cpp
    src/picker_image.cpp
    src/gui.cpp
    src/xcb_connection.cpp
    src/cli.cpp)
target_include_directories(keyloom_core PUBLIC src)
target_link_libraries(keyloom_core PUBLIC keyloom_options PkgConfig::X11 PkgConfig::XCB PkgConfig::LIBPNG)
target_compile_definitions(keyloom_core PUBLIC KEYLOOM_VERSION="${PROJECT_VERSION}")

add_executable(keyloom src/main.cpp)
target_link_libraries(keyloom PRIVATE keyloom_core)

if(BUILD_TESTING)
    add_executable(keyloom_tests
        tests/test_main.cpp
        tests/test_language.cpp
        tests/test_runtime.cpp
        tests/test_backends.cpp
        tests/test_mousepos.cpp
        tests/test_scheduler.cpp)
    target_link_libraries(keyloom_tests PRIVATE keyloom_core)
    target_compile_definitions(keyloom_tests PRIVATE KEYLOOM_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME keyloom_tests COMMAND keyloom_tests)

    add_executable(evdev_handoff tests/evdev_handoff.cpp)
    target_link_libraries(evdev_handoff PRIVATE keyloom_core)
    add_test(NAME evdev_handoff COMMAND evdev_handoff)
    set_tests_properties(evdev_handoff PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)

    # The picker needs a real X server; the shell test starts a private one and
    # skips (77) when none is available.
    add_executable(picker_event tests/picker_event.cpp)
    target_link_libraries(picker_event PRIVATE PkgConfig::X11)
    add_executable(picker_lifecycle tests/picker_lifecycle.cpp)
    target_link_libraries(picker_lifecycle PRIVATE keyloom_core)
    add_executable(picker_upload_probe tests/picker_upload_probe.cpp)
    target_link_libraries(picker_upload_probe PRIVATE keyloom_core)
    add_executable(gui_lifetime tests/gui_lifetime.cpp)
    target_link_libraries(gui_lifetime PRIVATE keyloom_core)
    add_executable(gui_startup_probe tests/gui_startup_probe.cpp)
    target_link_libraries(gui_startup_probe PRIVATE keyloom_core)
    # A probe for a recoverable alternative to Xlib's fatal I/O behavior.
    add_executable(xcb_disconnect_probe tests/xcb_disconnect_probe.cpp)
    target_link_libraries(xcb_disconnect_probe PRIVATE keyloom_core)
    add_test(NAME picker_integration
        COMMAND ${CMAKE_COMMAND} -E env
            KEYLOOM_BIN=$<TARGET_FILE:keyloom>
            KEYLOOM_PICKER_EVENT=$<TARGET_FILE:picker_event>
            KEYLOOM_PICKER_LIFECYCLE=$<TARGET_FILE:picker_lifecycle>
            KEYLOOM_PICKER_UPLOAD_PROBE=$<TARGET_FILE:picker_upload_probe>
            bash ${CMAKE_SOURCE_DIR}/tests/picker_integration.sh)
    set_tests_properties(picker_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RESOURCE_LOCK picker_display)
    find_program(KEYLOOM_XVFB Xvfb)
    if(KEYLOOM_XVFB)
        add_test(NAME picker_integration_16
            COMMAND ${CMAKE_COMMAND} -E env
                KEYLOOM_BIN=$<TARGET_FILE:keyloom>
                KEYLOOM_PICKER_EVENT=$<TARGET_FILE:picker_event>
                KEYLOOM_PICKER_LIFECYCLE=$<TARGET_FILE:picker_lifecycle>
                KEYLOOM_PICKER_UPLOAD_PROBE=$<TARGET_FILE:picker_upload_probe>
                PICKER_TEST_DEPTH=16
                bash ${CMAKE_SOURCE_DIR}/tests/picker_integration.sh)
        set_tests_properties(picker_integration_16 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RESOURCE_LOCK picker_display)
    endif()
    add_test(NAME gui_integration
        COMMAND ${CMAKE_COMMAND} -E env
            KEYLOOM_BIN=$<TARGET_FILE:keyloom>
            KEYLOOM_PICKER_EVENT=$<TARGET_FILE:picker_event>
            KEYLOOM_GUI_LIFETIME=$<TARGET_FILE:gui_lifetime>
            KEYLOOM_GUI_STARTUP_PROBE=$<TARGET_FILE:gui_startup_probe>
            KEYLOOM_XCB_PROBE=$<TARGET_FILE:xcb_disconnect_probe>
            bash ${CMAKE_SOURCE_DIR}/tests/gui_integration.sh)
    set_tests_properties(gui_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60)
endif()

install(TARGETS keyloom RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
