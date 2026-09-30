cmake_minimum_required(VERSION 3.24)
project(TabEngine VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(tabengine_core STATIC src/core/model.cpp src/core/layout.cpp src/core/theme.cpp)
target_include_directories(tabengine_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
set_target_properties(tabengine_core PROPERTIES EXPORT_NAME Core)

# Automation control runtime: platform-neutral protocol values, registry,
# queueing and observation. Depends on core only; the pinned JSON parser is
# a private implementation detail of this target.
add_library(tabengine_automation STATIC
    src/automation/json_value.cpp
    src/automation/errors.cpp
    src/automation/schema.cpp
    src/automation/artifacts.cpp
    src/automation/runtime.cpp)
target_include_directories(tabengine_automation PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)
target_include_directories(tabengine_automation SYSTEM PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/third_party/json")
target_link_libraries(tabengine_automation PUBLIC tabengine_core)
set_target_properties(tabengine_automation PROPERTIES EXPORT_NAME Automation)

# Test tree is a top-level development concern: add_subdirectory consumers
# get libraries only and opt in explicitly with -DBUILD_TESTING=ON.
if(PROJECT_IS_TOP_LEVEL)
    include(CTest)
endif()
if(BUILD_TESTING)
    add_executable(tabengine_core_test tests/core_test.cpp)
    target_link_libraries(tabengine_core_test PRIVATE tabengine_core)
    add_test(NAME tabengine_core_test COMMAND tabengine_core_test)

    add_executable(tabengine_automation_test tests/automation_values_test.cpp
        tests/automation_runtime_test.cpp)
    target_link_libraries(tabengine_automation_test PRIVATE tabengine_automation)
    add_test(NAME tabengine_automation_test COMMAND tabengine_automation_test)
endif()

option(TABENGINE_BUILD_WIN32_DEMO "Build the Win32 Skia reference host" OFF)
option(TABENGINE_BUILD_SKIA "Build the Windows Skia shell and renderer libraries" OFF)
option(TABENGINE_INSTALL_SDK "Install a redistributable Windows SDK with a bundled Skia DLL" OFF)
set(TABENGINE_SKIA_ROOT "" CACHE PATH "Path to a Skia source or binary package root")

if(TABENGINE_BUILD_WIN32_DEMO)
    set(TABENGINE_BUILD_SKIA ON)
endif()

if(WIN32)
    add_library(tabengine_win32 STATIC src/win32/win32_platform.cpp)
    target_include_directories(tabengine_win32 PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    set_target_properties(tabengine_win32 PROPERTIES EXPORT_NAME Win32)
    target_compile_definitions(tabengine_win32 PRIVATE UNICODE _UNICODE)
    target_link_libraries(tabengine_win32 PUBLIC tabengine_core user32 dwmapi)

    if(BUILD_TESTING)
        add_executable(tabengine_wake_test tests/wake_test.cpp)
        target_link_libraries(tabengine_wake_test PRIVATE tabengine_win32)
        add_test(NAME tabengine_wake_test COMMAND tabengine_wake_test)

        add_executable(tabengine_ime_test tests/ime_test.cpp)
        target_link_libraries(tabengine_ime_test PRIVATE tabengine_win32)
        add_test(NAME tabengine_ime_test COMMAND tabengine_ime_test)

        add_executable(tabengine_pointer_button_test tests/pointer_button_test.cpp)
        target_link_libraries(tabengine_pointer_button_test PRIVATE tabengine_win32)
        add_test(NAME tabengine_pointer_button_test COMMAND tabengine_pointer_button_test)
    endif()
endif()

if(TABENGINE_BUILD_SKIA AND NOT WIN32)
    message(FATAL_ERROR "The Skia shell currently has only a Windows backend")
endif()

if(NOT WIN32)
    message(STATUS "TabEngine: core-only configuration (the Win32 host and Skia shell require Windows; see docs/PORTING.md)")
endif()

if(WIN32 AND TABENGINE_BUILD_SKIA)
    if(NOT EXISTS "${TABENGINE_SKIA_ROOT}/include/core/SkCanvas.h" OR
       NOT EXISTS "${TABENGINE_SKIA_ROOT}/lib/skia.lib")
        message(FATAL_ERROR "Set TABENGINE_SKIA_ROOT to a Skia package with include/ and lib/skia.lib")
    endif()
    add_library(tabengine_skia STATIC src/core/shell.cpp src/skia/raster_renderer.cpp
        src/skia/d3d12_renderer.cpp src/skia/win32_text.cpp)
    target_include_directories(tabengine_skia PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    set_target_properties(tabengine_skia PROPERTIES EXPORT_NAME SkiaShell)
    target_include_directories(tabengine_skia PRIVATE "${TABENGINE_SKIA_ROOT}")
    target_compile_definitions(tabengine_skia PRIVATE SK_GANESH SK_DIRECT3D SK_BUILD_FOR_WIN)
    target_link_libraries(tabengine_skia PUBLIC tabengine_core)
    set(_tabengine_skia_dll "${TABENGINE_SKIA_ROOT}/bin/skia.dll")
    if(EXISTS "${_tabengine_skia_dll}")
        add_library(TabEngine::Skia SHARED IMPORTED GLOBAL)
        set_target_properties(TabEngine::Skia PROPERTIES
            IMPORTED_IMPLIB "${TABENGINE_SKIA_ROOT}/lib/skia.lib"
            IMPORTED_LOCATION "${_tabengine_skia_dll}"
            INTERFACE_INCLUDE_DIRECTORIES "${TABENGINE_SKIA_ROOT}"
            INTERFACE_COMPILE_DEFINITIONS SKIA_DLL)
        target_link_libraries(tabengine_skia PUBLIC TabEngine::Skia)
    else()
        target_link_libraries(tabengine_skia PUBLIC "${TABENGINE_SKIA_ROOT}/lib/skia.lib")
        foreach(component IN ITEMS d3d12allocator libpng skcms spirv_cross zlib)
            if(EXISTS "${TABENGINE_SKIA_ROOT}/lib/${component}.lib")
                target_link_libraries(tabengine_skia PUBLIC "${TABENGINE_SKIA_ROOT}/lib/${component}.lib")
            endif()
        endforeach()
    endif()
    target_link_libraries(tabengine_skia PUBLIC
        d3d12 dxgi dxguid d3dcompiler windowscodecs ole32 uuid dwrite usp10 gdi32)

    # Optional loopback HTTP transport: the only sockets in the engine.
    # Windows-only for now; other platforms report the transport as absent.
    add_library(tabengine_automation_transport STATIC
        src/automation/transport_win32.cpp)
    target_include_directories(tabengine_automation_transport PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_definitions(tabengine_automation_transport PRIVATE UNICODE _UNICODE)
    target_link_libraries(tabengine_automation_transport
        PUBLIC tabengine_automation PRIVATE ws2_32)
    set_target_properties(tabengine_automation_transport PROPERTIES
        EXPORT_NAME AutomationTransport)

    # Shell bridge: engine domains bound to one Shell (Window/Tab/Input/UI).
    # Windows-only because the logical-key table and input synthesis route
    # through the Win32 backend; links the Skia shell because Shell lives
    # there.
    add_library(tabengine_automation_shell STATIC
        src/automation/shell_bridge.cpp
        src/automation/shell_bridge_input.cpp
        src/automation/shell_bridge_ui.cpp
        src/automation/shell_bridge_capture.cpp)
    target_include_directories(tabengine_automation_shell PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
    target_compile_definitions(tabengine_automation_shell PRIVATE UNICODE _UNICODE)
    target_link_libraries(tabengine_automation_shell
        PUBLIC tabengine_automation tabengine_skia tabengine_win32)
    set_target_properties(tabengine_automation_shell PROPERTIES EXPORT_NAME AutomationShell)

    if(TABENGINE_INSTALL_SDK)
        if(NOT TARGET TabEngine::Skia)
            message(FATAL_ERROR "TABENGINE_INSTALL_SDK requires a shared Skia package with bin/skia.dll")
        endif()
        include(CMakePackageConfigHelpers)
        install(TARGETS tabengine_core tabengine_win32 tabengine_skia
            EXPORT TabEngineTargets ARCHIVE DESTINATION lib)
        install(EXPORT TabEngineTargets FILE TabEngineTargets.cmake
            NAMESPACE TabEngine:: DESTINATION lib/cmake/TabEngine)
        install(DIRECTORY include/ DESTINATION include)
        install(FILES SDK_PACKAGE_README.md DESTINATION . RENAME README.md)
        install(FILES docs/SDK_USER_GUIDE.md docs/API_REFERENCE.md DESTINATION docs)
        install(DIRECTORY examples/sdk_hello_tabs/ DESTINATION examples/sdk_hello_tabs)
        install(DIRECTORY "${TABENGINE_SKIA_ROOT}/include" DESTINATION skia)
        install(DIRECTORY "${TABENGINE_SKIA_ROOT}/modules" DESTINATION skia OPTIONAL)
        install(DIRECTORY "${TABENGINE_SKIA_ROOT}/third_party" DESTINATION skia OPTIONAL)
        install(FILES "${TABENGINE_SKIA_ROOT}/lib/skia.lib" DESTINATION lib)
        install(FILES
            "${TABENGINE_SKIA_ROOT}/lib/freetype2.lib"
            "${TABENGINE_SKIA_ROOT}/lib/libpng.lib"
            "${TABENGINE_SKIA_ROOT}/lib/zlib.lib"
            DESTINATION lib)
        install(FILES "${_tabengine_skia_dll}" DESTINATION bin)
        configure_package_config_file(cmake/TabEngineConfig.cmake.in
            "${CMAKE_CURRENT_BINARY_DIR}/TabEngineConfig.cmake"
            INSTALL_DESTINATION lib/cmake/TabEngine)
        write_basic_package_version_file(
            "${CMAKE_CURRENT_BINARY_DIR}/TabEngineConfigVersion.cmake"
            VERSION ${PROJECT_VERSION} COMPATIBILITY SameMajorVersion)
        install(FILES
            "${CMAKE_CURRENT_BINARY_DIR}/TabEngineConfig.cmake"
            "${CMAKE_CURRENT_BINARY_DIR}/TabEngineConfigVersion.cmake"
            DESTINATION lib/cmake/TabEngine)
    endif()

    if(TABENGINE_BUILD_WIN32_DEMO)
        add_executable(tabengine_win32_demo WIN32 examples/win32_demo.cpp)
        target_compile_definitions(tabengine_win32_demo PRIVATE UNICODE _UNICODE)
        target_include_directories(tabengine_win32_demo PRIVATE "${TABENGINE_SKIA_ROOT}")
        target_compile_definitions(tabengine_win32_demo PRIVATE SK_GANESH SK_DIRECT3D SK_BUILD_FOR_WIN)
        target_link_libraries(tabengine_win32_demo PRIVATE tabengine_skia tabengine_win32)
        tabengine_deploy_skia_dll(tabengine_win32_demo)
    endif()

    # Tests and demos must run unattended: a launch with the bundled DLL
    # missing from PATH pops a system loader dialog and blocks the whole
    # suite, so deploy skia.dll beside every executable that links the
    # shell whenever the shared Skia package carries one.
    function(tabengine_deploy_skia_dll target)
        if(EXISTS "${TABENGINE_SKIA_ROOT}/bin/skia.dll")
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${TABENGINE_SKIA_ROOT}/bin/skia.dll"
                    "$<TARGET_FILE_DIR:${target}>/skia.dll"
                VERBATIM)
        endif()
    endfunction()

    if(BUILD_TESTING)
        add_executable(tabengine_shell_test tests/shell_test.cpp)
        target_include_directories(tabengine_shell_test PRIVATE "${TABENGINE_SKIA_ROOT}")
        target_compile_definitions(tabengine_shell_test PRIVATE SK_GANESH SK_DIRECT3D SK_BUILD_FOR_WIN)
        target_link_libraries(tabengine_shell_test PRIVATE tabengine_skia)
        tabengine_deploy_skia_dll(tabengine_shell_test)
        add_test(NAME tabengine_shell_test COMMAND tabengine_shell_test)

        add_executable(tabengine_shell_lifecycle_test tests/shell_lifecycle_test.cpp)
        target_include_directories(tabengine_shell_lifecycle_test PRIVATE "${TABENGINE_SKIA_ROOT}")
        target_compile_definitions(tabengine_shell_lifecycle_test PRIVATE SK_GANESH SK_DIRECT3D SK_BUILD_FOR_WIN)
        target_link_libraries(tabengine_shell_lifecycle_test PRIVATE tabengine_skia)
        tabengine_deploy_skia_dll(tabengine_shell_lifecycle_test)
        add_test(NAME tabengine_shell_lifecycle_test COMMAND tabengine_shell_lifecycle_test)

        add_executable(tabengine_native_drag_cancel_test tests/native_drag_cancel_test.cpp)
        target_link_libraries(tabengine_native_drag_cancel_test PRIVATE tabengine_skia)
        tabengine_deploy_skia_dll(tabengine_native_drag_cancel_test)
        add_test(NAME tabengine_native_drag_cancel_test COMMAND tabengine_native_drag_cancel_test)

        add_executable(tabengine_text_input_test tests/text_input_test.cpp)
        target_link_libraries(tabengine_text_input_test PRIVATE tabengine_skia)
        tabengine_deploy_skia_dll(tabengine_text_input_test)
        add_test(NAME tabengine_text_input_test COMMAND tabengine_text_input_test)

        add_executable(tabengine_body_drag_test tests/body_drag_test.cpp)
        target_include_directories(tabengine_body_drag_test PRIVATE "${TABENGINE_SKIA_ROOT}")
        target_compile_definitions(tabengine_body_drag_test PRIVATE SK_GANESH SK_DIRECT3D SK_BUILD_FOR_WIN)
        target_link_libraries(tabengine_body_drag_test PRIVATE tabengine_skia)
        tabengine_deploy_skia_dll(tabengine_body_drag_test)
        add_test(NAME tabengine_body_drag_test COMMAND tabengine_body_drag_test)

        add_executable(tabengine_windows_renderer_test tests/windows_renderer_test.cpp)
        target_compile_definitions(tabengine_windows_renderer_test PRIVATE UNICODE _UNICODE)
        target_include_directories(tabengine_windows_renderer_test PRIVATE "${TABENGINE_SKIA_ROOT}")
        target_compile_definitions(tabengine_windows_renderer_test PRIVATE SK_GANESH SK_DIRECT3D SK_BUILD_FOR_WIN)
        target_link_libraries(tabengine_windows_renderer_test PRIVATE tabengine_skia user32)
        tabengine_deploy_skia_dll(tabengine_windows_renderer_test)
        add_test(NAME tabengine_windows_renderer_test COMMAND tabengine_windows_renderer_test)

        add_executable(tabengine_automation_bridge_test tests/automation_bridge_test.cpp)
        target_include_directories(tabengine_automation_bridge_test PRIVATE "${TABENGINE_SKIA_ROOT}")
        target_compile_definitions(tabengine_automation_bridge_test PRIVATE
            SK_GANESH SK_DIRECT3D SK_BUILD_FOR_WIN)
        target_link_libraries(tabengine_automation_bridge_test
            PRIVATE tabengine_automation_shell)
        tabengine_deploy_skia_dll(tabengine_automation_bridge_test)
        add_test(NAME tabengine_automation_bridge_test COMMAND tabengine_automation_bridge_test)

        add_executable(tabengine_automation_transport_test
            tests/automation_transport_test.cpp)
        target_compile_definitions(tabengine_automation_transport_test PRIVATE UNICODE _UNICODE)
        target_link_libraries(tabengine_automation_transport_test
            PRIVATE tabengine_automation_transport)
        add_test(NAME tabengine_automation_transport_test
            COMMAND tabengine_automation_transport_test)
    endif()
endif()

if(TABENGINE_INSTALL_SDK AND NOT TABENGINE_BUILD_SKIA)
    message(FATAL_ERROR "TABENGINE_INSTALL_SDK requires TABENGINE_BUILD_SKIA=ON")
endif()
