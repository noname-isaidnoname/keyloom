cmake_minimum_required(VERSION 3.25)

project(loomwright
    VERSION 0.5.4
    DESCRIPTION "A Linux-first desktop automation language and runtime"
    LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")

# The reference compiler is Clang: on the runtime benchmark in
# tests/bench_runtime.cpp it runs the workload about 15% faster than GCC and
# produces a smaller binary (see docs/benchmark.md). GCC stays supported, and
# the CI workflow builds with both.
set(LOOMWRIGHT_REFERENCE_COMPILER "Clang")
option(LOOMWRIGHT_REQUIRE_REFERENCE_COMPILER
    "Fail the configure step unless the reference compiler (${LOOMWRIGHT_REFERENCE_COMPILER}) is used" OFF)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 16)
        message(FATAL_ERROR
            "Clang 16 or newer is required (found ${CMAKE_CXX_COMPILER_VERSION}). "
            "C++20 support in older releases is incomplete.")
    endif()
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    if(CMAKE_CXX_COMPILER_VERSION VERSION_LESS 13)
        message(FATAL_ERROR
            "GCC 13 or newer is required (found ${CMAKE_CXX_COMPILER_VERSION}). "
            "C++20 support in older releases is incomplete.")
    endif()
else()
    message(FATAL_ERROR
        "Unsupported C++ compiler '${CMAKE_CXX_COMPILER_ID}'. "
        "Use Clang ${LOOMWRIGHT_REFERENCE_COMPILER} or GCC.")
endif()

if(LOOMWRIGHT_REQUIRE_REFERENCE_COMPILER AND
   NOT CMAKE_CXX_COMPILER_ID MATCHES "${LOOMWRIGHT_REFERENCE_COMPILER}")
    message(FATAL_ERROR
        "LOOMWRIGHT_REQUIRE_REFERENCE_COMPILER is ON but the compiler is "
        "'${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}'. "
        "Configure with -DCMAKE_CXX_COMPILER=${LOOMWRIGHT_REFERENCE_COMPILER}++.")
endif()

