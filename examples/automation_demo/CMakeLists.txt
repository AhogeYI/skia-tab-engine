# The packaged copy of this example builds against an extracted SDK only:
# it declares no source-tree dependency and links the public targets.
cmake_minimum_required(VERSION 3.24)
project(TabEngineAutomationDemo LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(TABENGINE_SDK_ROOT "" CACHE PATH "Extracted TabEngine SDK root")
if(NOT TABENGINE_SDK_ROOT)
    message(FATAL_ERROR "Set TABENGINE_SDK_ROOT to an extracted TabEngine SDK root")
endif()

# The packaged manifest is the version authority; the example tracks the
# SDK it ships in instead of pinning a release by hand.
file(READ "${TABENGINE_SDK_ROOT}/manifest.json" _sdk_manifest)
string(JSON _sdk_version ERROR_VARIABLE _sdk_version_error
    GET "${_sdk_manifest}" version)
if(_sdk_version_error OR NOT _sdk_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "The SDK manifest has no valid package version")
endif()

find_package(TabEngine ${_sdk_version} EXACT CONFIG REQUIRED
    PATHS "${TABENGINE_SDK_ROOT}" NO_DEFAULT_PATH)

add_executable(tabengine_automation_demo main.cpp)
target_compile_definitions(tabengine_automation_demo PRIVATE UNICODE _UNICODE
    TABENGINE_DEMO_ENGINE_VERSION="${TabEngine_VERSION}")
target_link_libraries(tabengine_automation_demo PRIVATE
    TabEngine::SDK
    TabEngine::Automation
    TabEngine::AutomationShell
    TabEngine::AutomationTransport)
add_custom_command(TARGET tabengine_automation_demo POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${TABENGINE_SDK_ROOT}/bin/skia.dll"
        "$<TARGET_FILE_DIR:tabengine_automation_demo>/skia.dll")