message(STATUS "loomwright: C++ compiler ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

option(LOOMWRIGHT_USE_MOLD "Link with mold when the linker is installed" ON)
option(LOOMWRIGHT_SANITIZE "Build with the address and undefined behaviour sanitizers" OFF)
option(LOOMWRIGHT_HARDEN "Enable the usual release hardening flags" ON)
option(LOOMWRIGHT_WAYLAND "Enable native Wayland panels when GTK3 and wayland-client are available" ON)
option(LOOMWRIGHT_DESKTOP_INTEGRATION
    "Install the desktop entry, icon, MIME type and terminal launcher" ON)

# ccache: honour an explicit -DCMAKE_CXX_COMPILER_LAUNCHER, otherwise use ccache
# when it exists. CMakePresets.json sets the same thing for preset builds.
if(NOT CMAKE_CXX_COMPILER_LAUNCHER)
    find_program(LOOMWRIGHT_CCACHE ccache)
    if(LOOMWRIGHT_CCACHE)
        set(CMAKE_CXX_COMPILER_LAUNCHER "${LOOMWRIGHT_CCACHE}")
    endif()
endif()

if(LOOMWRIGHT_USE_MOLD AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    find_program(LOOMWRIGHT_MOLD mold)
    if(LOOMWRIGHT_MOLD)
        add_link_options(-fuse-ld=mold)
        message(STATUS "loomwright: linking with mold (${LOOMWRIGHT_MOLD})")
    endif()
endif()

include(GNUInstallDirs)
include(CTest)
find_package(PkgConfig REQUIRED)
pkg_check_modules(X11 REQUIRED IMPORTED_TARGET x11)
pkg_check_modules(XCB REQUIRED IMPORTED_TARGET xcb)
pkg_check_modules(LIBPNG REQUIRED IMPORTED_TARGET libpng)
if(LOOMWRIGHT_WAYLAND)
    pkg_check_modules(GTK3 QUIET IMPORTED_TARGET gtk+-wayland-3.0)
    pkg_check_modules(WAYLAND QUIET IMPORTED_TARGET wayland-client)
endif()

add_library(loomwright_options INTERFACE)
target_compile_options(loomwright_options INTERFACE
    -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Woverloaded-virtual)

if(LOOMWRIGHT_HARDEN AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(loomwright_options INTERFACE
        -fstack-protector-strong
        -fPIE)
    target_compile_definitions(loomwright_options INTERFACE _FORTIFY_SOURCE=3)
    target_link_options(loomwright_options INTERFACE -pie)
endif()

if(LOOMWRIGHT_SANITIZE AND CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    # The sanitizers replace the allocator, so the hardening flags that rely on
    # the libc's own checks are pointless here.
    target_compile_options(loomwright_options INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer
        -fno-sanitize-recover=undefined)
    target_link_options(loomwright_options INTERFACE -fsanitize=address,undefined)
endif()

add_library(loomwright_core STATIC
    src/value.cpp
    src/builtins.cpp
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
    src/runtime_input.cpp
    src/runtime_output.cpp
    src/runtime_gui.cpp
    src/mousepos.cpp
    src/picker_xcb.cpp
    src/picker_image.cpp
    src/gui.cpp
    src/gui_wayland.cpp
    src/xcb_connection.cpp
    src/cli.cpp)
target_include_directories(loomwright_core PUBLIC src)
target_link_libraries(loomwright_core PUBLIC loomwright_options PkgConfig::X11 PkgConfig::XCB PkgConfig::LIBPNG)
if(LOOMWRIGHT_WAYLAND AND GTK3_FOUND AND WAYLAND_FOUND)
    target_link_libraries(loomwright_core PUBLIC PkgConfig::GTK3 PkgConfig::WAYLAND)
    target_compile_definitions(loomwright_core PRIVATE LOOMWRIGHT_WAYLAND_GUI=1)
    message(STATUS "loomwright: native Wayland panels enabled (GTK3)")
elseif(NOT LOOMWRIGHT_WAYLAND)
    message(STATUS "loomwright: native Wayland panels disabled")
else()
    message(STATUS "loomwright: native Wayland panels unavailable (GTK3/wayland-client development files missing)")
endif()
target_compile_definitions(loomwright_core PUBLIC LOOMWRIGHT_VERSION="${PROJECT_VERSION}")

add_executable(loomwright src/main.cpp)
target_link_libraries(loomwright PRIVATE loomwright_core)

if(BUILD_TESTING)
    target_compile_definitions(loomwright_core PRIVATE LOOMWRIGHT_WAYLAND_TEST_HOOKS=1)
    # The test harness and the probes are not performance critical: the code
    # they exercise lives in loomwright_core, which keeps the release optimisation
    # level. Building the harness itself at -O1 takes a noticeable slice off a
    # cold build without changing the behaviour under test.
    add_library(loomwright_test_options INTERFACE)
    target_compile_options(loomwright_test_options INTERFACE -O1)

    add_executable(loomwright_tests
        tests/test_main.cpp
        tests/test_language.cpp
        tests/test_runtime.cpp
        tests/test_runtime_language.cpp
        tests/test_backends.cpp
        tests/test_mousepos.cpp
        tests/test_scheduler.cpp)
    target_link_libraries(loomwright_tests PRIVATE loomwright_core loomwright_test_options)
    target_compile_definitions(loomwright_tests PRIVATE LOOMWRIGHT_SOURCE_DIR="${CMAKE_SOURCE_DIR}")
    add_test(NAME loomwright_tests COMMAND loomwright_tests)

    # A synthetic benchmark for the front end and the runtime. It is not a
    # test: it prints timings so a change to the code generation can be
    # compared against the previous build.
    add_executable(loomwright_bench tests/bench_runtime.cpp)
    target_link_libraries(loomwright_bench PRIVATE loomwright_core)
    target_compile_definitions(loomwright_bench PRIVATE
        LOOMWRIGHT_BENCH_COMPILER="${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION}")

    add_executable(evdev_handoff tests/evdev_handoff.cpp)
    target_link_libraries(evdev_handoff PRIVATE loomwright_core loomwright_test_options)
    add_test(NAME evdev_handoff COMMAND evdev_handoff)
    set_tests_properties(evdev_handoff PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 15)

    # The picker needs a real X server; the shell test starts a private one and
    # skips (77) when none is available.
    add_executable(picker_event tests/picker_event.cpp)
    target_link_libraries(picker_event PRIVATE PkgConfig::X11 loomwright_test_options)
    add_executable(picker_lifecycle tests/picker_lifecycle.cpp)
    target_link_libraries(picker_lifecycle PRIVATE loomwright_core loomwright_test_options)
    add_executable(picker_upload_probe tests/picker_upload_probe.cpp)
    target_link_libraries(picker_upload_probe PRIVATE loomwright_core loomwright_test_options)
    add_executable(gui_lifetime tests/gui_lifetime.cpp)
    target_link_libraries(gui_lifetime PRIVATE loomwright_core loomwright_test_options)
    add_executable(gui_mapping_runtime_probe tests/gui_mapping_runtime_probe.cpp)
    target_link_libraries(gui_mapping_runtime_probe PRIVATE loomwright_core loomwright_test_options)
    add_executable(gui_startup_probe tests/gui_startup_probe.cpp)
    target_link_libraries(gui_startup_probe PRIVATE loomwright_core loomwright_test_options)
    if(LOOMWRIGHT_WAYLAND AND GTK3_FOUND AND WAYLAND_FOUND)
        add_executable(wayland_gui_probe tests/wayland_gui_probe.cpp)
        target_link_libraries(wayland_gui_probe PRIVATE loomwright_core loomwright_test_options)
        add_test(NAME wayland_integration
            COMMAND ${CMAKE_COMMAND} -E env
                LOOMWRIGHT_BIN=$<TARGET_FILE:loomwright>
                LOOMWRIGHT_WAYLAND_PROBE=$<TARGET_FILE:wayland_gui_probe>
                LOOMWRIGHT_TEST_HOOKS=1
                bash ${CMAKE_SOURCE_DIR}/tests/wayland_integration.sh)
        set_tests_properties(wayland_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RESOURCE_LOCK graphical_integration)
    endif()
    # A probe for a recoverable alternative to Xlib's fatal I/O behavior.
    add_executable(xcb_disconnect_probe tests/xcb_disconnect_probe.cpp)
    target_link_libraries(xcb_disconnect_probe PRIVATE loomwright_core loomwright_test_options)
    # Real /dev/uinput ioctl fault injection (LD_PRELOAD shim + probe).
    add_executable(uinput_fault_probe tests/uinput_fault_probe.cpp)
    target_link_libraries(uinput_fault_probe PRIVATE loomwright_core loomwright_test_options)
    add_library(loomwright_uinput_ioctl_fault_shim SHARED tests/uinput_ioctl_fault_shim.c)
    target_link_libraries(loomwright_uinput_ioctl_fault_shim PRIVATE ${CMAKE_DL_LIBS})
    # Graphical integration tests create several compositors and helper
    # processes. Serialize them to avoid startup starvation on small CI
    # runners; the picker variants also share screenshot tools.
    add_test(NAME picker_integration
        COMMAND ${CMAKE_COMMAND} -E env
            LOOMWRIGHT_BIN=$<TARGET_FILE:loomwright>
            LOOMWRIGHT_PICKER_EVENT=$<TARGET_FILE:picker_event>
            LOOMWRIGHT_PICKER_LIFECYCLE=$<TARGET_FILE:picker_lifecycle>
            LOOMWRIGHT_PICKER_UPLOAD_PROBE=$<TARGET_FILE:picker_upload_probe>
            XDG_SESSION_TYPE=x11
            PICKER_TEST_DISPLAY_NUMBERS=91,92,93,94,95
            bash ${CMAKE_SOURCE_DIR}/tests/picker_integration.sh)
    set_tests_properties(picker_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RESOURCE_LOCK graphical_integration)
    find_program(LOOMWRIGHT_XVFB Xvfb)
    if(LOOMWRIGHT_XVFB)
        add_test(NAME picker_integration_16
            COMMAND ${CMAKE_COMMAND} -E env
                LOOMWRIGHT_BIN=$<TARGET_FILE:loomwright>
                LOOMWRIGHT_PICKER_EVENT=$<TARGET_FILE:picker_event>
                LOOMWRIGHT_PICKER_LIFECYCLE=$<TARGET_FILE:picker_lifecycle>
                LOOMWRIGHT_PICKER_UPLOAD_PROBE=$<TARGET_FILE:picker_upload_probe>
                XDG_SESSION_TYPE=x11
                PICKER_TEST_DEPTH=16
                PICKER_TEST_DISPLAY_NUMBERS=81,82,83,84,85
                bash ${CMAKE_SOURCE_DIR}/tests/picker_integration.sh)
        set_tests_properties(picker_integration_16 PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 120 RESOURCE_LOCK graphical_integration)
    endif()
    add_test(NAME gui_integration
        COMMAND ${CMAKE_COMMAND} -E env
            LOOMWRIGHT_BIN=$<TARGET_FILE:loomwright>
            LOOMWRIGHT_PICKER_EVENT=$<TARGET_FILE:picker_event>
            LOOMWRIGHT_GUI_LIFETIME=$<TARGET_FILE:gui_lifetime>
            LOOMWRIGHT_GUI_MAPPING_RUNTIME_PROBE=$<TARGET_FILE:gui_mapping_runtime_probe>
            LOOMWRIGHT_GUI_STARTUP_PROBE=$<TARGET_FILE:gui_startup_probe>
            LOOMWRIGHT_XCB_PROBE=$<TARGET_FILE:xcb_disconnect_probe>
            XDG_SESSION_TYPE=x11
            bash ${CMAKE_SOURCE_DIR}/tests/gui_integration.sh)
    set_tests_properties(gui_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 60 RESOURCE_LOCK graphical_integration)

    add_test(NAME uinput_fault_integration
        COMMAND ${CMAKE_COMMAND} -E env
            LOOMWRIGHT_UINPUT_FAULT_PROBE=$<TARGET_FILE:uinput_fault_probe>
            LOOMWRIGHT_UINPUT_IOCTL_FAULT_SHIM=$<TARGET_FILE:loomwright_uinput_ioctl_fault_shim>
            bash ${CMAKE_SOURCE_DIR}/tests/uinput_fault_integration.sh)
    set_tests_properties(uinput_fault_integration PROPERTIES SKIP_RETURN_CODE 77 TIMEOUT 30)
endif()

install(TARGETS loomwright RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(FILES LICENSE DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/loomwright")

# Desktop integration: an application entry with an icon, the .loom MIME type
# and the terminal launcher the entry calls. Every packaged build (DEB, RPM,
# tarball, Arch) installs these, so the program is visible in the application
# menu and double-clicking a script opens a terminal.
if(LOOMWRIGHT_DESKTOP_INTEGRATION)
    install(FILES packaging/common/loomwright.desktop
        DESTINATION "${CMAKE_INSTALL_DATADIR}/applications")
    install(FILES packaging/common/loomwright.svg
        DESTINATION "${CMAKE_INSTALL_DATADIR}/icons/hicolor/scalable/apps")
    install(FILES packaging/common/loomwright.xml
        DESTINATION "${CMAKE_INSTALL_DATADIR}/mime/packages")
    install(PROGRAMS packaging/common/loomwright-terminal
        DESTINATION "${CMAKE_INSTALL_BINDIR}")
endif()

set(CPACK_PACKAGE_NAME loomwright)
set(CPACK_PACKAGE_VENDOR "Loomwright contributors")
set(CPACK_PACKAGE_CONTACT "Loomwright contributors")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Linux desktop automation language and runtime")
set(CPACK_RESOURCE_FILE_LICENSE "${CMAKE_SOURCE_DIR}/LICENSE")
set(CPACK_PACKAGE_FILE_NAME "loomwright-${PROJECT_VERSION}-${CMAKE_SYSTEM_PROCESSOR}")
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_RECOMMENDS "shared-mime-info, desktop-file-utils, hicolor-icon-theme")
set(CPACK_RPM_PACKAGE_SUGGESTS "shared-mime-info, desktop-file-utils, hicolor-icon-theme")
set(CPACK_RPM_PACKAGE_LICENSE MIT)
set(CPACK_RPM_PACKAGE_AUTOREQPROV ON)
if(LOOMWRIGHT_DESKTOP_INTEGRATION)
    # Refresh the desktop, icon and MIME caches after the files land.
    set(CPACK_DEBIAN_PACKAGE_CONTROL_EXTRA
        "${CMAKE_SOURCE_DIR}/packaging/common/postinst"
        "${CMAKE_SOURCE_DIR}/packaging/common/postrm")
    set(CPACK_RPM_POST_INSTALL_SCRIPT_FILE
        "${CMAKE_SOURCE_DIR}/packaging/common/rpm/post-install.sh")
    set(CPACK_RPM_POST_UNINSTALL_SCRIPT_FILE
        "${CMAKE_SOURCE_DIR}/packaging/common/rpm/post-uninstall.sh")
endif()
include(CPack)
